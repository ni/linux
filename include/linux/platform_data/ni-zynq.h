/*
 * National Instruments Zynq-based Platform
 *
 * Copyright 2013 National Instruments
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

#ifndef _LINUX_NI_ZYNQ_H
#define _LINUX_NI_ZYNQ_H

/**
 * struct zynq_board_reset
 * @reset: A board-specific reset function
 *
 * This structure is used by the NI Zynq machine description and by any drivers
 * that provide the ability to reset NI Zynq-based boards.
 */
struct ni_zynq_board_reset {
	void (*reset)(struct ni_zynq_board_reset *);
};

/* The NI Zynq machine description uses this struct to determine how to reset
 * the board. Drivers providing the ability to reset NI Zynq-based boards
 * should register a reset handler in it. */
extern struct ni_zynq_board_reset *ni_zynq_board_reset;

#endif
