#ifndef DSA_IOCTL_H
#define DSA_IOCTL_H

#include <linux/ioctl.h>
#include "dsa.h"
#include <uapi/linux/dsa_sl.h>

struct dsa_user_ctx {
	pid_t pid;
	struct iommu_sva *sva;
	unsigned int pasid;
	struct mm_struct *mm;
	struct dsa_memcpy_reqs *reqs;
};

void dsa_register_chardev(struct dsa_device_info *dsa);
void dsa_unregister_chardev(struct dsa_device_info *dsa);

#endif
