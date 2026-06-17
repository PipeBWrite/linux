#ifndef DSA_IOCTL_UAPI_H
#define DSA_IOCTL_UAPI_H

#include <linux/types.h>

#define DSA_IOCTL_MAGIC 'k'

#define DSA_IOCTL_MEMCPY _IOW(DSA_IOCTL_MAGIC, 0, struct dsa_memcpy_args)
#define DSA_IOCTL_MEMCPY_PPL _IOW(DSA_IOCTL_MAGIC, 1, struct dsa_memcpy_args)
#define DSA_IOCTL_COPY_TO_USER _IOW(DSA_IOCTL_MAGIC, 2, void *)
#define DSA_IOCTL_CHECK_BUF _IO(DSA_IOCTL_MAGIC, 3)
#define DSA_IOCTL_MAX 4

enum dsa_memcpy_comp_status {
	DSA_MEMCPY_COMP_STATUS_INVALID = -1,
	DSA_MEMCPY_COMP_STATUS_PENDING = 0,
	DSA_MEMCPY_COMP_STATUS_DONE = 1,
};

struct dsa_memcpy_args {
	uint64_t src;
	uint64_t dst;
	uint64_t size;

	// This is the completion signature for userspace,
	// it will be set by kernel
	enum dsa_memcpy_comp_status comp_status;
};


#define RB_SIZE 64
struct dsa_memcpy_reqs {
	struct dsa_memcpy_args memcpys[RB_SIZE];
	uint32_t head;
	uint32_t tail;
};

// Multi producer enqueue
extern inline struct dsa_memcpy_args *
enqueue_nolock(struct dsa_memcpy_reqs *reqs, uint64_t src, uint64_t dst,
	       uint64_t size, uint64_t cr_addr)
{
	int current_head;
	int next_head;

	do {
		current_head = reqs->head;
		next_head = (current_head + 1) % RB_SIZE;

		if (next_head == reqs->tail) {
			return NULL;
		}
	} while (!__sync_bool_compare_and_swap(&reqs->head, current_head,
					       next_head));

	reqs->memcpys[current_head].src = src;
	reqs->memcpys[current_head].dst = dst;
	reqs->memcpys[current_head].size = size;
	reqs->memcpys[current_head].comp_status =
		DSA_MEMCPY_COMP_STATUS_PENDING;

	__sync_synchronize();

	return &reqs->memcpys[current_head];
}

// Single consumer dequeue
extern inline struct dsa_memcpy_args *
dequeue_nolock(struct dsa_memcpy_reqs *reqs)
{
	int current_tail;
	int next_tail;

	do {
		current_tail = reqs->tail;
		next_tail = (current_tail + 1) % RB_SIZE;

		if (current_tail == reqs->head) {
			return NULL;
		}
	} while (!__sync_bool_compare_and_swap(&reqs->tail, current_tail,
					       next_tail));

	__sync_synchronize();

	return &reqs->memcpys[current_tail % 64];
}

#endif
