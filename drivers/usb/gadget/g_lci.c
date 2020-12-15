/* ============================================================================
 * g_lci.c
 * ----------------------------------------------------------------------------
 * Modified version of the kernel's included mass_storage.c example. 
 * ============================================================================
 */

#include <linux/usb/ch9.h>
#include <linux/usb/g_hid.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/moduleparam.h>
#include <linux/platform_device.h>
#include <linux/stat.h>
#include <linux/utsname.h>

/*-------------------------------------------------------------------------*/

#define DRIVER_DESC     "National Instruments LCI gadget"
#define DRIVER_VERSION	"0.1"

/* USB strings */
static char *manufacturer = "National Instruments";
static char *product = "NI VB-8012";

module_param(manufacturer, charp, S_IRUGO);
module_param(product, charp, S_IRUGO);

/* Manufacturer string for mass storage must be shorter. */
static char *fsg_manuf = "NI";

module_param(fsg_manuf, charp, S_IRUGO);

/* Whether or not to enable RNDIS */
static bool rndis = 0;
module_param(rndis, bool, 0);

static inline int lci_use_rndis(void) {
	return rndis;
}

/*-------------------------------------------------------------------------*/

/*
 * kbuild is not very cooperative with respect to linking separately
 * compiled library objects into one module.  So for now we won't use
 * separate compilation ... ensuring init/exit sections work to shrink
 * the runtime footprint, and giving us at least some parts of what
 * a "gcc --combine ... part1.c part2.c part3.c ... " build would.
 */

#include "composite.c"
#include "config.c"
#include "epautoconf.c"
#include "f_hidBulk.c"
#include "f_mass_storage.c"
#include "usbstring.c"

#define USB_ETH_RNDIS
#include "f_rndis.c"
#include "rndis.c"
#include "u_ether.c"

static void lci_cleanup(void);
static int lci_unbind(struct usb_composite_dev*);

/* USB Data ******************************************************************/

static struct usb_device_descriptor lci_device_descriptor = {
	.bLength =		sizeof lci_device_descriptor,
	.bDescriptorType =	USB_DT_DEVICE,
	.bcdUSB =		cpu_to_le16(0x0200),
	.bDeviceClass =		0x00,
	.bDeviceSubClass =	0x00,
	.bDeviceProtocol =	0x00,

	/* Vendor and product id can be overridden by module parameters.  */
	.idVendor =		cpu_to_le16(0x0000),
	.idProduct =		cpu_to_le16(0x0000),
	.bNumConfigurations =	1,
};

static struct usb_configuration lci_configuration = {
	.label			= 0,
	.bConfigurationValue	= 1,
	.bmAttributes		= USB_CONFIG_ATT_SELFPOWER,
};

#define STRING_MANUFACTURER_IDX		0
#define STRING_PRODUCT_IDX		1

static struct usb_string strings_dev[] = {
	[STRING_MANUFACTURER_IDX].s = 0,
	[STRING_PRODUCT_IDX].s = 0,
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

static struct usb_composite_driver lci_composite_driver = {
	.name		= DRIVER_DESC,
	.dev		= &lci_device_descriptor,
	.strings	= dev_strings,
	.max_speed	= USB_SPEED_HIGH,
	.unbind		= __exit_p(lci_unbind),
};

static unsigned long lci_registered;


/* Mass Storage Gadget Data **************************************************/

static struct fsg_module_parameters mod_data = {
	.stall = 1 //Allow stalling on our Xilinx UDC
};
FSG_MODULE_PARAMETERS(/* no prefix */, mod_data);

static inline int lci_use_mass_storage(void) {
	/* This keys off of g_lci.cdrom=1 */
	return (mod_data.cdrom_count > 0) && (!!mod_data.cdrom[0]);
}

/* HID Gadget Data ***********************************************************/

/* hid descriptor for a keyboard */
static struct hidg_func_descriptor my_hid_data = {
	.subclass		= 0, /* No subclass */
	.protocol		= 0, /* No protocol */
	.report_length		= 512,
	.report_desc_length	= 35,
	.report_desc		= {
		0x06, 0x00, 0xFF,    //Usage Page (0xFF00 - vendor defined)
		0x09, 0x01,          //Usage ID (usage 1)
		0x15, 0x00,          //Logical minimum (0)
		0x26, 0xFF, 0x00,    //Logical maximum (255)
		0x75, 0x08,          //Report Size (8-bits/1-byte)
		0xA1, 0x01,          //Collection (Application)
		0x85, 0xD5,          //Report ID (D5)
		0x09, 0x01,          //Usage (1)
		0x96, 0xFF, 0x01,    //Report Count (511 a.k.a. 0x1FF)
		0x82, 0x02, 0x01,    //Input: data, variable, absolute, buffered
		0x85, 0xD5,          //Report ID (D5)
		0x09, 0x01,          //Usage (1)
		0x96, 0xFF, 0x01,    //Report Count (511 a.k.a. 0x1FF)
		0x92, 0x02, 0x01,    //Output: data, variable, absolute, buffered
		0xC0                 //End collection
	}
};

void platform_device_release(struct device *dev)
{
   //No work necessary, but the platform driver code spews debug warnings to
   //dmesg if this method is not provided.
}

static struct platform_device lci_hid_plat_device[] = {
	{  
		.name			= "lci_hid0",
		.id			= 0,
		.num_resources		= 0,
		.resource		= 0,
		.dev.platform_data	= &my_hid_data,
		.dev.release		= platform_device_release, 
	},
	{
		.name			= "lci_hid1",
		.id			= 0,
		.num_resources		= 0,
		.resource		= 0,
		.dev.platform_data	= &my_hid_data,
		.dev.release		= platform_device_release, 
	},
	{
		.name			= "lci_hid2",
		.id			= 0,
		.num_resources		= 0,
		.resource		= 0,
		.dev.platform_data	= &my_hid_data,
		.dev.release		= platform_device_release, 
	},
	{
		.name			= "lci_hid3",
		.id			= 0,
		.num_resources		= 0,
		.resource		= 0,
		.dev.platform_data	= &my_hid_data,
		.dev.release		= platform_device_release, 
	},
};

static int __devexit platform_driver_remove(struct platform_device *pdev);

static struct platform_driver lci_hid_plat_driver[] = {
   {
      .remove =         __devexit_p(platform_driver_remove),
      .driver.owner =   THIS_MODULE,
      .driver.name =    "lci_hid0",
   },
   {
      .remove =         __devexit_p(platform_driver_remove),
      .driver.owner =   THIS_MODULE,
      .driver.name =    "lci_hid1",
   },
   {
      .remove =         __devexit_p(platform_driver_remove),
      .driver.owner =   THIS_MODULE,
      .driver.name =    "lci_hid2",
   },
   {
      .remove =         __devexit_p(platform_driver_remove),
      .driver.owner =   THIS_MODULE,
      .driver.name =    "lci_hid3",
   },
};

static unsigned long lci_hid_device_registered = 0;
static unsigned long lci_hid_driver_probed = 0;

struct hidg_func_node {
	struct list_head node;
	struct hidg_func_descriptor *func;
};

static LIST_HEAD(hidg_func_list);

/* Mass Storage Gadget Helpers ***********************************************/

static int lci_pre_eject(struct fsg_common *common,
                         struct fsg_lun *lun, int num)
{
	//Per the mass storage file, returning positive means "don't eject"
	return 1;
}

const struct fsg_operations lci_fsg_operations = {
	.pre_eject = lci_pre_eject,
};

static int msg_add_to_config(struct usb_configuration *c)
{
	//Setup data structures for the file storage gadget, add it to the config
	//with fsg_bind_config()

	static struct fsg_common common;

	struct fsg_common *retp;
	struct fsg_config config;
	int ret;
	size_t product_len;
	size_t vendor_len;

	fsg_config_from_params(&config, &mod_data);
	config.ops = &lci_fsg_operations;
	/* vendor must be 8 chars, product 16 or less */
	config.vendor_name = fsg_manuf;

	/*
	 * If the 'product' string starts with the 'manufacturer' string
	 * followed by a space, skip over that part of it.
	 * (that is, if the product string is "NI VB-8012" and our
	 * manufacturer string is "NI", we just want "VB-8012").
	 */
	product_len = strlen(product);
	vendor_len = strlen(config.vendor_name);
	if ((product_len > vendor_len+1) &&
	    (!strncmp(product, config.vendor_name, vendor_len)) &&
	    (product[vendor_len] == ' ')) {
		/* Skip over vendor name and space. */
		config.product_name = product + (vendor_len + 1);
	} else {
		/* Use the whole product name. */
		config.product_name = product;
	}

	retp = fsg_common_init(&common, c->cdev, &config);
	if (IS_ERR(retp))	return PTR_ERR(retp);

	ret = fsg_bind_config(c->cdev, c, &common);
	fsg_common_put(&common);
	return ret;
}


/* HID Gadget Helpers ********************************************************/

static void hid_unregister(void);
static int __init hid_plat_driver_probe(struct platform_device *pdev);

static int __devexit platform_driver_remove(struct platform_device *pdev)
{
	return 0;
}

static int __init hid_plat_driver_probe(struct platform_device *pdev)
{
	struct hidg_func_descriptor *func = pdev->dev.platform_data;
	struct hidg_func_node *entry;

	if (!func) {
		dev_err(&pdev->dev, "Platform data missing\n");
		return -ENODEV;
	}

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;

	entry->func = func;
	list_add_tail(&entry->node, &hidg_func_list);

	return 0;
}

static int hid_register(void)
{
	//The HID gadget is a platform driver. To use the gadget, a data structure 
	//must be registered and probed into the platform driver for each interface.

	int status, i;

	for(i = 0; i < ARRAY_SIZE(lci_hid_plat_device); ++i)
	{
		status = platform_device_register(&lci_hid_plat_device[i]);
		if(status < 0) return status;
		set_bit(i, &lci_hid_device_registered);

		status = platform_driver_probe(&lci_hid_plat_driver[i], hid_plat_driver_probe);
		if(status < 0) return status;
		set_bit(i, &lci_hid_driver_probed);
	}   

	return status;
}

static void hid_unregister(void)
{
	int i;
	struct hidg_func_node *e, *n;

	for(i = 0; i < ARRAY_SIZE(lci_hid_plat_device); ++i)
	{
		if(test_and_clear_bit(i, &lci_hid_driver_probed))
		{
			platform_driver_unregister(&lci_hid_plat_driver[i]);
		}

		if(test_and_clear_bit(i, &lci_hid_device_registered))
		{
			platform_device_unregister(&lci_hid_plat_device[i]);
		}
	}

	list_for_each_entry_safe(e, n, &hidg_func_list, node) {
		list_del(&e->node);
		kfree(e);
	}
}

/*****************************************************************************/

static u8 macaddr[ETH_ALEN];

static int __init lci_bind_config(struct usb_configuration *c)
{
	struct hidg_func_node *e;
	int func = 0, status = 0;

	if (lci_use_rndis()) {
		status = rndis_bind_config(c, macaddr);
		if(status) return status;
	}

	list_for_each_entry(e, &hidg_func_list, node) {
		status = hidg_bind_config(c, e->func, func++);
		if (status) break;
	}

	if (lci_use_mass_storage()) {
		status = msg_add_to_config(c);
		if(status) return status;
	}

	return status;
}

static int __init lci_bind(struct usb_composite_dev *cdev)
{
	struct usb_gadget *gadget = cdev->gadget;
	struct list_head *tmp;
	int status, num_hid_interfaces = 0;

	//Sanity check HID gadget count
	list_for_each(tmp, &hidg_func_list)	num_hid_interfaces++;

	if (num_hid_interfaces != ARRAY_SIZE(lci_hid_plat_device)) {
		return -ENODEV;
	}

	if (lci_use_rndis()) {
		//Setup ether
		status = gether_setup(gadget, macaddr);
		if (status < 0) return status;
	}

	//Setup HID
	status = ghid_setup(gadget, num_hid_interfaces);
	if (status < 0) return status;

	//Setup HID bulk mode
	status = ghid_bulk_setup(gadget);
	if (status < 0) return status;

	//Write manufacturer string
	status = usb_string_id(cdev);
	if (status < 0) return status;
	strings_dev[STRING_MANUFACTURER_IDX].s = manufacturer;
	strings_dev[STRING_MANUFACTURER_IDX].id = status;
	lci_device_descriptor.iManufacturer = status;

	//Write product string
	status = usb_string_id(cdev);
	if (status < 0) return status;
	strings_dev[STRING_PRODUCT_IDX].s = product;
	strings_dev[STRING_PRODUCT_IDX].id = status;
	lci_device_descriptor.iProduct = status;

	//Add configuration to device
	lci_configuration.label = product;
	status = usb_add_config(cdev, &lci_configuration, lci_bind_config);
	if (status < 0) return status;

	dev_info(&gadget->dev, DRIVER_DESC ", version: " DRIVER_VERSION "\n");

	set_bit(0, &lci_registered);
	return 0;
}

static int __exit lci_unbind(struct usb_composite_dev *cdev)
{
	ghid_cleanup();
	ghid_bulk_cleanup();
	return 0;
}

static void lci_cleanup(void)
{
	if (test_and_clear_bit(0, &lci_registered))
	{
		usb_composite_unregister(&lci_composite_driver);
	}
}

MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_AUTHOR("William Earle");
MODULE_LICENSE("GPL");

static int __init lci_init(void)
{
	hid_register();
	return usb_composite_probe(&lci_composite_driver, lci_bind);
}
module_init(lci_init);

static void lci_exit(void)
{
	lci_cleanup();
	hid_unregister();
}
module_exit(lci_exit);
