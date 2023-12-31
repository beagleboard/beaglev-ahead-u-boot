// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) 2011 The Chromium OS Authors.
 */

#include <common.h>
#include <cpu_func.h>
#include <cros_ec.h>
#include <dm.h>
#include <init.h>
#include <led.h>
#include <os.h>
#include <asm/test.h>
#include <asm/u-boot-sandbox.h>
#include <malloc.h>

#include <extension_board.h>

/*
 * Pointer to initial global data area
 *
 * Here we initialize it.
 */
gd_t *gd;

/* Add a simple GPIO device */
U_BOOT_DEVICE(gpio_sandbox) = {
	.name = "gpio_sandbox",
};

void flush_cache(unsigned long start, unsigned long size)
{
}

#ifndef CONFIG_TIMER
/* system timer offset in ms */
static unsigned long sandbox_timer_offset;

void timer_test_add_offset(unsigned long offset)
{
	sandbox_timer_offset += offset;
}

unsigned long timer_read_counter(void)
{
	return os_get_nsec() / 1000 + sandbox_timer_offset * 1000;
}
#endif

int dram_init(void)
{
	gd->ram_size = CONFIG_SYS_SDRAM_SIZE;
	return 0;
}

int board_init(void)
{
	if (IS_ENABLED(CONFIG_LED))
		led_default_state();

	return 0;
}

#ifdef CONFIG_CMD_EXTENSION
int extension_board_scan(struct list_head *extension_list)
{
	struct extension *extension;
	int i;

	for (i = 0; i < 2; i++) {
		extension = calloc(1, sizeof(struct extension));
		snprintf(extension->overlay, sizeof(extension->overlay), "overlay%d.dtbo", i);
		snprintf(extension->name, sizeof(extension->name), "extension board %d", i);
		snprintf(extension->owner, sizeof(extension->owner), "sandbox");
		snprintf(extension->version, sizeof(extension->version), "1.1");
		snprintf(extension->other, sizeof(extension->other), "Fictionnal extension board");
		list_add_tail(&extension->list, extension_list);
	}

	return i;
}
#endif

#ifdef CONFIG_BOARD_LATE_INIT
int board_late_init(void)
{
	struct udevice *dev;
	int ret;

	ret = uclass_first_device_err(UCLASS_CROS_EC, &dev);
	if (ret && ret != -ENODEV) {
		/* Force console on */
		gd->flags &= ~GD_FLG_SILENT;

		printf("cros-ec communications failure %d\n", ret);
		puts("\nPlease reset with Power+Refresh\n\n");
		panic("Cannot init cros-ec device");
		return -1;
	}
	return 0;
}
#endif
