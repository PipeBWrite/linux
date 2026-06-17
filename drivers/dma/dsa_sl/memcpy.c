#include "asm/current.h"
#include "dsa.h"
#include "linux/device.h"
#include "linux/gfp_types.h"
#include "linux/slab.h"
#include "linux/timekeeping.h"
#include "linux/uaccess.h"
#include "memcpy.h"
#include "dump.h"
#include "ctrl.h"
#include "mm.h"
#include "linux/types.h"
#include <linux/dev_printk.h>
#include <linux/mman.h>
#include <linux/pci.h>
#include <linux/idxd.h>
#include <linux/delay.h>
#include <linux/string.h>
#include <linux/mm.h>

#define KB(x) ((x) << 10)

#define GB(x) ((x) << 30)

static unsigned long long total_time[233] = {};
static unsigned long total_count[233] = {};

static inline void add_stat(int pid, struct timespec64 *start,
			    struct timespec64 *end,
			    unsigned long long *total_time,
			    unsigned long *total_count)
{
	total_time[pid % 233] +=
		timespec64_to_ns(end) - timespec64_to_ns(start);
	total_count[pid % 233]++;

	if (total_count[pid % 233] % 100000 != 0) {
		return;
	}

	pr_info("Total time: %llu, total count: %lu, average time: %llu, pid: %d\n",
		total_time[pid % 233], total_count[pid % 233],
		total_time[pid % 233] / total_count[pid % 233], pid);
	total_count[pid % 233] = 0;
	total_time[pid % 233] = 0;
}

#define MEMCPY_STAT_BEGIN()           \
	struct timespec64 start, end; \
	ktime_get_real_ts64(&start);

#define MEMCPY_STAT_END()          \
	ktime_get_real_ts64(&end); \
	add_stat(current->pid, &start, &end, total_time, total_count);

static inline void submit_desc(void *wq_ptr, struct dsa_hw_desc *desc)
{
	iosubmit_cmds512(wq_ptr, desc, 1);
}

#define POLL_RETRY_MAX 100000

static inline bool is_dsa_capable(int pid)
{
	if (!dsa->enabled) {
		return false;
	}

	return (dsa->pid == 0 || pid == dsa->pid);
}

static inline uint8_t op_status(uint8_t status)
{
	return status & DSA_COMP_STATUS_MASK;
}

static inline int dsa_wait_for_completion(struct dsa_completion_record *cr)
{
	int retry = 0;
	struct device *dev = &dsa->pdev->dev;
	while (cr->status == 0 && retry <= POLL_RETRY_MAX)
		retry++;

	if (retry == POLL_RETRY_MAX) {
		dev_err(dev, "Exceeded retry count!\n");
		return -ETIMEDOUT;
	}

	if (op_status(cr->status) != DSA_COMP_SUCCESS) {
		dev_err(dev, "Operation failed: %d\n", cr->status);
		return -EIO;
	}

	return 0;
}

static inline unsigned long memcpy_user(void __user *dest,
					const void __user *src, size_t count)
{
	char *kernel_buffer;
	long ret;
	bool is_kmalloc = true;

	kernel_buffer = kmalloc(count, GFP_KERNEL);
	if (!kernel_buffer) {
		kernel_buffer = vmalloc(count);
		is_kmalloc = false;
		if (!kernel_buffer) {
			pr_err("Failed to allocate kernel buffer\n");
			return 1;
		}
	}

	ret = copy_from_user(kernel_buffer, src, count);
	if (ret) {
		kfree(kernel_buffer);
		return ret;
	}

	ret = copy_to_user(dest, kernel_buffer, count);

	if (is_kmalloc) {
		kfree(kernel_buffer);
	} else {
		vfree(kernel_buffer);
	}

	if (ret) {
		return ret;
	}

	return 0;
}

int dsa_memcpy_pp(uint64_t dst, uint64_t src, size_t n,
		  unsigned long (*copy_alt)(void *, const void *, size_t))
{
	struct dsa_hw_desc desc = {};
	struct dsa_completion_record cr __attribute__((aligned(32))) = {};
	const int blksize = 4096;
	const int offset = (4096 - dst % 4096) % 4096;
	n -= offset;
	int count = n / blksize;
	if (n % blksize != 0) {
		count--;
	}

	desc.opcode = DSA_OPCODE_MEMMOVE;
	desc.flags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_STORD | IDXD_OP_FLAG_CC;
	desc.xfer_size = blksize;
	desc.src_addr = src + offset;
	desc.dst_addr = dst + offset;
	desc.completion_addr = (unsigned long)&cr;

	int ret;
	struct page **pages =
		kvcalloc(count, sizeof(struct page *), GFP_KERNEL);
	if (!pages) {
		dev_err(&dsa->pdev->dev, "Failed to allocate pages\n");
		return -1;
	}

	for (int i = 0; i < count; i++) {
		ret = pin_user_pages_fast(desc.dst_addr, 1,
					  FOLL_WRITE | FOLL_LONGTERM,
					  &pages[i]);
		if (ret != 1) {
			dev_err(&dsa->pdev->dev, "Failed to pin user pages\n");
			return -1;
		}

		if (i == count - 1) {
			desc.flags |= IDXD_OP_FLAG_RCR;
		}

		wmb();
		submit_desc(dsa->wqs[0].wq_portal, &desc);
		desc.src_addr += blksize;
		desc.dst_addr += blksize;
	}

	copy_alt((void *)desc.dst_addr, (void *)desc.src_addr,
		 n - (count)*blksize);
	if (offset != 0) {
		copy_alt((void *)dst, (void *)src, offset);
	}

	ret = dsa_wait_for_completion(&cr);

	if (ret != 0) {
		dev_err(&dsa->pdev->dev, "Failed to complete memcpy: %d\n",
			ret);
		return -1;
	}

	unpin_user_pages(pages, count);
	kfree(pages);
	if (ret != DSA_COMP_SUCCESS) {
		dev_err(&dsa->pdev->dev, "Failed to complete memcpy: %d\n",
			ret);
		return -1;
	}

	return 0;
}

int dsa_memcpy_no_pagefault(uint64_t dst, uint64_t src, size_t size)
{
	struct device *dev = &dsa->pdev->dev;
	struct dsa_hw_desc desc = {};
	struct dsa_completion_record cr __attribute__((aligned(32))) = {};
	void *portal = dsa->wqs[0].wq_portal;

	desc.opcode = DSA_OPCODE_MEMMOVE;
	desc.flags = IDXD_OP_FLAG_RCR | IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_BOF |
		     IDXD_OP_FLAG_CC | IDXD_OP_FLAG_RCI;

	desc.xfer_size = size;
	desc.src_addr = (uint64_t)src;
	desc.dst_addr = (uint64_t)dst;
	desc.completion_addr = (uint64_t)&cr;
	desc.priv = 1;
	desc.int_handle = 1;

	wmb();
	submit_desc(portal, &desc);

	int ret = dsa_wait_for_completion(&cr);

	if (ret != 0) {
		dev_err(dev, "Failed to complete memcpy: %d\n", ret);
		msleep(1000);
		dsa_dump_swerr(dsa);
	}

	return 0;
}

int dsa_copy_from_user(void *dst, const void *src, size_t size)
{
	int pid = current->pid;
	MEMCPY_STAT_BEGIN();

	if (!is_dsa_capable(pid)) {
		int n = copy_user_generic(dst, src, size);
		MEMCPY_STAT_END();
		return n;
	}

	if (!is_all_addr_mapped((unsigned long)src, size)) {
		dev_err(&dsa->pdev->dev, "src is not initialized!");
		return -1;
	}

	int n = dsa_memcpy_no_pagefault((uint64_t)dst, (uint64_t)src, size);
	MEMCPY_STAT_END();
	return n;
}

int dsa_copy_to_user(void *dst, const void *src, size_t size)
{
	int pid = current->pid;
	MEMCPY_STAT_BEGIN();
	ktime_get_real_ts64(&start);

	if (!is_dsa_capable(pid)) {
		int n = copy_user_generic(dst, src, size);
		MEMCPY_STAT_END();
		return n;
	}

	if (is_all_addr_mapped((unsigned long)dst, size)) {
		int n = dsa_memcpy_no_pagefault((unsigned long)dst,
						(unsigned long)src, size);
		MEMCPY_STAT_END();
		return n;
	}

	int n = dsa_memcpy_pp((unsigned long)dst, (unsigned long)src, size,
			      copy_to_user);
	MEMCPY_STAT_END();
	return n;
}

int dsa_memcpy_user(void *dst, const void *src, size_t size)
{
	if (!is_dsa_capable(current->pid)) {
		return copy_user_generic(dst, src, size);
	}

	if (is_all_addr_mapped((unsigned long)dst, size)) {
		return dsa_memcpy_no_pagefault((unsigned long)dst,
					       (unsigned long)src, size);
	}

	return dsa_memcpy_pp((unsigned long)dst, (unsigned long)src, size,
			     memcpy_user);
}
