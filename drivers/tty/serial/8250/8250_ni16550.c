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
 */

#include "8250.h"
#include <linux/init.h>
#include <linux/uaccess.h>
#include <linux/device.h>

#define NI16550_PCR_OFFSET 0x0F
#define NI16550_PCR_RS422 0x00
#define NI16550_PCR_ECHO_RS485 0x01
#define NI16550_PCR_DTR_RS485 0x02
#define NI16550_PCR_AUTO_RS485 0x03
#define NI16550_PCR_WIRE_MODE_MASK 0x03
#define NI16550_PCR_TXVR_ENABLE_BIT (1 << 3)
#define NI16550_PCR_RS485_TERMINATION_BIT (1 << 6)

static int ni16550_enable_transceivers(struct uart_port *port)
{
	uint8_t pcr;

	dev_dbg(port->dev, ">ni16550_enable_transceivers\n");

	pcr = port->serial_in(port, NI16550_PCR_OFFSET);
	pcr |= NI16550_PCR_TXVR_ENABLE_BIT;
	dev_dbg(port->dev, "write pcr: 0x%08x\n", pcr);
	port->serial_out(port, NI16550_PCR_OFFSET, pcr);

	dev_dbg(port->dev, "<ni16550_enable_transceivers\n");

	return 0;
}

static int ni16550_disable_transceivers(struct uart_port *port)
{
	uint8_t pcr;

	dev_dbg(port->dev, ">ni16550_disable_transceivers\n");

	pcr = port->serial_in(port, NI16550_PCR_OFFSET);
	pcr &= ~NI16550_PCR_TXVR_ENABLE_BIT;
	dev_dbg(port->dev, "write pcr: 0x%08x\n", pcr);
	port->serial_out(port, NI16550_PCR_OFFSET, pcr);

	dev_dbg(port->dev, "<ni16550_disable_transceivers\n");

	return 0;
}

static int ni16550_config_rs485(struct uart_port *port,
		struct serial_rs485 *rs485)
{
	uint8_t pcr;

	dev_dbg(port->dev, ">ni16550_config_rs485\n");

	/* "rs485" should be given to us non-NULL. */
	BUG_ON(rs485 == NULL);

	pcr = port->serial_in(port, NI16550_PCR_OFFSET);
	pcr &= ~NI16550_PCR_WIRE_MODE_MASK;

	if (rs485->flags & SER_RS485_ENABLED) {
		/* RS-485 */
		if ((rs485->flags & SER_RS485_RX_DURING_TX) &&
		    (rs485->flags & SER_RS485_RTS_ON_SEND)) {
			dev_dbg(port->dev, "Invalid 2-wire mode\n");
			return -EINVAL;
		}

		if (rs485->flags & SER_RS485_RX_DURING_TX) {
			/* Echo */
			dev_vdbg(port->dev, "2-wire DTR with echo\n");
			pcr |= NI16550_PCR_ECHO_RS485;
		} else {
			/* Auto or DTR */
			if (rs485->flags & SER_RS485_RTS_ON_SEND) {
				/* Auto */
				dev_vdbg(port->dev, "2-wire Auto\n");
				pcr |= NI16550_PCR_AUTO_RS485;
			} else {
				/* DTR-controlled */
				/* No Echo */
				dev_vdbg(port->dev, "2-wire DTR no echo\n");
				pcr |= NI16550_PCR_DTR_RS485;
			}
		}
	} else {
		/* RS-422 */
		dev_vdbg(port->dev, "4-wire\n");
		pcr |= NI16550_PCR_RS422;
	}

	dev_dbg(port->dev, "write pcr: 0x%08x\n", pcr);
	port->serial_out(port, NI16550_PCR_OFFSET, pcr);

	/* Update the cache. */
	port->rs485 = *rs485;

	dev_dbg(port->dev, "<ni16550_config_rs485\n");
	return 0;
}

static struct txvr_ops ni16550_txvr_ops = {
	.enable_transceivers = ni16550_enable_transceivers,
	.disable_transceivers = ni16550_disable_transceivers,
};

void ni16550_port_setup(struct uart_port *port)
{
	port->txvr_ops = &ni16550_txvr_ops;
	port->rs485_config = &ni16550_config_rs485;
	/* The hardware comes up by default in 2-wire auto mode and we
	 * set the flags to represent that
	 */
	port->rs485.flags = SER_RS485_ENABLED | SER_RS485_RTS_ON_SEND;
}
