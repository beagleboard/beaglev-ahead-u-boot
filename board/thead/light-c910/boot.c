// SPDX-License-Identifier: GPL-2.0+

#include <common.h>
#include <command.h>
#include <net.h>
#include <asm/io.h>
#include <dm.h>
#include <fdt_support.h>
#include <fdtdec.h>
#include <opensbi.h>
#include <asm/csr.h>
#include <asm/arch-thead/boot_mode.h>

static struct fw_dynamic_info opensbi_info;

enum board_type {
	NT_DTS = 0,
	T_DTS,
	UBOOT_DTS,
};

long t_start_address;
long t_size;
long nt_start_address = 0;
long nt_size;
long t_dtb_address;
long t_kernel_address;
long nt_dtb_address;
struct pmp {
	long start;
	long end;
	int r, w, x;
};
struct pmp pmp_configs[32];
static int total_pmp = 0;
static int setup_nt_pmp_array(long start, long size)
{
	if (total_pmp == 31) {
		debug("PMP entries are full!!!\n");
		return -1;
	}

	pmp_configs[total_pmp].start = start;
	pmp_configs[total_pmp].end = start + size;

	total_pmp++;

	return 0;
}

static void __maybe_unused dump_nt_pmp_array(void)
{
	debug("total pmp number: %d\n", total_pmp);
	for (int i = 0; i < total_pmp; i++) {
		debug("pmp[%d]: 0x%lx ~ 0x%lx\n",
			i, pmp_configs[i].start, pmp_configs[i].end);
	}
}

static void setup_nt_pmp_configs(void)
{
	long pmp_entry = PMP_BASE_ADDR + csr_read(CSR_MHARTID) * 0x4000 + 0x100;
	long pmp_cfg = PMP_BASE_ADDR + csr_read(CSR_MHARTID) * 0x4000 + 0x0;

	for (int i = 0, j = 0; i < total_pmp; i++) {
		writel(pmp_configs[i].start >> 12, (void *)(pmp_entry + j * 4));
		j++;
		writel(pmp_configs[i].end >> 12, (void *)(pmp_entry + j * 4));
		j++;
	}

	for (int k = 0; k < total_pmp; k++) {
		int x, y;
		x = k / 4;
		y = k % 4;

		/* pmp_configs[0] must be memory */
		if (k == 0)
			writel(readl((void *)(pmp_cfg + x * 4)) | 0x87 << y * 8, (void *)(pmp_cfg + x * 4));
		else
			writel(readl((void *)(pmp_cfg + x * 4)) | 0x83 << y * 8, (void *)(pmp_cfg + x * 4));
	}

	/* Set default pmp all allow but for T
	 * So NT can't use it either
	 */
	writel(0xc7, (void *)(pmp_cfg + 0x20));

	sync_is();
}

static int parse_memory(const void *blob, int t)
{
	int node;
	fdt32_t *reg;
	long address, size;

	node = fdt_path_offset(blob, "/memory");
	if (node < 0)
		return -ENOENT;

	reg = (fdt32_t *)fdt_getprop(blob, node, "reg", 0);
	address = fdt_translate_address(blob, node, reg);
	reg += 2;
	size = fdt_translate_address(blob, node, reg);
	if (t == T_DTS) {
		t_start_address = address;
		t_size = size;
		debug("t_start_address: 0x%lx\n", t_start_address);
	} else if (t == NT_DTS) {
		nt_start_address = address;
		nt_size = size;
		if (setup_nt_pmp_array(address, size))
			return -EINVAL;
		debug("nt_start_address: 0x%lx\n", nt_start_address);
	}

	return 0;
}

/*
int irq_no[100] = {56, 57, 58, 59, 16, 17, 18, 19, 20, 21, 22, 23, 36, 38, 39, 40, 41,
					44, 54, 52, 24, 25, 74, 68, 27, 150, 66, 62, 64};
*/
int i = 0;
int irq_no[255] = {0};
static int setup_t_plic(const void *blob)
{
	int x, y;
	int i;

	writel(0x40000000, (void *)(PLIC_BASE_ADDR + 0x1ffff8));

	for (i = 0; i < 255 && irq_no[i]; i++) {
		debug("T irq_no: %d\n", irq_no[i]);
		x = irq_no[i] % 32;
		y = irq_no[i] / 32;

		writel(1 << x, (void *)(PLIC_BASE_ADDR + 0x1fe000 + y * 4));
	}

	/* Enable & Lock AMP */
	writel(0xc0000000, (void *)(PLIC_BASE_ADDR + 0x1ffff8));

	return 0;
}

static int parse_soc(const void *blob, int t)
{
	int node, device, irq;
	fdt32_t *intc, *reg;
	const char *status, *name;
	long address, size;

	node = fdt_path_offset(blob, "/soc");
	if (node < 0)
		return -ENOENT;

	debug("%s device ================\n", t == T_DTS ? "T" : "NT");
	for (device = fdt_first_subnode(blob, node);
		device >= 0; device = fdt_next_subnode(blob, device)) {
		if (device == -FDT_ERR_NOTFOUND)
			return -ENOENT;

		name = fdt_get_name(blob, device, NULL);
		debug("name: %s\n", name);
		status = (char *)fdt_getprop(blob, device, "status", NULL);
		if (status)
			debug("\tstatus: %s\n", status);

		intc = (fdt32_t *)fdt_getprop(blob, device, "interrupts", 0);
		if (intc) {
			irq = fdt_read_number(intc, 1);
			debug("\tirq_no: %d\n", irq);
			if (t == T_DTS) {
				irq_no[i] = irq;
				i++;
			}
		}

		reg = (fdt32_t *)fdt_getprop(blob, device, "reg", 0);
		if (reg) {
			address = fdt_translate_address(blob, device, reg);
			debug("\taddress: 0x%lx\n", address);
			reg += 2;
			size = fdt_translate_address(blob, device, reg);
			debug("\tsize: 0x%lx\n", size);
			if ((t == NT_DTS) && setup_nt_pmp_array(address, size))
				return -EINVAL;

			if (!strncmp(name, "mbox", 4)) {
				reg += 2;
				address = fdt_translate_address(blob, device, reg);
				debug("\taddress: 0x%lx\n", address);
				reg += 2;
				size = fdt_translate_address(blob, device, reg);
				debug("\tsize: 0x%lx\n", size);
				if ((t == NT_DTS) && setup_nt_pmp_array(address, size))
					return -EINVAL;

				reg += 2;
				address = fdt_translate_address(blob, device, reg);
				debug("\taddress: 0x%lx\n", address);
				reg += 2;
				size = fdt_translate_address(blob, device, reg);
				debug("\tsize: 0x%lx\n", size);
				if ((t == NT_DTS) && setup_nt_pmp_array(address, size))
					return -EINVAL;

				reg += 2;
				address = fdt_translate_address(blob, device, reg);
				debug("\taddress: 0x%lx\n", address);
				reg += 2;
				size = fdt_translate_address(blob, device, reg);
				debug("\tsize: 0x%lx\n", size);
				if ((t == NT_DTS) && setup_nt_pmp_array(address, size))
					return -EINVAL;
			}
		}
	}

	return 0;
}

static int parse_and_set_iopmp(const void *blob, int t)
{
	int node, device;
	fdt32_t *reg, *range;
	long base_addr, start, end, size;

	node = fdt_path_offset(blob, "/iopmp");
	if (node < 0)
		return -ENOENT;

	debug("%s iopmp ================\n", t == T_DTS ? "T" : "NT");
	for (device = fdt_first_subnode(blob, node);
		device >= 0; device = fdt_next_subnode(blob, device)) {
		if (device == -FDT_ERR_NOTFOUND)
			return -ENOENT;

		debug("name: %s\n", fdt_get_name(blob, device, NULL));
		reg = (fdt32_t *)fdt_getprop(blob, device, "reg", 0);
		range = (fdt32_t *)fdt_getprop(blob, device, "range", 0);
		if (reg && range) {
			base_addr = fdt_translate_address(blob, device, reg);
			debug("\tbase_addr: 0x%lx\n", base_addr);
			start = fdt_translate_address(blob, device, range);
			debug("\tstart: 0x%lx\n", start);
			range += 2;
			size = fdt_translate_address(blob, device, range);
			debug("\tsize: 0x%lx\n", size);
			end = start + size;
			if ((t == NT_DTS)) {
				writel(start >> 12, (void *)(base_addr + 0x280));
				writel(end >> 12, (void *)(base_addr + 0x284));
				writel(0x3, (void *)(base_addr + 0x80));
			}
		}
	}

	return 0;
}

static long boot_addr[4];
static long boot_addr_chk[4];

static void boot_kernel(long hart, long fly_addr, long krl_addr, long dtb_addr)
{
	void (*kernel)(ulong hart, void *dtb, struct fw_dynamic_info *p);

	kernel = (void *)fly_addr;

	opensbi_info.magic = FW_DYNAMIC_INFO_MAGIC_VALUE;
	opensbi_info.version = 0x1;
	opensbi_info.next_addr = krl_addr;
	opensbi_info.next_mode = FW_DYNAMIC_INFO_NEXT_MODE_S;
	opensbi_info.options = 0;
	opensbi_info.boot_hart = 0;

	kernel(hart, (void *)dtb_addr, &opensbi_info);
}

static void boot_t_core(void)
{
	long hart = csr_read(CSR_MHARTID);

	if (hart == 0)
		goto boot;

	csr_write(CSR_SMPEN, 0x1);
	csr_write(CSR_MCOR, 0x70013);
	csr_write(CSR_MCCR2, 0xe0010009);
	csr_write(CSR_MHCR, 0x11ff);
	csr_write(CSR_MXSTATUS, 0x638000);
	csr_write(CSR_MHINT, 0x16e30c);

boot:
	/* Set this for locking it */
	//csr_write(CSR_MTEE, 0xff);

	boot_kernel(hart, t_start_address, t_kernel_address, t_dtb_address);
}

static void boot_nt_core(void)
{
	void (*fly)(long, long);

	fly = (void *)nt_start_address;

	setup_nt_pmp_configs();

	csr_write(CSR_SMPEN, 0x1);
	csr_write(CSR_MCOR, 0x70013);
	csr_write(CSR_MCCR2, 0xe0010009);
	csr_write(CSR_MHCR, 0x11ff);
	csr_write(CSR_MXSTATUS, 0x638000);
	csr_write(CSR_MHINT, 0x16e30c);

	//csr_write(CSR_MTEE, 0x00); /* Do it in opensbi */

	fly(0xdeadbeef, nt_dtb_address);
}

static int parse_cpu(const void *blob, int t)
{
	int node, cpu, core;
	fdt32_t *reg;
	const char *status;

	node = fdt_path_offset(blob, "/cpus");
	if (node < 0)
		return -ENOENT;

	for (cpu = fdt_first_subnode(blob, node);
		cpu >= 0; cpu = fdt_next_subnode(blob, cpu)) {

		reg = (fdt32_t *)fdt_getprop(blob, cpu, "reg", 0);
		core = fdt_read_number(reg, 1);
		debug("core %d  ", core);

		status = fdt_getprop(blob, cpu, "status", NULL);
		if (t == T_DTS) {
			if (!strcmp(status, "okay")) {
				debug("T world\n");
				boot_addr[core] = (long)&boot_t_core;
			} else if (!strcmp(status, "disabled")) {
				debug("NT world\n");
				boot_addr[core] = (long)&boot_nt_core;
			} else {
				debug("Incorrect DTS! Not okay nor disabled\n");
				return -EINVAL;
			}
		} else if (t == NT_DTS) {
			if (!strcmp(status, "okay")) {
				debug("NT world\n");
				boot_addr_chk[core] = (long)&boot_nt_core;
			} else if (!strcmp(status, "disabled")) {
				debug("T world\n");
				boot_addr_chk[core] = (long)&boot_t_core;
			} else {
				debug("Incorrect DTS! Not okay nor disabled\n");
				return -EINVAL;
			}
		}
	}

	return 0;
}

int check_cpu(void)
{
	if ((boot_addr[0] != boot_addr_chk[0]) ||
		(boot_addr[1] != boot_addr_chk[1]) ||
		(boot_addr[2] != boot_addr_chk[2]) ||
		(boot_addr[3] != boot_addr_chk[3]))
		return -1;
	return 0;
}

static int parse_dtb(const void *blob_t, const void *blob_nt)
{
	int ret = 0;

	parse_memory(blob_t, T_DTS);
	parse_memory(blob_nt, NT_DTS);

	parse_soc(blob_t, T_DTS);
	parse_soc(blob_nt, NT_DTS);

	parse_and_set_iopmp(blob_nt, NT_DTS);

	ret = parse_cpu(blob_t, T_DTS);
	parse_cpu(blob_nt, NT_DTS);
	check_cpu();

	if (ret)
		return -EINVAL;

	return 0;
}

static int __maybe_unused boot_buddies(void)
{
	debug("cpu 0 ---0x%lx\n", boot_addr[0]);
	debug("cpu 1 ---0x%lx\n", boot_addr[1]);
	debug("cpu 2 ---0x%lx\n", boot_addr[2]);
	debug("cpu 3 ---0x%lx\n", boot_addr[3]);

	writel(boot_addr[1] & 0xffffffff, (void *)(0xffff018000 + 0x58));
	writel(boot_addr[1] >> 32,        (void *)(0xffff018000 + 0x5c));
	writel(0b00111, (void *)(0xffff014000 + 0x04));
	udelay(50000);

	writel(boot_addr[2] & 0xffffffff, (void *)(0xffff018000 + 0x60));
	writel(boot_addr[2] >> 32,        (void *)(0xffff018000 + 0x64));
	writel(0b01111, (void *)(0xffff014000 + 0x04));
	udelay(50000);

	writel(boot_addr[3] & 0xffffffff, (void *)(0xffff018000 + 0x68));
	writel(boot_addr[3] >> 32,        (void *)(0xffff018000 + 0x6c));
	writel(0b11111, (void *)(0xffff014000 + 0x04));

	return 0;
}

static int parse_img_verify(ulong *addr, char * const argv[])
{
	char *env;
	long sbi_addr = SBI_ENTRY_ADDR;
	long aon_addr = AON_DDR_ADDR;
	int header_off = 0;
	int ret;

	ret = csi_sec_init();
	if (ret)
		return ret;

	addr[1] = simple_strtoul(argv[1], NULL, 16);
	t_kernel_address = addr[1];
	if (image_have_head(addr[1]) == 1)
		header_off = HEADER_SIZE;
	ret = csi_sec_image_verify(T_KRLIMG, addr[1]);
	if (ret)
		return ret;
	addr[1] += header_off;
	t_kernel_address = addr[1];
	sprintf(argv[1], "0x%lx", addr[1]);
	debug("linux: 0x%lx\n", addr[1]);

	addr[2] = simple_strtoul(argv[2], NULL, 16);
#if LIGHT_ROOTFS_SEC_CHECK
	header_off = 0;
	if (image_have_head(addr[2]) == 1)
		header_off = HEADER_SIZE;
	ret = csi_sec_image_verify(T_ROOTFS, addr[2]);
	if (ret)
		return ret;
	addr[2] += header_off;
	sprintf(argv[2], "0x%lx", addr[2]);
#else
	sprintf(argv[2], "-");
#endif
	debug("rootfs: 0x%lx\n", addr[2]);

	addr[3] = simple_strtoul(argv[3], NULL, 16);
	header_off = 0;
	t_dtb_address = addr[3];
	if (image_have_head(addr[3]) == 1)
		header_off = HEADER_SIZE;
	ret = csi_sec_image_verify(T_DTB, addr[3]);
	if (ret)
		return ret;
	addr[3] += header_off;
	t_dtb_address = addr[3];
	sprintf(argv[3], "0x%lx", addr[3]);
	debug("t dtb: 0x%lx\n", addr[3]);

	addr[4] = simple_strtoul(argv[4], NULL, 16);
	nt_dtb_address = addr[4];
	debug("nt dtb: 0x%lx\n", addr[4]);

	env = env_get("t_opensbi_addr");
	if (env)
		sbi_addr = simple_strtol(env, NULL, 0);
	ret = csi_sec_image_verify(T_SBI, sbi_addr);
	if (ret)
		return ret;

	ret = csi_sec_image_verify(T_AON, aon_addr);
	if (ret)
		return ret;

	return 0;
}

#define BOOTCODE_SIZE    4
char tee_bootcode[BOOTCODE_SIZE] = {0x73, 0x50, 0x40, 0x7f};
int light_boot(int argc, char * const argv[])
{
	ulong addr[5] = {0};

	if (argc < 2) {
		debug("args not match!\n");
		return CMD_RET_USAGE;
	}

	if (parse_img_verify(addr, argv) < 0) {
		debug("parse args failed!\n");
		return CMD_RET_USAGE;
	}

	if (parse_dtb((const void *)addr[3], (const void *)addr[4]) < 0) {
		debug("parse dtb failed!\n");
		return CMD_RET_USAGE;
	}

	setup_t_plic((const void *)addr[3]);

	if (nt_start_address)
		memcpy((void *)nt_start_address, tee_bootcode, BOOTCODE_SIZE);

	run_command("bootslave", 0);

	return 0;
}

//#define TF_TEE_KEY_IN_RPMB_CASE
/* the sample rpmb key is only used for testing */
const unsigned char emmc_rpmb_key_sample[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,\
												0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};



int csi_get_tf_image_version(unsigned int *ver)
{
#ifdef TF_TEE_KEY_IN_RPMB_CASE
	char runcmd[64] = {0};
	unsigned long blkdata_addr = 0x80000000;

	/* tee version reside in RPMB block#0, offset#16*/
	sprintf(runcmd, "mmc rpmb read 0x%x 0 1", blkdata_addr);
	run_command(runcmd, 0);
	*ver = *(unsigned short *)(blkdata_addr+16);
#else 
	*ver = env_get_hex("tf_version", 0);
#endif
	return 0;
}

int csi_set_tf_image_version(unsigned int ver)
{
	env_set_hex("tf_version", ver);
	return 0;
}

int csi_get_tf_image_new_version(unsigned int *ver)
{
	*ver = env_get_hex("tf_new_version", 0);
	return 0;
}

int csi_get_tee_image_version(unsigned int *ver)
{
#ifdef TF_TEE_KEY_IN_RPMB_CASE
	char runcmd[64] = {0};
	unsigned long blkdata_addr = 0x80000000;

	/* tee version reside in RPMB block#0, offset#0*/
	sprintf(runcmd, "mmc rpmb read 0x%x 0 1", blkdata_addr);
	run_command(runcmd, 0);
	*ver = *(unsigned short *)blkdata_addr;
#else 
	*ver = env_get_hex("tee_version", 0);
#endif
	return 0;
}

int csi_set_tee_image_version(unsigned int ver)
{
	env_set_hex("tee_version", ver);
	return 0;
}

int csi_get_tee_image_new_version(unsigned int *ver)
{
	*ver = env_get_hex("tee_new_version", 0);
	return 0;
}

int light_vimage(int argc, char *const argv[])
{
	int ret = 0;
	int vimage_addr = 0;
	int code_offset = 0;
	int new_img_version = 0;
	int cur_img_version = 0;
	char imgname[32] = {0};

	if (argc < 3) 
		return CMD_RET_USAGE;
	
	vimage_addr = simple_strtoul(argv[1], NULL, 16);
	strcpy(imgname, argv[2]);

#if 0	
	if (image_have_head(vimage_addr) == 1)
		code_offset = HEADER_SIZE;

	new_img_version = get_image_version(vimage_addr);
	if (new_img_version == 0) {
		printf("get new img version fail\n");
		return -1;
	}
	printf("new image version: %4x\n", new_img_version);
#endif
	/* Check image version for ROLLBACK resisance */ 
	if (strcmp(imgname, "tf") == 0) {
		ret = csi_get_tf_image_new_version(&new_img_version);
		if (ret != 0) {
			printf("Get tf img new ersion fail\n");
			return -1;
		}
		ret = csi_get_tf_image_version(&cur_img_version);
		if (ret != 0) {
			printf("Get tf img version fail\n");
			return -1;
		}
	} else if (strcmp(imgname, "tee") == 0){
		ret = csi_get_tee_image_new_version(&new_img_version);
		if (ret != 0) {
			printf("Get tee img new ersion fail\n");
			return -1;
		}

		ret = csi_get_tee_image_version(&cur_img_version);
		if (ret != 0) {
			printf("Get tee img version fail\n");
			return -1;
		}
	} else {
		printf("unsupport image file\n");
		return -1;
	}
	

	/* Get secure version X from image version X.Y */
	printf("cur image version: %d.%d\n", cur_img_version >> 8, cur_img_version & 0xff);
	printf("new image version: %d.%d\n", new_img_version >> 8, new_img_version & 0xff);

	/* According the version rule, the X value must increase by 1 */
	if (((new_img_version >> 8) - (cur_img_version >> 8)) == 0) {
		/* This is unsecure function */
		printf("This is unsecure function upgrade, going on uprade anyway\n");
	}
	if (((new_img_version >> 8) - (cur_img_version >> 8)) != 1) {
		printf("upgrade version is not defined against the rule\n");
		return -1;
	}
	printf("check image verison rule pass\n");
	if (strcmp(imgname, "tf") == 0) {
		/* update tf image version */
		ret = csi_set_tf_image_version(new_img_version);
		if (ret != 0) {
			printf("Get tf img version fail\n");
			return -1;
		}
	} else if (strcmp(imgname, "tee") == 0){
		/* update tee image version */
		ret = csi_set_tee_image_version(new_img_version);
		if (ret != 0) {
			printf("Get tee img version fail\n");
			return -1;
		}
	} else {
		printf("unsupport image file\n");
		return -1;
	}
#if 0
	ret = csi_sec_init();
	if (ret != 0)
		return ret;

	ret = csi_sec_image_verify(T_TF, vimage_addr);
	if (ret != 0)
		return ret;
#endif
	return 0;
}
