/*
 * Copyright (C) 2013 National Instruments Corp.
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

#include <linux/acpi.h>
#include <linux/module.h>
#include <linux/leds.h>
#include <linux/input.h>
#include <linux/delay.h>
#include <acpi/acpi.h>

#define MODULE_NAME "nirtfeatures"

/* Register addresses */

#define NIRTF_YEAR		0x01
#define NIRTF_MONTH		0x02
#define NIRTF_DAY		0x03
#define NIRTF_HOUR		0x04
#define NIRTF_MINUTE		0x05
#define NIRTF_SCRATCH		0x06
#define NIRTF_PLATFORM_MISC	0x07
#define NIRTF_PROC_RESET_SOURCE	0x11
#define NIRTF_CONTROLLER_MODE	0x12
#define NIRTF_SYSTEM_LEDS	0x20
#define NIRTF_STATUS_LED_SHIFT1	0x21
#define NIRTF_STATUS_LED_SHIFT0	0x22
#define NIRTF_RT_LEDS		0x23

#define NIRTF_IO_SIZE	0x40

/* Register values */

#define NIRTF_PLATFORM_MISC_ID_MASK		0x07
#define NIRTF_PLATFORM_MISC_ID_MANHATTAN	0
#define NIRTF_PLATFORM_MISC_ID_HAMMERHEAD	4
#define NIRTF_PLATFORM_MISC_ID_WINGHEAD		5

#define NIRTF_CONTROLLER_MODE_NO_FPGA_SW	0x40
#define NIRTF_CONTROLLER_MODE_HARD_BOOT_N	0x20
#define NIRTF_CONTROLLER_MODE_NO_FPGA		0x10
#define NIRTF_CONTROLLER_MODE_RECOVERY		0x08
#define NIRTF_CONTROLLER_MODE_CONSOLE_OUT	0x04
#define NIRTF_CONTROLLER_MODE_IP_RESET		0x02
#define NIRTF_CONTROLLER_MODE_SAFE		0x01

#define NIRTF_SYSTEM_LEDS_STATUS_RED	0x08
#define NIRTF_SYSTEM_LEDS_STATUS_YELLOW	0x04
#define NIRTF_SYSTEM_LEDS_POWER_GREEN	0x02
#define NIRTF_SYSTEM_LEDS_POWER_YELLOW	0x01

/*=====================================================================
 * ACPI NI physical interface element support
 *===================================================================*/
#define MAX_NAMELEN	64
#define MAX_NODELEN	128
#define MIN_PIE_CAPS_VERSION	2
#define MAX_PIE_CAPS_VERSION	2

enum nirtfeatures_pie_class {
	PIE_CLASS_INPUT  = 0,
	PIE_CLASS_OUTPUT = 1
};

enum nirtfeatures_pie_type {
	PIE_TYPE_UNKNOWN = 0,
	PIE_TYPE_SWITCH  = 1,
	PIE_TYPE_LED     = 2
};

struct nirtfeatures_pie_descriptor {
	char                        name[MAX_NAMELEN];
	enum nirtfeatures_pie_class pie_class;
	enum nirtfeatures_pie_type  pie_type;
	bool                        is_user_visible;
	unsigned int                notification_value;
};

struct nirtfeatures_pie_descriptor_led_color {
	char name[MAX_NAMELEN];
	int  brightness_range_low;
	int  brightness_range_high;
};

struct nirtfeatures_pie_descriptor_switch {
	unsigned int num_states;
	unsigned int state_value[1];
};

struct nirtfeatures_pie_location {
	unsigned int element;
	unsigned int subelement;
};


/* Structures */

struct nirtfeatures {
	struct acpi_device *acpi_device;
	u16 io_base;
	u16 io_size;
	spinlock_t lock;
	u8 revision[5];
	const char *bpstring;
};

struct nirtfeatures_led {
	struct led_classdev cdev;
	struct nirtfeatures *nirtfeatures;
	struct nirtfeatures_pie_location pie_location;
	char name_string[MAX_NODELEN];
	u8 address;
	u8 mask;
	u8 pattern_hi_addr;
	u8 pattern_lo_addr;
	struct list_head node;
};
LIST_HEAD(nirtfeatures_led_pie_list);

struct nirtfeatures_switch {
	struct input_dev *cdev;
	struct nirtfeatures *nirtfeatures;
	struct nirtfeatures_pie_descriptor pie_descriptor;
	struct nirtfeatures_pie_location pie_location;
	char name_string[MAX_NODELEN];
	char phys_location_string[MAX_NODELEN];
	struct list_head node;
};
LIST_HEAD(nirtfeatures_switch_pie_list);

/* sysfs files */

static ssize_t nirtfeatures_revision_get(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct acpi_device *acpi_device = to_acpi_device(dev);
	struct nirtfeatures *nirtfeatures = acpi_device->driver_data;

	return sprintf(buf, "20%02X/%02X/%02X %02X:%02X\n",
		       nirtfeatures->revision[0], nirtfeatures->revision[1],
		       nirtfeatures->revision[2], nirtfeatures->revision[3],
		       nirtfeatures->revision[4]);
}

static DEVICE_ATTR(revision, S_IRUGO, nirtfeatures_revision_get, NULL);

static ssize_t nirtfeatures_scratch_get(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct acpi_device *acpi_device = to_acpi_device(dev);
	struct nirtfeatures *nirtfeatures = acpi_device->driver_data;
	u8 data;

	data = inb(nirtfeatures->io_base + NIRTF_SCRATCH);

	return sprintf(buf, "%02x\n", data);
}

static ssize_t nirtfeatures_scratch_set(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct acpi_device *acpi_device = to_acpi_device(dev);
	struct nirtfeatures *nirtfeatures = acpi_device->driver_data;
	unsigned long tmp;
	u8 data;

	if (kstrtoul(buf, 0, &tmp) || (tmp > 0xFF))
		return -EINVAL;

	data = (u8)tmp;

	outb(data, nirtfeatures->io_base + NIRTF_SCRATCH);

	return count;
}

static DEVICE_ATTR(scratch, S_IRUGO|S_IWUSR, nirtfeatures_scratch_get,
	nirtfeatures_scratch_set);

static ssize_t nirtfeatures_backplane_id_get(struct device *dev,
					     struct device_attribute *attr,
					     char *buf)
{
	struct acpi_device *acpi_device = to_acpi_device(dev);
	struct nirtfeatures *nirtfeatures = acpi_device->driver_data;

	return sprintf(buf, "%s\n", nirtfeatures->bpstring);
}

static DEVICE_ATTR(backplane_id, S_IRUGO, nirtfeatures_backplane_id_get, NULL);

static const char * const nirtfeatures_reset_source_strings[] = {
	"button", "processor", "fpga", "watchdog", "software",
};

static ssize_t nirtfeatures_reset_source_get(struct device *dev,
					     struct device_attribute *attr,
					     char *buf)
{
	struct acpi_device *acpi_device = to_acpi_device(dev);
	struct nirtfeatures *nirtfeatures = acpi_device->driver_data;
	u8 data;
	int i;

	data = inb(nirtfeatures->io_base + NIRTF_PROC_RESET_SOURCE);

	for (i = 0; i < ARRAY_SIZE(nirtfeatures_reset_source_strings); i++)
		if ((1 << i) & data)
			return sprintf(buf, "%s\n",
				       nirtfeatures_reset_source_strings[i]);

	return sprintf(buf, "poweron\n");
}

static DEVICE_ATTR(reset_source, S_IRUGO, nirtfeatures_reset_source_get, NULL);

static ssize_t nirtfeatures_no_fpga_sw_get(struct device *dev,
					   struct device_attribute *attr,
					   char *buf)
{
	struct acpi_device *acpi_device = to_acpi_device(dev);
	struct nirtfeatures *nirtfeatures = acpi_device->driver_data;
	u8 data;

	data = inb(nirtfeatures->io_base + NIRTF_CONTROLLER_MODE);

	data &= NIRTF_CONTROLLER_MODE_NO_FPGA_SW;

	return sprintf(buf, "%u\n", !!data);
}

static ssize_t nirtfeatures_no_fpga_sw_set(struct device *dev,
					   struct device_attribute *attr,
					   const char *buf, size_t count)
{
	struct acpi_device *acpi_device = to_acpi_device(dev);
	struct nirtfeatures *nirtfeatures = acpi_device->driver_data;
	unsigned long tmp;
	u8 data;

	if (kstrtoul(buf, 0, &tmp) || (tmp > 1))
		return -EINVAL;

	spin_lock(&nirtfeatures->lock);

	data = inb(nirtfeatures->io_base + NIRTF_CONTROLLER_MODE);

	if (tmp)
		data |= NIRTF_CONTROLLER_MODE_NO_FPGA_SW;
	else
		data &= ~NIRTF_CONTROLLER_MODE_NO_FPGA_SW;

	outb(data, nirtfeatures->io_base + NIRTF_CONTROLLER_MODE);

	spin_unlock(&nirtfeatures->lock);

	return count;
}

static DEVICE_ATTR(no_fpga_sw, S_IRUGO|S_IWUSR, nirtfeatures_no_fpga_sw_get,
	nirtfeatures_no_fpga_sw_set);

static ssize_t nirtfeatures_soft_reset_get(struct device *dev,
					   struct device_attribute *attr,
					   char *buf)
{
	struct acpi_device *acpi_device = to_acpi_device(dev);
	struct nirtfeatures *nirtfeatures = acpi_device->driver_data;
	u8 data;

	data = inb(nirtfeatures->io_base + NIRTF_CONTROLLER_MODE);

	data &= NIRTF_CONTROLLER_MODE_HARD_BOOT_N;

	return sprintf(buf, "%u\n", !!data);
}

static DEVICE_ATTR(soft_reset, S_IRUGO, nirtfeatures_soft_reset_get, NULL);

static ssize_t nirtfeatures_no_fpga_get(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct acpi_device *acpi_device = to_acpi_device(dev);
	struct nirtfeatures *nirtfeatures = acpi_device->driver_data;
	u8 data;

	data = inb(nirtfeatures->io_base + NIRTF_CONTROLLER_MODE);

	data &= NIRTF_CONTROLLER_MODE_NO_FPGA;

	return sprintf(buf, "%u\n", !!data);
}

static DEVICE_ATTR(no_fpga, S_IRUGO, nirtfeatures_no_fpga_get, NULL);

static ssize_t nirtfeatures_recovery_mode_get(struct device *dev,
					      struct device_attribute *attr,
					      char *buf)
{
	struct acpi_device *acpi_device = to_acpi_device(dev);
	struct nirtfeatures *nirtfeatures = acpi_device->driver_data;
	u8 data;

	data = inb(nirtfeatures->io_base + NIRTF_CONTROLLER_MODE);

	data &= NIRTF_CONTROLLER_MODE_RECOVERY;

	return sprintf(buf, "%u\n", !!data);
}

static DEVICE_ATTR(recovery_mode, S_IRUGO,
	nirtfeatures_recovery_mode_get, NULL);

static ssize_t nirtfeatures_console_out_get(struct device *dev,
					    struct device_attribute *attr,
					    char *buf)
{
	struct acpi_device *acpi_device = to_acpi_device(dev);
	struct nirtfeatures *nirtfeatures = acpi_device->driver_data;
	u8 data;

	data = inb(nirtfeatures->io_base + NIRTF_CONTROLLER_MODE);

	data &= NIRTF_CONTROLLER_MODE_CONSOLE_OUT;

	return sprintf(buf, "%u\n", !!data);
}

static DEVICE_ATTR(console_out, S_IRUGO, nirtfeatures_console_out_get, NULL);

static ssize_t nirtfeatures_ip_reset_get(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct acpi_device *acpi_device = to_acpi_device(dev);
	struct nirtfeatures *nirtfeatures = acpi_device->driver_data;
	u8 data;

	data = inb(nirtfeatures->io_base + NIRTF_CONTROLLER_MODE);

	data &= NIRTF_CONTROLLER_MODE_IP_RESET;

	return sprintf(buf, "%u\n", !!data);
}

static DEVICE_ATTR(ip_reset, S_IRUGO, nirtfeatures_ip_reset_get, NULL);

static ssize_t nirtfeatures_safe_mode_get(struct device *dev,
					  struct device_attribute *attr,
					  char *buf)
{
	struct acpi_device *acpi_device = to_acpi_device(dev);
	struct nirtfeatures *nirtfeatures = acpi_device->driver_data;
	u8 data;

	data = inb(nirtfeatures->io_base + NIRTF_CONTROLLER_MODE);

	data &= NIRTF_CONTROLLER_MODE_SAFE;

	return sprintf(buf, "%u\n", !!data);
}

static DEVICE_ATTR(safe_mode, S_IRUGO, nirtfeatures_safe_mode_get, NULL);

static const struct attribute *nirtfeatures_attrs[] = {
	&dev_attr_revision.attr,
	&dev_attr_scratch.attr,
	&dev_attr_backplane_id.attr,
	&dev_attr_reset_source.attr,
	&dev_attr_no_fpga_sw.attr,
	&dev_attr_soft_reset.attr,
	&dev_attr_no_fpga.attr,
	&dev_attr_recovery_mode.attr,
	&dev_attr_console_out.attr,
	&dev_attr_ip_reset.attr,
	&dev_attr_safe_mode.attr,
	NULL
};

/* LEDs */

static void nirtfeatures_led_brightness_set(struct led_classdev *led_cdev,
					    enum led_brightness brightness)
{
	struct nirtfeatures_led *led = (struct nirtfeatures_led *)led_cdev;
	u8 data;
	bool on;
	u16 pattern;

	on = !!brightness;
	pattern = brightness;

	spin_lock(&led->nirtfeatures->lock);

	data = inb(led->nirtfeatures->io_base + led->address);

	data &= ~led->mask;

	if (on)
		data |= led->mask;

	outb(data, led->nirtfeatures->io_base + led->address);

	if (led->pattern_hi_addr && led->pattern_lo_addr) {
		/* Write the high byte first. */
		outb(pattern >> 8,
		     led->nirtfeatures->io_base + led->pattern_hi_addr);
		outb(pattern & 0xFF,
		     led->nirtfeatures->io_base + led->pattern_lo_addr);
	}

	spin_unlock(&led->nirtfeatures->lock);
}

static enum led_brightness
nirtfeatures_led_brightness_get(struct led_classdev *led_cdev)
{
	struct nirtfeatures_led *led = (struct nirtfeatures_led *)led_cdev;
	u8 data;

	data = inb(led->nirtfeatures->io_base + led->address);

	/* For the yellow status LED, the blink pattern used for brightness
	   on write is write-only, so we just return on/off for all LEDs. */
	return (data & led->mask) ? LED_FULL : LED_OFF;
}

static struct nirtfeatures_led nirtfeatures_leds_common[] = {
	{
		{
			.name = CONFIG_NI_LED_PREFIX ":status:red",
		},
		.address = NIRTF_SYSTEM_LEDS,
		.mask = NIRTF_SYSTEM_LEDS_STATUS_RED,
	},
	{
		{
			.name = CONFIG_NI_LED_PREFIX ":status:yellow",
			.max_brightness = 0xFFFF,
		},
		.address = NIRTF_SYSTEM_LEDS,
		.mask = NIRTF_SYSTEM_LEDS_STATUS_YELLOW,
		.pattern_hi_addr = NIRTF_STATUS_LED_SHIFT1,
		.pattern_lo_addr = NIRTF_STATUS_LED_SHIFT0,
	},
	{
		{
			.name = CONFIG_NI_LED_PREFIX ":power:green",
		},
		.address = NIRTF_SYSTEM_LEDS,
		.mask = NIRTF_SYSTEM_LEDS_POWER_GREEN,
	},
	{
		{
			.name = CONFIG_NI_LED_PREFIX ":power:yellow",
		},
		.address = NIRTF_SYSTEM_LEDS,
		.mask = NIRTF_SYSTEM_LEDS_POWER_YELLOW,
	},
};

/*=====================================================================
 * ACPI NI physical interface element support
 *===================================================================*/

/* Note that callers of this function are responsible for deallocating
 * the buffer allocated by acpi_evaluate_object() by calling
 * kfree() on the pointer passed back in result_buffer.
 */
static int nirtfeatures_call_acpi_method(struct nirtfeatures *nirtfeatures,
	const char         *method_name,
	int                 argc,
	union acpi_object  *argv,
	acpi_size          *result_size,
	void              **result_buffer)
{
	acpi_status acpi_ret;
	acpi_handle acpi_hdl;
	struct acpi_object_list acpi_params;
	struct acpi_buffer acpi_result = { ACPI_ALLOCATE_BUFFER, NULL };

	if (NULL == nirtfeatures || NULL == result_size ||
	   NULL == result_buffer)
		return -EINVAL;

	acpi_ret = acpi_get_handle(nirtfeatures->acpi_device->handle,
	   (acpi_string) method_name, &acpi_hdl);
	if (ACPI_FAILURE(acpi_ret)) {
		dev_err(&nirtfeatures->acpi_device->dev,
		   "nirtfeatures: ACPI get handle for %s failed (%d)\n",
		   method_name, acpi_ret);
		return -1;
	}

	acpi_params.count = argc;
	acpi_params.pointer = argv;

	acpi_ret = acpi_evaluate_object(acpi_hdl, NULL,
	   &acpi_params, &acpi_result);
	if (ACPI_FAILURE(acpi_ret)) {
		dev_err(&nirtfeatures->acpi_device->dev,
		   "nirtfeatures: ACPI evaluate for %s failed (%d)\n",
		   method_name, acpi_ret);
		return -1;
	}

	*result_size = acpi_result.length;
	*result_buffer = acpi_result.pointer;
	return 0;
}

/* This is the generic PIE set state wrapper. It invokes the PIES
 * ACPI method to modify the state of the given PIE.
 */
static int nirtfeatures_pie_set_state(struct nirtfeatures *nirtfeatures,
	unsigned int element, unsigned int subelement, int state)
{
	union acpi_object        pies_args[3];
	acpi_size                result_size;
	void                    *result_buffer;
	union acpi_object       *acpi_buffer;
	int                      err = 0;

	if (NULL == nirtfeatures)
		return -EINVAL;

	pies_args[0].type = ACPI_TYPE_INTEGER;
	pies_args[0].integer.value = element;
	pies_args[1].type = ACPI_TYPE_INTEGER;
	pies_args[1].integer.value = subelement;
	pies_args[2].type = ACPI_TYPE_INTEGER;
	pies_args[2].integer.value = state;

	/* evaluate PIES(element, subelement, value) ACPI method */
	err = nirtfeatures_call_acpi_method(nirtfeatures, "PIES",
	   3, &pies_args[0], &result_size, &result_buffer);

	if (err == 0) {
		acpi_buffer = (union acpi_object *) result_buffer;
		if (ACPI_TYPE_INTEGER == acpi_buffer->type)
			err = (int) acpi_buffer->integer.value;
		kfree(result_buffer);
	}

	return err;
}

/* This is the generic PIE get state wrapper. It invokes the PIEG
 * ACPI method to query the state of the given PIE.
 */
static int nirtfeatures_pie_get_state(struct nirtfeatures *nirtfeatures,
	unsigned int element, unsigned int subelement, int *result)
{
	union acpi_object        pies_args[2];
	acpi_size                result_size;
	void                    *result_buffer;
	union acpi_object       *acpi_buffer;
	int                      err = 0;

	if (NULL == nirtfeatures || NULL == result)
		return -EINVAL;

	pies_args[0].type = ACPI_TYPE_INTEGER;
	pies_args[0].integer.value = element;
	pies_args[1].type = ACPI_TYPE_INTEGER;
	pies_args[1].integer.value = subelement;

	/* evaluate PIEG(element, subelement) ACPI method */
	err = nirtfeatures_call_acpi_method(nirtfeatures, "PIEG",
	   2, &pies_args[0], &result_size, &result_buffer);

	if (err == 0) {
		acpi_buffer = (union acpi_object *) result_buffer;
		if (ACPI_TYPE_INTEGER == acpi_buffer->type)
			*result = (int) acpi_buffer->integer.value;
		kfree(result_buffer);
	}

	return err;
}

/* This function enables or disables notifications for a particular
 * input class PIE.
 */
static int nirtfeatures_pie_enable_notifications(
	struct nirtfeatures *nirtfeatures,
	unsigned int element, unsigned int subelement, int enable)
{
	union acpi_object        pies_args[3];
	acpi_size                result_size;
	void                    *result_buffer;
	union acpi_object       *acpi_buffer;
	int                      err = 0;

	if (NULL == nirtfeatures)
		return -EINVAL;

	pies_args[0].type = ACPI_TYPE_INTEGER;
	pies_args[0].integer.value = element;
	pies_args[1].type = ACPI_TYPE_INTEGER;
	pies_args[1].integer.value = subelement;
	pies_args[2].type = ACPI_TYPE_INTEGER;
	pies_args[2].integer.value = enable;

	/* evaluate PIEF(element, subelement, enable) ACPI method */
	err = nirtfeatures_call_acpi_method(nirtfeatures, "PIEF",
	   3, &pies_args[0], &result_size, &result_buffer);

	if (err == 0) {
		acpi_buffer = (union acpi_object *) result_buffer;
		if (ACPI_TYPE_INTEGER == acpi_buffer->type)
			err = (int) acpi_buffer->integer.value;
		kfree(result_buffer);
	}

	return err;
}

/* This is the set_brightness callback for a PIE-enumerated LED.
 */
static void nirtfeatures_led_pie_brightness_set(
	struct led_classdev *led_cdev, enum led_brightness brightness)
{
	struct nirtfeatures_led *led = (struct nirtfeatures_led *)led_cdev;

	spin_lock(&led->nirtfeatures->lock);

	/* Delegate the control of the PIE to the ACPI method. */
	if (nirtfeatures_pie_set_state(led->nirtfeatures,
	   led->pie_location.element, led->pie_location.subelement,
	   brightness)) {
		dev_err(&led->nirtfeatures->acpi_device->dev,
		   "nirtfeatures: set brightness failed for %s\n",
		   led->name_string);
	}

	spin_unlock(&led->nirtfeatures->lock);
}

/* This is the get_brightness callback for a PIE-enumerated LED.
 */
static enum led_brightness nirtfeatures_led_pie_brightness_get(
	struct led_classdev *led_cdev)
{
	struct nirtfeatures_led *led = (struct nirtfeatures_led *)led_cdev;
	int                      state = 0;

	spin_lock(&led->nirtfeatures->lock);

	if (nirtfeatures_pie_get_state(led->nirtfeatures,
	   led->pie_location.element, led->pie_location.subelement, &state)) {
		dev_err(&led->nirtfeatures->acpi_device->dev,
		   "nirtfeatures: get brightness failed for %s\n",
		   led->name_string);
	}

	spin_unlock(&led->nirtfeatures->lock);
	return state;
}

/* Parse a PIE LED color caps package and populate the
 * corresponding nirtfeatures_pie_descriptor_led_color structure.
 */
static int nirtfeatures_parse_led_pie_color(struct nirtfeatures *nirtfeatures,
	unsigned int                                  pie_caps_version,
	struct nirtfeatures_pie_descriptor_led_color *led_color_descriptor,
	union acpi_object                            *acpi_buffer)
{
	unsigned int i;

	if (NULL == nirtfeatures || NULL == led_color_descriptor ||
	   NULL == acpi_buffer)
		return -EINVAL;

	/* element 0 of a PIE LED color caps package is the name */
	if (ACPI_TYPE_BUFFER == acpi_buffer->package.elements[0].type) {
		for (i = 0;
		   i < acpi_buffer->package.elements[0].buffer.length; i++) {
			/* get pointer to Nth Unicode character in name */
			unsigned short *unicode_char = (unsigned short *)
			   (acpi_buffer->package.elements[0].buffer.pointer +
			   (2 * i));
			/* naive convert to ASCII */
			led_color_descriptor->name[i] =
			   (char) *unicode_char & 0xff;
		}
	} else
		return -EINVAL;

	/* element 1 is the brightness min value */
	if (ACPI_TYPE_INTEGER == acpi_buffer->package.elements[1].type)
		led_color_descriptor->brightness_range_low =
		   (int) acpi_buffer->package.elements[1].integer.value;
	else
		return -EINVAL;

	/* element 2 is the brightness max value */
	if (ACPI_TYPE_INTEGER == acpi_buffer->package.elements[2].type)
		led_color_descriptor->brightness_range_high =
		   (int) acpi_buffer->package.elements[2].integer.value;
	else
		return -EINVAL;

	return 0;
}

/* Parse a PIE LED caps package and create an LED class device
 * with the appropriate metadata.
 */
static int nirtfeatures_parse_led_pie(
	struct nirtfeatures                *nirtfeatures,
	unsigned int                        pie_caps_version,
	unsigned int                        pie_element,
	struct nirtfeatures_pie_descriptor *pie,
	union acpi_object                  *acpi_buffer)
{
	unsigned int                                 num_colors;
	unsigned int                                 i;
	struct nirtfeatures_pie_descriptor_led_color led_descriptor;
	struct nirtfeatures_led                     *led_dev;
	int                                          err;

	if (NULL == nirtfeatures || NULL == pie ||
	   NULL == acpi_buffer)
		return -EINVAL;

	if (ACPI_TYPE_PACKAGE != acpi_buffer->type)
		return -EINVAL;

	/* element 0 is the number of colors */
	if (ACPI_TYPE_INTEGER == acpi_buffer->package.elements[0].type) {
		num_colors = (unsigned int)
		   acpi_buffer->package.elements[0].integer.value;
	} else {
		return -EINVAL;
	}

	/* parse color caps and create LED class device */
	for (i = 0; i < num_colors; i++) {
		if (nirtfeatures_parse_led_pie_color(nirtfeatures,
		   pie_caps_version, &led_descriptor,
		   &(acpi_buffer->package.elements[i + 1])))
			return -EINVAL;

		/* create an LED class device for this LED */
		led_dev = kzalloc(sizeof(struct nirtfeatures_led), GFP_KERNEL);
		if (NULL == led_dev)
			return -ENOMEM;

		/* for compatibility with existing LVRT support, PIEs beginning
		 * with 'user' or 'wifi' should not affix the uservisible
		 * attribute to their name */
		if (strncasecmp(pie->name, "user", strlen("user")) != 0 &&
		    strncasecmp(pie->name, "wifi", strlen("wifi")) != 0) {
			snprintf(led_dev->name_string, MAX_NODELEN,
			   "%s:%s:%s:uservisible=%d",
			   CONFIG_NI_LED_PREFIX,
			   pie->name, led_descriptor.name,
			   pie->is_user_visible);
		} else {
			snprintf(led_dev->name_string, MAX_NODELEN,
			   "%s:%s:%s",
			   CONFIG_NI_LED_PREFIX,
			   pie->name, led_descriptor.name);
		}

		led_dev->cdev.name = led_dev->name_string;
		led_dev->cdev.brightness =
		   led_descriptor.brightness_range_low;
		led_dev->cdev.max_brightness =
		   led_descriptor.brightness_range_high;
		led_dev->cdev.brightness_set =
		   nirtfeatures_led_pie_brightness_set;
		led_dev->cdev.brightness_get =
		   nirtfeatures_led_pie_brightness_get;
		led_dev->nirtfeatures = nirtfeatures;
		led_dev->pie_location.element = pie_element;
		led_dev->pie_location.subelement = i;

		err = led_classdev_register(&nirtfeatures->acpi_device->dev,
		   &led_dev->cdev);
		if (0 != err) {
			kfree(led_dev);
			return err;
		}

		list_add_tail(&led_dev->node, &nirtfeatures_led_pie_list);
	}

	return 0;
}

/* Parse a PIE switch caps package and create an input class device
 * with the appropriate metadata.
 */
static int nirtfeatures_parse_switch_pie(struct nirtfeatures *nirtfeatures,
	unsigned int                        pie_caps_version,
	unsigned int                        pie_element,
	struct nirtfeatures_pie_descriptor *pie,
	union acpi_object                  *acpi_buffer)
{
	unsigned int                               num_states;
	unsigned int                               i;
	struct nirtfeatures_pie_descriptor_switch *switch_descriptor = NULL;
	struct nirtfeatures_switch                *switch_dev = NULL;
	int                                        err = 0;

	if (NULL == nirtfeatures || NULL == pie || NULL == acpi_buffer)
		return -EINVAL;

	if (ACPI_TYPE_PACKAGE != acpi_buffer->type)
		return -EINVAL;

	/* element 0 is the number of states */
	if (ACPI_TYPE_INTEGER == acpi_buffer->package.elements[0].type)
		num_states = (unsigned int)
		   acpi_buffer->package.elements[0].integer.value;
	else
		return -EINVAL;

	/* allocate storage for switch descriptor */
	switch_descriptor = kzalloc(
	   sizeof(struct nirtfeatures_pie_descriptor_switch) +
	   sizeof(int) * (num_states - 1), GFP_KERNEL);
	if (NULL == switch_descriptor)
		return -ENOMEM;

	switch_descriptor->num_states = num_states;

	/* parse individual states in elements 1..N-1 */
	for (i = 0; i < num_states; i++) {
		if (ACPI_TYPE_INTEGER !=
		   acpi_buffer->package.elements[i + 1].type) {
			err = -EINVAL;
			goto exit;
		}

		switch_descriptor->state_value[i] =
		   (int) acpi_buffer->package.elements[i + 1].integer.value;
	}

	/* create an input class device for this switch */
	switch_dev = kzalloc(sizeof(struct nirtfeatures_switch), GFP_KERNEL);
	if (NULL == switch_dev) {
		err = -ENOMEM;
		goto exit;
	}

	switch_dev->cdev = input_allocate_device();
	if (NULL == switch_dev->cdev) {
		err = -ENOMEM;
		goto exit_dealloc_switch_dev;
	}

	switch_dev->nirtfeatures = nirtfeatures;
	switch_dev->pie_location.element = pie_element;
	switch_dev->pie_location.subelement = 0;
	memcpy(&switch_dev->pie_descriptor, pie,
	   sizeof(struct nirtfeatures_pie_descriptor));

	snprintf(switch_dev->name_string, MAX_NODELEN,
	   "%s:%s:uservisible=%d:states=(",
	   CONFIG_NI_LED_PREFIX, pie->name, pie->is_user_visible);
	for (i = 0; i < switch_descriptor->num_states; i++) {
		char temp[4] = { '\0' };

		sprintf(temp, "%d%c", switch_descriptor->state_value[i],
		   (i < switch_descriptor->num_states - 1) ? ',' : ')');
		strncat(switch_dev->name_string, temp, MAX_NODELEN);
	}

	snprintf(switch_dev->phys_location_string, MAX_NODELEN, "%s/%s/%s",
	   CONFIG_NI_LED_PREFIX, nirtfeatures->bpstring, pie->name);

	switch_dev->cdev->name = switch_dev->name_string;
	switch_dev->cdev->phys = switch_dev->phys_location_string;
	switch_dev->cdev->id.bustype = BUS_HOST;
	switch_dev->cdev->id.vendor = 0x3923;
	switch_dev->cdev->id.product = pie->pie_type;
	switch_dev->cdev->id.version = pie_caps_version;
	switch_dev->cdev->dev.parent = &nirtfeatures->acpi_device->dev;

	switch_dev->cdev->evbit[0] = BIT_MASK(EV_KEY);
	set_bit(BTN_0, switch_dev->cdev->keybit);

	err = input_register_device(switch_dev->cdev);
	if (0 != err) {
		input_free_device(switch_dev->cdev);
		goto exit_dealloc_switch_dev;
	}

	/* if this PIE supports notifications, enable them now */
	if (pie->notification_value != 0) {
		err = nirtfeatures_pie_enable_notifications(nirtfeatures,
		   pie_element, 0, 1);
		if (0 != err) {
			input_unregister_device(switch_dev->cdev);
			input_free_device(switch_dev->cdev);
			goto exit_dealloc_switch_dev;
		}
	}

	/* add the new device to our list of switch PIEs */
	list_add_tail(&switch_dev->node, &nirtfeatures_switch_pie_list);
	goto exit;

exit_dealloc_switch_dev:
	kfree(switch_dev);

exit:
	kfree(switch_descriptor);
	return err;
}


/* Parse a single PIE caps package from the PIEC buffer, determine the
 * type of PIE it is, then dispatch to the appropriate parsing routine.
 */
static int nirtfeatures_parse_one_pie(struct nirtfeatures *nirtfeatures,
	unsigned int       pie_caps_version,
	unsigned int       pie_element,
	union acpi_object *acpi_buffer)
{
	struct nirtfeatures_pie_descriptor pie;
	unsigned int                       i;

	if (NULL == nirtfeatures || NULL == acpi_buffer)
		return -EINVAL;

	/* check for proper type and number of elements */
	if (ACPI_TYPE_PACKAGE != acpi_buffer->type ||
	   6 != acpi_buffer->package.count)
		return -EINVAL;

	/* element 0 of the package is the name */
	if (ACPI_TYPE_BUFFER == acpi_buffer->package.elements[0].type) {
		for (i = 0;
		   i < acpi_buffer->package.elements[0].buffer.length &&
		   i < MAX_NAMELEN; i++) {
			/* get pointer to Nth Unicode character in name */
			unsigned short *unicode_char = (unsigned short *)
			   (acpi_buffer->package.elements[0].buffer.pointer +
			   (2 * i));
			/* naive convert to ASCII */
			pie.name[i] = (char) *unicode_char & 0xff;
		}
	} else
		return -EINVAL;

	/* element 1 of the package is the PIE class */
	if (ACPI_TYPE_INTEGER == acpi_buffer->package.elements[1].type)
		pie.pie_class = (enum nirtfeatures_pie_class)
		   acpi_buffer->package.elements[1].integer.value;
	else
		return -EINVAL;

	/* element 2 of the package is the PIE type */
	if (ACPI_TYPE_INTEGER == acpi_buffer->package.elements[2].type)
		pie.pie_type = (enum nirtfeatures_pie_type)
		   acpi_buffer->package.elements[2].integer.value;
	else
		return -EINVAL;

	/* element 4 of an package is the visible flag */
	if (ACPI_TYPE_INTEGER == acpi_buffer->package.elements[4].type)
		pie.is_user_visible =
		   (acpi_buffer->package.elements[4].integer.value != 0);
	else
		return -EINVAL;

	/* element 5 of the package is the notification value */
	if (ACPI_TYPE_INTEGER == acpi_buffer->package.elements[5].type)
		pie.notification_value =
		   acpi_buffer->package.elements[5].integer.value;
	else
		return -EINVAL;

	/* parse the type-specific descriptor in element 3 */
	switch (pie.pie_type) {
	case PIE_TYPE_LED:
		if (nirtfeatures_parse_led_pie(nirtfeatures,
		   pie_caps_version, pie_element, &pie,
		   &(acpi_buffer->package.elements[3])))
			return -EINVAL;
		break;
	case PIE_TYPE_SWITCH:
		if (nirtfeatures_parse_switch_pie(nirtfeatures,
		   pie_caps_version, pie_element, &pie,
		   &(acpi_buffer->package.elements[3])))
			return -EINVAL;
		break;

	default:
		return -EINVAL;
		break;
	}

	return 0;
}

/* Populate the list of physical interface elements from the table in
 * the DSDT and then generate the appropriate class devices.
 */
static int nirtfeatures_populate_pies(struct nirtfeatures *nirtfeatures)
{
	acpi_size          result_size;
	void              *result_buffer;
	union acpi_object *acpi_buffer;
	unsigned int       num_elements = 0;
	unsigned int       pie_caps_version;
	unsigned int       i;
	unsigned int       err = 0;

	if (NULL == nirtfeatures)
		return -EINVAL;

	/* get the PIE descriptor buffer from DSDT */
	if (nirtfeatures_call_acpi_method(nirtfeatures,
	   "PIEC", 0, NULL, &result_size, &result_buffer))
		return -1;

	acpi_buffer = (union acpi_object *) result_buffer;
	if (ACPI_TYPE_PACKAGE != acpi_buffer->type) {
		err = -1;
		goto exit;
	}

	/* the first element of the package is the caps version */
	if (ACPI_TYPE_INTEGER == acpi_buffer->package.elements[0].type)
		pie_caps_version = (unsigned int)
		   acpi_buffer->package.elements[0].integer.value;
	else {
		err = -1;
		goto exit;
	}

	if (pie_caps_version < MIN_PIE_CAPS_VERSION ||
	   pie_caps_version > MAX_PIE_CAPS_VERSION) {
		dev_err(&nirtfeatures->acpi_device->dev,
		   "nirtfeatures: invalid PIE caps version\n");
		err = -1;
		goto exit;
	}

	/* the second element of the package is the number of PIEs */
	if (ACPI_TYPE_INTEGER == acpi_buffer->package.elements[1].type)
		num_elements = (unsigned int)
		   acpi_buffer->package.elements[1].integer.value;
	else {
		err = -1;
		goto exit;
	}

   /* parse elements 2..N as PIE descriptors */
	for (i = 2; i < acpi_buffer->package.count; i++) {
		err = nirtfeatures_parse_one_pie(nirtfeatures,
		   pie_caps_version, i - 2,
		   &(acpi_buffer->package.elements[i]));
		if (0 != err)
			break;
	}

exit:
	kfree(result_buffer);
	return err;
}

static int nirtfeatures_create_leds(struct nirtfeatures *nirtfeatures)
{
	int i;
	int err;

	for (i = 0; i < ARRAY_SIZE(nirtfeatures_leds_common); ++i) {

		nirtfeatures_leds_common[i].nirtfeatures = nirtfeatures;

		if (0 == nirtfeatures_leds_common[i].cdev.max_brightness)
			nirtfeatures_leds_common[i].cdev.max_brightness = 1;

		nirtfeatures_leds_common[i].cdev.brightness_set =
			nirtfeatures_led_brightness_set;

		nirtfeatures_leds_common[i].cdev.brightness_get =
			nirtfeatures_led_brightness_get;

		err = led_classdev_register(&nirtfeatures->acpi_device->dev,
					    &nirtfeatures_leds_common[i].cdev);
		if (err)
			return err;
	}

	return 0;
}

static void nirtfeatures_remove_leds(struct nirtfeatures *nirtfeatures)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(nirtfeatures_leds_common); ++i)
		led_classdev_unregister(&nirtfeatures_leds_common[i].cdev);
}

static void nirtfeatures_remove_led_pies(struct nirtfeatures *nirtfeatures)
{
	struct nirtfeatures_led *cdev_iter;
	struct nirtfeatures_led *temp;

	spin_lock(&nirtfeatures->lock);

	/* walk the list of non-fixed LEDs and unregister/free their devices */
	list_for_each_entry_safe(
	   cdev_iter, temp, &nirtfeatures_led_pie_list, node) {
		led_classdev_unregister(&cdev_iter->cdev);
		kfree(cdev_iter);
	}

	spin_unlock(&nirtfeatures->lock);
}

static void nirtfeatures_remove_switch_pies(struct nirtfeatures *nirtfeatures)
{
	struct nirtfeatures_switch *cdev_iter;
	struct nirtfeatures_switch *temp;

	spin_lock(&nirtfeatures->lock);

	/* walk the list of switch devices and unregister/free each one */
	list_for_each_entry_safe(
	   cdev_iter, temp, &nirtfeatures_switch_pie_list, node) {
		/* disable notifications for this PIE if supported */
		if (cdev_iter->pie_descriptor.notification_value != 0) {
			nirtfeatures_pie_enable_notifications(nirtfeatures,
			   cdev_iter->pie_location.element,
				cdev_iter->pie_location.subelement,
			   0);
		}
		input_unregister_device(cdev_iter->cdev);
		input_free_device(cdev_iter->cdev);
		kfree(cdev_iter);
	}

	spin_unlock(&nirtfeatures->lock);
}

/* ACPI driver */

static acpi_status nirtfeatures_resources(struct acpi_resource *res, void *data)
{
	struct nirtfeatures *nirtfeatures = data;

	switch (res->type) {
	case ACPI_RESOURCE_TYPE_IO:
		if ((nirtfeatures->io_base != 0) ||
		    (nirtfeatures->io_size != 0)) {
			dev_err(&nirtfeatures->acpi_device->dev,
				"too many IO resources\n");
			return AE_ERROR;
		}

		nirtfeatures->io_base = res->data.io.minimum;
		nirtfeatures->io_size = res->data.io.address_length;

		return AE_OK;

	case ACPI_RESOURCE_TYPE_END_TAG:
		return AE_OK;

	default:
		dev_err(&nirtfeatures->acpi_device->dev,
			"unsupported resource type %u\n",
			res->type);
		return AE_ERROR;
	}

	return AE_OK;
}

/* Process a notification from ACPI, which typically occurs when a switch
 * PIE is signalling a change of state via its GPE.
 */
static void nirtfeatures_acpi_notify(struct acpi_device *device, u32 event)
{
	/* find the switch PIE for which this notification was generated,
	 * and push an event into its associated input subsystem node
	 */
	struct nirtfeatures_switch *iter;
	int                         state = 0;
	struct nirtfeatures        *nirtfeatures =
	   (struct nirtfeatures *)device->driver_data;

	spin_lock(&nirtfeatures->lock);

	list_for_each_entry(iter, &nirtfeatures_switch_pie_list, node) {
		if (event == iter->pie_descriptor.notification_value) {

			/* query instantaneous switch state */
			if (!nirtfeatures_pie_get_state(iter->nirtfeatures,
			   iter->pie_location.element,
			   iter->pie_location.subelement,
			   &state)) {
				/* push current state of switch */
				input_report_key(iter->cdev, BTN_0, !!state);
				input_sync(iter->cdev);
			}
			spin_unlock(&nirtfeatures->lock);
			return;
		}
	}

	spin_unlock(&nirtfeatures->lock);

	dev_err(&device->dev, "no input found for notification (event %02X)\n",
	   event);
}

static int nirtfeatures_acpi_remove(struct acpi_device *device)
{
	struct nirtfeatures *nirtfeatures = device->driver_data;

	nirtfeatures_remove_leds(nirtfeatures);

	nirtfeatures_remove_led_pies(nirtfeatures);
	nirtfeatures_remove_switch_pies(nirtfeatures);

	sysfs_remove_files(&nirtfeatures->acpi_device->dev.kobj,
			   nirtfeatures_attrs);

	if ((nirtfeatures->io_base != 0) &&
	    (nirtfeatures->io_size == NIRTF_IO_SIZE))
		release_region(nirtfeatures->io_base, nirtfeatures->io_size);

	device->driver_data = NULL;

	kfree(nirtfeatures);

	return 0;
}

static int nirtfeatures_acpi_add(struct acpi_device *device)
{
	struct nirtfeatures *nirtfeatures;
	acpi_status acpi_ret;
	u8 bpinfo;
	int err;

	nirtfeatures = kzalloc(sizeof(*nirtfeatures), GFP_KERNEL);

	if (!nirtfeatures)
		return -ENOMEM;

	device->driver_data = nirtfeatures;

	nirtfeatures->acpi_device = device;

	acpi_ret = acpi_walk_resources(device->handle, METHOD_NAME__CRS,
				       nirtfeatures_resources, nirtfeatures);

	if (ACPI_FAILURE(acpi_ret) ||
	    (nirtfeatures->io_base == 0) ||
	    (nirtfeatures->io_size != NIRTF_IO_SIZE)) {
		nirtfeatures_acpi_remove(device);
		return -ENODEV;
	}

	if (!request_region(nirtfeatures->io_base, nirtfeatures->io_size,
	    MODULE_NAME)) {
		nirtfeatures_acpi_remove(device);
		return -EBUSY;
	}

	bpinfo = inb(nirtfeatures->io_base + NIRTF_PLATFORM_MISC);

	bpinfo &= NIRTF_PLATFORM_MISC_ID_MASK;

	switch (bpinfo) {
	case NIRTF_PLATFORM_MISC_ID_MANHATTAN:
		nirtfeatures->bpstring = "Manhattan";
		break;
	case NIRTF_PLATFORM_MISC_ID_HAMMERHEAD:
		nirtfeatures->bpstring = "Hammerhead";
		break;
	case NIRTF_PLATFORM_MISC_ID_WINGHEAD:
		nirtfeatures->bpstring = "Winghead";
		break;
	default:
		dev_err(&nirtfeatures->acpi_device->dev,
			"Unrecognized backplane type %u\n",
			bpinfo);
		nirtfeatures->bpstring = "Unknown";
		break;
	}

	spin_lock_init(&nirtfeatures->lock);

	err = nirtfeatures_populate_pies(nirtfeatures);
	if (0 != err) {
		nirtfeatures_acpi_remove(device);
		return err;
	}

	nirtfeatures->revision[0] = inb(nirtfeatures->io_base + NIRTF_YEAR);
	nirtfeatures->revision[1] = inb(nirtfeatures->io_base + NIRTF_MONTH);
	nirtfeatures->revision[2] = inb(nirtfeatures->io_base + NIRTF_DAY);
	nirtfeatures->revision[3] = inb(nirtfeatures->io_base + NIRTF_HOUR);
	nirtfeatures->revision[4] = inb(nirtfeatures->io_base + NIRTF_MINUTE);

	err = sysfs_create_files(&nirtfeatures->acpi_device->dev.kobj,
				 nirtfeatures_attrs);
	if (0 != err) {
		nirtfeatures_acpi_remove(device);
		return err;
	}

	err = nirtfeatures_create_leds(nirtfeatures);
	if (0 != err) {
		nirtfeatures_acpi_remove(device);
		return err;
	}

	dev_info(&nirtfeatures->acpi_device->dev,
		 "IO range 0x%04X-0x%04X\n",
		 nirtfeatures->io_base,
		 nirtfeatures->io_base + nirtfeatures->io_size - 1);

	return 0;
}

static const struct acpi_device_id nirtfeatures_device_ids[] = {
	{"NIC775D", 0},
	{"", 0},
};

static struct acpi_driver nirtfeatures_acpi_driver = {
	.name = MODULE_NAME,
	.ids = nirtfeatures_device_ids,
	.ops = {
		.add = nirtfeatures_acpi_add,
		.remove = nirtfeatures_acpi_remove,
		.notify = nirtfeatures_acpi_notify,
		},
};

static int __init nirtfeatures_init(void)
{
	return acpi_bus_register_driver(&nirtfeatures_acpi_driver);
}

static void __exit nirtfeatures_exit(void)
{
	acpi_bus_unregister_driver(&nirtfeatures_acpi_driver);
}

module_init(nirtfeatures_init);
module_exit(nirtfeatures_exit);

MODULE_DEVICE_TABLE(acpi, nirtfeatures_device_ids);
MODULE_DESCRIPTION("NI RT Features");
MODULE_AUTHOR("Jeff Westfahl <jeff.westfahl@ni.com>");
MODULE_LICENSE("GPL");
