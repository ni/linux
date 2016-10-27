/*
 * Copyright (C) 2012 National Instruments Corp.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef _FPGA_PERIPHERAL_H_
#define _FPGA_PERIPHERAL_H_

#include <linux/notifier.h>

/*
 * The value passed to notifier callbacks will be one of these.
 * FPGA_PERIPHERAL_DOWN: The FPGA is about to be unprogrammed.
 * FPGA_PERIPHERAL_UP: The FPGA is now programmed.
 *  This may be called even if not preceded by an FPGA_PERIPHERAL_DOWN.
 * FPGA_PERIPHERAL_FAILED: The FPGA programming failed.
 *  If this is called, clients should not use the FPGA as it might be in
 *  a bad state. This is to notify the clients to abort any waiting for
 *  the FPGA to come back up.
 *
 * Client callbacks should always return notifier_from_errno(0) so the
 * call chain continues to all clients.
 */
#define FPGA_PERIPHERAL_DOWN	0
#define FPGA_PERIPHERAL_UP	1
#define FPGA_PERIPHERAL_FAILED	2

extern struct blocking_notifier_head fpgaperipheral_notifier_list;

#endif /* _FPGA_PERIPHERAL_H_ */
