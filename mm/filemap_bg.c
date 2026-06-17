// SPDX-License-Identifier: GPL-2.0-only
/*
 *	linux/mm/filemap_bg.c
 *
 * Copyright (C) 1994-1999  Linus Torvalds
 */

/*
 * This file handles the generic file mmap semantics used by
 * most "normal" filesystems (but you don't /have/ to use this:
 * the NFS filesystem used to do this differently, for example)
 *
 * This handles the background version if page table writes.
 */

#include "linux/mm_types.h"
#include <linux/page-flags.h>
#include <linux/spinlock.h>
#include <linux/export.h>
#include <linux/compiler.h>
#include <linux/dax.h>
#include <linux/fs.h>
#include <linux/sched/signal.h>
#include <linux/uaccess.h>
#include <linux/capability.h>
#include <linux/kernel_stat.h>
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/syscalls.h>
#include <linux/mman.h>
#include <linux/pagemap.h>
#include <linux/file.h>
#include <linux/uio.h>
#include <linux/error-injection.h>
#include <linux/writeback.h>
#include <linux/backing-dev.h>
#include <linux/pagevec.h>
#include <linux/security.h>
#include <linux/cpuset.h>
#include <linux/hugetlb.h>
#include <linux/memcontrol.h>
#include <linux/shmem_fs.h>
#include <linux/rmap.h>
#include <linux/delayacct.h>
#include <linux/psi.h>
#include <linux/ramfs.h>
#include <linux/page_idle.h>
#include <linux/migrate.h>
#include <linux/pipe_fs_i.h>
#include <linux/splice.h>
#include <linux/stats.h>
#include <linux/highmem.h>
#include <asm/pgalloc.h>
#include <asm/tlbflush.h>
#include <linux/pagemap_bg.h>
#include <linux/dsa_sl.h>
#include <linux/dsa_emu.h>
#include <linux/fbg.h>
#include <linux/prefetch.h>
#include <linux/log2.h>
#include <linux/mutex.h>

/*
 * FIXME: remove all knowledge of the buffer layer from the core VM
 */
#include <linux/buffer_head.h> /* for try_to_free_buffers */

#include <asm/mman.h>

struct kmem_cache *ongoing_node_cache;
EXPORT_SYMBOL(ongoing_node_cache);

void ongoing_node_cache_init(void)
{
	if (ongoing_node_cache)
		return;
	ongoing_node_cache = kmem_cache_create("ongoing_node",
		sizeof(struct ongoing_node), 0,
		SLAB_HWCACHE_ALIGN | SLAB_PANIC, NULL);
}
EXPORT_SYMBOL(ongoing_node_cache_init);

#ifdef CONFIG_MISC_STATS
#define FM_BG_STAT_INC(name) this_cpu_inc(stats_##name##_count)
#define FM_BG_STAT_ADD(name, count) this_cpu_add(stats_##name##_count, count)
#else
#define FM_BG_STAT_INC(name) \
	do {                 \
	} while (0)
#define FM_BG_STAT_ADD(name, count) \
	do {                       \
	} while (0)
#endif

static inline void prepare_dsa_emu_req_entry(struct dsa_emu_req_entry *req,
					     struct page *page,
					     unsigned long bytes, loff_t pos,
					     size_t copied, void *fsdata)
{
	req->page = page;
	req->bytes = bytes;
	req->pos = pos;
	req->copied = copied;
	req->fsdata = fsdata;
}

static inline size_t dsa_emu_entry_bytes(struct dsa_emu_req_entry *ent,
					 struct folio *folio, loff_t pos,
					 size_t remaining)
{
	size_t offset = offset_in_folio(folio, pos);
	loff_t entry_start = (loff_t)ent->index << PAGE_SHIFT;
	size_t entry_size = (size_t)(1U << ent->order) << PAGE_SHIFT;
	size_t entry_avail = entry_size;
	size_t bytes;

	if (pos > entry_start)
		entry_avail -= (size_t)(pos - entry_start);
	bytes = min_t(size_t, entry_avail, remaining);
	bytes = min_t(size_t, bytes, folio_size(folio) - offset);

	return bytes;
}

static inline bool dsa_emu_entry_unsafe(struct inode *inode,
					struct folio *folio, loff_t pos,
					size_t bytes)
{
	unsigned int blocksize;

	if (folio_test_uptodate(folio))
		return false;
	if (!bytes)
		return false;

	blocksize = 1U << inode->i_blkbits;
	if (!blocksize)
		return false;

	return ((pos | bytes) & (blocksize - 1)) != 0;
}

static inline bool dsa_emu_use_nt_copy(struct dsa_emu_req_entry *ent,
				       struct folio *folio, size_t offset,
				       size_t bytes)
{
	if (!IS_ENABLED(CONFIG_X86_64))
		return false;
	if (!READ_ONCE(dsa_emu_nt_store))
		return false;
	if (!ent->initial_alloc)
		return false;
	if (offset)
		return false;
	if (bytes != folio_size(folio))
		return false;
	if (bytes < READ_ONCE(dsa_emu_nt_store_min_bytes))
		return false;

	return true;
}

static size_t dsa_emu_copy_folio_from_iter_atomic(struct folio *folio,
						  size_t offset, size_t bytes,
						  struct iov_iter *i)
{
	size_t copied = 0;

	while (copied < bytes) {
		size_t page_off = offset_in_page(offset);
		size_t part =
			min_t(size_t, bytes - copied, PAGE_SIZE - page_off);
		struct page *page = folio_page(folio, offset >> PAGE_SHIFT);
		size_t n = dsa_emu_copy_page_from_iter_atomic(page, page_off,
							      part, i);

		flush_dcache_page(page);
		copied += n;
		offset += n;
		if (n != part)
			break;
	}

	return copied;
}

static struct filemap_bg_inode_batch *
filemap_bg_batch_get_or_alloc(struct inode *inode)
{
	struct filemap_bg_inode_batch *batch = READ_ONCE(inode->i_fg_batch);

	if (likely(batch))
		return batch;

	batch = kzalloc(sizeof(*batch), GFP_KERNEL);
	if (!batch)
		return NULL;
	spin_lock_init(&batch->lock);

	if (cmpxchg(&inode->i_fg_batch, NULL, batch) != NULL) {
		kfree(batch);
		batch = READ_ONCE(inode->i_fg_batch);
	}
	return batch;
}

void filemap_bg_batch_destroy(struct inode *inode)
{
	struct filemap_bg_inode_batch *batch = inode->i_fg_batch;

	if (!batch)
		return;
	WARN_ON_ONCE(batch->nr_reqs);
	kfree(batch);
	inode->i_fg_batch = NULL;
}
EXPORT_SYMBOL(filemap_bg_batch_destroy);

static inline void
filemap_bg_wait_merge_done(struct filemap_bg_inode_batch *batch)
{
	while (smp_load_acquire(&batch->merge_inflight))
		cpu_relax();
}

static inline bool
filemap_bg_batch_ctx_match(struct filemap_bg_inode_batch *batch,
			   struct file *file, dsa_emu_write_entry_fn_t write_fn)
{
	return batch->nr_reqs && batch->file == file &&
	       batch->write_fn == write_fn;
}

static inline void
filemap_bg_batch_set_ctx_locked(struct filemap_bg_inode_batch *batch,
				struct file *file,
				dsa_emu_write_entry_fn_t write_fn)
{
	batch->file = file;
	batch->write_fn = write_fn;
}

static inline void
filemap_bg_batch_set_target_wq_locked(struct filemap_bg_inode_batch *batch,
				      int target_wq_idx)
{
	batch->target_wq_idx = target_wq_idx;
}

static inline void
filemap_bg_batch_set_cached_target_locked(struct filemap_bg_inode_batch *batch,
					  unsigned int req_idx,
					  unsigned int ent_idx)
{
	batch->cached_target_valid = true;
	batch->cached_req_idx = req_idx;
	batch->cached_ent_idx = ent_idx;
}

static inline void filemap_bg_batch_clear_cached_target_locked(
	struct filemap_bg_inode_batch *batch)
{
	batch->cached_target_valid = false;
	batch->cached_req_idx = 0;
	batch->cached_ent_idx = 0;
}

static inline bool filemap_bg_batch_refresh_latest_target_locked(
	struct filemap_bg_inode_batch *batch)
{
	for (unsigned int r = batch->nr_reqs; r-- > 0;) {
		struct dsa_emu_memcpy_req *req = batch->reqs[r];

		if (!req || req->n_ents <= 0)
			continue;
		filemap_bg_batch_set_cached_target_locked(batch, r,
							  req->n_ents - 1);
		return true;
	}

	filemap_bg_batch_clear_cached_target_locked(batch);
	return false;
}

static inline void filemap_bg_batch_reset(struct filemap_bg_inode_batch *batch)
{
	batch->nr_reqs = 0;
	batch->nr_ents = 0;
	batch->total_bytes = 0;
	batch->max_end_index = 0;
	batch->target_wq_idx = -1;
	batch->file = NULL;
	batch->write_fn = NULL;
	filemap_bg_batch_clear_cached_target_locked(batch);
}

static inline size_t filemap_bg_req_total_bytes(struct dsa_emu_memcpy_req *req)
{
	size_t total = 0;

	for (int i = 0; i < req->n_ents; i++)
		total += req->entries[i].copied;
	return total;
}

static void filemap_bg_submit_req(struct dsa_emu_memcpy_req *req, bool draining)
{
	struct inode *inode = req->mapping->host;

	if (draining)
		dsa_emu_submit_in_drain(req, inode);
	else
		dsa_emu_submit(req, inode);
}

static void filemap_bg_submit_reqs(struct dsa_emu_memcpy_req **reqs,
				   unsigned int nr, bool draining)
{
	struct inode *inode;

	if (!nr)
		return;
	if (nr == 1) {
		filemap_bg_submit_req(reqs[0], draining);
		return;
	}

	inode = reqs[0]->mapping->host;

	if (draining)
		dsa_emu_submit_batch_in_drain(reqs, nr, inode);
	else
		dsa_emu_submit_batch(reqs, nr, inode);
}

static unsigned int
filemap_bg_detach_batch_locked(struct filemap_bg_inode_batch *batch,
			       struct dsa_emu_memcpy_req **submit_reqs)
{
	unsigned int out = 0;

	if (!batch->nr_reqs)
		return 0;

	for (unsigned int i = 0; i < batch->nr_reqs; i++)
		submit_reqs[out++] = batch->reqs[i];
	filemap_bg_batch_reset(batch);
	return out;
}

void filemap_bg_flush_inode_batches(struct inode *inode, bool draining)
{
	struct filemap_bg_inode_batch *batch;
	struct dsa_emu_memcpy_req *submit_reqs[FILEMAP_BG_BATCH_MAX_REQS];
	unsigned int nr = 0;

	if (!inode)
		return;
	batch = READ_ONCE(inode->i_fg_batch);
	if (!batch || !READ_ONCE(batch->nr_reqs))
		return;

retry:
	spin_lock(&batch->lock);
	if (unlikely(batch->merge_inflight)) {
		spin_unlock(&batch->lock);
		filemap_bg_wait_merge_done(batch);
		goto retry;
	}
	nr = filemap_bg_detach_batch_locked(batch, submit_reqs);
	spin_unlock(&batch->lock);

	filemap_bg_submit_reqs(submit_reqs, nr, draining);
}
EXPORT_SYMBOL(filemap_bg_flush_inode_batches);

static void filemap_bg_queue_or_submit_req(struct dsa_emu_memcpy_req *req,
					   struct inode *inode,
					   struct file *file,
					   dsa_emu_write_entry_fn_t write_fn)
{
	/*
	 * We may detach twice in one pass:
	 * 1) flush existing batch before enqueuing @req (pre_reqs)
	 * 2) flush again immediately after enqueueing @req (post_reqs)
	 * These may have different target_wq_idx, so submit separately.
	 */
	struct dsa_emu_memcpy_req *pre_reqs[FILEMAP_BG_BATCH_MAX_REQS];
	struct dsa_emu_memcpy_req *post_reqs[FILEMAP_BG_BATCH_MAX_REQS];
	unsigned int pre_nr = 0, post_nr = 0;
	struct filemap_bg_inode_batch *batch;
	u64 writes_since_fsync;
	u32 req_writes = req->n_ents > 0 ? req->n_ents : 1;

	writes_since_fsync = READ_ONCE(file->f_bg_writes_since_fsync);
	if (writes_since_fsync < U64_MAX - req_writes)
		writes_since_fsync += req_writes;
	else
		writes_since_fsync = U64_MAX;
	WRITE_ONCE(file->f_bg_writes_since_fsync, writes_since_fsync);
	FM_BG_STAT_INC(bg_write_total_req);
	FM_BG_STAT_ADD(bg_write_total_ent, req_writes);

	if (unlikely(atomic_read(&inode->i_draining))) {
		FM_BG_STAT_INC(bg_write_draining_async_req);
		FM_BG_STAT_ADD(bg_write_draining_async_ent, req_writes);
		FM_BG_STAT_INC(bg_write_async_req);
		FM_BG_STAT_ADD(bg_write_async_ent, req_writes);
		filemap_bg_submit_req(req, false);
		return;
	}

	if (unlikely(READ_ONCE(file->f_bg_fsync_fallback))) {
		FM_BG_STAT_INC(bg_write_fsync_fb_sync_req);
		FM_BG_STAT_ADD(bg_write_fsync_fb_sync_ent, req_writes);
		dsa_emu_process_req_sync(req, inode);
		return;
	}

	if (unlikely(READ_ONCE(dsa_emu_disable_batching))) {
		FM_BG_STAT_INC(bg_write_async_req);
		FM_BG_STAT_ADD(bg_write_async_ent, req_writes);
		filemap_bg_flush_inode_batches(inode, false);
		filemap_bg_submit_req(req, false);
		return;
	}

	/*
	 * Sync fallback: when BG kthreads are overloaded, process
	 * the request synchronously in the FG thread to relieve
	 * backpressure.  The FG memcpy and o-list work are already
	 * done; we just run the metadata (write_begin/write_end),
	 * o-list mark-done, and folio pool replenishment inline.
	 *
	 * Use the cached i_bg_ongoing_snapshot instead of a fresh
	 * atomic_read to avoid an extra cache-line bounce on the
	 * hot path.  The snapshot is refreshed every few allocations
	 * in prepare_pages, which is close enough for a threshold.
	 *
	 * i_bg_pending is bumped so that dsa_emu_process_req_entries
	 * can decrement it normally and signal drain waiters.
	 */
	{
		uint32_t threshold = READ_ONCE(dsa_emu_sync_fallback_threshold);

		if (unlikely(threshold &&
			     inode->i_bg_ongoing_snapshot >=
				     (int)threshold)) {
			FM_BG_STAT_INC(bg_write_threshold_sync_req);
			FM_BG_STAT_ADD(bg_write_threshold_sync_ent, req_writes);
			dsa_emu_process_req_sync(req, inode);
			return;
		}
	}

	FM_BG_STAT_INC(bg_write_async_req);
	FM_BG_STAT_ADD(bg_write_async_ent, req_writes);

	batch = filemap_bg_batch_get_or_alloc(inode);
	if (!batch) {
		filemap_bg_submit_req(req, false);
		return;
	}
retry:
	spin_lock(&batch->lock);
	if (unlikely(batch->merge_inflight)) {
		spin_unlock(&batch->lock);
		filemap_bg_wait_merge_done(batch);
		goto retry;
	}

	if (batch->nr_reqs &&
	    !filemap_bg_batch_ctx_match(batch, file, write_fn))
		pre_nr = filemap_bg_detach_batch_locked(batch, pre_reqs);

	if (batch->nr_reqs == 0) {
		filemap_bg_batch_set_ctx_locked(batch, file, write_fn);
		filemap_bg_batch_set_target_wq_locked(batch, req->target_wq_idx);
	}

	if (batch->nr_reqs &&
	    batch->target_wq_idx != req->target_wq_idx) {
		pre_nr += filemap_bg_detach_batch_locked(batch,
							 pre_reqs + pre_nr);
		filemap_bg_batch_set_ctx_locked(batch, file, write_fn);
		filemap_bg_batch_set_target_wq_locked(batch,
						      req->target_wq_idx);
	}

	if (batch->nr_reqs >= FILEMAP_BG_BATCH_MAX_REQS ||
	    batch->nr_ents + req->n_ents > DSA_EMU_FG_BATCH_MAX_ENTRIES) {
		pre_nr += filemap_bg_detach_batch_locked(batch,
							 pre_reqs + pre_nr);
		filemap_bg_batch_set_ctx_locked(batch, file, write_fn);
		filemap_bg_batch_set_target_wq_locked(batch, req->target_wq_idx);
	}

	batch->reqs[batch->nr_reqs++] = req;
	batch->nr_ents += req->n_ents;
	batch->total_bytes += filemap_bg_req_total_bytes(req);
	if (req->end_index > batch->max_end_index)
		batch->max_end_index = req->end_index;
	if (req->n_ents)
		filemap_bg_batch_set_cached_target_locked(
			batch, batch->nr_reqs - 1, req->n_ents - 1);

	if (batch->nr_ents >= DSA_EMU_FG_BATCH_MAX_ENTRIES ||
	    batch->total_bytes >= DSA_EMU_FG_BATCH_MAX_BYTES)
		post_nr = filemap_bg_detach_batch_locked(batch, post_reqs);

	spin_unlock(&batch->lock);

	filemap_bg_submit_reqs(pre_reqs, pre_nr, false);
	filemap_bg_submit_reqs(post_reqs, post_nr, false);
}

static noinline ssize_t
filemap_bg_try_merge_small_write(struct kiocb *iocb, struct iov_iter *i,
				 dsa_emu_write_entry_fn_t write_fn)
{
	struct file *file = iocb->ki_filp;
	loff_t pos = iocb->ki_pos;
	struct address_space *mapping = file->f_mapping;
	struct inode *inode = mapping->host;
	struct filemap_bg_inode_batch *batch;
	struct dsa_emu_req_entry *target = NULL;
	struct dsa_emu_memcpy_req *submit_reqs[FILEMAP_BG_BATCH_MAX_REQS];
	unsigned int nr_submit = 0;
	unsigned int target_req_idx = 0;
	unsigned int target_ent_idx = 0;
	ssize_t ret = 0;
	size_t bytes = iov_iter_count(i);
	size_t copied = 0;
	pgoff_t pos_index = pos >> PAGE_SHIFT;
	bool merged_hit = false;
	bool fg_locked = false;
	bool saw_contig_miss = false;
	struct folio *folio = NULL;
	size_t offset = 0;

	if (!bytes || bytes > DSA_EMU_FG_BATCH_SMALL_WRITE_MAX)
		return 0;
	if (unlikely(atomic_read(&inode->i_draining)))
		return 0;
	if (unlikely(READ_ONCE(dsa_emu_disable_batching)))
		return 0;

	batch = READ_ONCE(inode->i_fg_batch);
	if (!batch)
		return 0;

	/* Lockless pre-check: skip mutex if batch has no queued requests */
	if (!READ_ONCE(batch->nr_reqs))
		return 0;

	spin_lock(&batch->lock);
	if (unlikely(batch->merge_inflight))
		goto out_unlock;
	if (!batch->nr_reqs) {
		goto out_unlock;
	}
	if (batch->file != file || batch->write_fn != write_fn) {
		goto out_unlock;
	}

	/*
	 * Quick range check: if pos is at or beyond the highest page index
	 * covered by any batched request, no entry can contain this write.
	 */
	if (pos_index >= batch->max_end_index) {
		goto out_unlock;
	}

	if (!batch->cached_target_valid)
		filemap_bg_batch_refresh_latest_target_locked(batch);

	if (batch->cached_target_valid &&
	    batch->cached_req_idx < batch->nr_reqs) {
		struct dsa_emu_memcpy_req *req =
			batch->reqs[batch->cached_req_idx];

		if (req && batch->cached_ent_idx < req->n_ents) {
			struct dsa_emu_req_entry *ent =
				&req->entries[batch->cached_ent_idx];
			loff_t entry_start, entry_end, old_end;

			if (ent->page &&
			    dsa_emu_entry_covers_index(ent, pos_index)) {
				entry_start = (loff_t)ent->index << PAGE_SHIFT;
				entry_end =
					entry_start + (((loff_t)1 << ent->order)
						       << PAGE_SHIFT);
				old_end = ent->pos + ent->copied;
				if (old_end == pos && pos >= entry_start &&
				    pos + (loff_t)bytes <= entry_end) {
					target = ent;
					target_req_idx = batch->cached_req_idx;
					target_ent_idx = batch->cached_ent_idx;
				} else if (old_end != pos) {
					saw_contig_miss = true;
				}
			}
		}
	}

	if (!target) {
		goto out_unlock;
	}
	filemap_bg_batch_set_cached_target_locked(batch, target_req_idx,
						  target_ent_idx);

	folio = page_folio(target->page);
	offset = offset_in_folio(folio, pos);
	batch->merge_inflight = true;
	spin_unlock(&batch->lock);

	if (unlikely(fault_in_iov_iter_readable(i, bytes) == bytes)) {
		ret = -EFAULT;
		goto out_finish;
	}
	if (fatal_signal_pending(current)) {
		ret = -EINTR;
		goto out_finish;
	}

	if (!target->initial_alloc) {
		folio_lock(folio);
		fg_locked = true;
	}

	if (dsa_emu_prefetch)
		dsa_emu_prefetch_folio_range(folio, offset, bytes);

	copied = dsa_emu_copy_folio_from_iter_atomic(folio, offset, bytes, i);
	while (copied != bytes) {
		if (copied)
			iov_iter_revert(i, copied);
		if (fg_locked) {
			folio_unlock(folio);
			fg_locked = false;
		}
		if (unlikely(fault_in_iov_iter_readable(i, bytes) == bytes)) {
			ret = -EFAULT;
			goto out_finish;
		}
		if (fatal_signal_pending(current)) {
			ret = -EINTR;
			goto out_finish;
		}
		if (!target->initial_alloc) {
			folio_lock(folio);
			fg_locked = true;
		}
		copied = dsa_emu_copy_folio_from_iter_atomic(folio, offset,
							     bytes, i);
	}

	ret = copied;

out_finish:
	if (fg_locked)
		folio_unlock(folio);

	/*
	 * Commit without the batch lock.  merge_inflight is true here,
	 * which blocks the drain/flush path from touching the batch.
	 * i_rwsem serialises all writers, so no concurrent queue/merge.
	 * smp_store_release below pairs with smp_load_acquire in the
	 * drain poll to publish the entry/batch field updates.
	 */
	if (ret > 0) {
		merged_hit = true;
		target->flags &= ~DSA_EMU_REQ_F_FG_BWB;
		target->bytes += copied;
		target->copied += copied;
		batch->total_bytes += copied;
		i_size_write(inode, max(i_size_read(inode),
					pos + (loff_t)copied));

		if (batch->nr_ents >= DSA_EMU_FG_BATCH_MAX_ENTRIES ||
		    batch->total_bytes >= DSA_EMU_FG_BATCH_MAX_BYTES)
			nr_submit = filemap_bg_detach_batch_locked(batch,
								   submit_reqs);
	}
	smp_store_release(&batch->merge_inflight, false);

	filemap_bg_submit_reqs(submit_reqs, nr_submit, false);

	return ret;

out_unlock:
	spin_unlock(&batch->lock);

	filemap_bg_submit_reqs(submit_reqs, nr_submit, false);

	return ret;
}

// Prepare entries based on the start and end index, the actual number of
// entries prepared depend on the ongoing list
// All of the pages will have its reference count increased
static int prepare_pages(struct inode *inode,
				  struct address_space *mapping,
				  unsigned long start_idx,
				  unsigned long end_idx,
				  struct dsa_emu_req_entry *ents, loff_t pos,
				  bool large_folio_support)
{
	unsigned long count = end_idx - start_idx;

	if (unlikely(count > 4096)) {
		return -EFBIG;
	}
	if (unlikely(!ents)) {
		pr_warn("No req entries array\n");
		return -ENOMEM;
	}

	return __filemap_get_folio_bg(inode, mapping, start_idx, end_idx,
				      FGP_WRITEBEGIN, mapping_gfp_mask(mapping),
				      ents, pos, large_folio_support);
}

ssize_t generic_perform_write_bg_cb(struct kiocb *iocb, struct iov_iter *i,
				    dsa_emu_write_entry_fn_t write_fn,
				    bool large_folio_support)
{
	struct file *file = iocb->ki_filp;
	loff_t pos = iocb->ki_pos;
	struct address_space *mapping = file->f_mapping;
	const struct address_space_operations *a_ops = mapping->a_ops;
	struct inode *inode = mapping->host;
	long status = 0;
	ssize_t written = 0;
	int total_count = iov_iter_count(i);
	bool can_merge;

	TEST_BEGIN_BY_INODE_ALWAYS(inode);
	TIME_BEGIN_ALWAYS(perform_write);

	loff_t end = pos + total_count;
	loff_t start_page_boundary = round_down(pos, PAGE_SIZE);
	loff_t end_page_boundary = round_up(end, PAGE_SIZE);
	unsigned long start_idx = start_page_boundary >> PAGE_SHIFT;
	unsigned long end_idx = end_page_boundary >> PAGE_SHIFT;
	unsigned long max_pages = DSA_EMU_SUBMIT_REQ_ENTRIES;

	int idx = 0;

	if (!iov_iter_count(i))
		goto out;

	can_merge = total_count <= DSA_EMU_FG_BATCH_SMALL_WRITE_MAX &&
		    !atomic_read(&inode->i_draining);

	if (can_merge) {
		ssize_t merged;

		merged = filemap_bg_try_merge_small_write(iocb, i, write_fn);

		if (merged < 0) {
			status = merged;
			goto out;
		}
		if (merged > 0) {
			written = merged;
			goto out;
		}
	}

	do {
		ssize_t curr_written = 0;
		unsigned long end_index = min(start_idx + max_pages, end_idx);
		size_t total_bytes = 0;
		while (start_idx < end_index) {
				int nent;
			struct dsa_emu_req_entry *ent_head;
			struct dsa_emu_memcpy_req *req;

			/*
			 * Keep default CPU-local submission for NUMA
			 * locality. We may override this with an o-list
			 * queue hint after prepare_pages().
			 */
			int target_wq_idx = dsa_emu_select_wq(-1);

			req = dsa_emu_get_req_from_pool(target_wq_idx);
			if (!req) {
				status = -ENOMEM;
				break;
			}
			nent = prepare_pages(inode, mapping, start_idx,
					     end_idx, req->entries, pos,
					     large_folio_support);
			if (nent <= 0) {
				pr_warn("prepare_pages returned no entries\n");
				status = nent < 0 ? nent : 0;
				dsa_emu_put_req(req);
				break;
			}
			ent_head = req->entries;
			for (int j = 0; j < nent; j++) {
				if (ent_head[j].target_wq_idx >= 0) {
					target_wq_idx = dsa_emu_select_wq(
						ent_head[j].target_wq_idx);
					break;
				}
			}
			for (int j = 0; j < nent; j++)
				ent_head[j].target_wq_idx = target_wq_idx;

			unsigned long planned_pages = 0;
			unsigned long run_first_index = ent_head[0].index;
			for (idx = 0; idx < nent; idx++)
				planned_pages += 1U << ent_head[idx].order;
			start_idx = run_first_index + planned_pages;

			{
				uint32_t bgf = READ_ONCE(dsa_emu_debug_bg_fsync);

				/* 1 = always (manual override, ungated);
				 * 2 = adaptive (per-task flag set by the
				 * fsync-path state machine in
				 * vfs_fsync_range), gated on the fs
				 * declaring its write path safe to overlap
				 * with paced writeback (bg_pace_safe;
				 * ext4 yes, xfs no — ILOCK_EXCL convoy). */
				req->fsync = (bgf == 1) ||
					     (bgf == 2 &&
					      current->bg_fsync_on &&
					      mapping->a_ops->bg_pace_safe);
			}
			req->mapping = mapping;
			/*
			 * Hold a file reference until request teardown so
			 * close(2) can drop the fd without forcing a
			 * synchronous inode drain.
			 */
			req->file = get_file(file);
			req->start_index = ent_head[0].index;
			req->target_wq_idx = target_wq_idx;

			unsigned long processed_pages = 0;
			for (idx = 0; idx < nent; idx++) {
				struct dsa_emu_req_entry *ent = &ent_head[idx];
				struct folio *folio = NULL;
				size_t offset;
				size_t bytes;
				size_t copied;
				void *fsdata = NULL;
				bool fg_locked = false;
				bool nt_issued = false;

				if (!ent->page) {
					pr_err("missing folio in ent\n");
					BUG();
				}
				folio = page_folio(ent->page);
				ent->flags = 0;
				offset = offset_in_folio(folio, pos);
				bytes = dsa_emu_entry_bytes(ent, folio, pos,
							    iov_iter_count(i));
				if (ent->initial_alloc) {
					/*
					 * Always set mapping and index for newly
					 * allocated folios. We cannot rely on
					 * folio->mapping being NULL because folios
					 * from the pool may have stale data
					 * (replenishment may skip zeroing).
					 * A stale mapping matching ours could cause
					 * us to skip setting index, leading to
					 * EEXIST in BG.
					 */
					folio->mapping = mapping;
					folio->index = ent->index;
				}

				if (unlikely(fault_in_iov_iter_readable(
						     i, bytes) == bytes)) {
					status = -EFAULT;
					break;
				}

				if (fatal_signal_pending(current)) {
					status = -EINTR;
					break;
				}

				fsdata = 0;

				if (dsa_emu_prefetch)
					dsa_emu_prefetch_folio_range(
						folio, offset, bytes);

				if (unlikely(status < 0)) {
					pr_err("status < 0\n");
					break;
				}

				bool unsafe = dsa_emu_entry_unsafe(inode, folio,
								   pos, bytes);
				/*
				 * fg_bwb_all: also run blk_write_begin here for
				 * safe entries so da reservations stay in file
				 * order (prevents es-tree fragmentation that
				 * makes writeback/fsync pay one handle +
				 * map_blocks per fragment).  FG begin always
				 * precedes the memcpy, so the BG-defer
				 * corruption mode does not apply.
				 */
				if ((unsafe || READ_ONCE(dsa_emu_fg_bwb_all)) &&
				    !READ_ONCE(
					    dsa_emu_debug_defer_blk_write_begin) &&
				    a_ops->blk_write_begin) {
					if (ent->initial_alloc) {
						/*
						 * New folio is private - trylock always
						 * succeeds, skip might_sleep() overhead.
						 */
						bool locked =
							folio_trylock(folio);
						BUG_ON(!locked);
					} else {
						folio_lock(folio);
					}
					if (unlikely(!folio->mapping ||
						     folio->mapping !=
							     mapping)) {
						pr_err_ratelimited(
							"filemap_bg: blk_write_begin folio %px idx %lu mapping %px expected %px initial_alloc=%d pos=%lld bytes=%zu\n",
							folio, ent->index,
							folio->mapping, mapping,
							ent->initial_alloc, pos,
							bytes);
					}
					status = a_ops->blk_write_begin(
						folio, pos, bytes);
					if (ent->initial_alloc)
						folio_unlock(folio);
					else
						fg_locked = true;
					if (status) {
						if (fg_locked) {
							folio_unlock(folio);
							fg_locked = false;
						}
						break;
					}
					ent->flags |= DSA_EMU_REQ_F_FG_BWB;
				} else if (unsafe && !a_ops->blk_write_begin &&
					   !write_fn) {
					status = -EOPNOTSUPP;
					break;
				}

				if (!ent->initial_alloc && !fg_locked) {
					folio_lock(folio);
					fg_locked = true;
				}

				copied = dsa_emu_copy_folio_from_iter_atomic(
					folio, offset, bytes,
					i);

				while (copied != bytes) {
					if (copied)
						iov_iter_revert(i,
								copied);
					if (fg_locked) {
						folio_unlock(folio);
						fg_locked = false;
					}
					if (unlikely(
						    fault_in_iov_iter_readable(
							    i, bytes) ==
						    bytes)) {
						status = -EFAULT;
						break;
					}

					if (fatal_signal_pending(
						    current)) {
						status = -EINTR;
						break;
					}
					if (!ent->initial_alloc) {
						folio_lock(folio);
						fg_locked = true;
					}
					copied = dsa_emu_copy_folio_from_iter_atomic(
						folio, offset,
						bytes, i);
				}
				if (nt_issued)
					dsa_emu_nt_sfence();

				if (unlikely(copied != bytes)) {
					pr_err("copied != bytes\n");
					bytes = copied;
				}

				if (fg_locked) {
					i_size_write(inode,
						     max(i_size_read(inode),
							 pos + (loff_t)copied));
					folio_unlock(folio);
					fg_locked = false;
				}

				prepare_dsa_emu_req_entry(ent, &folio->page,
							  bytes, pos, copied,
							  fsdata);

				pos += copied;
				total_bytes += copied;
				curr_written += copied;
				processed_pages += 1U << ent->order;
			}
			if (status != 0) {
				/*
				 * Error mid-loop: entries 0..idx-1 are prepared,
				 * entries idx..nent-1 need cleanup.
				 */
				pr_warn("Error %ld during copy, cleaning up\n",
					status);
				for (int j = idx; j < nent; j++) {
					struct dsa_emu_req_entry *e =
						&ent_head[j];
					struct folio *f =
						e->page ? page_folio(e->page) :
							  NULL;

					/* Remove from ongoing list if linked */
					if (e->onode) {
						ongoing_node_delete_rcu(e->onode);
						e->onode = NULL;
					}

					/* Release folio reference */
					if (f)
						folio_put(f);
				}

				if (idx > 0) {
					/* Submit prepared entries */
					req->n_ents = idx;
					req->end_index = req->start_index +
							 processed_pages;
					req->write_fn = write_fn;
					i_size_write(inode,
						     max(i_size_read(inode),
							 pos));
					{
						filemap_bg_queue_or_submit_req(
							req, inode, file,
							write_fn);
					}
				} else {
					/* No entries prepared, return req */
					dsa_emu_put_req(req);
				}
				break;
			}

			TIME_BEGIN_ALWAYS(fg_submit);
			req->n_ents = nent;
			req->end_index = req->start_index + processed_pages;
			req->write_fn = write_fn;
			i_size_write(inode, max(i_size_read(inode), pos));
			{
				filemap_bg_queue_or_submit_req(req, inode, file,
							       write_fn);
			}
			total_bytes = 0;
			TIME_END_ALWAYS(fg_submit);
		}

		if (status != 0)
			break;

		written += curr_written;
	} while (iov_iter_count(i));

out:
	if (!written)
		return status;

	if (status < 0) {
		pr_info("Status = %ld\n", status);
		return status;
	}

	iocb->ki_pos += written;
	TIME_END_ALWAYS(perform_write);

	return written;
}
EXPORT_SYMBOL(generic_perform_write_bg_cb);

/*
 * Wrapper for filesystems that don't need a custom write callback (e.g., ext4).
 * Uses the default a_ops->write_begin/write_end path in the background.
 */
ssize_t generic_perform_write_bg(struct kiocb *iocb, struct iov_iter *i,
				 bool large_folio_support)
{
	return generic_perform_write_bg_cb(iocb, i, NULL, large_folio_support);
}
EXPORT_SYMBOL(generic_perform_write_bg);

/**
 * __filemap_get_folio_bg - Find and get a reference to a folio.
 * @mapping: The address_space to search.
 * @index: The page index.
 * @fgp_flags: %FGP flags modify how the folio is returned.
 * @gfp: Memory allocation flags to use if %FGP_CREAT is specified.
 *
 * Looks up the page cache entry at @mapping & @index.
 *
 * If %FGP_LOCK or %FGP_CREAT are specified then the function may sleep even
 * if the %GFP flags specified for %FGP_CREAT are atomic.
 *
 * If this function returns a folio, it is returned with an increased refcount.
 *
 * Return: The found folio or an ERR_PTR() otherwise.
 */
#define FOLIOS_ARR_MAX_COUNT DSA_EMU_SUBMIT_REQ_ENTRIES
static unsigned int filemap_bg_folio_order(struct address_space *mapping,
					   pgoff_t start_index, int count,
					   bool large_folio_support)
{
	unsigned int order;
	unsigned int max_order;

	if (!large_folio_support)
		return 0;

	if (count <= 1)
		return 0;

	order = min_t(unsigned int, ilog2(count), MAX_PAGECACHE_ORDER);
	max_order = dsa_emu_folio_pool_order_max_read();
	if (order > max_order)
		order = max_order;

	if (order && start_index) {
		unsigned int align = __builtin_ctzl(start_index);

		if (align < order)
			order = align;
	}

	return order;
}

int __filemap_get_folio_bg(struct inode *curr_inode,
			   struct address_space *mapping, pgoff_t start_index,
			   pgoff_t end_index, fgf_t fgp_flags, gfp_t gfp,
			   struct dsa_emu_req_entry *ents, loff_t pos,
			   bool large_folio_support)
{
	TEST_BEGIN_AALWAYS();
	int nr_pages = end_index - start_index;
	unsigned int max_order = dsa_emu_folio_pool_order_max_read();
	BUG_ON(!nr_pages);
	BUG_ON(start_index >= end_index);

	if (max_order > MAX_PAGECACHE_ORDER)
		max_order = MAX_PAGECACHE_ORDER;

	if (nr_pages > (FOLIOS_ARR_MAX_COUNT << DSA_EMU_FOLIO_POOL_ORDER_MAX)) {
		pr_err("nr_pages too large: %d\n", nr_pages);
		return -EINVAL;
	}
	const bool naive_mode = READ_ONCE(dsa_emu_naive_mode);

	/* Buckets pointer is stable after inode init; cache to avoid reloads. */
	struct ongoing_list_bucket *buckets = NULL;

	if (!naive_mode)
		buckets = READ_ONCE(curr_inode->i_ongoing_buckets);
	/*
	 * Naive mode skips ongoing-list search/insert/delete to eliminate
	 * bucket-lock contention for non-overlapping workloads.  Page-cache
	 * lookup is always performed (lock-free xa_load) so that existing
	 * folios are reused and BG filemap_add_folio_test avoids EEXIST.
	 */
	const bool can_writeback = true;
	int curr_count = 0;
	bool expect_null = false;
	struct dsa_emu_req_entry *ent = ents;

	for (pgoff_t index = start_index;
	     index < end_index && curr_count < FOLIOS_ARR_MAX_COUNT;) {
		struct folio *folio = NULL;
		struct page *page = NULL;
		bool from_olist = false;
		int olist_wq_idx = -1;
		unsigned int order = 0;
		unsigned int folio_pages = 1;
		pgoff_t next_index;
		struct ongoing_list_bucket *bucket = NULL;
		struct ongoing_node *reuse_node = NULL;

		if (buckets) {
			int bucket_idx =
				(index >> max_order) % ONGOING_HASH_NUM;

			bucket = &buckets[bucket_idx];
		}

		/* Search o-list (i_rwsem exclusive provides mutual exclusion) */
		if (bucket) {
			struct ongoing_node *onode;
			struct page *opage;

			TIME_BEGIN_ALWAYS(fg_olist_lookup);
			onode = search_in_ongoing_buckets_locked(index, bucket,
								 &reuse_node);
			TIME_END_ALWAYS(fg_olist_lookup);
			if (onode) {
				opage = rcu_dereference_protected(onode->page,
					true);
				if (opage) {
					folio = page_folio(opage);
					if (!folio_try_get(folio))
						folio = NULL;
					else {
						page = &folio->page;
						from_olist = true;
						olist_wq_idx =
							onode->target_wq_idx;
					}
				}
			}
		}

		/* Page cache lookup */
		if (!folio) {
			folio = filemap_get_entry(mapping, index);
			if (xa_is_value(folio))
				folio = NULL;
		}
		/* folio from o-list already has ref from search above */
		if (folio &&
		    unlikely(!folio->mapping || folio->mapping != mapping)) {
			pr_warn_ratelimited(
				"filemap_bg: %s folio %px idx %lu mapping %px expected %px inode %lu\n",
				from_olist ? "olist" : "pagecache", folio,
				index, folio->mapping, mapping,
				curr_inode->i_ino);
		}

		/*
		 * Unaligned writes to non-existent pages are handled by the
		 * FG blk_write_begin path (dsa_emu_entry_unsafe check).
		 * No fallback needed.
		 */

		if (!curr_count) {
			expect_null = folio == NULL;
		} else if (expect_null != (folio == NULL)) {
			/* This is end of the list */
			if (expect_null && folio) {
				folio_put(folio);
			}
			break;
		}

		if (folio) {
			unsigned int remaining = end_index - index;
			unsigned int folio_order_val = folio_order(folio);

			order = folio_order_val;
			if (order > max_order)
				order = max_order;
			if (remaining <= 1) {
				order = 0;
			} else {
				unsigned int max_fit = ilog2(remaining);

				if (max_fit < order)
					order = max_fit;
			}
			if (order && index) {
				unsigned int align = __builtin_ctzl(index);

				if (align < order)
					order = align;
			}

			folio_pages = 1U << order;
			next_index = index + folio_pages;

			/* Tight store sequence to FG-write cache line */
			ent->page = &folio->page;
			ent->index = index;
			ent->order = order;
			ent->initial_alloc = false;
			ent->static_folio = false;
			ent->folio_idx = -1;
			ent->target_wq_idx = olist_wq_idx;
		} else {
			order = filemap_bg_folio_order(mapping, index,
						       end_index - index,
						       large_folio_support);
			folio_pages = 1U << order;
			next_index = index + folio_pages;

			/* Tight store sequence to FG-write cache line */
			ent->page = NULL;
			ent->index = index;
			ent->order = order;
			ent->initial_alloc = true;
			ent->static_folio = false;
			ent->folio_idx = -1;
			ent->target_wq_idx = -1;
		}
		ent->onode = NULL;

		/* Insert into o-list (i_rwsem exclusive) */
		if (bucket) {
			struct ongoing_node *node;

			TIME_BEGIN_ALWAYS(fg_olist_insert);
			if (reuse_node) {
				/*
				 * In-place reuse of a BG-completed node.
				 * Overwrite fields while node->done is still
				 * true (readers skip done nodes).  The final
				 * smp_store_release publishes the new entry;
				 * on x86 TSO this is a plain store — no lock
				 * prefix, no store-buffer drain.
				 */
				node = reuse_node;
				node->index = ent->index;
				node->order = ent->order;
				node->target_wq_idx = ent->target_wq_idx;
				RCU_INIT_POINTER(node->page, ent->page);
				/* Publish: readers see done==false only after
				 * all field stores above are visible. */
				smp_store_release(&node->done, false);
				ent->onode = node;
				reuse_node = NULL;
			} else {
				node = kmem_cache_alloc(ongoing_node_cache,
							GFP_ATOMIC);
				if (node) {
					node->index = ent->index;
					node->order = ent->order;
					node->done = false;
					node->target_wq_idx =
						ent->target_wq_idx;
					RCU_INIT_POINTER(node->page, ent->page);
					hlist_add_head_rcu(&node->hnode,
							   &bucket->list);
					ent->onode = node;
				}
			}
			TIME_END_ALWAYS(fg_olist_insert);
		}

		/* folio_wait_writeback may sleep */
		if (folio)
			folio_wait_writeback(folio);

		ent++;
		curr_count++;
		index = next_index;
	}

	int count = curr_count;

	if (expect_null && (fgp_flags & FGP_CREAT)) {
		if ((fgp_flags & FGP_WRITE) && can_writeback)
			gfp |= __GFP_WRITE;
		if (fgp_flags & FGP_NOFS)
			gfp &= ~__GFP_FS;
		if (fgp_flags & FGP_NOWAIT) {
			gfp &= ~GFP_KERNEL;
			gfp |= GFP_NOWAIT | __GFP_NOWARN;
		}
		if (WARN_ON_ONCE(!(fgp_flags & (FGP_LOCK | FGP_FOR_MMAP))))
			fgp_flags |= FGP_LOCK;

		/* Count initial_alloc entries and check common order */
		int alloc_count = 0;
		int common_order = -1;
		bool same_order = true;

		for (int i = 0; i < count; i++) {
			if (!ents[i].initial_alloc)
				continue;
			if (common_order == -1)
				common_order = ents[i].order;
			else if (ents[i].order != common_order)
				same_order = false;
			alloc_count++;
		}

		int bg_ongoing_snapshot = 0;
		uint32_t fg_alloc_threshold =
			READ_ONCE(dsa_emu_fg_alloc_threshold);
		if (curr_inode->i_snapshot_counter-- <= 0) {
			curr_inode->i_bg_ongoing_snapshot =
				atomic_read(&curr_inode->i_bg_pending);
			curr_inode->i_snapshot_counter = DSA_EMU_BG_SNAPSHOT_COUNTER;
		}
		bg_ongoing_snapshot = curr_inode->i_bg_ongoing_snapshot;

		if (alloc_count > 0 && same_order &&
		    bg_ongoing_snapshot < (int)fg_alloc_threshold) {
			/* Batch allocation: single get_folio call */
			struct folio *folios[FOLIOS_ARR_MAX_COUNT];
			bool static_folio = false;
			int folio_idx = -1;
			int j = 0;

			get_folio(common_order, true, &static_folio,
				  &folio_idx, alloc_count, folios);

			for (int i = 0; i < count; i++) {
				struct folio *folio;

				ent = &ents[i];
				if (!ent->initial_alloc)
					continue;
				folio = folios[j++];
				ent->static_folio = static_folio;
				ent->folio_idx = folio_idx;
				ent->order = folio_order(folio);
				folio_set_count(folio,
						1 + folio_nr_pages(folio));
				ent->page = &folio->page;
				if (ent->onode)
					rcu_assign_pointer(ent->onode->page,
							   &folio->page);
			}
		} else if (alloc_count > 0) {
			/* Per-entry fallback: mixed orders or high bg_pending */
			for (int i = 0; i < count; i++) {
				struct folio *folio;
				bool static_folio = false;
				int folio_idx = -1;
				gfp_t alloc_gfp = 0x101CCA;

				ent = &ents[i];
				if (!ent->initial_alloc)
					continue;

				if (bg_ongoing_snapshot < (int)fg_alloc_threshold) {
					get_folio(ent->order, true,
						  &static_folio, &folio_idx,
						  1, &folio);
				} else {
					if (READ_ONCE(dsa_emu_no_zero_alloc))
						alloc_gfp |= __GFP_SKIP_ZERO_BBN;
					folio = filemap_alloc_folio(alloc_gfp,
								   ent->order);
					if (unlikely(!folio)) {
						get_folio(ent->order, true,
							  &static_folio,
							  &folio_idx,
							  1, &folio);
					}
				}

				ent->static_folio = static_folio;
				ent->folio_idx = folio_idx;
				ent->order = folio_order(folio);
				folio_set_count(folio,
						1 + folio_nr_pages(folio));
				ent->page = &folio->page;
				if (ent->onode)
					rcu_assign_pointer(ent->onode->page,
							   &folio->page);
			}
		}
	}

	return count;
}
EXPORT_SYMBOL(__filemap_get_folio_bg);
