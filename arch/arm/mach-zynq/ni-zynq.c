/*
 * This file is based on common.c.
 *
 * Behaviors specific to National Instruments Zynq-based targets
 *
 * Copyright (C) 2011 Xilinx
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/init.h>
#include <linux/export.h>
#include <linux/platform_data/ni-zynq.h>
#include <asm/mach/arch.h>
#include <asm/mach-types.h>
#include "common.h"

struct ni_zynq_board_reset *ni_zynq_board_reset;
EXPORT_SYMBOL_GPL(ni_zynq_board_reset);

static void ni_cpld_system_reset(char mode, const char *cmd)
{
	if (ni_zynq_board_reset)
		ni_zynq_board_reset->reset(ni_zynq_board_reset);
}

static const char * const ni_zynq_dt_match[] = {
	"ni,zynq",
	NULL
};

/* TODO: Get an NI mach-types entry. See arch/arm/tools/mach-types for details.
 * */
MACHINE_START(XILINX_EP107, "NI Zynq-based Target")
	.smp		= smp_ops(zynq_smp_ops),
	.map_io		= zynq_map_io,
	.init_machine	= zynq_init_machine,
	.init_time	= zynq_timer_init,
	.dt_compat	= ni_zynq_dt_match,
	.reserve	= zynq_memory_init,
	.restart	= ni_cpld_system_reset,
MACHINE_END
