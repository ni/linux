#ifndef UIO_LCI_IOCTL_H
#define UIO_LCI_IOCTL_H
#include <linux/ioctl.h>
 
typedef struct
{
	size_t offset;
	size_t size;
} lci_arg_t;
 
#define UIO_LCI_DEV_TO_CPU _IOW('l', 0x80, lci_arg_t)
#define UIO_LCI_CPU_TO_DEV _IOW('l', 0x81, lci_arg_t)
 
#endif
