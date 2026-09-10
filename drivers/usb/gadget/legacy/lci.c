/*
 * lci.c -- National Instruments LCI (VirtualBench) composite gadget
 *
 * Composition: 4x hid_bulk interfaces (HID class, alt0 interrupt, the first
 * switches alt1 to 8 bulk endpoints) plus an optional mass-storage CD-ROM
 * (g_lci.cdrom=1).
 *
 * Module parameters, set by the U-Boot bootscript:
 *   g_lci.idVendor / idProduct / iSerialNumber  (composite core)
 *   g_lci.manufacturer / product                (this driver)
 *   g_lci.cdrom / file / ...                     (mass-storage)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/usb/ch9.h>
#include <linux/usb/composite.h>

#include "u_hidbulk.h"
#include "f_mass_storage.h"

#define DRIVER_DESC	"National Instruments LCI gadget"
#define DRIVER_VERSION	"0.2"

/* Number of hid_bulk interfaces the VirtualBench presents. */
#define LCI_NUM_HID	4

/* USB strings, overridable by module parameters. */
static char *manufacturer = "National Instruments";
static char *product = "NI VB-8012";
module_param(manufacturer, charp, S_IRUGO);
module_param(product, charp, S_IRUGO);

/* Manufacturer string for the mass-storage inquiry must be short. */
static char *fsg_manuf = "NI";
module_param(fsg_manuf, charp, S_IRUGO);

/*-------------------------------------------------------------------------*/
USB_GADGET_COMPOSITE_OPTIONS();

static struct usb_device_descriptor lci_device_desc = {
	.bLength =		sizeof lci_device_desc,
	.bDescriptorType =	USB_DT_DEVICE,
	.bcdUSB =		cpu_to_le16(0x0200),
	.bDeviceClass =		USB_CLASS_PER_INTERFACE,
	.bDeviceSubClass =	0,
	.bDeviceProtocol =	0,
	/* idVendor / idProduct overridden by module parameters. */
	.idVendor =		cpu_to_le16(0x0000),
	.idProduct =		cpu_to_le16(0x0000),
	.bNumConfigurations =	1,
};

static const struct usb_descriptor_header *otg_desc[2];

static struct usb_string strings_dev[] = {
	[USB_GADGET_MANUFACTURER_IDX].s = "",
	[USB_GADGET_PRODUCT_IDX].s = "",
	[USB_GADGET_SERIAL_IDX].s = "",
	{  } /* end of list */
};

static struct usb_gadget_strings stringtab_dev = {
	.language	= 0x0409,	/* en-us */
	.strings	= strings_dev,
};

static struct usb_gadget_strings *dev_strings[] = {
	&stringtab_dev,
	NULL,
};

/*-------------------------------------------------------------------------*/
/* Vendor-defined HID report: report ID 0xD5, 512-byte IN and OUT reports. */

static unsigned char lci_hid_report_desc[] = {
	0x06, 0x00, 0xFF,	/* Usage Page (0xFF00, vendor defined) */
	0x09, 0x01,		/* Usage (1) */
	0x15, 0x00,		/* Logical minimum (0) */
	0x26, 0xFF, 0x00,	/* Logical maximum (255) */
	0x75, 0x08,		/* Report Size (8 bits) */
	0xA1, 0x01,		/* Collection (Application) */
	0x85, 0xD5,		/*   Report ID (0xD5) */
	0x09, 0x01,		/*   Usage (1) */
	0x96, 0xFF, 0x01,	/*   Report Count (511) */
	0x82, 0x02, 0x01,	/*   Input (data, var, abs, buffered) */
	0x85, 0xD5,		/*   Report ID (0xD5) */
	0x09, 0x01,		/*   Usage (1) */
	0x96, 0xFF, 0x01,	/*   Report Count (511) */
	0x92, 0x02, 0x01,	/*   Output (data, var, abs, buffered) */
	0xC0			/* End Collection */
};

static struct usb_function_instance *fi_hid[LCI_NUM_HID];
static struct usb_function *f_hid[LCI_NUM_HID];

/*-------------------------------------------------------------------------*/
/* Optional mass-storage CD-ROM, keyed off g_lci.cdrom=1. */

static struct fsg_module_parameters mod_data = {
	.stall = 1
};
FSG_MODULE_PARAMETERS(/* no prefix */, mod_data);

#define fsg_num_buffers	CONFIG_USB_GADGET_STORAGE_NUM_BUFFERS

static struct usb_function_instance *fi_msg;
static struct usb_function *f_msg;

static inline int lci_use_mass_storage(void)
{
	return (mod_data.cdrom_count > 0) && (!!mod_data.cdrom[0]);
}

/*-------------------------------------------------------------------------*/

static struct usb_configuration lci_config_driver = {
	.label			= DRIVER_DESC,
	.bConfigurationValue	= 1,
	.bmAttributes		= USB_CONFIG_ATT_SELFPOWER,
};

static int lci_do_config(struct usb_configuration *c)
{
	int i, status;

	if (gadget_is_otg(c->cdev->gadget)) {
		c->descriptors = otg_desc;
		c->bmAttributes |= USB_CONFIG_ATT_WAKEUP;
	}

	for (i = 0; i < LCI_NUM_HID; i++) {
		f_hid[i] = usb_get_function(fi_hid[i]);
		if (IS_ERR(f_hid[i])) {
			status = PTR_ERR(f_hid[i]);
			goto put_hid;
		}
		status = usb_add_function(c, f_hid[i]);
		if (status) {
			usb_put_function(f_hid[i]);
			goto put_hid;
		}
	}

	if (lci_use_mass_storage()) {
		f_msg = usb_get_function(fi_msg);
		if (IS_ERR(f_msg)) {
			status = PTR_ERR(f_msg);
			goto put_hid;
		}
		status = usb_add_function(c, f_msg);
		if (status) {
			usb_put_function(f_msg);
			goto put_hid;
		}
	}

	return 0;

put_hid:
	while (--i >= 0) {
		usb_remove_function(c, f_hid[i]);
		usb_put_function(f_hid[i]);
	}
	return status;
}

static int lci_setup_hid(void)
{
	struct f_hidbulk_opts *opts;
	int i, status;

	for (i = 0; i < LCI_NUM_HID; i++) {
		fi_hid[i] = usb_get_function_instance("hid_bulk");
		if (IS_ERR(fi_hid[i])) {
			status = PTR_ERR(fi_hid[i]);
			goto put;
		}
		opts = container_of(fi_hid[i], struct f_hidbulk_opts, func_inst);
		opts->index = i;
		opts->subclass = 0;
		opts->protocol = 0;
		opts->report_length = 512;
		opts->report_desc_length = sizeof(lci_hid_report_desc);
		opts->report_desc = lci_hid_report_desc;
	}
	return 0;

put:
	while (--i >= 0)
		usb_put_function_instance(fi_hid[i]);
	return status;
}

static int lci_setup_mass_storage(struct usb_composite_dev *cdev)
{
	struct fsg_opts *opts;
	struct fsg_config config;
	int status;

	fi_msg = usb_get_function_instance("mass_storage");
	if (IS_ERR(fi_msg))
		return PTR_ERR(fi_msg);

	fsg_config_from_params(&config, &mod_data, fsg_num_buffers);
	config.vendor_name = fsg_manuf;
	/* Strip a leading "NI " so the inquiry product string fits 16 chars. */
	if (strlen(product) > strlen(fsg_manuf) + 1 &&
	    !strncmp(product, fsg_manuf, strlen(fsg_manuf)) &&
	    product[strlen(fsg_manuf)] == ' ')
		config.product_name = product + strlen(fsg_manuf) + 1;
	else
		config.product_name = product;

	opts = fsg_opts_from_func_inst(fi_msg);
	opts->no_configfs = true;
	status = fsg_common_set_num_buffers(opts->common, fsg_num_buffers);
	if (status)
		goto fail;

	status = fsg_common_set_cdev(opts->common, cdev, config.can_stall);
	if (status)
		goto fail_set_cdev;

	fsg_common_set_sysfs(opts->common, true);
	status = fsg_common_create_luns(opts->common, &config);
	if (status)
		goto fail_set_cdev;

	fsg_common_set_inquiry_string(opts->common, config.vendor_name,
				      config.product_name);
	return 0;

fail_set_cdev:
	fsg_common_free_buffers(opts->common);
fail:
	usb_put_function_instance(fi_msg);
	fi_msg = NULL;
	return status;
}

static int lci_bind(struct usb_composite_dev *cdev)
{
	struct usb_gadget *gadget = cdev->gadget;
	int status;

	status = lci_setup_hid();
	if (status)
		return status;

	if (lci_use_mass_storage()) {
		status = lci_setup_mass_storage(cdev);
		if (status)
			goto put_hid;
	}

	strings_dev[USB_GADGET_MANUFACTURER_IDX].s = manufacturer;
	strings_dev[USB_GADGET_PRODUCT_IDX].s = product;

	status = usb_string_ids_tab(cdev, strings_dev);
	if (status < 0)
		goto put_msg;
	lci_device_desc.iManufacturer =
		strings_dev[USB_GADGET_MANUFACTURER_IDX].id;
	lci_device_desc.iProduct = strings_dev[USB_GADGET_PRODUCT_IDX].id;

	status = usb_add_config(cdev, &lci_config_driver, lci_do_config);
	if (status < 0)
		goto put_msg;

	usb_composite_overwrite_options(cdev, &coverwrite);
	dev_info(&gadget->dev, DRIVER_DESC ", version: " DRIVER_VERSION "\n");
	return 0;

put_msg:
	if (lci_use_mass_storage() && !IS_ERR_OR_NULL(fi_msg))
		usb_put_function_instance(fi_msg);
put_hid:
	{
		int i;

		for (i = 0; i < LCI_NUM_HID; i++)
			usb_put_function_instance(fi_hid[i]);
	}
	return status;
}

static int lci_unbind(struct usb_composite_dev *cdev)
{
	int i;

	for (i = 0; i < LCI_NUM_HID; i++) {
		if (!IS_ERR_OR_NULL(f_hid[i]))
			usb_put_function(f_hid[i]);
		if (!IS_ERR_OR_NULL(fi_hid[i]))
			usb_put_function_instance(fi_hid[i]);
	}

	if (lci_use_mass_storage()) {
		if (!IS_ERR_OR_NULL(f_msg))
			usb_put_function(f_msg);
		if (!IS_ERR_OR_NULL(fi_msg))
			usb_put_function_instance(fi_msg);
	}

	return 0;
}

static struct usb_composite_driver lci_driver = {
	.name		= "g_lci",
	.dev		= &lci_device_desc,
	.strings	= dev_strings,
	.max_speed	= USB_SPEED_HIGH,
	.bind		= lci_bind,
	.unbind		= lci_unbind,
};

module_usb_composite_driver(lci_driver);

MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_AUTHOR("William Earle");
MODULE_LICENSE("GPL");
