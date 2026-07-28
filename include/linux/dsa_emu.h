#ifndef DSA_SL_DSA_EMU_H
#define DSA_SL_DSA_EMU_H

#include "linux/stats.h"
#include <linux/uio.h>
#include <linux/types.h>
#include <linux/bvec.h>
#include <linux/mm.h>
#include <linux/minmax.h>

typedef size_t (*iov_step_f)(void *iter_base, size_t progress, size_t len,
			     void *priv, void *priv2);
typedef size_t (*iov_ustep_f)(void __user *iter_base, size_t progress,
			      size_t len, void *priv, void *priv2);

static __always_inline size_t dsa_emu_iterate_ubuf(struct iov_iter *iter,
						   size_t len, void *priv,
						   void *priv2,
						   iov_ustep_f step)
{
	void __user *base = iter->ubuf;
	size_t progress = 0, remain;

	remain = step(base + iter->iov_offset, 0, len, priv, priv2);
	progress = len - remain;
	iter->iov_offset += progress;
	iter->count -= progress;
	return progress;
}

/*
 * Handle ITER_IOVEC.
 */
static __always_inline size_t dsa_emu_iterate_iovec(struct iov_iter *iter,
						    size_t len, void *priv,
						    void *priv2,
						    iov_ustep_f step)
{
	const struct iovec *p = iter->__iov;
	size_t progress = 0, skip = iter->iov_offset;

	do {
		size_t remain, consumed;
		size_t part = min(len, p->iov_len - skip);

		if (likely(part)) {
			remain = step(p->iov_base + skip, progress, part, priv,
				      priv2);
			consumed = part - remain;
			progress += consumed;
			skip += consumed;
			len -= consumed;
			if (skip < p->iov_len)
				break;
		}
		p++;
		skip = 0;
	} while (len);

	iter->nr_segs -= p - iter->__iov;
	iter->__iov = p;
	iter->iov_offset = skip;
	iter->count -= progress;
	return progress;
}

/*
 * Handle ITER_KVEC.
 */
static __always_inline size_t dsa_emu_iterate_kvec(struct iov_iter *iter,
						   size_t len, void *priv,
						   void *priv2, iov_step_f step)
{
	const struct kvec *p = iter->kvec;
	size_t progress = 0, skip = iter->iov_offset;

	do {
		size_t remain, consumed;
		size_t part = min(len, p->iov_len - skip);

		if (likely(part)) {
			remain = step(p->iov_base + skip, progress, part, priv,
				      priv2);
			consumed = part - remain;
			progress += consumed;
			skip += consumed;
			len -= consumed;
			if (skip < p->iov_len)
				break;
		}
		p++;
		skip = 0;
	} while (len);

	iter->nr_segs -= p - iter->kvec;
	iter->kvec = p;
	iter->iov_offset = skip;
	iter->count -= progress;
	return progress;
}

/*
 * Handle ITER_BVEC.
 */
static __always_inline size_t dsa_emu_iterate_bvec(struct iov_iter *iter,
						   size_t len, void *priv,
						   void *priv2, iov_step_f step)
{
	const struct bio_vec *p = iter->bvec;
	size_t progress = 0, skip = iter->iov_offset;

	do {
		size_t remain, consumed;
		size_t offset = p->bv_offset + skip, part;
		void *kaddr = kmap_local_page(p->bv_page + offset / PAGE_SIZE);

		part = min3(len, (size_t)(p->bv_len - skip),
			    (size_t)(PAGE_SIZE - offset % PAGE_SIZE));
		remain = step(kaddr + offset % PAGE_SIZE, progress, part, priv,
			      priv2);
		kunmap_local(kaddr);
		consumed = part - remain;
		len -= consumed;
		progress += consumed;
		skip += consumed;
		if (skip >= p->bv_len) {
			skip = 0;
			p++;
		}
		if (remain)
			break;
	} while (len);

	iter->nr_segs -= p - iter->bvec;
	iter->bvec = p;
	iter->iov_offset = skip;
	iter->count -= progress;
	return progress;
}

/*
 * Handle ITER_XARRAY.
 */
static __always_inline size_t dsa_emu_iterate_xarray(struct iov_iter *iter,
						     size_t len, void *priv,
						     void *priv2,
						     iov_step_f step)
{
	struct folio *folio;
	size_t progress = 0;
	loff_t start = iter->xarray_start + iter->iov_offset;
	pgoff_t index = start / PAGE_SIZE;
	XA_STATE(xas, iter->xarray, index);

	rcu_read_lock();
	xas_for_each(&xas, folio, ULONG_MAX) {
		size_t remain, consumed, offset, part, flen;

		if (xas_retry(&xas, folio))
			continue;
		if (WARN_ON(xa_is_value(folio)))
			break;
		if (WARN_ON(folio_test_hugetlb(folio)))
			break;

		offset = offset_in_folio(folio, start + progress);
		flen = min(folio_size(folio) - offset, len);

		while (flen) {
			void *base = kmap_local_folio(folio, offset);

			part = min_t(size_t, flen,
				     PAGE_SIZE - offset_in_page(offset));
			remain = step(base, progress, part, priv, priv2);
			kunmap_local(base);

			consumed = part - remain;
			progress += consumed;
			len -= consumed;

			if (remain || len == 0)
				goto out;
			flen -= consumed;
			offset += consumed;
		}
	}

out:
	rcu_read_unlock();
	iter->iov_offset += progress;
	iter->count -= progress;
	return progress;
}

/*
 * Handle ITER_DISCARD.
 */
static __always_inline size_t das_emu_iterate_discard(struct iov_iter *iter,
						      size_t len, void *priv,
						      void *priv2,
						      iov_step_f step)
{
	size_t progress = len;

	iter->count -= progress;
	return progress;
}

static __always_inline size_t
dsa_emu_iterate_and_advance2(struct iov_iter *iter, size_t len, void *priv,
			     void *priv2, iov_ustep_f ustep, iov_step_f step)
{
	if (unlikely(iter->count < len)) {
		pr_info("iter->count = 0\n");
		len = iter->count;
	}
	if (unlikely(!len)) {
		pr_info("len = 0\n");
		return 0;
	}

	if (likely(iter_is_ubuf(iter))) {
		return dsa_emu_iterate_ubuf(iter, len, priv, priv2, ustep);
	}
	if (likely(iter_is_iovec(iter)))
		return dsa_emu_iterate_iovec(iter, len, priv, priv2, ustep);
	if (iov_iter_is_bvec(iter))
		return dsa_emu_iterate_bvec(iter, len, priv, priv2, step);
	if (iov_iter_is_kvec(iter))
		return dsa_emu_iterate_kvec(iter, len, priv, priv2, step);
	if (iov_iter_is_xarray(iter))
		return dsa_emu_iterate_xarray(iter, len, priv, priv2, step);
	return das_emu_iterate_discard(iter, len, priv, priv2, step);
}

static inline bool page_copy_sane(struct page *page, size_t offset, size_t n)
{
	struct page *head;
	size_t v = n + offset;

	/*
	 * The general case needs to access the page order in order
	 * to compute the page size.
	 * However, we mostly deal with order-0 pages and thus can
	 * avoid a possible cache line miss for requests that fit all
	 * page orders.
	 */
	if (n <= v && v <= PAGE_SIZE)
		return true;

	head = compound_head(page);
	v += (page - head) << PAGE_SHIFT;

	if (WARN_ON(n > v || v > page_size(head)))
		return false;
	return true;
}

static __always_inline size_t iterate_and_advance(struct iov_iter *iter,
						  size_t len, void *priv,
						  iov_ustep_f ustep,
						  iov_step_f step)
{
	return dsa_emu_iterate_and_advance2(iter, len, priv, NULL, ustep, step);
}

static __always_inline size_t dsa_emu_memcpy_from_iter(void *iter_from,
						       size_t progress,
						       size_t len, void *to,
						       void *priv2)
{
	memcpy(to + progress, iter_from, len);
	return 0;
}

static __always_inline __must_check unsigned long
dsa_emu_raw_copy_from_user(void *dst, const void __user *src,
			   unsigned long size);

#ifdef CONFIG_X86_64
struct dsa_emu_nt_copy_ctx {
	bool nt_issued;
};

static __always_inline void dsa_emu_memcpy_nt(void *dst, const void *src,
					      size_t len)
{
	u8 *d = dst;
	const u8 *s = src;

	if (((unsigned long)d & 7) && len) {
		size_t prefix = min_t(size_t, len, 8 - ((unsigned long)d & 7));

		memcpy(d, s, prefix);
		d += prefix;
		s += prefix;
		len -= prefix;
	}

	while (len >= 32) {
		u64 v0, v1, v2, v3;

		memcpy(&v0, s + 0, sizeof(v0));
		memcpy(&v1, s + 8, sizeof(v1));
		memcpy(&v2, s + 16, sizeof(v2));
		memcpy(&v3, s + 24, sizeof(v3));
		asm volatile(
			"movnti %1, 0(%0)\n\t"
			"movnti %2, 8(%0)\n\t"
			"movnti %3, 16(%0)\n\t"
			"movnti %4, 24(%0)"
			:
			: "r"(d), "r"(v0), "r"(v1), "r"(v2), "r"(v3)
			: "memory");
		d += 32;
		s += 32;
		len -= 32;
	}

	while (len >= 8) {
		u64 v;

		memcpy(&v, s, sizeof(v));
		asm volatile("movnti %1, (%0)" : : "r"(d), "r"(v) : "memory");
		d += 8;
		s += 8;
		len -= 8;
	}

	if (len)
		memcpy(d, s, len);
}

static __always_inline size_t dsa_emu_memcpy_nt_from_iter(void *iter_from,
							  size_t progress,
							  size_t len, void *to,
							  void *priv2)
{
	struct dsa_emu_nt_copy_ctx *ctx = priv2;

	dsa_emu_memcpy_nt(to + progress, iter_from, len);
	ctx->nt_issued = true;
	return 0;
}

#define DSA_EMU_NT_COPY_USER_BOUNCE 256

static __always_inline size_t
dsa_emu_copy_from_user_iter_nt(void __user *iter_from, size_t progress,
			       size_t len, void *to, void *priv2)
{
	struct dsa_emu_nt_copy_ctx *ctx = priv2;
	size_t copied = 0;
	u8 bounce[DSA_EMU_NT_COPY_USER_BOUNCE] __aligned(8);
	u8 *dst = to + progress;

	if (should_fail_usercopy()) {
		pr_info("should_fail_usercopy()\n");
		return len;
	}

	while (copied < len) {
		void __user *src;
		size_t chunk = min_t(size_t, len - copied,
				     (size_t)DSA_EMU_NT_COPY_USER_BOUNCE);
		size_t remain;

		src = (void __user *)((char __user *)iter_from + copied);
		if (!access_ok(src, chunk)) {
			pr_info("!access_ok()!\n");
			break;
		}

		instrument_copy_from_user_before(bounce, src, chunk);
		remain = dsa_emu_raw_copy_from_user(bounce, src, chunk);
		instrument_copy_from_user_after(bounce, src, chunk, remain);
		chunk -= remain;

		if (!chunk)
			break;

		dsa_emu_memcpy_nt(dst + copied, bounce, chunk);
		ctx->nt_issued = true;
		copied += chunk;

		if (remain)
			break;
	}

	return len - copied;
}
#endif

static __always_inline __must_check unsigned long
dsa_emu_copy_user_generic(void *to, const void *from, unsigned long len)
{
	stac();
	/*
	 * If CPU has FSRM feature, use 'rep movs'.
	 * Otherwise, use rep_movs_alternative.
	 */
	asm volatile(
		"1:\n\t" ALTERNATIVE(
			"rep movsb", "call rep_movs_alternative",
			ALT_NOT(X86_FEATURE_FSRM)) "2:\n" _ASM_EXTABLE_UA(1b,
									  2b)
		: "+c"(len), "+D"(to), "+S"(from), ASM_CALL_CONSTRAINT
		:
		: "memory", "rax");
	clac();
	return len;
}

static __always_inline __must_check unsigned long
dsa_emu_raw_copy_from_user(void *dst, const void __user *src,
			   unsigned long size)
{
	return dsa_emu_copy_user_generic(dst, (__force void *)src, size);
}

static __always_inline size_t
dsa_emu_copy_from_user_iter(void __user *iter_from, size_t progress, size_t len,
			    void *to, void *priv2)
{
	size_t res = len;

	if (should_fail_usercopy()) {
		pr_info("should_fail_usercopy()\n");
		return len;
	}
	if (access_ok(iter_from, len)) {
		to += progress;
		instrument_copy_from_user_before(to, iter_from, len);
		res = dsa_emu_raw_copy_from_user(to, iter_from, len);
		instrument_copy_from_user_after(to, iter_from, len, res);
	} else {
		pr_info("!access_ok()!\n");
	}
	return res;
}

static __always_inline size_t __dsa_emu_copy_from_iter(void *addr, size_t bytes,
						       struct iov_iter *i)
{
	return iterate_and_advance(i, bytes, addr, dsa_emu_copy_from_user_iter,
				   dsa_emu_memcpy_from_iter);
}

static __always_inline void dsa_emu_nt_sfence(void)
{
#ifdef CONFIG_X86_64
	asm volatile("sfence" ::: "memory");
#else
	barrier();
#endif
}

static __always_inline size_t
__dsa_emu_copy_from_iter_nt(void *addr, size_t bytes, struct iov_iter *i,
			    bool *nt_issued)
{
#ifdef CONFIG_X86_64
	struct dsa_emu_nt_copy_ctx ctx = { };
	size_t copied;

	copied = dsa_emu_iterate_and_advance2(i, bytes, addr, &ctx,
					      dsa_emu_copy_from_user_iter_nt,
					      dsa_emu_memcpy_nt_from_iter);
	if (nt_issued)
		*nt_issued = ctx.nt_issued;
	return copied;
#else
	if (nt_issued)
		*nt_issued = false;
	return __dsa_emu_copy_from_iter(addr, bytes, i);
#endif
}

void prefetch_page(struct page *page);
void prefetch_page_range(struct page *page, unsigned long offset,
			 unsigned long bytes);
void prefetch_page_range_stride2(struct page *page, unsigned long offset,
				 unsigned long bytes);
void prefetch_page_range_stride4(struct page *page, unsigned long offset,
				 unsigned long bytes);
void cflush_page(struct page *page);
void flush_page(struct page *page);

static inline void dsa_emu_prefetch_folio_range(struct folio *folio,
						size_t offset, size_t bytes)
{
	size_t done = 0;

	while (done < bytes) {
		size_t page_off = offset_in_page(offset);
		size_t part = min_t(size_t, bytes - done,
				    PAGE_SIZE - page_off);
		struct page *page = folio_page(folio, offset >> PAGE_SHIFT);

		prefetch_page_range(page, page_off, part);
		done += part;
		offset += part;
	}
}

/*
 * Non-temporal zeroing: uses movnti to bypass the cache entirely.
 * Avoids read-for-ownership misses on freshly allocated folios where
 * the zeroed ranges won't be touched again by this CPU soon.
 * Requires 8-byte aligned dest and count for the fast path.
 */
#ifdef CONFIG_X86_64
static __always_inline void memzero_nt(void *dest, size_t count)
{
	/* 32-byte unrolled NT store loop */
	while (count >= 32) {
		asm volatile(
			"movnti %1, 0(%0)\n\t"
			"movnti %1, 8(%0)\n\t"
			"movnti %1, 16(%0)\n\t"
			"movnti %1, 24(%0)"
			:: "r"(dest), "r"((unsigned long)0) : "memory");
		dest += 32;
		count -= 32;
	}
	while (count >= 8) {
		asm volatile(
			"movnti %1, (%0)"
			:: "r"(dest), "r"((unsigned long)0) : "memory");
		dest += 8;
		count -= 8;
	}
	/* Ensure NT stores are globally visible */
	asm volatile("sfence" ::: "memory");
}
#endif

static inline void dsa_emu_folio_zero_segments_nt(struct folio *folio,
						  size_t start1, size_t xend1,
						  size_t start2, size_t xend2)
{
#ifdef CONFIG_X86_64
	void *addr = page_address(&folio->page);

	if (xend1 > start1)
		memzero_nt(addr + start1, xend1 - start1);
	if (xend2 > start2)
		memzero_nt(addr + start2, xend2 - start2);
#else
	folio_zero_segments(folio, start1, xend1, start2, xend2);
#endif
}

/*
 * Explicit prefetchw that bypasses alternative patching.
 * On Intel CPUs, the alternative_input() macro may not properly patch
 * prefetcht0 to prefetchw even when 3dnowprefetch is supported, causing
 * the cache line to be brought in Shared state instead of Exclusive.
 * This explicit version forces the prefetchw instruction.
 */
#ifdef CONFIG_X86
static __always_inline void prefetchw_for_write(const void *x)
{
	asm volatile("prefetchw (%0)" : : "r" (x) : "memory");
}
#else
static __always_inline void prefetchw_for_write(const void *x)
{
	prefetchw(x);
}
#endif

extern uint32_t dsa_emu_prefetch;
extern uint32_t dsa_emu_nt_store;
extern uint32_t dsa_emu_nt_store_min_bytes;
extern uint32_t dsa_emu_sleep_policy;

extern uint32_t dsa_emu_no_zero_alloc;

extern uint32_t dsa_emu_ignored_inode_min;
extern uint32_t dsa_emu_ignored_inode_max;

extern uint32_t dsa_emu_debug_origin_bdp_mode;
extern uint32_t dsa_emu_debug_origin_bdp_fg_wb_nrpages;
extern uint32_t dsa_emu_debug_bg_fsync;
extern uint32_t dsa_emu_debug_defer_blk_write_begin;
/* Run blk_write_begin in FG for all entries (keeps es-tree da reservations
 * in file order; see sysfs_emu.c). */
extern uint32_t dsa_emu_fg_bwb_all;
/* Share one jbd2 handle across a BG request's write_end calls. */
extern uint32_t dsa_emu_bg_batch_handle;
/* Drain BG backlog before sync_file_range write hints. */
extern uint32_t dsa_emu_sfr_drain;
extern int dsa_emu_thread_numa;
extern uint32_t dsa_emu_debug_dirty_threshold;

extern uint32_t dsa_emu_fsync_skip;
extern uint32_t dsa_emu_naive_mode;
extern uint32_t dsa_emu_disable_batching;
extern uint32_t dsa_emu_fg_alloc_threshold;
extern uint32_t dsa_emu_sync_fallback_threshold;
extern uint32_t dsa_emu_fsync_fallback_numerator;
extern uint32_t dsa_emu_fsync_fallback_denominator;
extern uint32_t dsa_emu_fsync_fallback_recovery_writes;
extern uint32_t dsa_emu_backoff_threshold_ns;
extern uint32_t dsa_emu_backoff_duration_ns;
extern uint32_t dsa_emu_backoff_progress_gate;
extern uint32_t dsa_emu_worker_sched_idle;
extern uint32_t dsa_emu_cpu_to_wq_mirror;
extern uint32_t dsa_emu_hybrid_hot_iters;
extern uint32_t dsa_emu_hybrid_warm_iters;

/* dsa_emu_sleep_policy values */
enum {
	DSA_EMU_SLEEP_POLL = 0,
	DSA_EMU_SLEEP_WAITQUEUE = 1,
	DSA_EMU_SLEEP_HYBRID = 2,
};

void dsa_emu_rebuild_cpu_to_wq_pub(void);

#endif
