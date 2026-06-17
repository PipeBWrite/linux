/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SC_MEMORY_H
#define _LINUX_SC_MEMORY_H

/*
 * StreamCache Two-Layer Memory Management
 *
 * Layer 1: System-level memory pool with per-CPU regions.
 *          Pages are pre-allocated at init time, eliminating buddy allocator
 *          overhead from the I/O critical path.
 *
 * Layer 2: Per-file caches that batch allocations from the pool.
 *          Near-zero allocation latency on the fast path (no lock, no
 *          splitting, no zeroing).
 *
 * Reference: "StreamCache: Revisiting Page Cache for File Scanning on Fast
 * Storage Devices" (USENIX ATC '24), Section 4.3.
 */

#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>
#include <linux/types.h>

struct page;

#define SC_FLAG_NORMAL	0
#define SC_FLAG_LOW	1
#define SC_FLAG_URGENT	2

#define SC_LOW_MARK_DEFAULT	50
#define SC_HIGH_MARK_DEFAULT	100

#define SC_THRESHOLD_LOW_DEFAULT	20	/* percent */
#define SC_THRESHOLD_URGENT_DEFAULT	5	/* percent */

struct sc_memory_region {
	struct list_head	free_list;
	spinlock_t		lock;
	unsigned long		nr_free;
	int			node;
} ____cacheline_aligned_in_smp;

struct sc_per_file_cache {
	struct list_head	free_list;
	unsigned int		nr_free;
	unsigned int		low_mark;
	unsigned int		high_mark;
};

struct sc_memory_pool {
	struct sc_memory_region	*regions;
	unsigned int		nr_regions;
	unsigned long		total_pages;
	atomic_t		memory_flag;
	unsigned int		threshold_low;
	unsigned int		threshold_urgent;
	bool			enabled;
};

/* Global pool instance */
extern struct sc_memory_pool sc_pool;

/* Pool lifecycle */
int sc_memory_pool_init(struct sc_memory_pool *pool, unsigned int nr_regions,
			unsigned long pages_per_region);
void sc_memory_pool_destroy(struct sc_memory_pool *pool);

/* Per-file cache lifecycle */
void sc_per_file_cache_init(struct sc_per_file_cache *cache,
			    unsigned int low_mark, unsigned int high_mark);
void sc_per_file_cache_destroy(struct sc_per_file_cache *cache,
			       struct sc_memory_pool *pool);

/* Hot-path allocation and free */
struct page *sc_alloc_page(struct sc_per_file_cache *cache,
			   struct sc_memory_pool *pool);
void sc_free_page(struct page *page, struct sc_per_file_cache *cache,
		  struct sc_memory_pool *pool);
void sc_release_page(struct page *page);

/* Batch operations */
int sc_refill_per_file_cache(struct sc_per_file_cache *cache,
			     struct sc_memory_pool *pool);
void sc_return_pages_to_pool(struct sc_per_file_cache *cache,
			     struct sc_memory_pool *pool,
			     unsigned int nr_pages);

/* Memory pressure monitoring */
void sc_update_memory_flag(struct sc_memory_pool *pool);
void sc_shrink_all_caches(struct sc_memory_pool *pool,
			  struct sc_per_file_cache **caches,
			  unsigned int nr_caches);

/* sysfs interface */
int sc_memory_init_sysfs(void);
void sc_memory_exit_sysfs(void);

#endif /* _LINUX_SC_MEMORY_H */
