/*
 *  NI 16550 Transceiver Driver
 *
 *  The National Instruments (NI) 16550 has built-in RS-485 transceiver control
 *  circuitry. This driver provides the transceiver control functionality
 *  for the RS-485 ports and uses the 8250 driver for the UART functionality.
 *
 *  Copyright 2012 National Instruments Corporation
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  This program is distributed in the hope that it will be useful, but WITHOUT
 *  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 *  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 *  more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc., 51
 *  Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#ifndef _NI16550_H
#define _NI16550_H

int ni16550_register_port(struct uart_8250_port *uart);
void ni16550_unregister_port(int line);

#endif
