// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018, Bin Meng <bmeng.cn@gmail.com>
 */

#include <common.h>
#include <command.h>

static __attribute__((naked))void sys_wdt_reset(void)
{
    asm volatile (
        "li      a0, 0xFFE0000000 \n\r"
        "li      a1, 0xFFE0180000 \n\r"
        "bgeu    a0, a1, 2f \n\r"
    "1: \n\r"
        "sw      zero, (a0) \n\r"
        "addi    a0, a0, 4 \n\r"
        "bltu    a0, a1, 1b \n\r"
    "2: \n\r"
        "li      a0, 0xFFEFC30000 \n\r"
        "li      a1, 1          \n\r"
        "sw      a1, 0(a0)      \n\r"
        "sw      a1, 4(a0)      \n\r"
        "j      2b              \n\r"
    /* doulbe reset to avoid FI attack */
    "3: \n\r"
        "li      a0, 0xFFEFC30000 \n\r"
        "li      a1, 1          \n\r"
        "sw      a1, 0(a0)      \n\r"
        "sw      a1, 4(a0)      \n\r"
        "j      3b              \n\r"
        "ret                    \n\r"
    );
}
int do_reset(cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[])
{
	printf("resetting ...\n");

	sys_wdt_reset();
	hang();

	return 0;
}
