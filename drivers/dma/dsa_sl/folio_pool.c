#include "linux/preempt.h"
#include <linux/types.h>
#include <linux/signal.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/dsa_sl.h>
#include <linux/uio.h>
#include <linux/fs.h>
#include <linux/pagemap_bg.h>
#include <linux/stats.h>
#include <linux/cache.h>
#include <linux/mutex.h>
#include <asm-generic/cacheflush.h>
#include <linux/fbg.h>
#include <linux/swap.h>
#include <linux/dsa_emu.h>
#include <linux/topology.h>

#include "folio_pool.h"

#ifdef DSA_EMU_BG_ALLOC_THREAD

// stats
static unsigned long long stat_fail_static;
static unsigned long long stat_fail_dynamic;

#ifdef CONFIG_MISC_STATS
#define FOLIO_POOL_STAT_INC(name) this_cpu_inc(stats_##name##_count)
#define FOLIO_POOL_STAT_ADD(name, count) \
	this_cpu_add(stats_##name##_count, count)
#else
#define FOLIO_POOL_STAT_INC(name) \
	do {                      \
	} while (0)
#define FOLIO_POOL_STAT_ADD(name, count) \
	do {                            \
	} while (0)
#endif

// Dynamic allocation (order-based pools)
#define DSA_EMU_FOLIO_POOL_ORDER_CNT (DSA_EMU_FOLIO_POOL_ORDER_MAX + 1)

struct dsa_emu_ring_cons_state {
	u32 head;
	u32 cached_tail;
} ____cacheline_aligned_in_smp;

struct dsa_emu_ring_prod_state {
	spinlock_t put_lock;
	u32 tail;
	u32 cached_head;
} ____cacheline_aligned_in_smp;

struct dsa_emu_order_pool {
	struct folio *pool[DSA_EMU_FOLIO_POOL_SIZE];
	struct dsa_emu_ring_cons_state cons;
	struct dsa_emu_ring_prod_state prod;
};

static struct task_struct *alloc_func_task = NULL;
static DEFINE_PER_CPU(struct dsa_emu_order_pool,
		      order_pools[DSA_EMU_FOLIO_POOL_ORDER_CNT]);

static inline bool order_pool_valid(unsigned int order)
{
	unsigned int max_order = dsa_emu_folio_pool_order_max_read();

	return order >= DSA_EMU_FOLIO_POOL_ORDER_MIN &&
	       order <= max_order;
}

static inline void prefetch_t0(void *addr)
{
	asm volatile("prefetcht0 (%0)" :: "r"(addr));
}

static inline void prefetch_t1(void *addr)
{
	asm volatile("prefetcht1 (%0)" :: "r"(addr));
}

static inline void prefetch_t2(void *addr)
{
	asm volatile("prefetcht2 (%0)" :: "r"(addr));
}

void prefetch_page_range(struct page *page, unsigned long offset,
			 unsigned long bytes)
{
	unsigned long start;
	unsigned long end;
	unsigned long pos;
	void *addr;

	if (!bytes || offset >= PAGE_SIZE)
		return;

	end = offset + bytes;
	if (end > PAGE_SIZE)
		end = PAGE_SIZE;
	start = offset & ~(L1_CACHE_BYTES - 1);

	prefetchw(page);

	addr = page_address(page);
	/* Use cache-line stride to prefetch each line explicitly. */
	for (pos = start; pos < end; pos += L1_CACHE_BYTES)
		prefetchw_for_write(addr + pos);
}
EXPORT_SYMBOL(prefetch_page_range);

void prefetch_page_range_stride2(struct page *page, unsigned long offset,
				 unsigned long bytes)
{
	unsigned long start;
	unsigned long end;
	unsigned long pos;
	void *addr;

	if (!bytes || offset >= PAGE_SIZE)
		return;

	end = offset + bytes;
	if (end > PAGE_SIZE)
		end = PAGE_SIZE;
	start = offset & ~(L1_CACHE_BYTES - 1);

	addr = page_address(page);
	/* Prefetch every other cache line; HW prefetcher fills the gaps. */
	for (pos = start; pos < end; pos += 2 * L1_CACHE_BYTES)
		prefetchw(addr + pos);
}
EXPORT_SYMBOL(prefetch_page_range_stride2);

void prefetch_page_range_stride4(struct page *page, unsigned long offset,
				 unsigned long bytes)
{
	unsigned long start;
	unsigned long end;
	unsigned long pos;
	void *addr;

	if (!bytes || offset >= PAGE_SIZE)
		return;

	end = offset + bytes;
	if (end > PAGE_SIZE)
		end = PAGE_SIZE;
	start = offset & ~(L1_CACHE_BYTES - 1);

	addr = page_address(page);
	/* Prefetch every 4th cache line; HW prefetcher fills the gaps. */
	for (pos = start; pos < end; pos += 4 * L1_CACHE_BYTES)
		prefetchw(addr + pos);
}
EXPORT_SYMBOL(prefetch_page_range_stride4);

void prefetch_page(struct page *page)
{
	prefetch_page_range(page, 0, PAGE_SIZE);
}
EXPORT_SYMBOL(prefetch_page);

static inline void clwbline(void *addr) {
	asm volatile("clwb (%0)" :: "r"(addr));
}

static inline void cldemoteline(void *addr) {
	asm volatile("cldemote (%0)" :: "r"(addr));
}

void cflush_page(struct page *page)
{
	int num_ccline = PAGE_SIZE / 64;
	for (int idx = 0; idx < num_ccline; idx++) {
		// clflushopt(page_address(page) + idx * 64);
		clwbline(page_address(page) + idx * 64);
	}
	clwbline(page);
}
EXPORT_SYMBOL(cflush_page);

void flush_page(struct page *page)
{
	int num_ccline = PAGE_SIZE / 64;
	for (int idx = 0; idx < num_ccline; idx++) {
		clflushopt(page_address(page) + idx * 64);
	}
	clflushopt(page);
	mb();
}
EXPORT_SYMBOL(flush_page);

static inline u32 folio_ring_avail(u32 head, u32 tail)
{
	return tail - head;
}

/*
 * Consumer-side check with cached producer tail. Refresh only when near empty
 * to reduce cacheline traffic on the producer-owned tail.
 */
static inline bool folio_ring_has_more_than_cached(u32 *head, u32 *tail,
						   u32 *cached_tail, int count)
{
	u32 head_local = READ_ONCE(*head);
	u32 tail_local = READ_ONCE(*cached_tail);

	if (folio_ring_avail(head_local, tail_local) <= (u32)count) {
		tail_local = smp_load_acquire(tail);
		WRITE_ONCE(*cached_tail, tail_local);
	}

	return folio_ring_avail(head_local, tail_local) > (u32)count;
}

/*
 * Producer-side space check with cached consumer head. Refresh only when near
 * full to reduce cacheline traffic on the consumer-owned head.
 */
static inline bool folio_ring_has_space_cached(u32 *head, u32 *tail,
					       u32 *cached_head, u32 size,
					       u32 *avail_out)
{
	u32 tail_local = READ_ONCE(*tail);
	u32 head_local = READ_ONCE(*cached_head);
	u32 avail = folio_ring_avail(head_local, tail_local);

	if (avail >= size) {
		head_local = smp_load_acquire(head);
		WRITE_ONCE(*cached_head, head_local);
		avail = folio_ring_avail(head_local, tail_local);
	}

	if (avail_out)
		*avail_out = avail;

	return avail < size;
}

static inline int drain_folio_ring_bulk(struct folio **pool, u32 *head,
					int count, int size,
					struct folio **arr)
{
	u32 head_local = READ_ONCE(*head);

	for (int i = 0; i < count; i++) {
		arr[i] = pool[head_local % size];
		head_local++;
	}
	/*
	 * Publish consumed entries after loading folios so producers can
	 * observe new space only after this consumer finished reading.
	 */
	smp_store_release(head, head_local);

	return count;
}

static int get_order_folio_bulk(unsigned int order, int count,
				struct folio **arr, int *cpu_idx)
{
	struct dsa_emu_order_pool *pool;
	int ret_count = 0;

	if (!order_pool_valid(order))
		return 0;

	pool = &this_cpu_ptr(order_pools)[order];
	if (!folio_ring_has_more_than_cached(&pool->cons.head, &pool->prod.tail,
					     &pool->cons.cached_tail, count)) {
		INCR_COUNT(plan2_get_folio_fail_empty);
		stat_fail_dynamic++;
		return 0;
	}

	preempt_disable();
	pool = &this_cpu_ptr(order_pools)[order];
	if (!folio_ring_has_more_than_cached(&pool->cons.head, &pool->prod.tail,
					     &pool->cons.cached_tail, count)) {
		INCR_COUNT(plan2_get_folio_fail_empty);
		stat_fail_dynamic++;
		goto out_enable;
	}

	ret_count = drain_folio_ring_bulk(pool->pool, &pool->cons.head,
					  count, DSA_EMU_FOLIO_POOL_SIZE, arr);

out_enable:
	if (cpu_idx)
		*cpu_idx = smp_processor_id();
	preempt_enable();
	return ret_count;
}

void put_order_folio(struct folio *f, unsigned int order, int idx)
{
	struct dsa_emu_order_pool *pool;

	if (!f)
		return;
	if (!order_pool_valid(order)) {
		folio_put(f);
		return;
	}

	pool = &per_cpu(order_pools, idx)[order];
	spin_lock(&pool->prod.put_lock);
	{
		u32 tail = READ_ONCE(pool->prod.tail);
		u32 avail;

		if (!folio_ring_has_space_cached(&pool->cons.head,
						 &pool->prod.tail,
						 &pool->prod.cached_head,
						 DSA_EMU_FOLIO_POOL_SIZE,
						 &avail)) {
			pr_err("Folio pool order %u is full (%u), should not happen\n",
			       order, avail);
			spin_unlock(&pool->prod.put_lock);
			folio_put(f);
			return;
		}

		pool->pool[tail % DSA_EMU_FOLIO_POOL_SIZE] = f;
		smp_store_release(&pool->prod.tail, tail + 1);
	}
	spin_unlock(&pool->prod.put_lock);
}

// function pointer to get_dyn or get_staic
typedef struct folio *(*folio_alloc_func)(bool *static_folio, int *folio_idx);

// bool check_and_preget_folio(int n_ent){
// 	// FIXME : need spin_lock?
// 	if (atomic_read(&avail_folios) < n_ent)
// 		return false;
// 	atomic_sub(n_ent, &avail_folios);
// 	return true;
// }

int init_folio_alloc_task(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		int nid = cpu_to_node(cpu);

		for (unsigned int order = DSA_EMU_FOLIO_POOL_ORDER_MIN;
		     order <= DSA_EMU_FOLIO_POOL_ORDER_MAX; order++) {
			struct dsa_emu_order_pool *pool =
				&per_cpu(order_pools, cpu)[order];
			gfp_t alloc_gfp =
				0x101CCA | __GFP_NORETRY | __GFP_NOWARN;

			spin_lock_init(&pool->prod.put_lock);
			pool->cons.head = 0;
			pool->cons.cached_tail = 0;
			pool->prod.tail = 0;
			pool->prod.cached_head = 0;

			for (int i = 0; i < DSA_EMU_FOLIO_POOL_SIZE; i++) {
				struct folio *f =
					__folio_alloc_node(alloc_gfp, order, nid);

				if (!f) {
					pr_err("Failed to allocate folio order %u on node %d\n",
					       order, nid);
					break;
				}

				pool->pool[pool->prod.tail % DSA_EMU_FOLIO_POOL_SIZE] = f;
				pool->prod.tail++;
			}

			pool->cons.cached_tail = pool->prod.tail;
		}
	}

	// alloc_func_task = kthread_create_on_cpu(folio_alloc_thread, NULL, 76,
	// 					"folio_alloc_func");

	// wake_up_process(alloc_func_task);
	return 0;
}
#endif

#ifdef DSA_EMU_STATIC_FOLIO_POOL

static bool static_folio_pool_inited = false;
static bool static_folio_pool_resizing;
static DEFINE_MUTEX(static_folio_pool_resize_lock);
uint32_t dsa_emu_static_folio_pool_size __read_mostly =
	DSA_EMU_STATIC_FOLIO_POOL_SIZE;

struct dsa_emu_static_ring_cons_state {
	u32 head;
	u32 cached_tail;
} ____cacheline_aligned_in_smp;

struct dsa_emu_static_ring_prod_state {
	spinlock_t put_lock;
	u32 tail;
	u32 cached_head;
} ____cacheline_aligned_in_smp;

struct dsa_emu_static_pool {
	struct folio *pool[DSA_EMU_STATIC_FOLIO_POOL_SIZE_MAX];
	struct dsa_emu_static_ring_cons_state cons;
	struct dsa_emu_static_ring_prod_state prod;
};

static DEFINE_PER_CPU(struct dsa_emu_static_pool, static_pools);

void init_static_folio_pool(void)
{
	unsigned int active_size = READ_ONCE(dsa_emu_static_folio_pool_size);

	if (static_folio_pool_inited) {
		pr_err("Static folio pool already inited\n");
		return;
	}

	int cpu;
	for_each_possible_cpu(cpu) {
		struct dsa_emu_static_pool *pool =
			per_cpu_ptr(&static_pools, cpu);
		int nid = cpu_to_node(cpu);

		spin_lock_init(&pool->prod.put_lock);

		for (int i = 0; i < DSA_EMU_STATIC_FOLIO_POOL_SIZE_MAX; i++) {
			struct folio *f =
				__folio_alloc_node(0x101CCA | __GFP_THISNODE,
						   0, nid);
			if (!f) {
				pr_err("Failed to allocate folio on node %d\n",
				       nid);
				BUG();
			}
			pool->pool[i] = f;
		}

		pool->cons.head = 0;
		pool->prod.tail = active_size;
		pool->prod.cached_head = 0;
		pool->cons.cached_tail = active_size;
	}
	static_folio_pool_inited = true;
	pr_info("Static folio pool inited, active size %u, max size %u\n",
		active_size, DSA_EMU_STATIC_FOLIO_POOL_SIZE_MAX);

	return;
}

int dsa_emu_static_folio_pool_resize(unsigned int size)
{
	unsigned int old_size;
	int cpu;
	int ret = 0;

	if (size == 0 || size > DSA_EMU_STATIC_FOLIO_POOL_SIZE_MAX)
		return -EINVAL;

	mutex_lock(&static_folio_pool_resize_lock);

	old_size = READ_ONCE(dsa_emu_static_folio_pool_size);
	if (size == old_size)
		goto out_unlock;

	WRITE_ONCE(static_folio_pool_resizing, true);
	smp_mb();

	if (static_folio_pool_inited) {
		for_each_possible_cpu(cpu) {
			struct dsa_emu_static_pool *pool =
				per_cpu_ptr(&static_pools, cpu);
			u32 head, tail, avail;

			spin_lock(&pool->prod.put_lock);
			head = smp_load_acquire(&pool->cons.head);
			tail = READ_ONCE(pool->prod.tail);
			avail = folio_ring_avail(head, tail);
			spin_unlock(&pool->prod.put_lock);

			if (avail != old_size) {
				pr_err("Static folio pool resize busy: cpu=%d avail=%u expected=%u\n",
				       cpu, avail, old_size);
				ret = -EBUSY;
				goto out_clear_resize;
			}
		}

		for_each_possible_cpu(cpu) {
			struct dsa_emu_static_pool *pool =
				per_cpu_ptr(&static_pools, cpu);

			spin_lock(&pool->prod.put_lock);
			WRITE_ONCE(pool->cons.head, 0);
			WRITE_ONCE(pool->cons.cached_tail, size);
			WRITE_ONCE(pool->prod.tail, size);
			WRITE_ONCE(pool->prod.cached_head, 0);
			spin_unlock(&pool->prod.put_lock);
		}
	}

	WRITE_ONCE(dsa_emu_static_folio_pool_size, size);
	pr_info("Static folio pool active size changed from %u to %u\n",
		old_size, size);

out_clear_resize:
	WRITE_ONCE(static_folio_pool_resizing, false);
out_unlock:
	mutex_unlock(&static_folio_pool_resize_lock);
	return ret;
}

inline int get_static_folio_bulk(int *folio_idx, int count, struct folio **arr)
{
	struct dsa_emu_static_pool *pool;
	unsigned int size;

	int ret_count = 0;
	if (unlikely(READ_ONCE(static_folio_pool_resizing)))
		return ret_count;

	size = READ_ONCE(dsa_emu_static_folio_pool_size);
	pool = this_cpu_ptr(&static_pools);
	if (!folio_ring_has_more_than_cached(&pool->cons.head,
					     &pool->prod.tail,
					     &pool->cons.cached_tail, count)) {
		return ret_count;
	}

	preempt_disable();
	if (unlikely(READ_ONCE(static_folio_pool_resizing)))
		goto out_enable;

	size = READ_ONCE(dsa_emu_static_folio_pool_size);
	pool = this_cpu_ptr(&static_pools);

	if (!folio_ring_has_more_than_cached(&pool->cons.head,
					     &pool->prod.tail,
					     &pool->cons.cached_tail, count)) {
		goto out_enable;
	}

	ret_count = drain_folio_ring_bulk(pool->pool, &pool->cons.head,
					  count, size, arr);
	// pr_info("get_static_folio_bulk: got %d folios from static pool\n",
	// 	ret_count);

out_enable:
	*folio_idx = smp_processor_id();
	preempt_enable();

	return ret_count;
}

// Put a static folio back to the pool
void put_static_folio(struct folio *f, int idx)
{
	struct dsa_emu_static_pool *spool;
	unsigned int size = READ_ONCE(dsa_emu_static_folio_pool_size);

	// BUG_ON(folio_ref_count(f) != 1);
	// BUG_ON(folio_test_locked(f));
	spool = per_cpu_ptr(&static_pools, idx);

	spin_lock(&spool->prod.put_lock);
	{
		u32 *headp = &spool->cons.head;
		u32 *tailp = &spool->prod.tail;
		u32 *cached_headp = &spool->prod.cached_head;
		u32 tail = READ_ONCE(*tailp);
		u32 avail;
		struct folio **pool = spool->pool;

		if (!folio_ring_has_space_cached(headp, tailp, cached_headp,
						 size,
						 &avail)) {
			pr_err("Static folio pool is full (%u), idx = %d, should not happen\n",
			       avail, idx);
			spin_unlock(&spool->prod.put_lock);
			return;
		}

		pool[tail % size] = f;
		smp_store_release(tailp, tail + 1);
	}
	spin_unlock(&spool->prod.put_lock);
	// cflush_page(&f->page);
}
#endif

int get_folio(unsigned int order, bool force, bool *static_folio,
	      int *folio_idx, int count, struct folio **folios)
{
	int cnt = 0;
	gfp_t alloc_gfp = 0x101CCA;

	*static_folio = false;
	*folio_idx = -1;

	if (order == 0) {
		cnt = get_static_folio_bulk(folio_idx, count, folios);
		if (cnt == count)
			*static_folio = true;
	} else if (order_pool_valid(order)) {
		int cpu_idx = -1;

		cnt = get_order_folio_bulk(order, count, folios, &cpu_idx);
		if (cnt == count)
			*folio_idx = cpu_idx;
	}

	if (cnt != count && force) {
		if (cnt != 0) {
			cnt = 0;
			pr_warn("%s: cnt (%d) should either be 0 or count (%d)!",
				__func__, cnt, count);
		}
		if (READ_ONCE(dsa_emu_no_zero_alloc))
			alloc_gfp |= __GFP_SKIP_ZERO_BBN;
		if (order > 0)
			alloc_gfp |= __GFP_NORETRY | __GFP_NOWARN;
		for (int i = cnt; i < count; i++) {
			struct folio *f = filemap_alloc_folio(alloc_gfp, order);
			if (!f) {
				pr_err("Failed to allocate folio\n");
				BUG();
			}
			folios[i] = f;
		}
		*static_folio = false;
		*folio_idx = -1;
	}

	cnt = count;

	FOLIO_POOL_STAT_INC(get_folio_calls);
	FOLIO_POOL_STAT_ADD(get_folio_entries, count);
	if (*static_folio || *folio_idx >= 0) {
		FOLIO_POOL_STAT_INC(pool_hit_calls);
		FOLIO_POOL_STAT_ADD(pool_hit_entries, count);
	} else {
		FOLIO_POOL_STAT_INC(fg_alloc_fallback_calls);
		FOLIO_POOL_STAT_ADD(fg_alloc_fallback_entries, count);
	}

	return cnt;
}
