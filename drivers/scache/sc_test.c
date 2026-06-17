// SPDX-License-Identifier: GPL-2.0
/*
 * StreamCache Two-Layer Memory Management - test module
 *
 * Runs self-tests at module load time.  Results are printed to dmesg.
 * Load with: insmod sc_test.ko  (after sc_memory is loaded/built-in)
 *
 * Tests:
 *   1. Pool init/destroy accounting
 *   2. Per-file cache init and basic alloc/free
 *   3. Batch refill
 *   4. High-mark overflow returns to pool
 *   5. Memory flag transitions
 *   6. URGENT shrink
 *   7. Pool exhaustion
 *   8. Per-file cache destroy returns all pages
 */

#define pr_fmt(fmt) "sc_test: " fmt

#include <linux/module.h>
#include <linux/sc_memory.h>

#define TEST_NR_REGIONS		4
#define TEST_PAGES_PER_REGION	200
#define TEST_TOTAL_PAGES	(TEST_NR_REGIONS * TEST_PAGES_PER_REGION)

static int tests_run;
static int tests_passed;

#define TEST_START(name) do {					\
	pr_info("TEST: %s ...\n", name);			\
	tests_run++;						\
} while (0)

#define TEST_PASS(name) do {					\
	pr_info("  PASS: %s\n", name);				\
	tests_passed++;						\
} while (0)

#define TEST_FAIL(name, fmt, ...) do {				\
	pr_err("  FAIL: %s: " fmt "\n", name, ##__VA_ARGS__);	\
} while (0)

static unsigned long count_pool_free(struct sc_memory_pool *pool)
{
	unsigned long total = 0;
	unsigned int i;

	for (i = 0; i < pool->nr_regions; i++)
		total += pool->regions[i].nr_free;
	return total;
}

/* Test 1: Pool init/destroy */
static void test_pool_init_destroy(void)
{
	struct sc_memory_pool pool = {};
	unsigned long free_count;
	int ret;

	TEST_START("pool_init_destroy");

	ret = sc_memory_pool_init(&pool, TEST_NR_REGIONS,
				  TEST_PAGES_PER_REGION);
	if (ret) {
		TEST_FAIL("pool_init_destroy", "init failed: %d", ret);
		return;
	}

	if (pool.nr_regions != TEST_NR_REGIONS) {
		TEST_FAIL("pool_init_destroy", "nr_regions=%u expected=%u",
			  pool.nr_regions, TEST_NR_REGIONS);
		sc_memory_pool_destroy(&pool);
		return;
	}

	if (pool.total_pages != TEST_TOTAL_PAGES) {
		TEST_FAIL("pool_init_destroy", "total_pages=%lu expected=%u",
			  pool.total_pages, TEST_TOTAL_PAGES);
		sc_memory_pool_destroy(&pool);
		return;
	}

	free_count = count_pool_free(&pool);
	if (free_count != TEST_TOTAL_PAGES) {
		TEST_FAIL("pool_init_destroy",
			  "free_count=%lu expected=%u",
			  free_count, TEST_TOTAL_PAGES);
		sc_memory_pool_destroy(&pool);
		return;
	}

	sc_memory_pool_destroy(&pool);

	if (pool.regions != NULL || pool.nr_regions != 0) {
		TEST_FAIL("pool_init_destroy", "pool not cleaned up");
		return;
	}

	TEST_PASS("pool_init_destroy");
}

/* Test 2: Per-file cache init and basic alloc/free */
static void test_basic_alloc_free(void)
{
	struct sc_memory_pool pool = {};
	struct sc_per_file_cache cache;
	struct page *page;
	unsigned long free_before, free_after;

	TEST_START("basic_alloc_free");

	if (sc_memory_pool_init(&pool, TEST_NR_REGIONS,
				TEST_PAGES_PER_REGION)) {
		TEST_FAIL("basic_alloc_free", "pool init failed");
		return;
	}

	sc_per_file_cache_init(&cache, SC_LOW_MARK_DEFAULT,
			       SC_HIGH_MARK_DEFAULT);

	if (cache.nr_free != 0 || cache.low_mark != SC_LOW_MARK_DEFAULT) {
		TEST_FAIL("basic_alloc_free", "cache init wrong");
		goto out;
	}

	free_before = count_pool_free(&pool);

	page = sc_alloc_page(&cache, &pool);
	if (!page) {
		TEST_FAIL("basic_alloc_free", "alloc returned NULL");
		goto out;
	}

	/* First alloc triggers batch refill of low_mark pages */
	free_after = count_pool_free(&pool);
	if (free_before - free_after != SC_LOW_MARK_DEFAULT) {
		TEST_FAIL("basic_alloc_free",
			  "refill took %lu pages, expected %u",
			  free_before - free_after, SC_LOW_MARK_DEFAULT);
		goto out;
	}

	/* cache should have low_mark - 1 remaining */
	if (cache.nr_free != SC_LOW_MARK_DEFAULT - 1) {
		TEST_FAIL("basic_alloc_free",
			  "cache.nr_free=%u expected=%u",
			  cache.nr_free, SC_LOW_MARK_DEFAULT - 1);
		goto out;
	}

	/* Free the page back */
	sc_free_page(page, &cache, &pool);

	if (cache.nr_free != SC_LOW_MARK_DEFAULT) {
		TEST_FAIL("basic_alloc_free",
			  "after free: cache.nr_free=%u expected=%u",
			  cache.nr_free, SC_LOW_MARK_DEFAULT);
		goto out;
	}

	TEST_PASS("basic_alloc_free");
out:
	sc_per_file_cache_destroy(&cache, &pool);
	sc_memory_pool_destroy(&pool);
}

/* Test 3: Batch refill */
static void test_batch_refill(void)
{
	struct sc_memory_pool pool = {};
	struct sc_per_file_cache cache;
	int refilled;

	TEST_START("batch_refill");

	if (sc_memory_pool_init(&pool, TEST_NR_REGIONS,
				TEST_PAGES_PER_REGION)) {
		TEST_FAIL("batch_refill", "pool init failed");
		return;
	}

	sc_per_file_cache_init(&cache, 30, 60);

	refilled = sc_refill_per_file_cache(&cache, &pool);
	if (refilled != 30) {
		TEST_FAIL("batch_refill", "refilled=%d expected=30",
			  refilled);
		goto out;
	}
	if (cache.nr_free != 30) {
		TEST_FAIL("batch_refill", "cache.nr_free=%u expected=30",
			  cache.nr_free);
		goto out;
	}

	TEST_PASS("batch_refill");
out:
	sc_per_file_cache_destroy(&cache, &pool);
	sc_memory_pool_destroy(&pool);
}

/* Test 4: High-mark overflow */
static void test_high_mark_overflow(void)
{
	struct sc_memory_pool pool = {};
	struct sc_per_file_cache cache;
	struct page *pages[10];
	unsigned long pool_free_before, pool_free_after;
	int i;

	TEST_START("high_mark_overflow");

	if (sc_memory_pool_init(&pool, TEST_NR_REGIONS,
				TEST_PAGES_PER_REGION)) {
		TEST_FAIL("high_mark_overflow", "pool init failed");
		return;
	}

	/* Use a tiny cache: low=5, high=5 */
	sc_per_file_cache_init(&cache, 5, 5);

	/* Allocate 5 pages (triggers refill of 5) */
	for (i = 0; i < 5; i++) {
		pages[i] = sc_alloc_page(&cache, &pool);
		if (!pages[i]) {
			TEST_FAIL("high_mark_overflow",
				  "alloc %d failed", i);
			goto out;
		}
	}

	/* cache is now empty, refill 5 more to fill it */
	sc_refill_per_file_cache(&cache, &pool);

	/* cache.nr_free == 5 == high_mark. Free one more → goes to pool */
	pool_free_before = count_pool_free(&pool);
	sc_free_page(pages[0], &cache, &pool);
	pool_free_after = count_pool_free(&pool);

	if (pool_free_after != pool_free_before + 1) {
		TEST_FAIL("high_mark_overflow",
			  "overflow page not returned to pool: "
			  "before=%lu after=%lu",
			  pool_free_before, pool_free_after);
		/* Don't double-free pages[0] */
		pages[0] = NULL;
		goto out;
	}

	if (cache.nr_free != 5) {
		TEST_FAIL("high_mark_overflow",
			  "cache.nr_free=%u expected=5", cache.nr_free);
		pages[0] = NULL;
		goto out;
	}

	pages[0] = NULL; /* Already freed */
	TEST_PASS("high_mark_overflow");
out:
	for (i = 1; i < 5; i++) {
		if (pages[i])
			sc_free_page(pages[i], &cache, &pool);
	}
	sc_per_file_cache_destroy(&cache, &pool);
	sc_memory_pool_destroy(&pool);
}

/* Test 5: Memory flag transitions */
static void test_memory_flags(void)
{
	struct sc_memory_pool pool = {};
	int flag;

	TEST_START("memory_flags");

	if (sc_memory_pool_init(&pool, 2, 100)) {
		TEST_FAIL("memory_flags", "pool init failed");
		return;
	}

	/* All 200 pages free → should be NORMAL (100% > 20%) */
	sc_update_memory_flag(&pool);
	flag = atomic_read(&pool.memory_flag);
	if (flag != SC_FLAG_NORMAL) {
		TEST_FAIL("memory_flags", "expected NORMAL, got %d", flag);
		goto out;
	}

	/*
	 * Drain region 0 to simulate allocation.
	 * Leave only 15% free → LOW (5% <= 15% < 20%).
	 */
	{
		struct sc_memory_region *r = &pool.regions[0];
		int to_remove = 100; /* remove all of region 0 */
		struct page *page;

		spin_lock(&r->lock);
		while (to_remove > 0 && r->nr_free > 0) {
			page = list_first_entry(&r->free_list,
						struct page, lru);
			list_del(&page->lru);
			r->nr_free--;
			__free_page(page);
			to_remove--;
		}
		spin_unlock(&r->lock);

		/* Also drain 70 from region 1 → 30 free / 200 total = 15% */
		r = &pool.regions[1];
		to_remove = 70;
		spin_lock(&r->lock);
		while (to_remove > 0 && r->nr_free > 0) {
			page = list_first_entry(&r->free_list,
						struct page, lru);
			list_del(&page->lru);
			r->nr_free--;
			__free_page(page);
			to_remove--;
		}
		spin_unlock(&r->lock);
	}

	sc_update_memory_flag(&pool);
	flag = atomic_read(&pool.memory_flag);
	if (flag != SC_FLAG_LOW) {
		TEST_FAIL("memory_flags", "expected LOW, got %d "
			  "(free=%lu)", flag, count_pool_free(&pool));
		goto out;
	}

	/* Drain almost everything → URGENT (< 5%) */
	{
		struct sc_memory_region *r = &pool.regions[1];
		int to_remove = 22; /* leave 8/200 = 4% */
		struct page *page;

		spin_lock(&r->lock);
		while (to_remove > 0 && r->nr_free > 0) {
			page = list_first_entry(&r->free_list,
						struct page, lru);
			list_del(&page->lru);
			r->nr_free--;
			__free_page(page);
			to_remove--;
		}
		spin_unlock(&r->lock);
	}

	sc_update_memory_flag(&pool);
	flag = atomic_read(&pool.memory_flag);
	if (flag != SC_FLAG_URGENT) {
		TEST_FAIL("memory_flags", "expected URGENT, got %d "
			  "(free=%lu)", flag, count_pool_free(&pool));
		goto out;
	}

	TEST_PASS("memory_flags");
out:
	/*
	 * Some pages were freed directly — adjust total_pages so
	 * destroy doesn't warn.
	 */
	pool.total_pages = count_pool_free(&pool);
	sc_memory_pool_destroy(&pool);
}

/* Test 6: URGENT shrink */
static void test_urgent_shrink(void)
{
	struct sc_memory_pool pool = {};
	struct sc_per_file_cache c1, c2;
	struct sc_per_file_cache *caches[2] = { &c1, &c2 };
	unsigned long pool_free_before, pool_free_after;

	TEST_START("urgent_shrink");

	if (sc_memory_pool_init(&pool, TEST_NR_REGIONS,
				TEST_PAGES_PER_REGION)) {
		TEST_FAIL("urgent_shrink", "pool init failed");
		return;
	}

	sc_per_file_cache_init(&c1, 50, 100);
	sc_per_file_cache_init(&c2, 50, 100);

	/* Fill each cache with 50 pages */
	sc_refill_per_file_cache(&c1, &pool);
	sc_refill_per_file_cache(&c2, &pool);

	if (c1.nr_free != 50 || c2.nr_free != 50) {
		TEST_FAIL("urgent_shrink", "refill failed: c1=%u c2=%u",
			  c1.nr_free, c2.nr_free);
		goto out;
	}

	pool_free_before = count_pool_free(&pool);

	sc_shrink_all_caches(&pool, caches, 2);

	pool_free_after = count_pool_free(&pool);

	/* Each cache shrunk by half: 25 each = 50 returned */
	if (c1.nr_free != 25 || c2.nr_free != 25) {
		TEST_FAIL("urgent_shrink",
			  "after shrink: c1=%u c2=%u",
			  c1.nr_free, c2.nr_free);
		goto out;
	}

	if (pool_free_after - pool_free_before != 50) {
		TEST_FAIL("urgent_shrink",
			  "pool gained %lu pages, expected 50",
			  pool_free_after - pool_free_before);
		goto out;
	}

	TEST_PASS("urgent_shrink");
out:
	sc_per_file_cache_destroy(&c1, &pool);
	sc_per_file_cache_destroy(&c2, &pool);
	sc_memory_pool_destroy(&pool);
}

/* Test 7: Pool exhaustion */
static void test_pool_exhaustion(void)
{
	struct sc_memory_pool pool = {};
	struct sc_per_file_cache cache;
	struct page *page;
	int count = 0;

	TEST_START("pool_exhaustion");

	/* Small pool: 2 regions x 10 pages = 20 total */
	if (sc_memory_pool_init(&pool, 2, 10)) {
		TEST_FAIL("pool_exhaustion", "pool init failed");
		return;
	}

	sc_per_file_cache_init(&cache, 10, 20);

	/* Allocate all 20 pages */
	while ((page = sc_alloc_page(&cache, &pool)) != NULL)
		count++;

	if (count != 20) {
		TEST_FAIL("pool_exhaustion",
			  "allocated %d pages, expected 20", count);
		goto out;
	}

	/* Next alloc should return NULL */
	page = sc_alloc_page(&cache, &pool);
	if (page != NULL) {
		TEST_FAIL("pool_exhaustion",
			  "alloc after exhaustion returned non-NULL");
		goto out;
	}

	TEST_PASS("pool_exhaustion");
out:
	/*
	 * Pages were allocated out and never freed back, so cache and
	 * pool are empty.  Just destroy.
	 */
	sc_memory_pool_destroy(&pool);
}

/* Test 8: Per-file cache destroy returns pages */
static void test_cache_destroy_returns(void)
{
	struct sc_memory_pool pool = {};
	struct sc_per_file_cache cache;
	unsigned long pool_free_before, pool_free_after;

	TEST_START("cache_destroy_returns");

	if (sc_memory_pool_init(&pool, TEST_NR_REGIONS,
				TEST_PAGES_PER_REGION)) {
		TEST_FAIL("cache_destroy_returns", "pool init failed");
		return;
	}

	sc_per_file_cache_init(&cache, 50, 100);
	sc_refill_per_file_cache(&cache, &pool);

	if (cache.nr_free != 50) {
		TEST_FAIL("cache_destroy_returns",
			  "refill: nr_free=%u", cache.nr_free);
		sc_per_file_cache_destroy(&cache, &pool);
		sc_memory_pool_destroy(&pool);
		return;
	}

	pool_free_before = count_pool_free(&pool);
	sc_per_file_cache_destroy(&cache, &pool);
	pool_free_after = count_pool_free(&pool);

	if (pool_free_after - pool_free_before != 50) {
		TEST_FAIL("cache_destroy_returns",
			  "returned %lu pages, expected 50",
			  pool_free_after - pool_free_before);
		sc_memory_pool_destroy(&pool);
		return;
	}

	if (cache.nr_free != 0) {
		TEST_FAIL("cache_destroy_returns",
			  "cache.nr_free=%u after destroy", cache.nr_free);
		sc_memory_pool_destroy(&pool);
		return;
	}

	/* All pages should be back */
	if (count_pool_free(&pool) != TEST_TOTAL_PAGES) {
		TEST_FAIL("cache_destroy_returns",
			  "pool total=%lu expected=%u",
			  count_pool_free(&pool), TEST_TOTAL_PAGES);
		sc_memory_pool_destroy(&pool);
		return;
	}

	TEST_PASS("cache_destroy_returns");
	sc_memory_pool_destroy(&pool);
}

static int __init sc_test_init(void)
{
	pr_info("=== StreamCache memory tests starting ===\n");

	test_pool_init_destroy();
	test_basic_alloc_free();
	test_batch_refill();
	test_high_mark_overflow();
	test_memory_flags();
	test_urgent_shrink();
	test_pool_exhaustion();
	test_cache_destroy_returns();

	pr_info("=== Results: %d/%d tests passed ===\n",
		tests_passed, tests_run);

	/* Return -EAGAIN so the module unloads after running tests */
	return -EAGAIN;
}

static void __exit sc_test_exit(void)
{
}

module_init(sc_test_init);
module_exit(sc_test_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("StreamCache Two-Layer Memory Management - tests");
