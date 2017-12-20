/*
 * Copyright (C) 2017 National Instruments Corp.
 *
 * Based on ks0108 driver (v0.1.0)
 *     Copyright (C) 2006 by Miguel Ojeda Sandonis
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

#ifndef _SH1107_H_
#define _SH1107_H_

/* sh1107 device private data */
struct sh1107;

/* Write a byte to the data port */
int sh1107_writedata(struct sh1107 *sh, unsigned char byte);

/* Write a byte to the control port */
int sh1107_writecontrol(struct sh1107 *sh, unsigned char byte);

/* Set internal DC-DC control register (0..15) */
int sh1107_dccontrol(struct sh1107 *sh, unsigned char value);

/* Set controller VCOM level (0..255) */
int sh1107_vcomcontrol(struct sh1107 *sh, unsigned char vcom);

/* Set controller pre/dis-charge period (0..255) */
int sh1107_phaseperiod(struct sh1107 *sh, unsigned char period);

/* Set the forcibly turn entire display state (0..1) */
int sh1107_entiredisplaystate(struct sh1107 *sh, unsigned char state);

/* Set the controller current display state (0..1) */
int sh1107_displaystate(struct sh1107 *sh, unsigned char state);

/* Set invert display (0..1) */
int sh1107_displayinvert(struct sh1107 *sh, unsigned char invert);

/* Set display contrast level (0..255) */
int sh1107_displaycontrast(struct sh1107 *sh, unsigned char contrast);

/* Set the controller common output scan direction (0..1) */
int sh1107_scandir(struct sh1107 *sh, unsigned char direction);

/* Set the controller internal display clocks. (0..255) */
int sh1107_displayfreq(struct sh1107 *sh, unsigned char frequency);

/* Set the controller multiplex mode to any multiplex ratio (0..127) */
int sh1107_multiplexratio(struct sh1107 *sh, unsigned char ratio);

/* Set the controller memory addressing mode (0..1) */
int sh1107_addressingmode(struct sh1107 *sh, unsigned char memmode);

/* Set the controller segment remap (0..1) */
int sh1107_segremap(struct sh1107 *sh, unsigned char uprotation);

/* Set the controller current startline (0..127) */
int sh1107_startline(struct sh1107 *sh, unsigned char startline);

/* Set the controller current address (0..127) */
int sh1107_address(struct sh1107 *sh, unsigned char address);

/* Set the controller current page (0..15) */
int sh1107_page(struct sh1107 *sh, unsigned char page);

/* Set the controller offset (0..127) */
int sh1107_offset(struct sh1107 *sh, unsigned char offset);

#endif /* _SH1107_H_ */
