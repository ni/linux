/*
 * Copyright (C) 2017 National Instruments Corp.
 *
 * Based on cfag12864b driver (v0.1.0)
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

#ifndef _OEL9M1027_H_
#define _OEL9M1027_H_

#define OEL9M1027_WIDTH			(64)
#define OEL9M1027_HEIGHT		(128)

#define OEL9M1027_PAGES			(16)
#define OEL9M1027_ADDRESSES		(64)
#define OEL9M1027_ADDRESSES_OFFSET	(32)
#define OEL9M1027_SIZE			((OEL9M1027_PAGES) * \
					(OEL9M1027_ADDRESSES))

#define OEL9M1027_DISPLAYON	1
#define OEL9M1027_DISPLAYOFF	0
#define OEL9M1027_DISPNORMAL	0
#define OEL9M1027_DISPINVERT	1

/*
 * Default parameter that recommended by Truly manufacturer.
 *
 * Don't change if you unsure. This value is sufficient by
 * default.
 */
#define OEL9M1027_DEF_MULTIRATIO	0x3F
#define OEL9M1027_DEF_DISPFREQ		0xF1
#define OEL9M1027_DEF_SCANDIR		0x00
#define OEL9M1027_DEF_DISPOFFSET	0x60
#define OEL9M1027_DEF_STARTLINE		0x20
#define OEL9M1027_DEF_ADDRMODE		0x00
#define OEL9M1027_DEF_CONTRAST		0xFF
#define OEL9M1027_DEF_SEGREMAP		0x00
#define OEL9M1027_DEF_DCCONTROL		0x0A
#define OEL9M1027_DEF_PHASEPERIOD	0x22
#define OEL9M1027_DEF_VCOMCONTROL	0x35

/*
 * OEL9M1027 device private data
 */
struct oel9m1027 {
	struct device *dev;	/* oel9m1027 device */
	struct sh1107 *sh;	/* sh1107 privdata */

	struct fb_info *fb;	/* oel9m1027 framebuffer info */
	unsigned char *framebuffer;

	unsigned char contrast; /* oled display contrast */

	struct delayed_work dwork; /* oled update worker */
	struct mutex lock;
};

/*
 * Register OEL9M1027 framebuffer device
 *
 * Returns 0 if registered successfully,
 * or != 0 if failed to register.
 */
int oel9m1027fb_init(struct oel9m1027 *oled);

/*
 * Remove OEL9M1027 framebuffer device
 */
void oel9m1027fb_exit(struct oel9m1027 *oled);

#endif /* _OEL9M1027_H_ */

