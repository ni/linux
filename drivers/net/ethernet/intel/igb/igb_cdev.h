#ifndef _IGB_CDEV_H_
#define _IGB_CDEV_H_

#include <asm/page.h>
#include <asm/ioctl.h>

struct igb_adapter;
/* queues reserved for user mode */
#define IGB_USER_TX_QUEUES        2
#define IGB_USER_RX_QUEUES        2
#define IGB_MAX_DEV_NUM  64

/* TSN char dev ioctls */
#define IGB_BIND       _IOW('E', 200, int)
#define IGB_MAPRING    _IOW('E', 201, int)
#define IGB_UNMAPRING  _IOW('E', 202, int)
#define IGB_MAPBUF     _IOW('E', 203, int)
#define IGB_UNMAPBUF   _IOW('E', 204, int)

/* Used with both map/unmap ring & buf ioctls */
struct igb_buf_cmd {
	u64		physaddr;
	u32		queue;
	u32		mmap_size;
	u32		flags;
};

struct igb_user_page {
	struct list_head page_node;
	struct page *page;
	dma_addr_t page_dma;
	u32 flags;
};

int igb_tsn_setup_all_tx_resources(struct igb_adapter *);
int igb_tsn_setup_all_rx_resources(struct igb_adapter *);
void igb_tsn_free_all_tx_resources(struct igb_adapter *);
void igb_tsn_free_all_rx_resources(struct igb_adapter *);

int igb_add_cdev(struct igb_adapter *adapter);
void igb_remove_cdev(struct igb_adapter *adapter);
int igb_cdev_init(char *igb_driver_name);
void igb_cdev_destroy(void);

#endif
