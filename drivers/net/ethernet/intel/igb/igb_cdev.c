#include "igb.h"
#include "igb_cdev.h"

#include <linux/pagemap.h>
#include <linux/bitops.h>
#include <linux/types.h>
#include <linux/cdev.h>

/* TSN char dev */
static DECLARE_BITMAP(cdev_minors, IGB_MAX_DEV_NUM);

static int igb_major;
static struct class *igb_class;
static const char * const igb_class_name = "igb_tsn";
static const char * const igb_dev_name = "igb_tsn_%s";

/* user-mode API forward definitions */
static int igb_open_file(struct inode *inode, struct file *file);
static int igb_close_file(struct inode *inode, struct file *file);
static int igb_mmap(struct file *file, struct vm_area_struct *vma);
static long igb_ioctl_file(struct file *file, unsigned int cmd,
			   unsigned long arg);

/* user-mode IO API registrations */
static const struct file_operations igb_fops = {
		.owner   = THIS_MODULE,
		.llseek  = no_llseek,
		.open	= igb_open_file,
		.release = igb_close_file,
		.mmap	= igb_mmap,
		.unlocked_ioctl = igb_ioctl_file,
};

int igb_tsn_setup_all_tx_resources(struct igb_adapter *adapter)
{
	struct pci_dev *pdev = adapter->pdev;
	int i, err = 0;

	for (i = 0; i < IGB_USER_TX_QUEUES; i++) {
		err = igb_setup_tx_resources(adapter->tx_ring[i]);
		if (err) {
			dev_err(&pdev->dev,
				"Allocation for Tx Queue %u failed\n", i);
			for (i--; i >= 0; i--)
				igb_free_tx_resources(adapter->tx_ring[i]);
			break;
		}
	}

	return err;
}

int igb_tsn_setup_all_rx_resources(struct igb_adapter *adapter)
{
	struct pci_dev *pdev = adapter->pdev;
	int i, err = 0;

	for (i = 0; i < IGB_USER_RX_QUEUES; i++) {
		err = igb_setup_rx_resources(adapter->rx_ring[i]);
		if (err) {
			dev_err(&pdev->dev,
				"Allocation for Rx Queue %u failed\n", i);
			for (i--; i >= 0; i--)
				igb_free_rx_resources(adapter->rx_ring[i]);
			break;
		}
	}

	return err;
}

void igb_tsn_free_all_tx_resources(struct igb_adapter *adapter)
{
	int i;

	for (i = 0; i < IGB_USER_TX_QUEUES; i++)
		igb_free_tx_resources(adapter->tx_ring[i]);
}

void igb_tsn_free_all_rx_resources(struct igb_adapter *adapter)
{
	int i;

	for (i = 0; i < IGB_USER_RX_QUEUES; i++)
		igb_free_rx_resources(adapter->rx_ring[i]);
}

static int igb_bind(struct file *file, void __user *argp)
{
	struct igb_adapter *adapter;
	u32 mmap_size;

	adapter = (struct igb_adapter *)file->private_data;

	if (!adapter)
		return -ENOENT;

	mmap_size = pci_resource_len(adapter->pdev, 0);

	if (copy_to_user(argp, &mmap_size, sizeof(mmap_size)))
		return -EFAULT;

	return 0;
}

static long igb_mapring(struct file *file, void __user *arg)
{
	struct igb_adapter *adapter;
	struct igb_buf_cmd req;
	int queue_size;
	unsigned long *uring_init;
	struct igb_ring *ring;
	int err;

	if (copy_from_user(&req, arg, sizeof(req)))
		return -EFAULT;

	if (req.flags != 0 && req.flags != 1)
		return -EINVAL;

	adapter = file->private_data;
	if (!adapter) {
		dev_err(&adapter->pdev->dev, "map to unbound device!\n");
		return -ENOENT;
	}

	/* Req flags, Tx: 0, Rx: 1 */
	if (req.flags == 0) {
		queue_size = IGB_USER_TX_QUEUES;
		uring_init =  &adapter->tx_uring_init;
		ring = adapter->tx_ring[req.queue];
	} else {
		queue_size = IGB_USER_RX_QUEUES;
		uring_init =  &adapter->rx_uring_init;
		ring = adapter->rx_ring[req.queue];
	}

	mutex_lock(&adapter->user_ring_mutex);
	if (test_bit(req.queue, uring_init)) {
		dev_err(&adapter->pdev->dev, "the queue is in using\n");
		err = -EBUSY;
		goto failed;
	}

	if (req.queue >= queue_size) {
		err = -EINVAL;
		goto failed;
	}

	set_bit(req.queue, uring_init);
	mutex_unlock(&adapter->user_ring_mutex);

	req.physaddr = ring->dma;
	req.mmap_size = ring->size;

	if (copy_to_user(arg, &req, sizeof(req))) {
		dev_err(&adapter->pdev->dev, "copyout to user failed\n");
		return -EFAULT;
	}

	return 0;
failed:
	mutex_unlock(&adapter->user_ring_mutex);
	return err;
}

static long igb_mapbuf(struct file *file, void __user *arg)
{
	struct igb_adapter *adapter;
	struct igb_buf_cmd req;
	struct page *page;
	dma_addr_t page_dma;
	struct igb_user_page *userpage;
	int err = 0;
	int direction;

	if (copy_from_user(&req, arg, sizeof(req)))
		return -EFAULT;

	if (req.flags != 0 && req.flags != 1)
		return -EINVAL;

	adapter = file->private_data;
	if (!adapter) {
		dev_err(&adapter->pdev->dev, "map to unbound device!\n");
		return -ENOENT;
	}

	userpage = kzalloc(sizeof(*userpage), GFP_KERNEL);
	if (unlikely(!userpage))
		return -ENOMEM;

	page = alloc_page(GFP_KERNEL | __GFP_COLD);
	if (unlikely(!page)) {
		err = -ENOMEM;
		goto failed;
	}

	direction = req.flags ? DMA_FROM_DEVICE : DMA_TO_DEVICE;
	page_dma = dma_map_page(&adapter->pdev->dev, page,
				0, PAGE_SIZE, direction);

	if (dma_mapping_error(&adapter->pdev->dev, page_dma)) {
		put_page(page);
		err = -ENOMEM;
		goto failed;
	}

	userpage->page = page;
	userpage->page_dma = page_dma;
	userpage->flags = req.flags;

	mutex_lock(&adapter->user_page_mutex);
	list_add_tail(&userpage->page_node, &adapter->user_page_list);
	mutex_unlock(&adapter->user_page_mutex);

	req.physaddr = page_dma;
	req.mmap_size = PAGE_SIZE;

	if (copy_to_user(arg, &req, sizeof(req))) {
		dev_err(&adapter->pdev->dev, "copyout to user failed\n");
		return -EFAULT;
	}
	return 0;

failed:
	kfree(userpage);
	return err;
}

static long igb_unmapring(struct file *file, void __user *arg)
{
	struct igb_adapter *adapter;
	struct igb_buf_cmd req;
	struct igb_ring *ring;
	int queue_size;
	unsigned long *uring_init;
	int err;

	if (copy_from_user(&req, arg, sizeof(req)))
		return -EFAULT;

	if (req.flags != 0 && req.flags != 1)
		return -EINVAL;

	adapter = file->private_data;
	if (!adapter) {
		dev_err(&adapter->pdev->dev, "map to unbound device!\n");
		return -ENOENT;
	}

	if (req.flags == 0) {
		queue_size = IGB_USER_TX_QUEUES;
		uring_init =  &adapter->tx_uring_init;
		ring = adapter->tx_ring[req.queue];
	} else {
		queue_size = IGB_USER_RX_QUEUES;
		uring_init =  &adapter->rx_uring_init;
		ring = adapter->rx_ring[req.queue];
	}

	if (req.queue >= queue_size)
		return -EINVAL;

	mutex_lock(&adapter->user_ring_mutex);
	if (!test_bit(req.queue, uring_init)) {
		dev_err(&adapter->pdev->dev, "the ring is already unmap\n");
		err = -EINVAL;
		goto failed;
	}

	clear_bit(req.queue, uring_init);
	mutex_unlock(&adapter->user_ring_mutex);

	return 0;
failed:
	mutex_unlock(&adapter->user_ring_mutex);
	return err;
}

static void igb_free_page(struct igb_adapter *adapter,
			  struct igb_user_page *userpage)
{
	int direction = userpage->flags ? DMA_FROM_DEVICE : DMA_TO_DEVICE;

	dma_unmap_page(&adapter->pdev->dev,
		       userpage->page_dma,
		       PAGE_SIZE,
		       direction);

	put_page(userpage->page);
	list_del(&userpage->page_node);
	kfree(userpage);
	userpage = NULL;
}

static long igb_unmapbuf(struct file *file, void __user *arg)
{
	int err = 0;
	struct igb_adapter *adapter;
	struct igb_buf_cmd req;
	struct igb_user_page *userpage, *tmp;

	if (copy_from_user(&req, arg, sizeof(req)))
		return -EFAULT;

	adapter = file->private_data;
	if (!adapter) {
		dev_err(&adapter->pdev->dev, "map to unbound device!\n");
		return -ENOENT;
	}

	mutex_lock(&adapter->user_page_mutex);
	if (list_empty(&adapter->user_page_list)) {
		err = -EINVAL;
		goto failed;
	}

	list_for_each_entry_safe(userpage, tmp, &adapter->user_page_list,
				 page_node) {
		if (req.physaddr == userpage->page_dma) {
			igb_free_page(adapter, userpage);
			break;
		}
	}
	mutex_unlock(&adapter->user_page_mutex);

	return 0;
failed:
	mutex_unlock(&adapter->user_page_mutex);
	return err;
}

static long igb_ioctl_file(struct file *file, unsigned int cmd,
			   unsigned long arg)
{
	void __user *argp = (void __user *)arg;
	int err;

	switch (cmd) {
	case IGB_BIND:
		err = igb_bind(file, argp);
		break;
	case IGB_MAPRING:
		err = igb_mapring(file, argp);
		break;
	case IGB_MAPBUF:
		err = igb_mapbuf(file, argp);
		break;
	case IGB_UNMAPRING:
		err = igb_unmapring(file, argp);
		break;
	case IGB_UNMAPBUF:
		err = igb_unmapbuf(file, argp);
		break;
	default:
		err = -EINVAL;
		break;
	};

	return err;
}

static int igb_open_file(struct inode *inode, struct file *file)
{
	struct igb_adapter *adapter;
	int err = 0;

	adapter = container_of(inode->i_cdev, struct igb_adapter, char_dev);
	if (!adapter)
		return -ENOENT;

	if (!adapter->qav_mode)
		return -EPERM;

	mutex_lock(&adapter->cdev_mutex);
	if (adapter->cdev_in_use) {
		err = -EBUSY;
		goto failed;
	}

	file->private_data = adapter;
	adapter->cdev_in_use = true;
	mutex_unlock(&adapter->cdev_mutex);

	return 0;
failed:
	mutex_unlock(&adapter->cdev_mutex);
	return err;
}

static int igb_close_file(struct inode *inode, struct file *file)
{
	struct igb_adapter *adapter = file->private_data;

	if (!adapter)
		return 0;

	mutex_lock(&adapter->cdev_mutex);
	if (!adapter->cdev_in_use)
		goto out;

	mutex_lock(&adapter->user_page_mutex);
	if (!list_empty(&adapter->user_page_list)) {
		struct igb_user_page *userpage, *tmp;

		list_for_each_entry_safe(userpage, tmp,
					 &adapter->user_page_list, page_node) {
			if (!userpage)
				igb_free_page(adapter, userpage);
		}
	}
	mutex_unlock(&adapter->user_page_mutex);

	file->private_data = NULL;
	adapter->cdev_in_use = false;
	adapter->tx_uring_init = 0;
	adapter->rx_uring_init = 0;

out:
	mutex_unlock(&adapter->cdev_mutex);
	return 0;
}

static int igb_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct igb_adapter *adapter = file->private_data;
	unsigned long size  = vma->vm_end - vma->vm_start;
	dma_addr_t pgoff = vma->vm_pgoff;
	dma_addr_t physaddr;

	if (!adapter)
		return -ENODEV;

	if (pgoff == 0)
		physaddr = pci_resource_start(adapter->pdev, 0) >> PAGE_SHIFT;
	else
		physaddr = pgoff;

	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

	if (remap_pfn_range(vma, vma->vm_start,
			    physaddr, size, vma->vm_page_prot))
		return -EAGAIN;

	return 0;
}

int igb_add_cdev(struct igb_adapter *adapter)
{
	int result = 0;
	dev_t dev_num;
	int igb_minor;

	igb_minor = find_first_zero_bit(cdev_minors, IGB_MAX_DEV_NUM);
	if (igb_minor >= IGB_MAX_DEV_NUM)
		return -EBUSY;
	set_bit(igb_minor, cdev_minors);

	dev_num = MKDEV(igb_major, igb_minor);
	cdev_init(&adapter->char_dev, &igb_fops);
	adapter->char_dev.owner = THIS_MODULE;
	adapter->char_dev.ops = &igb_fops;
	result = cdev_add(&adapter->char_dev, dev_num, 1);

	if (result) {
		dev_err(&adapter->pdev->dev,
			"igb_tsn: add character device failed\n");
		return result;
	}

	device_create(igb_class, NULL, dev_num, NULL, igb_dev_name,
		      adapter->netdev->name);

	return 0;
}

void igb_remove_cdev(struct igb_adapter *adapter)
{
	device_destroy(igb_class, adapter->char_dev.dev);
	cdev_del(&adapter->char_dev);
}

int igb_cdev_init(char *igb_driver_name)
{
	dev_t dev_num;
	int ret;

	ret = alloc_chrdev_region(&dev_num, 0, IGB_MAX_DEV_NUM,
				  igb_driver_name);
	if (ret)
		return ret;
	igb_major = MAJOR(dev_num);

	igb_class = class_create(THIS_MODULE, igb_class_name);
	if (IS_ERR(igb_class))
		pr_info("igb_tsn: create device class failed\n");

	return ret;
}

void igb_cdev_destroy(void)
{
	class_destroy(igb_class);
	unregister_chrdev_region(MKDEV(igb_major, 0), IGB_MAX_DEV_NUM);
}
