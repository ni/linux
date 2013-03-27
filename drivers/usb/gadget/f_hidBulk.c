/*
 * f_hid.c -- USB HID/Bulk switchable function driver
 *
 * Copyright (C) 2012 Nathan Sullivan
 * Based on f_hid.c, Copyright (C) 2010 Fabien Chouteau <fabien.chouteau@barco.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/kernel.h>
#include <linux/utsname.h>
#include <linux/module.h>
#include <linux/hid.h>
#include <linux/cdev.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>
#include <linux/usb/g_hid.h>

//In bulk mode, how many endpoints do we have?
#define BULK_ENDPOINTS 8

//Whether to use interrupt out, or the set_report control request for output
#define USE_INTR_OUT

//How many requests to keep outstanding
#define REQ_COUNT 1

//The minor number of the first hid interface - the one that switches
//to bulk.
#define FIRST_HID_MINOR 0

#define MAX_REQ_SIZE 1024

static int major, minors, bulk_major, bulk_minors;
static struct class *hidg_class;
static struct class *hidg_bulk_class;

/*-------------------------------------------------------------------------*/
/*                            HID gadget struct                            */

struct f_hidg {
	/* configuration */
	unsigned char			bInterfaceSubClass;
	unsigned char			bInterfaceProtocol;
	unsigned short			report_desc_length;
	char				*report_desc;
	unsigned short			report_length;

	/* recv report */
	char				*set_report_buff;
	unsigned short			set_report_length;
	spinlock_t			spinlock;
	wait_queue_head_t		read_queue;
#ifdef USE_INTR_OUT
	struct usb_request		*out_req;
#endif

	/* send report */
	struct mutex			lock;
	bool				write_pending;
	wait_queue_head_t		write_queue;
	struct usb_request		*req;

	int				minor;
	struct cdev			cdev;
	struct cdev			bulk_cdev;
	struct usb_function		func;
	struct usb_ep			*in_ep;
#ifdef USE_INTR_OUT
	struct usb_ep			*out_ep;
#endif

	/* endpoints for bulk mode */
	struct usb_ep			*bulk_eps[BULK_ENDPOINTS];
	struct list_head		bulk_reqs[BULK_ENDPOINTS];
	spinlock_t			bulk_spinlock;
	wait_queue_head_t		bulk_queues[BULK_ENDPOINTS];
};

static inline struct f_hidg *func_to_hidg(struct usb_function *f)
{
	return container_of(f, struct f_hidg, func);
}

/*-------------------------------------------------------------------------*/
/*                           Static descriptors                            */

static struct usb_interface_descriptor hidg_interface_desc = {
	.bLength		= sizeof hidg_interface_desc,
	.bDescriptorType	= USB_DT_INTERFACE,
	/* .bInterfaceNumber	= DYNAMIC */
	.bAlternateSetting	= 0,
#ifdef USE_INTR_OUT
	.bNumEndpoints		= 2,
#else
	.bNumEndpoints		= 1,
#endif
	.bInterfaceClass	= USB_CLASS_HID,
	/* .bInterfaceSubClass	= DYNAMIC */
	/* .bInterfaceProtocol	= DYNAMIC */
	/* .iInterface		= DYNAMIC */
};

//Only one of these two alternate descriptors will be used,
//the first HID interface alternates to bulk while others get empty.

//Alternate interface with bulk endpoints
static struct usb_interface_descriptor hidg_bulk_interface_desc = {
	.bLength		= sizeof hidg_interface_desc,
	.bDescriptorType	= USB_DT_INTERFACE,
	/* .bInterfaceNumber	= DYNAMIC */
	.bAlternateSetting	= 1,
	.bNumEndpoints		= 8,
	.bInterfaceClass	= USB_CLASS_VENDOR_SPEC,
	/* .bInterfaceSubClass	= DYNAMIC */
	/* .bInterfaceProtocol	= DYNAMIC */
	/* .iInterface		= DYNAMIC */
};

//Alternate interface with no endpoints
static struct usb_interface_descriptor hidg_empty_interface_desc = {
	.bLength		= sizeof hidg_empty_interface_desc,
	.bDescriptorType	= USB_DT_INTERFACE,
	/* .bInterfaceNumber    = DYNAMIC */
	.bAlternateSetting      = 1,
	.bNumEndpoints          = 0,
	.bInterfaceClass        = USB_CLASS_VENDOR_SPEC,
	/* .bInterfaceSubClass  = DYNAMIC */
	/* .bInterfaceProtocol  = DYNAMIC */
	/* .iInterface          = DYNAMIC */
};

static struct hid_descriptor hidg_desc = {
	.bLength			= sizeof hidg_desc,
	.bDescriptorType		= HID_DT_HID,
	.bcdHID				= 0x0101,
	.bCountryCode			= 0x00,
	.bNumDescriptors		= 0x1,
	/*.desc[0].bDescriptorType	= DYNAMIC */
	/*.desc[0].wDescriptorLenght	= DYNAMIC */
};

/* High-Speed Support */

static struct usb_endpoint_descriptor hidg_hs_in_ep_desc = {
	.bLength		= USB_DT_ENDPOINT_SIZE,
	.bDescriptorType	= USB_DT_ENDPOINT,
	.bEndpointAddress	= USB_DIR_IN,
	.bmAttributes		= USB_ENDPOINT_XFER_INT,
	/*.wMaxPacketSize	= DYNAMIC */
	.bInterval		= 1, /* FIXME: Add this field in the
				      * HID gadget configuration?
				      * (struct hidg_func_descriptor)
				      */
};

#ifdef USE_INTR_OUT
static struct usb_endpoint_descriptor hidg_hs_out_ep_desc = {
	.bLength		= USB_DT_ENDPOINT_SIZE,
	.bDescriptorType	= USB_DT_ENDPOINT,
	.bEndpointAddress	= USB_DIR_OUT,
	.bmAttributes		= USB_ENDPOINT_XFER_INT,
	/*.wMaxPacketSize	= DYNAMIC */
	.bInterval		= 1, /* FIXME: Add this field in the
				      * HID gadget configuration?
				      * (struct hidg_func_descriptor)
				      */
};
#endif

static struct usb_endpoint_descriptor hidg_bulk_ep_descs[] = {
	{
		.bLength		= USB_DT_ENDPOINT_SIZE,
		.bDescriptorType	= USB_DT_ENDPOINT,
		.bEndpointAddress	= USB_DIR_IN,
		.bmAttributes		= USB_ENDPOINT_XFER_BULK,
		/*.wMaxPacketSize	= DYNAMIC */
		/*.bInterval		= DONTCARE */
	},
	{
		.bLength		= USB_DT_ENDPOINT_SIZE,
		.bDescriptorType	= USB_DT_ENDPOINT,
		.bEndpointAddress	= USB_DIR_OUT,
		.bmAttributes		= USB_ENDPOINT_XFER_BULK,
		/*.wMaxPacketSize	= DYNAMIC */
		/*.bInterval		= DONTCARE */
	},
	{
		.bLength		= USB_DT_ENDPOINT_SIZE,
		.bDescriptorType	= USB_DT_ENDPOINT,
		.bEndpointAddress	= USB_DIR_IN,
		.bmAttributes		= USB_ENDPOINT_XFER_BULK,
		/*.wMaxPacketSize	= DYNAMIC */
		/*.bInterval		= DONTCARE */
	},
	{
		.bLength		= USB_DT_ENDPOINT_SIZE,
		.bDescriptorType	= USB_DT_ENDPOINT,
		.bEndpointAddress	= USB_DIR_OUT,
		.bmAttributes		= USB_ENDPOINT_XFER_BULK,
		/*.wMaxPacketSize	= DYNAMIC */
		/*.bInterval		= DONTCARE */
	},
	{
		.bLength		= USB_DT_ENDPOINT_SIZE,
		.bDescriptorType	= USB_DT_ENDPOINT,
		.bEndpointAddress	= USB_DIR_IN,
		.bmAttributes		= USB_ENDPOINT_XFER_BULK,
		/*.wMaxPacketSize	= DYNAMIC */
		/*.bInterval		= DONTCARE */
	},
	{
		.bLength		= USB_DT_ENDPOINT_SIZE,
		.bDescriptorType	= USB_DT_ENDPOINT,
		.bEndpointAddress	= USB_DIR_OUT,
		.bmAttributes		= USB_ENDPOINT_XFER_BULK,
		/*.wMaxPacketSize	= DYNAMIC */
		/*.bInterval		= DONTCARE */
	},
	{
		.bLength		= USB_DT_ENDPOINT_SIZE,
		.bDescriptorType	= USB_DT_ENDPOINT,
		.bEndpointAddress	= USB_DIR_IN,
		.bmAttributes		= USB_ENDPOINT_XFER_BULK,
		/*.wMaxPacketSize	= DYNAMIC */
		/*.bInterval		= DONTCARE */
	},
	{
		.bLength		= USB_DT_ENDPOINT_SIZE,
		.bDescriptorType	= USB_DT_ENDPOINT,
		.bEndpointAddress	= USB_DIR_OUT,
		.bmAttributes		= USB_ENDPOINT_XFER_BULK,
		/*.wMaxPacketSize	= DYNAMIC */
		/*.bInterval		= DONTCARE */
	}
};

//indeces into the descriptors array for the descs we change
#define ALT_INTF_DESC_INDEX 	4
#define BULK_EP_DESCS_INDEX	5

static struct usb_descriptor_header *hidg_hs_descriptors[] = {
	(struct usb_descriptor_header *)&hidg_interface_desc,
	(struct usb_descriptor_header *)&hidg_desc,
	(struct usb_descriptor_header *)&hidg_hs_in_ep_desc,
#ifdef USE_INTR_OUT
	(struct usb_descriptor_header *)&hidg_hs_out_ep_desc,
#endif
	(struct usb_descriptor_header *)&hidg_bulk_interface_desc,
	(struct usb_descriptor_header *)&hidg_bulk_ep_descs[0],
	(struct usb_descriptor_header *)&hidg_bulk_ep_descs[1],
	(struct usb_descriptor_header *)&hidg_bulk_ep_descs[2],
	(struct usb_descriptor_header *)&hidg_bulk_ep_descs[3],
	(struct usb_descriptor_header *)&hidg_bulk_ep_descs[4],
	(struct usb_descriptor_header *)&hidg_bulk_ep_descs[5],
	(struct usb_descriptor_header *)&hidg_bulk_ep_descs[6],
	(struct usb_descriptor_header *)&hidg_bulk_ep_descs[7],
	NULL,
};

static struct usb_descriptor_header *hidg_hs_empty_descriptors[] = {
	(struct usb_descriptor_header *)&hidg_interface_desc,
	(struct usb_descriptor_header *)&hidg_desc,
	(struct usb_descriptor_header *)&hidg_hs_in_ep_desc,
#ifdef USE_INTR_OUT
	(struct usb_descriptor_header *)&hidg_hs_out_ep_desc,
#endif
	(struct usb_descriptor_header *)&hidg_empty_interface_desc,
	NULL,
};

/* Full-Speed Support */

static struct usb_endpoint_descriptor hidg_fs_in_ep_desc = {
	.bLength		= USB_DT_ENDPOINT_SIZE,
	.bDescriptorType	= USB_DT_ENDPOINT,
	.bEndpointAddress	= USB_DIR_IN,
	.bmAttributes		= USB_ENDPOINT_XFER_INT,
	/*.wMaxPacketSize	= DYNAMIC */
	.bInterval		= 1, /* FIXME: Add this field in the
				       * HID gadget configuration?
				       * (struct hidg_func_descriptor)
				       */
};

#ifdef USE_INTR_OUT
static struct usb_endpoint_descriptor hidg_fs_out_ep_desc = {
	.bLength		= USB_DT_ENDPOINT_SIZE,
	.bDescriptorType	= USB_DT_ENDPOINT,
	.bEndpointAddress	= USB_DIR_OUT,
	.bmAttributes		= USB_ENDPOINT_XFER_INT,
	/*.wMaxPacketSize	= DYNAMIC */
	.bInterval		= 1, /* FIXME: Add this field in the
				       * HID gadget configuration?
				       * (struct hidg_func_descriptor)
				       */
};
#endif

static struct usb_descriptor_header *hidg_fs_descriptors[] = {
	(struct usb_descriptor_header *)&hidg_interface_desc,
	(struct usb_descriptor_header *)&hidg_desc,
	(struct usb_descriptor_header *)&hidg_fs_in_ep_desc,
#ifdef USE_INTR_OUT
	(struct usb_descriptor_header *)&hidg_fs_out_ep_desc,
#endif
	(struct usb_descriptor_header *)&hidg_bulk_interface_desc,
	(struct usb_descriptor_header *)&hidg_bulk_ep_descs[0],
	(struct usb_descriptor_header *)&hidg_bulk_ep_descs[1],
	(struct usb_descriptor_header *)&hidg_bulk_ep_descs[2],
	(struct usb_descriptor_header *)&hidg_bulk_ep_descs[3],
	(struct usb_descriptor_header *)&hidg_bulk_ep_descs[4],
	(struct usb_descriptor_header *)&hidg_bulk_ep_descs[5],
	(struct usb_descriptor_header *)&hidg_bulk_ep_descs[6],
	(struct usb_descriptor_header *)&hidg_bulk_ep_descs[7],
	NULL,
};

static struct usb_descriptor_header *hidg_fs_empty_descriptors[] = {
	(struct usb_descriptor_header *)&hidg_interface_desc,
	(struct usb_descriptor_header *)&hidg_desc,
	(struct usb_descriptor_header *)&hidg_fs_in_ep_desc,
#ifdef USE_INTR_OUT
	(struct usb_descriptor_header *)&hidg_fs_out_ep_desc,
#endif
	(struct usb_descriptor_header *)&hidg_empty_interface_desc,
	NULL,
};

/*-------------------------------------------------------------------------*/
/*                              Char Device                                */

static ssize_t f_hidg_read(struct file *file, char __user *buffer,
			size_t count, loff_t *ptr)
{
	struct f_hidg	*hidg     = file->private_data;
	char		*tmp_buff = NULL;
	unsigned long	flags;

	if (!count)
		return 0;

	if (!access_ok(VERIFY_WRITE, buffer, count))
		return -EFAULT;

	spin_lock_irqsave(&hidg->spinlock, flags);

#define READ_COND (hidg->set_report_buff != NULL)

	while (!READ_COND) {
		spin_unlock_irqrestore(&hidg->spinlock, flags);
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;

		if (wait_event_interruptible(hidg->read_queue, READ_COND))
			return -ERESTARTSYS;

		spin_lock_irqsave(&hidg->spinlock, flags);
	}


	count = min_t(unsigned, count, hidg->set_report_length);
	tmp_buff = hidg->set_report_buff;
	hidg->set_report_buff = NULL;

	spin_unlock_irqrestore(&hidg->spinlock, flags);

#ifdef USE_INTR_OUT
	/* resubmit this request since the read is done */
	usb_ep_queue(hidg->out_ep, hidg->out_req, GFP_ATOMIC);
#endif

	if (tmp_buff != NULL) {
		/* copy to user outside spinlock */
		count -= copy_to_user(buffer, tmp_buff, count);
		kfree(tmp_buff);
	} else
		count = -ENOMEM;

	return count;
}

static void f_hidg_req_complete(struct usb_ep *ep, struct usb_request *req)
{
	struct f_hidg *hidg = (struct f_hidg *)ep->driver_data;

	if (req->status != 0) {
		ERROR(hidg->func.config->cdev,
			"End Point Request ERROR: %d\n", req->status);
	}

	hidg->write_pending = 0;
	wake_up(&hidg->write_queue);
}

static ssize_t f_hidg_write(struct file *file, const char __user *buffer,
			    size_t count, loff_t *offp)
{
	struct f_hidg *hidg  = file->private_data;
	ssize_t status = -ENOMEM;

	if (!access_ok(VERIFY_READ, buffer, count))
		return -EFAULT;

	mutex_lock(&hidg->lock);

#define WRITE_COND (!hidg->write_pending)

	/* write queue */
	while (!WRITE_COND) {
		mutex_unlock(&hidg->lock);
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;

		if (wait_event_interruptible_exclusive(
				hidg->write_queue, WRITE_COND))
			return -ERESTARTSYS;

		mutex_lock(&hidg->lock);
	}

	count  = min_t(unsigned, count, hidg->report_length);
	status = copy_from_user(hidg->req->buf, buffer, count);

	if (status != 0) {
		ERROR(hidg->func.config->cdev,
			"copy_from_user error\n");
		mutex_unlock(&hidg->lock);
		return -EINVAL;
	}

	hidg->req->status   = 0;
	hidg->req->zero     = 0;
	hidg->req->length   = count;
	hidg->req->complete = f_hidg_req_complete;
	hidg->req->context  = hidg;
	hidg->write_pending = 1;

	status = usb_ep_queue(hidg->in_ep, hidg->req, GFP_ATOMIC);
	if (status < 0) {
		ERROR(hidg->func.config->cdev,
			"usb_ep_queue error on int endpoint %zd\n", status);
		hidg->write_pending = 0;
		wake_up(&hidg->write_queue);
	} else {
		status = count;
	}

	mutex_unlock(&hidg->lock);

	return status;
}

static unsigned int f_hidg_poll(struct file *file, poll_table *wait)
{
	struct f_hidg	*hidg  = file->private_data;
	unsigned int	ret = 0;

	poll_wait(file, &hidg->read_queue, wait);
	poll_wait(file, &hidg->write_queue, wait);

	if (WRITE_COND)
		ret |= POLLOUT | POLLWRNORM;

	if (READ_COND)
		ret |= POLLIN | POLLRDNORM;

	return ret;
}

#undef WRITE_COND
#undef READ_COND

static int f_hidg_release(struct inode *inode, struct file *fd)
{
	fd->private_data = NULL;
	return 0;
}

static int f_hidg_open(struct inode *inode, struct file *fd)
{
	struct f_hidg *hidg =
		container_of(inode->i_cdev, struct f_hidg, cdev);

	fd->private_data = hidg;

	return 0;
}

/*-------------------------------------------------------------------------*/
/*                           Bulk Char Device                              */

static ssize_t f_hidg_bulk_read(struct file *file, char __user *buffer,
			size_t count, loff_t *ptr)
{
	size_t actual = 0;
	struct usb_request *req, *safe_req;
	struct f_hidg *hidg = file->private_data;
	unsigned long flags;
	struct list_head *req_list;
	struct usb_ep *ep;

	/* out eps are every 2N+1 in the list */
	int index = 2 * iminor(file->f_dentry->d_inode) + 1;

	if(index > BULK_ENDPOINTS)
		return -EINVAL;

	req_list = &hidg->bulk_reqs[index];
	ep = hidg->bulk_eps[index];

	if (!count)
		return 0;

	if (!access_ok(VERIFY_WRITE, buffer, count))
		return -EFAULT;

	spin_lock_irqsave(&hidg->bulk_spinlock, flags);

	while (actual <= 0) {
		/* dump completed requests into the read buffer */
		list_for_each_entry_safe(req, safe_req, req_list, list) {
			if (req->actual > 0) {
				if (count < req->actual)
				{
					actual = -EINVAL;
					goto out;
				}
				copy_to_user(buffer, req->buf, req->actual);
				actual += req->actual;

				usb_ep_queue(ep, req, GFP_ATOMIC);
				list_del(&req->list);
			}
			else {
				usb_ep_queue(ep, req, GFP_ATOMIC);
				list_del(&req->list);
			}
		}

		if (actual <= 0) {
			/* hmm, not enough data ready.  Sleep */
			spin_unlock_irqrestore(&hidg->bulk_spinlock, flags);
			if (file->f_flags & O_NONBLOCK)
				return -EAGAIN;

			if (wait_event_interruptible(hidg->bulk_queues[index],
						!list_empty(req_list)))
				return -ERESTARTSYS;

			spin_lock_irqsave(&hidg->bulk_spinlock, flags);
		}
	}

out:
	spin_unlock_irqrestore(&hidg->bulk_spinlock, flags);

	return actual;
}

static ssize_t f_hidg_bulk_write(struct file *file, const char __user *buffer,
			    size_t count, loff_t *offp)
{
	struct f_hidg *hidg = file->private_data;
	ssize_t status = -ENOMEM;
	unsigned long flags;
	struct list_head *req_list;
	struct usb_request *req;
	struct usb_ep *ep;

	/* in eps are every 2N in the list */
	int index = 2 * iminor(file->f_dentry->d_inode);

	if (index > BULK_ENDPOINTS) {
		return -EINVAL;
	}

	if (!access_ok(VERIFY_READ, buffer, count))
		return -EFAULT;

	req_list = &hidg->bulk_reqs[index];
	ep = hidg->bulk_eps[index];

	spin_lock_irqsave(&hidg->bulk_spinlock, flags);

	while (list_empty(req_list))
	{
		spin_unlock_irqrestore(&hidg->bulk_spinlock, flags);

		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;

		if (wait_event_interruptible_exclusive(hidg->bulk_queues[index],
					!list_empty(req_list)))
			return -ERESTARTSYS;

		spin_lock_irqsave(&hidg->bulk_spinlock, flags);
	}

	req = list_entry(req_list->next, struct usb_request, list);
	list_del(&req->list);

	spin_unlock_irqrestore(&hidg->bulk_spinlock, flags);

	if (req->buf)
		kfree(req->buf);

	req->buf = kmalloc(count, GFP_KERNEL);

	if (!req->buf)
		goto out;

	copy_from_user(req->buf, buffer, count);

	req->status   = 0;
	req->zero     = 0;
	req->length   = count;

	status = usb_ep_queue(ep, req, GFP_ATOMIC);

	if(status < 0)
	{
		spin_lock_irqsave(&hidg->bulk_spinlock, flags);
		list_add_tail(&req->list, req_list);
		spin_unlock_irqrestore(&hidg->bulk_spinlock, flags);

		goto out;
	}

	status = count;
out:

	return status;
}

static int f_hidg_bulk_release(struct inode *inode, struct file *fd)
{
	fd->private_data = NULL;
	return 0;
}

static int f_hidg_bulk_open(struct inode *inode, struct file *fd)
{
	struct f_hidg *hidg =
		container_of(inode->i_cdev, struct f_hidg, bulk_cdev);

	fd->private_data = hidg;

	return 0;
}

/*-------------------------------------------------------------------------*/
/*                                usb_function                             */

static void hidg_set_report_complete(struct usb_ep *ep, struct usb_request *req)
{
	struct f_hidg *hidg = (struct f_hidg *)req->context;

	if (req->status != 0 || req->buf == NULL || req->actual == 0) {
		ERROR(hidg->func.config->cdev, "%s FAILED\n", __func__);
		return;
	}

	spin_lock(&hidg->spinlock);

	hidg->set_report_buff = krealloc(hidg->set_report_buff,
					 req->actual, GFP_ATOMIC);

	if (hidg->set_report_buff == NULL) {
		spin_unlock(&hidg->spinlock);
		return;
	}
	hidg->set_report_length = req->actual;
	memcpy(hidg->set_report_buff, req->buf, req->actual);

	spin_unlock(&hidg->spinlock);

	wake_up(&hidg->read_queue);
}

static void hidg_bulk_complete(struct usb_ep *ep, struct usb_request *req)
{
	struct f_hidg *hidg = ep->driver_data;
	int index = (int)req->context;

	list_add_tail(&req->list, &(hidg->bulk_reqs[index]));

	wake_up(&(hidg->bulk_queues[index]));
}

static int hidg_setup(struct usb_function *f,
		const struct usb_ctrlrequest *ctrl)
{
	struct f_hidg			*hidg = func_to_hidg(f);
	struct usb_composite_dev	*cdev = f->config->cdev;
	struct usb_request		*req  = cdev->req;
	int status = 0;
	__u16 value, length;

	value	= __le16_to_cpu(ctrl->wValue);
	length	= __le16_to_cpu(ctrl->wLength);

	VDBG(cdev, "hid_setup crtl_request : bRequestType:0x%x bRequest:0x%x "
		"Value:0x%x\n", ctrl->bRequestType, ctrl->bRequest, value);

	switch ((ctrl->bRequestType << 8) | ctrl->bRequest) {
	case ((USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE) << 8
		  | HID_REQ_GET_REPORT):
		VDBG(cdev, "get_report\n");

		/* send an empty report */
		length = min_t(unsigned, length, hidg->report_length);
		memset(req->buf, 0x0, length);

		goto respond;
		break;

	case ((USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE) << 8
		  | HID_REQ_GET_PROTOCOL):
		VDBG(cdev, "get_protocol\n");
		goto stall;
		break;

	case ((USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE) << 8
		  | HID_REQ_SET_REPORT):
		VDBG(cdev, "set_report | wLenght=%d\n", ctrl->wLength);
		req->context  = hidg;
		req->complete = hidg_set_report_complete;
		goto respond;
		break;

	case ((USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE) << 8
		  | HID_REQ_SET_PROTOCOL):
		VDBG(cdev, "set_protocol\n");
		goto stall;
		break;

	case ((USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_INTERFACE) << 8
		  | USB_REQ_GET_DESCRIPTOR):
		switch (value >> 8) {
		case HID_DT_HID:
			VDBG(cdev, "USB_REQ_GET_DESCRIPTOR: HID\n");
			length = min_t(unsigned short, length,
						   hidg_desc.bLength);
			memcpy(req->buf, &hidg_desc, length);
			goto respond;
			break;
		case HID_DT_REPORT:
			VDBG(cdev, "USB_REQ_GET_DESCRIPTOR: REPORT\n");
			length = min_t(unsigned short, length,
						   hidg->report_desc_length);
			memcpy(req->buf, hidg->report_desc, length);
			goto respond;
			break;

		default:
			VDBG(cdev, "Unknown decriptor request 0x%x\n",
				 value >> 8);
			goto stall;
			break;
		}
		break;

	default:
		VDBG(cdev, "Unknown request 0x%x\n",
			 ctrl->bRequest);
		goto stall;
		break;
	}

stall:
	return -EOPNOTSUPP;

respond:
	req->zero = 0;
	req->length = length;
	status = usb_ep_queue(cdev->gadget->ep0, req, GFP_ATOMIC);
	if (status < 0)
		ERROR(cdev, "usb_ep_queue error on ep0 %d\n", value);
	return status;
}

static void hidg_bulk_set_maxpacket(struct f_hidg *hidg)
{
	int i = BULK_EP_DESCS_INDEX;

	while(hidg_hs_descriptors[i] != NULL) {
		struct usb_endpoint_descriptor* ep_desc =
			(struct usb_endpoint_descriptor*)hidg_hs_descriptors[i];

		ep_desc->wMaxPacketSize = 
			min_t(short, hidg->report_length, 512);

		i++;
	}
}

static int hidg_bulk_enable_eps(struct usb_function *f, struct f_hidg *hidg)
{
	int i;
	int status=0;

	for(i=0; i < BULK_ENDPOINTS; i++) {

		if (hidg->bulk_eps[i] == NULL) continue;

		if (hidg->bulk_eps[i]->driver_data != NULL)
			usb_ep_disable(hidg->bulk_eps[i]);

		status = config_ep_by_speed(f->config->cdev->gadget, f,
						    hidg->bulk_eps[i]);
		if (status) {
			goto fail;
		}
		status = usb_ep_enable(hidg->bulk_eps[i]);
		if (status < 0) {
			goto fail;
		}
			
		hidg->bulk_eps[i]->driver_data = hidg;
	}
fail:
	return status;
}

static void hidg_bulk_submit_reqs(struct f_hidg *hidg)
{
	int i;
	struct usb_request *req;

	for(i=0; i < BULK_ENDPOINTS; i++) {

		if (!(hidg->bulk_eps[i]->desc->bEndpointAddress
			       	& USB_ENDPOINT_DIR_MASK)) {

			while (!list_empty(&hidg->bulk_reqs[i])) {

				req = list_entry(hidg->bulk_reqs[i].next,
					       	struct usb_request, list);

				req->status	= 0;
				req->zero	= 0;
				req->length	= hidg->bulk_eps[i]->maxpacket;
				req->context	= (void*)i;
				req->complete	= hidg_bulk_complete;

				usb_ep_queue(hidg->bulk_eps[i], req, GFP_ATOMIC);
				list_del(&req->list);
			}
		}
	}
}

static void hidg_bulk_disable_eps(struct f_hidg *hidg)
{
	int i;

	for(i=0; i < BULK_ENDPOINTS; i++) {
		if(hidg->bulk_eps[i]->driver_data != NULL)
			usb_ep_disable(hidg->bulk_eps[i]);
		hidg->bulk_eps[i]->driver_data = NULL;
	}
}

static void hidg_disable(struct usb_function *f)
{
	struct f_hidg *hidg = func_to_hidg(f);

	usb_ep_disable(hidg->in_ep);
	hidg->in_ep->driver_data = NULL;

#ifdef USE_INTR_OUT
	usb_ep_disable(hidg->out_ep);
	hidg->out_ep->driver_data = NULL;
#endif

	if (hidg->minor == FIRST_HID_MINOR)
	{
		hidg_bulk_disable_eps(hidg);
	}
}

static int hidg_set_alt(struct usb_function *f, unsigned intf, unsigned alt)
{
	struct usb_composite_dev		*cdev = f->config->cdev;
	struct f_hidg				*hidg = func_to_hidg(f);
	int status = 0;

	VDBG(cdev, "hidg_set_alt intf:%d alt:%d\n", intf, alt);

	if (hidg->in_ep != NULL) {
		/* restart endpoint */
		if (hidg->in_ep->driver_data != NULL)
			usb_ep_disable(hidg->in_ep);

		if(alt == 0) {
			status = config_ep_by_speed(f->config->cdev->gadget, f,
						    hidg->in_ep);
			if (status) {
				ERROR(cdev, "config_ep_by_speed FAILED!\n");
				goto fail;
			}
			status = usb_ep_enable(hidg->in_ep);
			if (status < 0) {
				ERROR(cdev, "Enable endpoint FAILED!\n");
				goto fail;
			}
			hidg->in_ep->driver_data = hidg;
		}
	}

#ifdef USE_INTR_OUT
	if(hidg->out_ep != NULL) {
		/* restart endpoint */
		if(hidg->out_ep->driver_data != NULL)
			usb_ep_disable(hidg->out_ep);

		if (alt == 0) {
			status = config_ep_by_speed(f->config->cdev->gadget, f,
						    hidg->out_ep);

			if (status) {
				ERROR(cdev, "config_ep_by_speed FAILED for out!\n");
				goto fail;
			}
			status = usb_ep_enable(hidg->out_ep);
			if (status < 0) {
				ERROR(cdev, "Enable endpoint FAILED! for out\n");
				goto fail;
			}
			hidg->out_ep->driver_data = hidg;

			/* submit first out request */
			hidg->out_req->status   = 0;
			hidg->out_req->zero     = 0;
			hidg->out_req->length   = hidg->report_length;
			hidg->out_req->complete = hidg_set_report_complete;
			hidg->out_req->context  = hidg;

			status = usb_ep_queue(hidg->out_ep, hidg->out_req, GFP_ATOMIC);
		}
	}
#endif

	if(hidg->minor == FIRST_HID_MINOR && alt == 1) {
		/* enable, submit all the requests on bulk out endpoints */
		hidg_bulk_enable_eps(f, hidg);
		hidg_bulk_submit_reqs(hidg);
	}

fail:
	return status;
}

const struct file_operations f_hidg_fops = {
	.owner		= THIS_MODULE,
	.open		= f_hidg_open,
	.release	= f_hidg_release,
	.write		= f_hidg_write,
	.read		= f_hidg_read,
	.poll		= f_hidg_poll,
	.llseek		= noop_llseek,
};

const struct file_operations f_hidg_bulk_fops = {
	.owner		= THIS_MODULE,
	.open		= f_hidg_bulk_open,
	.release	= f_hidg_bulk_release,
	.write		= f_hidg_bulk_write,
	.read		= f_hidg_bulk_read,
	.llseek		= noop_llseek,
};

static int __init hidg_bind(struct usb_configuration *c, struct usb_function *f)
{
	struct usb_ep		*ep;
	struct usb_request	*req, *safe_req;
	struct f_hidg		*hidg = func_to_hidg(f);
	int			status;
	dev_t			dev;
	int			i;

	/* allocate instance-specific interface IDs, and patch descriptors */
	status = usb_interface_id(c, f);
	if (status < 0)
		goto fail;
	hidg_interface_desc.bInterfaceNumber = status;
	hidg_bulk_interface_desc.bInterfaceNumber = status;
	hidg_empty_interface_desc.bInterfaceNumber = status;

	/* allocate instance-specific endpoints */
	status = -ENODEV;
	ep = usb_ep_autoconfig(c->cdev->gadget, &hidg_fs_in_ep_desc);
	if (!ep)
		goto fail;
	ep->driver_data = c->cdev;	/* claim */
	hidg->in_ep = ep;

#ifdef USE_INTR_OUT
	ep = usb_ep_autoconfig(c->cdev->gadget, &hidg_fs_out_ep_desc);
	if (!ep)
		goto fail;
	ep->driver_data = c->cdev;
	hidg->out_ep = ep;
#endif

	/* preallocate request and buffer */
	status = -ENOMEM;
	hidg->req = usb_ep_alloc_request(hidg->in_ep, GFP_KERNEL);
	if (!hidg->req)
		goto fail;

#ifdef USE_INTR_OUT
	hidg->out_req = usb_ep_alloc_request(hidg->out_ep, GFP_KERNEL);
	if (!hidg->out_req)
		goto fail;
#endif

	hidg->req->buf = kmalloc(hidg->report_length, GFP_KERNEL);
	if (!hidg->req->buf)
		goto fail;

#ifdef USE_INTR_OUT
	hidg->out_req->buf = kmalloc(hidg->report_length, GFP_KERNEL);
	if(!hidg->out_req->buf)
		goto fail;
#endif

	/* set descriptor dynamic values */
	hidg_interface_desc.bInterfaceSubClass = hidg->bInterfaceSubClass;
	hidg_interface_desc.bInterfaceProtocol = hidg->bInterfaceProtocol;
	hidg_hs_in_ep_desc.wMaxPacketSize = cpu_to_le16(min_t(short, hidg->report_length, 512));
	hidg_fs_in_ep_desc.wMaxPacketSize = cpu_to_le16(min_t(short, hidg->report_length, 64));
#ifdef USE_INTR_OUT
	hidg_hs_out_ep_desc.wMaxPacketSize = cpu_to_le16(min_t(short, hidg->report_length, 512));
	hidg_fs_out_ep_desc.wMaxPacketSize = cpu_to_le16(min_t(short, hidg->report_length, 64));
#endif
	hidg_desc.desc[0].bDescriptorType = HID_DT_REPORT;
	hidg_desc.desc[0].wDescriptorLength =
		cpu_to_le16(hidg->report_desc_length);

	/* the first intf is special - it can switch to bulk */
	/* others switch to empty for their alternate */
	if(hidg->minor == FIRST_HID_MINOR)
	{
		/* high and low speed use the same bulk descriptors */
		struct usb_request *req;
		struct usb_endpoint_descriptor **bulk_descs = 
			(struct usb_endpoint_descriptor **)
			   &hidg_hs_descriptors[BULK_EP_DESCS_INDEX];

		i = 0;
		while(bulk_descs[i] != NULL)
		{
			int j = 0;

			ep = usb_ep_autoconfig(c->cdev->gadget, bulk_descs[i]);
			if (!ep)
				goto fail;

			ep->driver_data = hidg;		/* claim */
			hidg->bulk_eps[i] = ep;

			INIT_LIST_HEAD(&(hidg->bulk_reqs[i]));
			/* preallocate bulk reqs for endpoints */
			for(j = 0; j < REQ_COUNT; j++)
			{
				struct usb_request *req =
					usb_ep_alloc_request(hidg->bulk_eps[i], GFP_KERNEL);

				if(!req)
					goto fail;

				req->context = (void*)i;
				req->complete = hidg_bulk_complete;

				list_add(&req->list, &(hidg->bulk_reqs[i]));
			}
			i++;
		}
		/* allocate buffers for out requests */
		for( i = 1; i < BULK_ENDPOINTS; i += 2) {
			list_for_each_entry(req, &(hidg->bulk_reqs[i]), list) {
				req->buf = kmalloc(4096, GFP_KERNEL);
				if (!req->buf)
					goto fail;
			}
		}
	}

	hidg->set_report_buff = NULL;

	/* copy descriptors */
	if(hidg->minor == FIRST_HID_MINOR)
		f->descriptors = usb_copy_descriptors(hidg_fs_descriptors);
	else
		f->descriptors = usb_copy_descriptors(hidg_fs_empty_descriptors);

	if (!f->descriptors)
		goto fail;

	if (gadget_is_dualspeed(c->cdev->gadget)) {
		hidg_hs_in_ep_desc.bEndpointAddress =
			hidg_fs_in_ep_desc.bEndpointAddress;
#ifdef USE_INTR_OUT
		hidg_hs_out_ep_desc.bEndpointAddress =
			hidg_fs_out_ep_desc.bEndpointAddress;
#endif
		/* usb_ep_autoconfig sets max packet to 64 for fs */
		if(hidg->minor == FIRST_HID_MINOR) {
			hidg_bulk_set_maxpacket(hidg);
		}
		if(hidg->minor == FIRST_HID_MINOR)
			f->hs_descriptors = usb_copy_descriptors(hidg_hs_descriptors);
		else
			f->hs_descriptors = usb_copy_descriptors(hidg_hs_empty_descriptors);

		if (!f->hs_descriptors)
			goto fail;
	}

	mutex_init(&hidg->lock);
	spin_lock_init(&hidg->spinlock);
	init_waitqueue_head(&hidg->write_queue);
	init_waitqueue_head(&hidg->read_queue);

	/* create char device */
	cdev_init(&hidg->cdev, &f_hidg_fops);
	dev = MKDEV(major, hidg->minor);
	status = cdev_add(&hidg->cdev, dev, 1);
	if (status)
		goto fail;

	device_create(hidg_class, NULL, dev, NULL, "%s%d", "hidg", hidg->minor);

	if (hidg->minor == FIRST_HID_MINOR)
	{
		spin_lock_init(&hidg->bulk_spinlock);

		for(i=0; i < BULK_ENDPOINTS; i++)
			init_waitqueue_head(&hidg->bulk_queues[i]);

		/* create the bulk related char devices */
		cdev_init(&hidg->bulk_cdev, &f_hidg_bulk_fops);
		dev = MKDEV(bulk_major, 0);
		status = cdev_add(&hidg->bulk_cdev, dev, 6);
		if (status)
			goto fail;

		for(i = 0; i < BULK_ENDPOINTS / 2; i++) {
			device_create(hidg_bulk_class, NULL, MKDEV(bulk_major, i),
					NULL, "hidg_bulk%d", i);
		}
	}

	return 0;

fail:
	ERROR(f->config->cdev, "hidg_bind FAILED\n");
	if (hidg->req != NULL) {
		kfree(hidg->req->buf);
		if (hidg->in_ep != NULL)
			usb_ep_free_request(hidg->in_ep, hidg->req);
	}

	if (hidg->minor == FIRST_HID_MINOR)
	{
		for(i=0; i<BULK_ENDPOINTS; i++)
		{
			device_destroy(hidg_bulk_class, MKDEV(bulk_major, i));

			usb_ep_disable(hidg->bulk_eps[i]);

			list_for_each_entry_safe(req, safe_req, 
						 &(hidg->bulk_reqs[i]), list) {
				list_del(&req->list);

				if (req->buf)
					kfree(req->buf);

				usb_ep_free_request(hidg->bulk_eps[i], req);
			}
		}
	}

	usb_free_descriptors(f->hs_descriptors);
	usb_free_descriptors(f->descriptors);

	return status;
}

static void hidg_unbind(struct usb_configuration *c, struct usb_function *f)
{
	struct f_hidg *hidg = func_to_hidg(f);
	struct usb_request *req, *safe_req;
	int i;

	device_destroy(hidg_class, MKDEV(major, hidg->minor));
	cdev_del(&hidg->cdev);

	/* disable/free request and end point */
	usb_ep_disable(hidg->in_ep);
	kfree(hidg->req->buf);
	usb_ep_free_request(hidg->in_ep, hidg->req);

	/* free descriptors copies */
	usb_free_descriptors(f->hs_descriptors);
	usb_free_descriptors(f->descriptors);

	if (hidg->minor == FIRST_HID_MINOR)
	{
		for(i=0; i<BULK_ENDPOINTS; i++)
		{
			device_destroy(hidg_bulk_class, MKDEV(bulk_major, i));

			usb_ep_disable(hidg->bulk_eps[i]);

			list_for_each_entry_safe(req, safe_req, 
						 &(hidg->bulk_reqs[i]), list) {
				list_del(&req->list);

				if (req->buf)
					kfree(req->buf);

				usb_ep_free_request(hidg->bulk_eps[i], req);
			}
		}

		cdev_del(&hidg->bulk_cdev);

	}

	kfree(hidg->report_desc);
	kfree(hidg->set_report_buff);
	kfree(hidg);
}

/*-------------------------------------------------------------------------*/
/*                                 Strings                                 */

#define CT_FUNC_HID_IDX	0

static struct usb_string ct_func_string_defs[] = {
	[CT_FUNC_HID_IDX].s	= "HID Interface",
	{},			/* end of list */
};

static struct usb_gadget_strings ct_func_string_table = {
	.language	= 0x0409,	/* en-US */
	.strings	= ct_func_string_defs,
};

static struct usb_gadget_strings *ct_func_strings[] = {
	&ct_func_string_table,
	NULL,
};

/*-------------------------------------------------------------------------*/
/*                             usb_configuration                           */

int __init hidg_bind_config(struct usb_configuration *c,
			    struct hidg_func_descriptor *fdesc, int index)
{
	struct f_hidg *hidg;
	int status;

	if (index >= minors)
		return -ENOENT;

	/* maybe allocate device-global string IDs, and patch descriptors */
	if (ct_func_string_defs[CT_FUNC_HID_IDX].id == 0) {
		status = usb_string_id(c->cdev);
		if (status < 0)
			return status;
		ct_func_string_defs[CT_FUNC_HID_IDX].id = status;
		hidg_interface_desc.iInterface = status;
	}

	/* allocate and initialize one new instance */
	hidg = kzalloc(sizeof *hidg, GFP_KERNEL);
	if (!hidg)
		return -ENOMEM;

	hidg->minor = index;
	hidg->bInterfaceSubClass = fdesc->subclass;
	hidg->bInterfaceProtocol = fdesc->protocol;
	hidg->report_length = fdesc->report_length;
	hidg->report_desc_length = fdesc->report_desc_length;
	hidg->report_desc = kmemdup(fdesc->report_desc,
				    fdesc->report_desc_length,
				    GFP_KERNEL);
	if (!hidg->report_desc) {
		kfree(hidg);
		return -ENOMEM;
	}

	hidg->func.name    = "hid";
	hidg->func.strings = ct_func_strings;
	hidg->func.bind    = hidg_bind;
	hidg->func.unbind  = hidg_unbind;
	hidg->func.set_alt = hidg_set_alt;
	hidg->func.disable = hidg_disable;
	hidg->func.setup   = hidg_setup;

	status = usb_add_function(c, &hidg->func);
	if (status)
		kfree(hidg);

	return status;
}

int __init ghid_setup(struct usb_gadget *g, int count)
{
	int status;
	dev_t dev;

	hidg_class = class_create(THIS_MODULE, "hidg");

	status = alloc_chrdev_region(&dev, 0, count, "hidg");
	if (!status) {
		major = MAJOR(dev);
		minors = count;
	}

	return status;
}

int __init ghid_bulk_setup(struct usb_gadget *g)
{
	int status;
	dev_t dev;

	hidg_bulk_class = class_create(THIS_MODULE, "hidg_bulk");
	status = alloc_chrdev_region(&dev, 0, BULK_ENDPOINTS, "hidg_bulk");
	if(!status) {
		bulk_major = MAJOR(dev);
		bulk_minors = BULK_ENDPOINTS;
	}

	return status;
}

void ghid_cleanup(void)
{
	if (major) {
		unregister_chrdev_region(MKDEV(major, 0), minors);
		major = minors = 0;
	}

	class_destroy(hidg_class);
	hidg_class = NULL;
}

void ghid_bulk_cleanup(void)
{
	if(bulk_major)
	{
		unregister_chrdev_region(MKDEV(bulk_major, 0), bulk_minors);
		bulk_major = bulk_minors = 0;
	}

	class_destroy(hidg_bulk_class);
	hidg_bulk_class = NULL;
}

