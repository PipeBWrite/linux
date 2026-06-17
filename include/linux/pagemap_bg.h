/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PAGEMAP_BG_H
#define _LINUX_PAGEMAP_BG_H

/*
 * Copyright 1995 Linus Torvalds
 */
#include <linux/blkdev.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/list.h>
#include <linux/rculist.h>
#include <linux/highmem.h>
#include <linux/compiler.h>
#include <linux/prefetch.h>
#include <linux/uaccess.h>
#include <linux/gfp.h>
#include <linux/bitops.h>
#include <linux/hardirq.h> /* for in_interrupt() */
#include <linux/hugetlb_inline.h>
#include <linux/slab.h>
#include <linux/stats.h>
#include <linux/dsa_sl.h>

#include "pagemap.h"

/*
 * Decoupled ongoing-list node. Allocated independently from req entries so
 * req entries can return to pool immediately after hlist_del_rcu().
 * The node is reclaimed after a grace period.
 */
struct ongoing_node {
	struct hlist_node	hnode;		/* RCU hlist linkage */
	pgoff_t			index;		/* page cache index */
	struct page __rcu	*page;		/* folio ptr, RCU-published */
	u8			order;		/* folio order */
	bool			done;		/* BG sets true; FG cleans up */
	s16			target_wq_idx;	/* BG queue hint */
	u32			__pad2;
	struct rcu_head		rcu;		/* for call_rcu */
};

extern struct kmem_cache *ongoing_node_cache;

void ongoing_node_cache_init(void);

static inline bool ongoing_node_covers_index(struct ongoing_node *node,
					     pgoff_t index)
{
	unsigned int pages = 1U << node->order;

	return index >= node->index && index < node->index + pages;
}

static inline void ongoing_node_free_rcu(struct rcu_head *head)
{
	struct ongoing_node *node = container_of(head, struct ongoing_node, rcu);

	kmem_cache_free(ongoing_node_cache, node);
}

static inline void ongoing_node_delete_rcu(struct ongoing_node *node)
{
	hlist_del_rcu(&node->hnode);
	call_rcu(&node->rcu, ongoing_node_free_rcu);
}

int __filemap_get_folio_bg(struct inode *curr_inode,
			   struct address_space *mapping, pgoff_t start_index,
			   pgoff_t end_index, fgf_t fgp_flags, gfp_t gfp,
			   struct dsa_emu_req_entry *ent, loff_t pos,
			   bool large_folio_support);

size_t dsa_emu_copy_page_from_iter_atomic(struct page *page, size_t offset,
					  size_t bytes, struct iov_iter *i);
void filemap_bg_flush_inode_batches(struct inode *inode, bool draining);
void filemap_bg_batch_destroy(struct inode *inode);

#define FILEMAP_BG_BATCH_MAX_REQS DSA_EMU_FG_BATCH_MAX_ENTRIES

struct filemap_bg_inode_batch {
	spinlock_t		lock;
	bool			merge_inflight;
	bool			cached_target_valid;
	unsigned int		nr_reqs;
	unsigned int		nr_ents;
	unsigned int		cached_req_idx;
	unsigned int		cached_ent_idx;
	pgoff_t			max_end_index;
	size_t			total_bytes;
	int			target_wq_idx;
	struct file		*file;
	dsa_emu_write_entry_fn_t write_fn;
	struct dsa_emu_memcpy_req *reqs[FILEMAP_BG_BATCH_MAX_REQS];
} ____cacheline_aligned_in_smp;

extern uint32_t dsa_emu_ignored_inode_min;
extern uint32_t dsa_emu_ignored_inode_max;
static bool inline filemap_bg_compatible(struct inode *i)
{
	unsigned long ino;
	if (!i)
		return false;

	ino = i->i_ino;
	if (ino < 12)
		return false;

	if (ino <= dsa_emu_ignored_inode_max &&
	    ino >= dsa_emu_ignored_inode_min) {
		return false;
	}

	if (_bg_allowed_disk_major == 0 && _bg_allowed_disk_minor == 0) {
		goto check_inum;
	}

	if (i->i_sb && i->i_sb->s_bdev && i->i_sb->s_bdev->bd_dev) {
		if (MAJOR(i->i_sb->s_bdev->bd_dev) == _bg_allowed_disk_major &&
		    MINOR(i->i_sb->s_bdev->bd_dev) == _bg_allowed_disk_minor) {
			return true;
		}
		return false;
	}

check_inum:
	return (ino >= _bg_allowed_inode_min && ino <= _bg_allowed_inode_max);
}

static inline int get_ongoing_hash_index(pgoff_t index)
{
	unsigned int order = dsa_emu_folio_pool_order_max_read();

	if (order > MAX_PAGECACHE_ORDER)
		order = MAX_PAGECACHE_ORDER;

	return ((index >> order) % ONGOING_HASH_NUM);
}

/*
 * Search ongoing bucket for a node covering the given index.
 * Also stashes the first BG-completed (done) node in @reuse for
 * in-place reuse by the caller, avoiding kmem_cache_alloc + call_rcu.
 * Extra done nodes beyond the first are freed normally.
 * Caller must have exclusive access (i_rwsem exclusive).
 */
static inline struct ongoing_node *
search_in_ongoing_buckets_locked(pgoff_t index,
				 struct ongoing_list_bucket *bucket,
				 struct ongoing_node **reuse)
{
	struct ongoing_node *node;
	struct hlist_node *tmp;

	*reuse = NULL;
	hlist_for_each_entry_safe(node, tmp, &bucket->list, hnode) {
		if (READ_ONCE(node->done)) {
			if (!*reuse) {
				*reuse = node;
			} else {
				ongoing_node_delete_rcu(node);
			}
			continue;
		}
		if (ongoing_node_covers_index(node, index)) {
			return node;
		}
	}
	return NULL;
}

/*
 * Search ongoing list and get folio reference if found.
 * Returns folio with elevated refcount, or NULL.
 * RCU lockless — no spinlock required.
 */
static inline struct folio *
search_ongoing_and_get_folio(pgoff_t index, struct ongoing_list_bucket *buckets)
{
	int ongoing_index = get_ongoing_hash_index(index);
	struct ongoing_list_bucket *bucket = &buckets[ongoing_index];
	struct ongoing_node *node;
	struct page *page;
	struct folio *folio = NULL;

	rcu_read_lock();
	hlist_for_each_entry_rcu(node, &bucket->list, hnode) {
		if (READ_ONCE(node->done))
			continue;
		if (ongoing_node_covers_index(node, index)) {
			page = rcu_dereference(node->page);
			if (page) {
				folio = page_folio(page);
				if (folio_try_get_rcu(folio))
					goto found;
				folio = NULL;
			}
		}
	}
found:
	rcu_read_unlock();
	return folio;
}

/*
 * Check if an entry EXISTS in o-list at given index.
 * RCU lockless. Returns:
 *   0: no entry found
 *   1: entry found with page=NULL (write pending, folio not yet allocated)
 *   2: entry found with page set
 */
static inline int
search_ongoing_entry_state(pgoff_t index, struct ongoing_list_bucket *buckets)
{
	int ongoing_index = get_ongoing_hash_index(index);
	struct ongoing_list_bucket *bucket = &buckets[ongoing_index];
	struct ongoing_node *node;
	int state = 0;

	rcu_read_lock();
	hlist_for_each_entry_rcu(node, &bucket->list, hnode) {
		if (READ_ONCE(node->done))
			continue;
		if (ongoing_node_covers_index(node, index)) {
			state = rcu_dereference(node->page) ? 2 : 1;
			break;
		}
	}
	rcu_read_unlock();
	return state;
}
#endif
