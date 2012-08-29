/* arch/arm/mach-zynq/include/mach/debug-macro.S
 *
 * Debugging macro include header
 *
 *  Copyright (C) 2011 Xilinx
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

#include <mach/zynq_soc.h>
#include <mach/uart.h>

#ifdef CONFIG_SERIAL_8250_CONSOLE

		.macro	addruart, rp, rv, tmp
		ldr	\rp, =0x80000000	@ physical
		ldr	\rv, =0xE0700000	@ virtual
		.endm

#define UART_SHIFT 0
#include <asm/hardware/debug-8250.S>

#else
		.macro	addruart, rp, rv, tmp
		ldr	\rp, =LL_UART_PADDR	@ physical
		ldr	\rv, =LL_UART_VADDR	@ virtual
		.endm

		.macro	senduart,rd,rx
		str	\rd, [\rx, #UART_FIFO_OFFSET]	@ TXDATA
		.endm

		.macro	waituart,rd,rx
		.endm

		.macro	busyuart,rd,rx
1002:		ldr	\rd, [\rx, #UART_SR_OFFSET]	@ get status register
		tst	\rd, #UART_SR_TXFULL		@
		bne	1002b			@ wait if FIFO is full
		.endm
#endif
