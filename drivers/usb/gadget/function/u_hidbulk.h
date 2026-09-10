/*
 * u_hidbulk.h
 *
 * Utility definitions for the hid-bulk function (NI LCI g_lci gadget).
 * Based on u_hid.h, with distinct exported names (ghid_bulk_*, f_hidbulk_opts)
 * so f_hidbulk can coexist with the stock f_hid in the same kernel.
 *
 * Copyright (c) 2014 Samsung Electronics Co., Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef U_HIDBULK_H
#define U_HIDBULK_H

#include <linux/usb/composite.h>

struct f_hidbulk_opts {
	struct usb_function_instance	func_inst;
	int				minor;
	unsigned char			subclass;
	unsigned char			protocol;
	unsigned short			report_length;
	unsigned short			report_desc_length;
	unsigned char			*report_desc;
	bool				report_desc_alloc;

	/*
	 * index in the g_lci gadget: instance 0 owns the shared pool of bulk
	 * endpoints (alt setting 1 is vendor-specific with 8 bulk eps); the
	 * other instances present an empty alt setting 1.
	 */
	int				index;

/* Protect the data from concurrent access by read/write
	  * and create symlink/remove symlink. */
	 struct mutex			lock;
	 int				refcnt;
};

#endif /* U_HIDBULK_H */
