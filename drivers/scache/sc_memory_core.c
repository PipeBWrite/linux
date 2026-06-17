// SPDX-License-Identifier: GPL-2.0
/*
 * StreamCache Two-Layer Memory Management
 *
 * Pre-allocates pages into per-CPU regions at module load time, then serves
 * allocation requests from per-file caches that batch-refill from the pool.
 * This eliminates buddy allocator overhead (splitting, spinlock contention,
 * page zeroing) from the buffered I/O critical path.
 *
 * Pages are zeroed on free (eviction), not on allocation, so the hot
 * allocation path does no work beyond a list_del.
 */

#define pr_fmt(fmt) "sc_memory: " fmt

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/smp.h>
#include <linux/page-flags.h>
#include <linux/page_ref.h>
#include <linux/pagemap.h>
#include <linux/sc_memory.h>
#include <linux/topology.h>

struct sc_memory_pool sc_pool;
EXPORT_SYMBOL(sc_pool);

/*
 * sc_memory_pool_init - Allocate regions and pre-allocate pages.
 * @pool: pool to initialize
 * @nr_regions: number of per-CPU regions
 * @pages_per_region: pages to pre-allocate per region
 *
 * Returns 0 on success, negative errno on failure.  On failure, any
 * already-allocated pages are freed.
 */
int sc_memory_pool_init(struct sc_memory_pool *pool, unsigned int nr_regions,
			unsigned long pages_per_region)
{
	unsigned int i;
	unsigned long j;
	unsigned long total_allocated = 0;

	if (!nr_regions || !pages_per_region)
		return -EINVAL;

	pool->regions = kvmalloc_array(nr_regions, sizeof(*pool->regions),
				       GFP_KERNEL | __GFP_ZERO);
	if (!pool->regions)
		return -ENOMEM;

	pool->nr_regions = nr_regions;
	pool->total_pages = (unsigned long)nr_regions * pages_per_region;
	atomic_set(&pool->memory_flag, SC_FLAG_NORMAL);
	pool->threshold_low = SC_THRESHOLD_LOW_DEFAULT;
	pool->threshold_urgent = SC_THRESHOLD_URGENT_DEFAULT;
	pool->enabled = false;

	for (i = 0; i < nr_regions; i++) {
		struct sc_memory_region *region = &pool->regions[i];
		int nid = (i < nr_cpu_ids) ? cpu_to_node(i) : NUMA_NO_NODE;

		INIT_LIST_HEAD(&region->free_list);
		spin_lock_init(&region->lock);
		region->nr_free = 0;
		region->node = nid;

		for (j = 0; j < pages_per_region; j++) {
			struct page *page;

			page = alloc_pages_node(nid, GFP_KERNEL, 0);
			if (!page) {
				pr_err("failed after allocating %lu pages "
				       "(region %u, page %lu)\n",
				       total_allocated, i, j);
				goto out_free;
			}
			/* Zero on allocation into pool (first time only) */
			clear_highpage(page);
			list_add(&page->lru, &region->free_list);
			region->nr_free++;
			total_allocated++;

			/* Yield periodically to avoid soft lockups */
			if (total_allocated % 8192 == 0)
				cond_resched();
		}
	}

	pool->enabled = true;
	pr_info("initialized: %u regions, %lu pages/region, %lu MB total\n",
		nr_regions, pages_per_region,
		(total_allocated * PAGE_SIZE) >> 20);
	return 0;

out_free:
	/* Free all pages we managed to allocate */
	for (i = 0; i < nr_regions; i++) {
		struct sc_memory_region *region = &pool->regions[i];
		struct page *page, *tmp;

		list_for_each_entry_safe(page, tmp, &region->free_list, lru) {
			list_del(&page->lru);
			__free_page(page);
		}
	}
	kvfree(pool->regions);
	pool->regions = NULL;
	pool->nr_regions = 0;
	pool->total_pages = 0;
	return -ENOMEM;
}
EXPORT_SYMBOL(sc_memory_pool_init);

/*
 * sc_memory_pool_destroy - Free every page and the regions array.
 */
void sc_memory_pool_destroy(struct sc_memory_pool *pool)
{
	unsigned int i;
	unsigned long freed = 0;

	if (!pool->regions)
		return;

	pool->enabled = false;

	for (i = 0; i < pool->nr_regions; i++) {
		struct sc_memory_region *region = &pool->regions[i];
		struct page *page, *tmp;

		spin_lock(&region->lock);
		list_for_each_entry_safe(page, tmp, &region->free_list, lru) {
			list_del(&page->lru);
			__free_page(page);
			freed++;
		}
		region->nr_free = 0;
		spin_unlock(&region->lock);
	}

	kvfree(pool->regions);
	pool->regions = NULL;
	pool->nr_regions = 0;
	pool->total_pages = 0;
	pr_info("destroyed: freed %lu pages\n", freed);
}
EXPORT_SYMBOL(sc_memory_pool_destroy);

/*
 * sc_per_file_cache_init - Initialize a per-file page cache.
 */
void sc_per_file_cache_init(struct sc_per_file_cache *cache,
			    unsigned int low_mark, unsigned int high_mark)
{
	INIT_LIST_HEAD(&cache->free_list);
	cache->nr_free = 0;
	cache->low_mark = low_mark;
	cache->high_mark = high_mark;
}
EXPORT_SYMBOL(sc_per_file_cache_init);

/*
 * sc_per_file_cache_destroy - Return all cached pages to the pool.
 *
 * Called when the last reference to a file's per-file entity is dropped.
 * Caller must ensure no concurrent access to this cache.
 */
void sc_per_file_cache_destroy(struct sc_per_file_cache *cache,
			       struct sc_memory_pool *pool)
{
	struct page *page, *tmp;
	unsigned int cpu_id;
	struct sc_memory_region *region;

	if (!cache->nr_free)
		return;

	/*
	 * Pool already destroyed (e.g. inode evicted after "echo 0 > enabled").
	 * Return the cached pages to the buddy allocator instead of leaking
	 * them onto a list referenced by nobody.
	 */
	if (!pool->regions) {
		list_for_each_entry_safe(page, tmp, &cache->free_list, lru) {
			list_del(&page->lru);
			ClearPageScache(page);
			set_page_count(page, 1);
			__free_page(page);
		}
		cache->nr_free = 0;
		return;
	}

	cpu_id = raw_smp_processor_id();
	region = &pool->regions[cpu_id % pool->nr_regions];

	spin_lock(&region->lock);
	list_for_each_entry_safe(page, tmp, &cache->free_list, lru) {
		list_move(&page->lru, &region->free_list);
		region->nr_free++;
	}
	spin_unlock(&region->lock);

	cache->nr_free = 0;
}
EXPORT_SYMBOL(sc_per_file_cache_destroy);

/*
 * sc_refill_per_file_cache - Batch-allocate pages from pool to per-file cache.
 *
 * Tries the current CPU's preferred region first, then steals from others
 * round-robin.  Returns number of pages allocated.
 *
 * Caller must ensure exclusive access to @cache (e.g., inode lock held).
 */
int sc_refill_per_file_cache(struct sc_per_file_cache *cache,
			     struct sc_memory_pool *pool)
{
	unsigned int cpu_id = raw_smp_processor_id();
	unsigned int target = cpu_id % pool->nr_regions;
	struct sc_memory_region *region;
	int allocated = 0;
	unsigned int i;

	/* Try preferred region first */
	region = &pool->regions[target];
	spin_lock(&region->lock);
	while (allocated < cache->low_mark && region->nr_free > 0) {
		struct page *page;

		page = list_first_entry(&region->free_list,
					struct page, lru);
		list_move(&page->lru, &cache->free_list);
		region->nr_free--;
		allocated++;
	}
	spin_unlock(&region->lock);

	/* Steal from other regions if needed: same NUMA node first */
	if (allocated < cache->low_mark) {
		int local_node = pool->regions[target].node;

		for (i = 1; i < pool->nr_regions; i++) {
			unsigned int idx = (target + i) % pool->nr_regions;

			region = &pool->regions[idx];
			if (region->node != local_node)
				continue;
			spin_lock(&region->lock);
			while (allocated < cache->low_mark &&
			       region->nr_free > 0) {
				struct page *page;

				page = list_first_entry(
					&region->free_list,
					struct page, lru);
				list_move(&page->lru, &cache->free_list);
				region->nr_free--;
				allocated++;
			}
			spin_unlock(&region->lock);
			if (allocated >= cache->low_mark)
				break;
		}
	}

	/* Last resort: steal from remote NUMA nodes */
	if (allocated < cache->low_mark) {
		int local_node = pool->regions[target].node;

		for (i = 1; i < pool->nr_regions; i++) {
			unsigned int idx = (target + i) % pool->nr_regions;

			region = &pool->regions[idx];
			if (region->node == local_node)
				continue;
			spin_lock(&region->lock);
			while (allocated < cache->low_mark &&
			       region->nr_free > 0) {
				struct page *page;

				page = list_first_entry(
					&region->free_list,
					struct page, lru);
				list_move(&page->lru, &cache->free_list);
				region->nr_free--;
				allocated++;
			}
			spin_unlock(&region->lock);
			if (allocated >= cache->low_mark)
				break;
		}
	}

	cache->nr_free += allocated;
	return allocated;
}
EXPORT_SYMBOL(sc_refill_per_file_cache);

/*
 * sc_alloc_page - Allocate one page from the two-layer system.
 *
 * Fast path: dequeue from per-file cache (no lock needed if caller
 * serializes per-file access).
 * Slow path: batch refill from pool, then dequeue.
 *
 * Returns NULL if the pool is completely exhausted.
 */
struct page *sc_alloc_page(struct sc_per_file_cache *cache,
			   struct sc_memory_pool *pool)
{
	struct page *page;

	/* Fast path: per-file cache has pages */
	if (likely(cache->nr_free > 0)) {
		page = list_first_entry(&cache->free_list,
					struct page, lru);
		list_del(&page->lru);
		cache->nr_free--;
		set_page_count(page, 1);
		SetPageScache(page);
		return page;
	}

	/* Slow path: refill from pool */
	if (sc_refill_per_file_cache(cache, pool) == 0)
		return NULL;

	page = list_first_entry(&cache->free_list, struct page, lru);
	list_del(&page->lru);
	cache->nr_free--;
	set_page_count(page, 1);
	SetPageScache(page);
	return page;
}
EXPORT_SYMBOL(sc_alloc_page);

/*
 * sc_free_page - Return an evicted page.
 *
 * Zeros the page (moving zeroing cost off the allocation path), then
 * places it in the per-file cache if below high_mark, otherwise returns
 * it to the pool.
 */
void sc_free_page(struct page *page, struct sc_per_file_cache *cache,
		  struct sc_memory_pool *pool)
{
	/* Reinitialize struct page before returning to pool */
	page->flags &= ~PAGE_FLAGS_CHECK_AT_PREP;
	ClearPageScache(page);
	page->mapping = NULL;
	page->index = 0;
	set_page_private(page, 0);
	page_mapcount_reset(page);
	clear_highpage(page);

	if (cache->nr_free < cache->high_mark) {
		list_add(&page->lru, &cache->free_list);
		cache->nr_free++;
	} else {
		/* Overflow: return to pool */
		unsigned int cpu_id = raw_smp_processor_id();
		struct sc_memory_region *region;

		region = &pool->regions[cpu_id % pool->nr_regions];
		spin_lock(&region->lock);
		list_add(&page->lru, &region->free_list);
		region->nr_free++;
		spin_unlock(&region->lock);
	}
}
EXPORT_SYMBOL(sc_free_page);

/*
 * sc_release_page - Return a single evicted page-cache page to the pool.
 *
 * Used by the page-free paths (__folio_put_small and the batch path
 * folios_put_refs). The caller must have already removed the page from the
 * LRU and uncharged it from its memcg. Reinitializes struct page, restores the
 * pool refcount invariant (count == 1), and lists it on the local region.
 */
void sc_release_page(struct page *page)
{
	unsigned int cpu_id;
	struct sc_memory_region *region;

	page->flags &= ~PAGE_FLAGS_CHECK_AT_PREP;
	ClearPageScache(page);
	page->mapping = NULL;
	page->index = 0;
	set_page_private(page, 0);
	page_mapcount_reset(page);
	clear_highpage(page);
	set_page_count(page, 1);

	cpu_id = raw_smp_processor_id();
	region = &sc_pool.regions[cpu_id % sc_pool.nr_regions];
	spin_lock(&region->lock);
	list_add(&page->lru, &region->free_list);
	region->nr_free++;
	spin_unlock(&region->lock);
}
EXPORT_SYMBOL(sc_release_page);

/*
 * sc_return_pages_to_pool - Move pages from per-file cache back to pool.
 *
 * Used during URGENT shrink to rebalance pages across files.
 */
void sc_return_pages_to_pool(struct sc_per_file_cache *cache,
			     struct sc_memory_pool *pool,
			     unsigned int nr_pages)
{
	unsigned int cpu_id = raw_smp_processor_id();
	struct sc_memory_region *region;
	unsigned int returned = 0;

	region = &pool->regions[cpu_id % pool->nr_regions];

	spin_lock(&region->lock);
	while (returned < nr_pages && cache->nr_free > 0) {
		struct page *page;

		page = list_first_entry(&cache->free_list,
					struct page, lru);
		list_move(&page->lru, &region->free_list);
		region->nr_free++;
		cache->nr_free--;
		returned++;
	}
	spin_unlock(&region->lock);
}
EXPORT_SYMBOL(sc_return_pages_to_pool);

/*
 * sc_update_memory_flag - Scan regions and update memory pressure flag.
 *
 * Called periodically by a background monitor thread.
 */
void sc_update_memory_flag(struct sc_memory_pool *pool)
{
	unsigned long total_free = 0;
	unsigned long free_ratio;
	unsigned int i;

	for (i = 0; i < pool->nr_regions; i++) {
		/*
		 * Read nr_free without the lock for monitoring purposes.
		 * Slight inaccuracy is acceptable here.
		 */
		total_free += READ_ONCE(pool->regions[i].nr_free);
	}

	if (pool->total_pages == 0) {
		atomic_set(&pool->memory_flag, SC_FLAG_URGENT);
		return;
	}

	free_ratio = (total_free * 100) / pool->total_pages;

	if (free_ratio >= pool->threshold_low)
		atomic_set(&pool->memory_flag, SC_FLAG_NORMAL);
	else if (free_ratio >= pool->threshold_urgent)
		atomic_set(&pool->memory_flag, SC_FLAG_LOW);
	else
		atomic_set(&pool->memory_flag, SC_FLAG_URGENT);
}
EXPORT_SYMBOL(sc_update_memory_flag);

/*
 * sc_shrink_all_caches - Shrink every per-file cache by half.
 *
 * Called in URGENT mode to rapidly rebalance pages across files.
 * Caller is responsible for collecting the cache pointers and ensuring
 * safe access (e.g., via RCU or holding a table lock).
 */
void sc_shrink_all_caches(struct sc_memory_pool *pool,
			  struct sc_per_file_cache **caches,
			  unsigned int nr_caches)
{
	unsigned int i;

	for (i = 0; i < nr_caches; i++) {
		struct sc_per_file_cache *cache = caches[i];
		unsigned int nr_to_return;

		if (!cache || cache->nr_free == 0)
			continue;

		nr_to_return = cache->nr_free / 2;
		if (nr_to_return > 0)
			sc_return_pages_to_pool(cache, pool, nr_to_return);
	}
}
EXPORT_SYMBOL(sc_shrink_all_caches);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("StreamCache Two-Layer Memory Management");
