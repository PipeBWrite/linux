#include "linux/bvec.h"
#include "linux/gfp.h"
#include "linux/highmem.h"
#include "linux/sched.h"
#include "linux/spinlock_types.h"
#include <linux/types.h>
#include <linux/signal.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/sched/cputime.h>
#include <linux/dsa_sl.h>
#include <linux/uio.h>
#include <linux/fs.h>
#include <linux/pagemap_bg.h>
#include <linux/stats.h>
#include <asm-generic/cacheflush.h>
#include <linux/fbg.h>
#include <linux/swap.h>
#include <linux/percpu.h>
#include <linux/smp.h>
#include <linux/topology.h>
#include <linux/nodemask.h>
#include <linux/slab.h>
#include <linux/fbg.h>
#include <linux/wait_bit.h>
#include <linux/workqueue.h>

#include "dsa_emu.h"
#include "folio_pool.h"
#include "sysfs_emu.h"

#ifdef CONFIG_MISC_STATS
#define DSA_EMU_STAT_INC(name) this_cpu_inc(stats_##name##_count)
#define DSA_EMU_STAT_ADD(name, count) \
	this_cpu_add(stats_##name##_count, count)
#else
#define DSA_EMU_STAT_INC(name) \
	do {                   \
	} while (0)
#define DSA_EMU_STAT_ADD(name, count) \
	do {                         \
	} while (0)
#endif

#define REQ_SIZE 1024
#define REQ_POOL_SLOTS (REQ_SIZE + 1)  /* One extra slot for SPSC empty/full distinction */
#define LOOP_WAIT_MAX 50
#define NUM_THREADS_MAX DSA_EMU_NUM_THREADS_MAX
/* Quiet idle BG workers while foreground orig fallback is active. */
#define DSA_EMU_BACKOFF_POLL_USECS 100000
static struct task_struct *kthreads[NUM_THREADS_MAX];
static struct workqueue_struct *dsa_emu_flush_wq;

static inline void dsa_emu_record_pending_submit_depth(int pending_after,
						       int submitted_ents)
{
	u64 depth_before;
	u64 samples;
	u64 weighted_sum;

	if (unlikely(submitted_ents <= 0))
		return;

	/*
	 * i_bg_pending is entry-based. For batched submit, account the
	 * conceptual depths seen by each submitted entry: old+1 ... old+n.
	 */
	samples = submitted_ents;
	depth_before = pending_after > submitted_ents ?
			       pending_after - submitted_ents :
			       0;
	weighted_sum = samples * depth_before + samples * (samples + 1) / 2;

	DSA_EMU_STAT_ADD(bg_pending_submit_depth_sum, weighted_sum);
	DSA_EMU_STAT_ADD(bg_pending_submit_depth_samples, samples);
}

struct dsa_emu_flush_work {
	struct work_struct work;
	struct inode *inode;
};

/* progress-gated backoff: consecutive stalled+backlogged batches before backing off */
#define DSA_EMU_BACKOFF_BAD_BATCHES 2

struct dsa_emu_kthread_queue {
	struct llist_head head;
	wait_queue_head_t wait;		/* wake-on-submit (sleep_policy=waitqueue) */
	int backoff;			/* BG sets when overloaded; FG reads */
	u64 backoff_until_ns;		/* ktime deadline before clearing */
	u32 stall_streak;		/* consecutive stalled+backlogged batches */
} ____cacheline_aligned_in_smp;

static struct dsa_emu_kthread_queue kthread_queue[NUM_THREADS_MAX];
static bool dsa_emu_orig_fallback_active;

static void dsa_emu_flush_workfn(struct work_struct *work)
{
	struct dsa_emu_flush_work *flush_work =
		container_of(work, struct dsa_emu_flush_work, work);
	struct inode *inode = flush_work->inode;
	struct address_space *mapping = inode->i_mapping;
	int ret;

	ret = filemap_flush(mapping);
	if (ret)
		pr_err("dsa_emu: async filemap_flush failed: %d\n", ret);

	atomic_set(&inode->i_bg_fsync_lock, 0);
	iput(inode);
	kfree(flush_work);
}

static bool dsa_emu_queue_flush_work(struct inode *inode)
{
	struct dsa_emu_flush_work *flush_work;

	if (unlikely(!dsa_emu_flush_wq))
		return false;

	flush_work = kmalloc(sizeof(*flush_work), GFP_KERNEL);
	if (!flush_work)
		return false;

	ihold(inode);
	flush_work->inode = inode;
	INIT_WORK(&flush_work->work, dsa_emu_flush_workfn);

	if (!queue_work(dsa_emu_flush_wq, &flush_work->work)) {
		iput(inode);
		kfree(flush_work);
		return false;
	}

	return true;
}

static bool dsa_emu_any_backoff_active(void)
{
	int i;
	int n = READ_ONCE(num_threads);

	for (i = 0; i < n; i++) {
		if (READ_ONCE(kthread_queue[i].backoff))
			return true;
	}

	return false;
}

static inline void dsa_emu_set_backoff(struct dsa_emu_kthread_queue *q)
{
	WRITE_ONCE(q->backoff, 1);
	q->backoff_until_ns = ktime_get_ns() +
		READ_ONCE(dsa_emu_backoff_duration_ns);
	WRITE_ONCE(dsa_emu_orig_fallback_active, 1);
}

static inline void dsa_emu_clear_backoff(struct dsa_emu_kthread_queue *q)
{
	WRITE_ONCE(q->backoff, 0);
	if (!dsa_emu_any_backoff_active())
		WRITE_ONCE(dsa_emu_orig_fallback_active, 0);
}

static inline u32 dsa_emu_idle_poll_usecs(void)
{
	u32 poll_usecs = READ_ONCE(dsa_emu_poll_usecs);

	if (READ_ONCE(dsa_emu_orig_fallback_active) &&
	    poll_usecs < DSA_EMU_BACKOFF_POLL_USECS)
		return DSA_EMU_BACKOFF_POLL_USECS;

	return poll_usecs;
}

static inline void dsa_emu_enter_submit_backoff(int wq_idx)
{
	struct dsa_emu_kthread_queue *q;

	if (!READ_ONCE(dsa_emu_backoff_threshold_ns))
		return;
	if (wq_idx < 0 || wq_idx >= num_threads)
		return;

	q = &kthread_queue[wq_idx];
	dsa_emu_set_backoff(q);
}

static int dsa_emu_init_flush_worker(bool *created)
{
	if (dsa_emu_flush_wq) {
		*created = false;
		return 0;
	}

	dsa_emu_flush_wq = alloc_ordered_workqueue("dsa_emu_flush", WQ_MEM_RECLAIM);
	if (!dsa_emu_flush_wq)
		return -ENOMEM;

	*created = true;
	return 0;
}

static void dsa_emu_destroy_flush_worker(void)
{
	if (!dsa_emu_flush_wq)
		return;

	destroy_workqueue(dsa_emu_flush_wq);
	dsa_emu_flush_wq = NULL;
}

#include <linux/writeback.h>
#include <linux/backing-dev.h>
#include <linux/dsa_emu.h>

struct dsa_emu_req_pool {
	/* Consumer's cache line - only local CPU with preempt_disable */
	struct {
		u32 head;
		u32 cached_tail;  /* Local copy of tail for fast empty check */
	} ____cacheline_aligned_in_smp get;
	/*
	 * Producer's cache line.  NOT single-producer: requests from one FG
	 * CPU are processed by different BG workers (o-list target_wq hints)
	 * and by the FG sync-fallback path, so concurrent puts to the same
	 * pool are possible and the tail update must be serialized.
	 */
	struct {
		spinlock_t lock;
		u32 tail;
		u32 cached_head;  /* Local copy of head for fast full check */
	} ____cacheline_aligned_in_smp put;
	struct dsa_emu_memcpy_req *reqs[REQ_POOL_SLOTS];
	struct dsa_emu_memcpy_req *prev_req;
} ____cacheline_aligned;

static DEFINE_PER_CPU(struct dsa_emu_req_pool, req_pool);
static int wq_cpu[NUM_THREADS_MAX] __read_mostly;

/*
 * Submitters inside the wq_inited-check -> llist_add window.  Teardown
 * clears wq_inited, waits for this to reach zero, and only then stops
 * the workers and drains the queues - so a request can never be added
 * to a queue no worker will ever poll.  Requests submitted while no
 * workers exist are processed synchronously in the caller instead.
 */
static atomic_t dsa_emu_submit_inflight = ATOMIC_INIT(0);

/* Poll interval in microseconds for kthread workers */
#define DSA_EMU_NUMA_NODES 4

static int cpu_to_wq[NR_CPUS] __read_mostly;

static int dsa_emu_numa_nodes[DSA_EMU_NUMA_NODES];
static int dsa_emu_numa_node_count __read_mostly = 1;

uint32_t dsa_emu_poll_usecs __read_mostly = 50;

/* NUMA-aware BG kthread placement via the dsa_emu_thread_numa sysfs knob. */
static void dsa_emu_init_numa_nodes(unsigned int num_wqs)
{
	int nid;
	int count = 0;
	unsigned int target = DSA_EMU_NUMA_NODES;

	if (num_wqs && num_wqs < target)
		target = num_wqs;
	if (!target)
		target = 1;

	for_each_online_node(nid) {
		dsa_emu_numa_nodes[count++] = nid;
		if (count == target)
			break;
	}

	if (!count) {
		dsa_emu_numa_nodes[0] = 0;
		count = 1;
	}

	WRITE_ONCE(dsa_emu_numa_node_count, count);
}

static int dsa_emu_pick_node_cpu(int nid, int local_idx)
{
	const struct cpumask *mask = cpumask_of_node(nid);
	int cpu_count = cpumask_weight(mask);
	int target;
	int cpu;

	if (!cpu_count)
		return -1;

	target = local_idx % cpu_count;
	cpu = cpumask_first(mask);
	for (int i = 0; i < target; i++)
		cpu = cpumask_next(cpu, mask);

	return cpu;
}

static void dsa_emu_rebuild_cpu_to_wq(uint32_t num_wqs)
{
	int cpu;
	int nid;

	if (!num_wqs) {
		for_each_possible_cpu(cpu)
			WRITE_ONCE(cpu_to_wq[cpu], 0);
		return;
	}

	/* Fallback for CPUs whose node has no worker bound to it. */
	for_each_possible_cpu(cpu)
		WRITE_ONCE(cpu_to_wq[cpu], cpu % num_wqs);

	/*
	 * Map each CPU to a BG worker bound to the SAME NUMA node,
	 * round-robin among the on-node workers by the CPU's rank within
	 * its node.  Keeps the FG->BG handoff and folio access NUMA-local
	 * for any worker count / thread_numa placement.
	 */
	for_each_online_node(nid) {
		const struct cpumask *mask = cpumask_of_node(nid);
		int on_node_wqs[NUM_THREADS_MAX];
		int n_on_node = 0;
		int local_idx = 0;

		for (int i = 0; i < num_wqs && i < NUM_THREADS_MAX; i++) {
			int wcpu = READ_ONCE(wq_cpu[i]);

			if (wcpu >= 0 && wcpu < nr_cpu_ids &&
			    cpu_to_node(wcpu) == nid)
				on_node_wqs[n_on_node++] = i;
		}
		if (!n_on_node)
			continue;	/* no worker on this node: keep fallback */

		/*
		 * Order on_node_wqs[] by ascending pinned CPU so both the
		 * default round-robin and the mirror (i -> n-1-i) map by
		 * actual CPU position, not the worker-index order the array
		 * was built in.  One-worker-per-core layouts are already
		 * sorted; insertion sort is cheap (n_on_node <= per-node CPUs).
		 */
		for (int a = 1; a < n_on_node; a++) {
			int key = on_node_wqs[a];
			int kc = READ_ONCE(wq_cpu[key]);
			int b = a - 1;

			while (b >= 0 &&
			       READ_ONCE(wq_cpu[on_node_wqs[b]]) > kc) {
				on_node_wqs[b + 1] = on_node_wqs[b];
				b--;
			}
			on_node_wqs[b + 1] = key;
		}

		bool mirror = READ_ONCE(dsa_emu_cpu_to_wq_mirror);

		for_each_cpu(cpu, mask) {
			int slot;

			if (cpu >= nr_cpu_ids || !cpu_online(cpu))
				continue;
			slot = local_idx % n_on_node;
			/*
			 * mirror: map the i-th on-node CPU to the (n-1-i)-th
			 * on-node worker, so a submitter and its default BG
			 * worker sit at opposite ends of the node (e.g. node-0
			 * CPU0 -> CPU76 with one worker per core).
			 */
			if (mirror)
				slot = n_on_node - 1 - slot;
			WRITE_ONCE(cpu_to_wq[cpu], on_node_wqs[slot]);
			local_idx++;
		}
	}
}

void dsa_emu_rebuild_cpu_to_wq_pub(void)
{
	if (num_threads)
		dsa_emu_rebuild_cpu_to_wq(num_threads);
}

static inline void dsa_emu_reset_req(struct dsa_emu_memcpy_req *req, bool from_pool)
{
	req->mapping = NULL;
	req->file = NULL;
	req->n_ents = 0;
	req->start_index = 0;
	req->end_index = 0;
	req->lnode.next = NULL;
	req->from_pool = from_pool;
	req->target_wq_idx = 0;
	req->fsync = false;
	req->write_fn = NULL;
}

static void init_req_pool(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		struct dsa_emu_req_pool *pool = per_cpu_ptr(&req_pool, cpu);

		for (int j = 0; j < REQ_SIZE; j++) {
			pool->reqs[j] = kmalloc(sizeof(struct dsa_emu_memcpy_req),
						GFP_KERNEL);
			if (!pool->reqs[j]) {
				pr_err("Failed to allocate req pool\n");
				BUG();
			}
			dsa_emu_reset_req(pool->reqs[j], true);
			pool->reqs[j]->pool_cpu = cpu;
		}
		/*
		 * SPSC-style ring buffer initialization:
		 * - head = 0: consumer starts reading at slot 0
		 * - tail = REQ_SIZE: producer has "written" REQ_SIZE items (slots 0..REQ_SIZE-1)
		 * - Pool starts full with REQ_SIZE items available
		 * - Empty when head == tail, full when (tail + 1) % REQ_POOL_SLOTS == head
		 */
		spin_lock_init(&pool->put.lock);
		pool->get.head = 0;
		pool->get.cached_tail = REQ_SIZE;
		pool->put.tail = REQ_SIZE;
		pool->put.cached_head = 0;
		pool->prev_req = NULL;
	}
	pr_info("Initialized req pool\n");
}

struct dsa_emu_memcpy_req *dsa_emu_get_req_from_pool(int idx)
{
	struct dsa_emu_memcpy_req *req;
	struct dsa_emu_req_pool *pool;
	int loop_count = 0;
	int cpu;
	u32 head;

	(void)idx;
	preempt_disable();
	cpu = smp_processor_id();
	pool = this_cpu_ptr(&req_pool);
	head = READ_ONCE(pool->get.head);

	/* Check cached tail first - avoids cross-cacheline read most of the time */
	while (head == READ_ONCE(pool->get.cached_tail)) {
		/* Cache says empty, refresh from actual tail */
		pool->get.cached_tail = smp_load_acquire(&pool->put.tail);
		if (head == pool->get.cached_tail) {
			/* Actually empty, wait or fallback to kmalloc */
			loop_count++;
			if (loop_count > LOOP_WAIT_MAX) {
				preempt_enable();
				req = kcalloc(1, sizeof(struct dsa_emu_memcpy_req),
					      GFP_KERNEL);
				if (req) {
					dsa_emu_reset_req(req, false);
					req->pool_cpu = cpu;
				}
				return req;
			}
			cpu_relax();
		}
	}

	req = pool->reqs[head];
	dsa_emu_reset_req(req, true);
	req->pool_cpu = cpu;
	/* Release ensures the head update is visible after we've read the request */
	smp_store_release(&pool->get.head, (head + 1) % REQ_POOL_SLOTS);
	preempt_enable();
	return req;
}
EXPORT_SYMBOL(dsa_emu_get_req_from_pool);

static void put_req_to_pool(struct dsa_emu_memcpy_req *req, int cpu)
{
	struct dsa_emu_req_pool *pool;
	u32 tail, next_tail;

	if (cpu < 0 || cpu >= nr_cpu_ids)
		cpu = smp_processor_id();

	pool = per_cpu_ptr(&req_pool, cpu);

	/*
	 * Multiple producers can put to this pool concurrently (BG workers
	 * chosen by o-list target_wq hints + FG sync fallback).  Without the
	 * lock, a slow producer can store a stale next_tail and move tail
	 * BACKWARDS, after which the consumer re-serves already-issued slots:
	 * one request object then circulates twice, its second llist_add
	 * severs the first worker queue's chain, and the lost requests leave
	 * i_bg_pending stuck forever (observed as fsync/dsa_emu_drain hangs
	 * on XFS).
	 */
	spin_lock(&pool->put.lock);
	tail = READ_ONCE(pool->put.tail);
	next_tail = (tail + 1) % REQ_POOL_SLOTS;

	/* Check if full using cached head - avoids cross-cacheline read */
	if (next_tail == pool->put.cached_head) {
		/* Cache says full, refresh from actual head */
		pool->put.cached_head = smp_load_acquire(&pool->get.head);
		if (next_tail == pool->put.cached_head) {
			/* Actually full - shouldn't happen with correct sizing */
			spin_unlock(&pool->put.lock);
			pr_warn_once("Request pool full, freeing request\n");
			kfree(req);
			return;
		}
	}

	pool->reqs[tail] = req;
	/* Release ensures the request pointer is visible before tail update */
	smp_store_release(&pool->put.tail, next_tail);
	spin_unlock(&pool->put.lock);
}

void dsa_emu_put_req(struct dsa_emu_memcpy_req *req)
{
	struct file *file;
	int pool_cpu;

	if (!req)
		return;

	file = req->file;
	pool_cpu = req->pool_cpu;
	dsa_emu_reset_req(req, true);
	req->pool_cpu = pool_cpu;
	if (file)
		fput(file);
	put_req_to_pool(req, pool_cpu);
}
EXPORT_SYMBOL(dsa_emu_put_req);

size_t dsa_emu_copy_page_from_iter_atomic(struct page *page, size_t offset,
					  size_t bytes, struct iov_iter *i)
{
	size_t n, copied = 0;

	if (!page_copy_sane(page, offset, bytes)) {
		pr_err("Not a sane copy\n");
		return 0;
	}
	if (WARN_ON_ONCE(!i || !i->data_source))
		return 0;

	do {
		char *p;

		n = bytes - copied;
		if (PageHighMem(page)) {
			page += offset / PAGE_SIZE;
			offset %= PAGE_SIZE;
			n = min_t(size_t, n, PAGE_SIZE - offset);
		}

		p = kmap_atomic(page) + offset;
		n = __dsa_emu_copy_from_iter(p, n, i);
		kunmap_atomic(p);
		copied += n;
		offset += n;
	} while (PageHighMem(page) && copied != bytes && n > 0);

	return copied;
}
EXPORT_SYMBOL(dsa_emu_copy_page_from_iter_atomic);

/*
 * ext4-specific background write function.
 * Static folio replenishment is handled by dsa_emu_process_req_entries().
 */
static int dsa_emu_ext4_write_entry(struct dsa_emu_memcpy_req *req,
				    struct dsa_emu_req_entry *ent)
{
	struct address_space *mapping = req->mapping;
	struct file *file = req->file;
	struct page *page = ent->page;
	unsigned long bytes = ent->bytes;
	loff_t pos = ent->pos;
	void *fsdata = ent->fsdata;
	long copied = ent->copied;
	bool initial_alloc = ent->initial_alloc;
	long status;
	const struct address_space_operations *aops = mapping->a_ops;
	struct folio *folio;
	bool full_folio_write;
	if (page) {
		folio = page_folio(page);
	} else {
		pr_warn("%s: No page supplied!\n", __func__);
		BUG();
	}

	/*
	 * Note: Static folio replenishment is now handled by the generic
	 * dsa_emu_process_req_entries() function after this callback returns.
	 */

	if (folio->mapping != mapping) {
		pr_warn("folio->mapping != mapping: folio=%px mapping=%px expected=%px\n",
			folio, folio->mapping, mapping);
		pr_warn("initial_alloc=%d index=%lu pos=%lld bytes=%lu\n",
			initial_alloc, ent->index, pos, bytes);
		BUG();
	}
	full_folio_write = copied == bytes &&
			   offset_in_folio(folio, pos) == 0 &&
			   bytes == folio_size(folio);

	fgf_t fgp_flags = FGP_WRITEBEGIN;
	gfp_t gfp = mapping_gfp_mask(mapping);
	if ((fgp_flags & FGP_WRITE) && mapping_can_writeback(mapping))
		gfp |= __GFP_WRITE;
	if (fgp_flags & FGP_NOFS)
		gfp &= ~__GFP_FS;
	if (fgp_flags & FGP_NOWAIT) {
		gfp &= ~GFP_KERNEL;
		gfp |= GFP_NOWAIT | __GFP_NOWARN;
	}

	bool is_head = page == &folio->page;

	if (!initial_alloc || !is_head) {
		struct address_space *mapping_before_lock = folio->mapping;

		folio_lock(folio);

		/* Check if folio was truncated/invalidated while we waited for lock */
		if (!folio->mapping) {
			pr_err("BUG DEBUG: folio %px mapping became NULL after lock (was %px, expected %px)\n",
			       folio, mapping_before_lock, mapping);
			pr_err("BUG DEBUG: initial_alloc=%d is_head=%d index=%lu pos=%lld bytes=%lu\n",
			       initial_alloc, is_head, ent->index, pos, bytes);
			folio_unlock(folio);
			/* Skip this entry - folio was invalidated */
			return -ESTALE;
		}
		if (folio->mapping != mapping) {
			pr_err("BUG DEBUG: folio %px mapping changed after lock: %px -> expected %px\n",
			       folio, folio->mapping, mapping);
			pr_err("BUG DEBUG: initial_alloc=%d is_head=%d index=%lu pos=%lld bytes=%lu\n",
			       initial_alloc, is_head, ent->index, pos, bytes);
			folio_unlock(folio);
			return -ESTALE;
		}

		if (folio_test_idle(folio))
			folio_clear_idle(folio);

		folio_wait_stable(folio);

		goto next;
	}

		int err;
		unsigned long index = ent->index;

		if (full_folio_write)
			err = filemap_add_folio_test_dirty(mapping, folio, index,
							   gfp);
		else
			err = filemap_add_folio_test(mapping, folio, index, gfp);
		if (err) {
			pr_err("filemap_add_folio_test%s failed: err=%d index=%lu inode=%lu\n",
			       full_folio_write ? "_dirty" : "", err,
			       ent->index, mapping->host->i_ino);
			return err;
		}

	if (folio_test_idle(folio))
		folio_clear_idle(folio);

	folio_wait_stable(folio);

next:;

	int ret = 0;

	if (!(ent->flags & DSA_EMU_REQ_F_FG_BWB)) {
		ret = aops->blk_write_begin(folio, pos, bytes);
		if (ret != 0)
			pr_err("a_ops->blk_write_begin failed: %d\n", ret);
	}

	if (unlikely(ret)) {
		pr_err("BG: blk_write_begin failed (%d), still calling write_end - potential data loss! ino=%lu pos=%lld bytes=%lu\n",
		       ret, mapping->host->i_ino, pos, bytes);
	}

	status = aops->write_end(file, mapping, pos, bytes, copied, page,
				 fsdata);

	if (unlikely(status <= 0)) {
		pr_err("BG write_end status=%ld (expected %lu): ino=%lu pos=%lld bytes=%lu init=%d bwb_ret=%d\n",
		       status, copied, mapping->host->i_ino, pos, bytes,
		       initial_alloc, ret);
	}

	return 0;
}

static void process_req(struct dsa_emu_memcpy_req *req)
{
	dsa_emu_process_req_entries(req, dsa_emu_ext4_write_entry);
}

/*
 * Generic request processing function for background writes.
 * Handles all common operations: entry iteration, busy waiting, o-list release,
 * static folio replenishment, i_copy cleanup, fsync, balance_dirty_pages, iput, cleanup.
 *
 * The write_fn callback is called for each entry that needs filesystem-specific
 * write handling (i.e., entries with a page that aren't merged).
 */
void dsa_emu_process_req_entries(struct dsa_emu_memcpy_req *req,
				 dsa_emu_write_entry_fn_t write_fn)
{
	struct address_space *mapping = req->mapping;
	struct inode *inode = mapping->host;
	struct file *file = req->file;
	const struct address_space_operations *aops = mapping->a_ops;
	int total_ent = req->n_ents;
	int pending_ents = total_ent;
	struct dsa_emu_req_entry *ents = req->entries;
	long off_min = LONG_MAX, off_max = 0;
	bool handled = false;
	void *batch_cookie = NULL;
	TEST_BEGIN_AALWAYS();

	if (unlikely(WARN_ON_ONCE(pending_ents <= 0)))
		pending_ents = 1;

	/*
	 * Share one fs-side batch context (ext4: a single jbd2 handle) across
	 * all write_end calls of this request instead of paying one handle
	 * start/stop per entry.  The handle lives in current->journal_info,
	 * so the per-entry ext4_journal_start in ext4_da_do_write_end nests
	 * for free (h_ref++).  Stopped before the i_bg_pending decrement so
	 * drain waiters never observe completion while we still hold it.
	 */
	if (READ_ONCE(dsa_emu_bg_batch_handle) && aops->bg_batch_begin)
		batch_cookie = aops->bg_batch_begin(inode);

	for (int i = 0; i < total_ent; i++) {
		struct dsa_emu_req_entry *ent = &ents[i];
		int write_err = 0;

		/* Call filesystem-specific write function if entry has a page */
		if (ent->page) {
			/* Track dirty range for fsync */
			if (off_min > ent->pos)
				off_min = ent->pos;
			if (off_max < ent->pos + ent->bytes)
				off_max = ent->pos + ent->bytes;

			write_err = write_fn(req, ent);
			if (!write_err)
				handled = true;
		}

		/* Release o-list entry if this entry was linked. */
		if (ent->onode) {
			struct ongoing_node *node = ent->onode;

			/* Mark done; FG cleans up under write-side serialization. */
			WRITE_ONCE(node->done, true);

			ent->onode = NULL;
		}
		if (ent->page) {
			struct folio *f = page_folio(ent->page);

			if (unlikely(!f->mapping || f->mapping != mapping)) {
					pr_warn_ratelimited("dsa_emu: release olist idx %lu folio %px mapping %px expected %px initial_alloc=%d pos=%lld bytes=%u\n",
							    ent->index, f, f->mapping,
							    mapping, ent->initial_alloc,
							    ent->pos, ent->bytes);
			}
		}

		/* Replenish folio pools if needed */
		/*
		 * Check ent->page before calling page_folio to avoid NULL
		 * dereference. Also skip replenishment if write_fn failed
		 * (e.g., EEXIST) - the folio wasn't added to page cache.
		 */
		if (ent->initial_alloc && ent->page && !write_err &&
		    ent->page == &page_folio(ent->page)->page) {
			struct folio *new_f = NULL;
			gfp_t alloc_gfp = 0x101CCA;
			unsigned int order = 0;
			bool do_replenish = false;
			int pool_cpu = ent->folio_idx;

			if (dsa_emu_no_zero_alloc)
				alloc_gfp |= __GFP_SKIP_ZERO_BBN;

			if (dsa_emu_force_node == 2)
				alloc_gfp |= __GFP_THISNODE;

			if (ent->static_folio) {
				do_replenish = true;
			} else if (ent->folio_idx >= 0 &&
				   ent->order >=
					   DSA_EMU_FOLIO_POOL_ORDER_MIN &&
				   ent->order <=
					   dsa_emu_folio_pool_order_max_read()) {
				do_replenish = true;
				order = ent->order;
				alloc_gfp |= __GFP_NORETRY | __GFP_NOWARN;
			}

			if (do_replenish) {
				if (dsa_emu_force_node) {
					new_f = __folio_alloc_node(
						alloc_gfp, order,
						dsa_emu_force_node_nid);
				} else {
					new_f = folio_alloc(alloc_gfp, order);
				}

				if (new_f) {
					if (ent->static_folio)
						put_static_folio(new_f,
								 pool_cpu);
					else
						put_order_folio(new_f, order,
								pool_cpu);
				} else {
					pr_err("Failed to replenish folio order %u\n",
					       order);
				}
			}
		}
	}

	if (batch_cookie)
		aops->bg_batch_end(inode, batch_cookie);

	/*
	 * i_bg_pending tracks pending BG entries. Update it once per request
	 * completion so accounting is batched at req granularity.
	 *
	 * Complete drain before any potentially blocking operations. Drain
	 * waiters may hold journal handles (e.g., ext4_block_zero_page_range
	 * -> __filemap_get_folio -> dsa_emu_drain). If BG completion were
	 * blocked behind journal wait, we could deadlock.
	 */
	if (atomic_sub_and_test(pending_ents, &inode->i_bg_pending))
		complete_all(&inode->i_bg_drained);

	/* Background fsync handling (if enabled) */
	if (req->fsync && handled && file) {
		file->f_est_dirty_count += off_max - off_min;

		if (file->f_est_dirty_count > dsa_emu_debug_dirty_threshold) {
			if (atomic_cmpxchg(&inode->i_bg_fsync_lock, 0, 1) == 0) {
				if (dsa_emu_queue_flush_work(inode)) {
					file->f_est_dirty_count = 0;
				} else {
					pr_err_ratelimited("dsa_emu: failed to queue async flush work\n");
					atomic_set(&inode->i_bg_fsync_lock, 0);
				}
			}
		}
	}

	/* Common cleanup */
	if (dsa_emu_enable_bdp)
		balance_dirty_pages_ratelimited(mapping);

	dsa_emu_put_req(req);
}
EXPORT_SYMBOL(dsa_emu_process_req_entries);

/*
 * Synchronous fallback: process a request in the caller's (FG) context
 * when BG kthreads are overloaded.  Bumps i_bg_pending so that
 * dsa_emu_process_req_entries() can decrement it normally and signal
 * drain waiters.
 */
void dsa_emu_process_req_sync(struct dsa_emu_memcpy_req *req,
			      struct inode *inode)
{
	int pending_ents = req->n_ents;

	if (unlikely(WARN_ON_ONCE(pending_ents <= 0)))
		pending_ents = 1;

	DSA_EMU_STAT_INC(foreground_sync_fallback_calls);
	DSA_EMU_STAT_ADD(foreground_sync_fallback_entries, pending_ents);

	atomic_add(pending_ents, &inode->i_bg_pending);

	if (req->write_fn)
		dsa_emu_process_req_entries(req, req->write_fn);
	else
		process_req(req);
}
EXPORT_SYMBOL(dsa_emu_process_req_sync);

/*
 * Process a single request using either the stored write_fn callback
 * (for filesystems like XFS) or the legacy process_req (for ext4).
 */
static inline void dsa_emu_process_one_req(struct dsa_emu_memcpy_req *req)
{
	if (req->write_fn) {
		/* Use generic processing with filesystem-specific callback */
		dsa_emu_process_req_entries(req, req->write_fn);
	} else {
		/* Legacy ext4 path */
		process_req(req);
	}
}

static inline void
dsa_emu_process_req_sync_accounted(struct dsa_emu_memcpy_req *req,
				   int pending_ents)
{
	DSA_EMU_STAT_INC(submit_pressure_sync_req);
	DSA_EMU_STAT_ADD(submit_pressure_sync_ent, pending_ents);
	DSA_EMU_STAT_INC(foreground_sync_fallback_calls);
	DSA_EMU_STAT_ADD(foreground_sync_fallback_entries, pending_ents);
	dsa_emu_process_one_req(req);
}

static int dsa_emu_kthread_fn(void *arg)
{
	int idx = (long)arg;
	struct llist_node *node;
	struct dsa_emu_memcpy_req *req;
	struct dsa_emu_kthread_queue *q = &kthread_queue[idx];
	u32 idle_iters = 0;

	while (!kthread_should_stop()) {
		/*
		 * Grab all pending requests atomically.  Gate the atomic
		 * xchg behind a plain llist_empty() read: we are the sole
		 * consumer of this queue, so idle spinning/polling can probe
		 * with a shared load instead of an exclusive RMW that would
		 * ping-pong the head cacheline FG submitters write.
		 */
		node = llist_empty(&q->head) ? NULL : llist_del_all(&q->head);
		if (node) {
			u64 start_wall_ns = 0;
			u64 start_cpu_ns = 0;
			u64 threshold = READ_ONCE(dsa_emu_backoff_threshold_ns);
			int total_ents = 0;

			if (threshold) {
				start_wall_ns = ktime_get_ns();
				start_cpu_ns = task_sched_runtime(current);
			}

			/* Reverse to maintain FIFO order */
			node = llist_reverse_order(node);

			/* Process all requests in batch */
			struct dsa_emu_memcpy_req *tmp;
			llist_for_each_entry_safe(req, tmp, node, lnode) {
				total_ents += req->n_ents;
				TEST_BEGIN_AALWAYS();
				TIME_BEGIN_ALWAYS(bg_process_one_req);
				dsa_emu_process_one_req(req);
				TIME_END_ALWAYS(bg_process_one_req);
			}

			/*
			 * Contention backoff: compare wall time with this
			 * worker's actual scheduled runtime.  The gap is time
			 * the worker spent off-CPU or blocked, which is the
			 * signal that foreground work is competing with BG work.
			 *
			 * Also clears backoff early if a batch during
			 * the cooldown period shows contention is gone.
			 */
			if (threshold && total_ents) {
				u64 wall_ns = ktime_get_ns() - start_wall_ns;
				u64 cpu_ns = task_sched_runtime(current) -
					     start_cpu_ns;
				u64 stalled_ns = wall_ns > cpu_ns ?
						 wall_ns - cpu_ns : 0;
				u64 per_ent = stalled_ns / total_ents;
				bool stalled = per_ent > threshold;
				bool congested = false;

				/*
				 * A high stall alone is ambiguous: a SCHED_IDLE
				 * worker is off-CPU whenever the scheduler lets
				 * the app run, which is benign.  With the progress
				 * gate, treat it as congestion only if the queue
				 * ALSO failed to drain (backlog left after this
				 * batch) for >= 2 consecutive batches.  Draining
				 * to empty (idle path) resets the streak.
				 */
				if (READ_ONCE(dsa_emu_backoff_progress_gate)) {
					if (stalled && !llist_empty(&q->head))
						congested = (++q->stall_streak >=
							DSA_EMU_BACKOFF_BAD_BATCHES);
					else
						q->stall_streak = 0;
				} else {
					congested = stalled;
				}

				if (congested) {
					DSA_EMU_STAT_INC(backoff_enter);
					DSA_EMU_STAT_ADD(backoff_stall_ns,
							 stalled_ns);
					DSA_EMU_STAT_ADD(backoff_wall_ns,
							 wall_ns);
					DSA_EMU_STAT_ADD(backoff_cpu_ns,
							 cpu_ns);
					dsa_emu_set_backoff(q);
				} else if (!stalled && READ_ONCE(q->backoff)) {
					/* Contention gone — recover early */
					dsa_emu_clear_backoff(q);
				}
			}
			/* Busy-poll: skip sleep when work was available */
			idle_iters = 0;
			continue;
		}

		/* Drained to empty: caught up, so reset the stall streak. */
		q->stall_streak = 0;
		/* Clear backoff when idle and cooldown has expired. */
		if (READ_ONCE(q->backoff) &&
		    ktime_get_ns() >= q->backoff_until_ns)
			dsa_emu_clear_backoff(q);

		/* Sleep only when idle */
		switch (READ_ONCE(dsa_emu_sleep_policy)) {
		case DSA_EMU_SLEEP_WAITQUEUE:
			/*
			 * wake-on-submit: sleep on the per-queue waitqueue,
			 * woken by submitters on the empty->nonempty edge.
			 * Bounded timeout is a lost-wakeup safety net and lets
			 * the loop re-check backoff expiry.
			 */
			wait_event_interruptible_timeout(q->wait,
				!llist_empty(&q->head) || kthread_should_stop(),
				usecs_to_jiffies(1000));
			break;
		case DSA_EMU_SLEEP_HYBRID: {
			/*
			 * hot/warm/cold: spin briefly while work is recent,
			 * then short-sleep, then fall back to the waitqueue
			 * once sustained-idle.  Keeps 4 KB bursts low-latency
			 * (hot/warm poll) while still yielding the core and
			 * cutting wakeups when genuinely idle (cold wait).
			 */
			u32 hot = READ_ONCE(dsa_emu_hybrid_hot_iters);
			u32 warm = READ_ONCE(dsa_emu_hybrid_warm_iters);

			if (idle_iters < hot) {
				cpu_relax();
			} else if ((u64)idle_iters < (u64)hot + warm) {
				u32 poll_usecs = dsa_emu_idle_poll_usecs();

				usleep_range(poll_usecs, poll_usecs + 10);
			} else {
				wait_event_interruptible_timeout(q->wait,
					!llist_empty(&q->head) ||
						kthread_should_stop(),
					usecs_to_jiffies(1000));
			}
			idle_iters++;
			break;
		}
		default: {
			u32 poll_usecs = dsa_emu_idle_poll_usecs();

			usleep_range(poll_usecs, poll_usecs + 10);
			break;
		}
		}
	}

	return 0;
}

int dsa_emu_drain(struct inode *inode)
{
	int drain_pending;

	if (!filemap_bg_compatible(inode))
		return 0;

	/* Serialize drainers and block new submissions. */
	while (atomic_cmpxchg_release(&inode->i_draining, 0, 1) != 0)
		wait_var_event(&inode->i_draining,
			       !atomic_read(&inode->i_draining));

	/*
	 * Flush FG-side per-CPU unsent batches for this inode after we mark
	 * draining, so new submissions are blocked and the flush can account
	 * all pending work in i_bg_pending.
	 */
	filemap_bg_flush_inode_batches(inode, true);
	drain_pending = atomic_read(&inode->i_bg_pending);

	/* Wait for operations that became visible after flushing FG batches. */
	if (drain_pending > 0) {
		reinit_completion(&inode->i_bg_drained);
		/* Re-check after reinit to avoid missed wakeup */
		if (atomic_read(&inode->i_bg_pending) > 0)
			wait_for_completion(&inode->i_bg_drained);
	}

	/* Allow submissions again and wake waiting submitters */
	atomic_set_release(&inode->i_draining, 0);
	wake_up_var(&inode->i_draining);

	return drain_pending;
}
EXPORT_SYMBOL(dsa_emu_drain);

int dsa_emu_select_wq(int hint)
{
	int hash = hint;

	if (!num_threads)
		return 0;

	/*
	 * TODO: Assume workqueue indices track CPU numbering/NUMA layout
	 * (e.g., wq[0] -> CPU0/node0, wq[1] -> CPU1/node1, ...).
	 * Revisit if WQ placement changes.
	 */
	if (hash >= 0)
		return hash % num_threads;

	{
		int cpu = smp_processor_id();

		if (cpu >= 0 && cpu < NR_CPUS) {
			int wq = READ_ONCE(cpu_to_wq[cpu]);

			if (wq >= 0 && wq < num_threads)
				return wq;
		}
		return cpu % num_threads;
	}
}
EXPORT_SYMBOL(dsa_emu_select_wq);

bool dsa_emu_is_backed_off(int wq_idx)
{
	if (!READ_ONCE(dsa_emu_backoff_threshold_ns))
		return false;
	if (wq_idx < 0 || wq_idx >= num_threads)
		return false;
	return READ_ONCE(kthread_queue[wq_idx].backoff);
}
EXPORT_SYMBOL(dsa_emu_is_backed_off);

bool dsa_emu_should_use_orig_fallback(void)
{
	if (!READ_ONCE(dsa_emu_backoff_threshold_ns))
		return false;

	return READ_ONCE(dsa_emu_orig_fallback_active);
}
EXPORT_SYMBOL(dsa_emu_should_use_orig_fallback);

void dsa_emu_submit(struct dsa_emu_memcpy_req *req, struct inode *inode)
{
	int pending_ents = req->n_ents;
	int pending_after;
	uint32_t threshold;

	if (unlikely(WARN_ON_ONCE(pending_ents <= 0)))
		pending_ents = 1;

	DSA_EMU_STAT_INC(async_submit_single_calls);
	DSA_EMU_STAT_ADD(async_submit_single_entries, pending_ents);

retry:
	/* Wait if drain is in progress */
	if (unlikely(atomic_read_acquire(&inode->i_draining)))
		wait_var_event(&inode->i_draining, !atomic_read(&inode->i_draining));

	pending_after = atomic_add_return_relaxed(pending_ents,
						  &inode->i_bg_pending);

	/* Double-check: drain may have started after our read but before inc */
	if (unlikely(atomic_read(&inode->i_draining))) {
		/* Roll back - drain is active */
		if (atomic_sub_and_test(pending_ents, &inode->i_bg_pending))
			complete_all(&inode->i_bg_drained);
		goto retry;
	}

	dsa_emu_record_pending_submit_depth(pending_after, pending_ents);

	threshold = READ_ONCE(dsa_emu_sync_fallback_threshold);
	if (likely(READ_ONCE(wq_inited)) &&
	    unlikely(threshold && pending_after > (int)threshold)) {
		dsa_emu_enter_submit_backoff(req->target_wq_idx);
		dsa_emu_process_req_sync_accounted(req, pending_ents);
		return;
	}

	/* A req fresh from the pool has lnode.next == NULL (reset_req);
	 * anything else means the same req is circulating twice. */
	WARN_ONCE(req->lnode.next,
		  "dsa_emu: req %px resubmitted while still chained\n", req);
	atomic_inc(&dsa_emu_submit_inflight);
	if (likely(READ_ONCE(wq_inited))) {
		if (llist_add(&req->lnode,
			      &kthread_queue[req->target_wq_idx].head) &&
		    READ_ONCE(dsa_emu_sleep_policy))
			wake_up(&kthread_queue[req->target_wq_idx].wait);
		atomic_dec(&dsa_emu_submit_inflight);
	} else {
		atomic_dec(&dsa_emu_submit_inflight);
		/* No BG workers: pending already accounted, process inline. */
		dsa_emu_process_one_req(req);
	}
}
EXPORT_SYMBOL(dsa_emu_submit);

void dsa_emu_submit_in_drain(struct dsa_emu_memcpy_req *req,
			     struct inode *inode)
{
	int pending_ents = req->n_ents;
	int pending_after;

	if (unlikely(WARN_ON_ONCE(pending_ents <= 0)))
		pending_ents = 1;

	/*
	 * Draining already blocks new normal submissions. This helper is for
	 * forcing pre-existing FG batches into BG so drain can observe them.
	 */
	WARN_ON_ONCE(!atomic_read(&inode->i_draining));

	pending_after = atomic_add_return_relaxed(pending_ents,
						  &inode->i_bg_pending);
	dsa_emu_record_pending_submit_depth(pending_after, pending_ents);
	atomic_inc(&dsa_emu_submit_inflight);
	if (likely(READ_ONCE(wq_inited))) {
		if (llist_add(&req->lnode,
			      &kthread_queue[req->target_wq_idx].head) &&
		    READ_ONCE(dsa_emu_sleep_policy))
			wake_up(&kthread_queue[req->target_wq_idx].wait);
		atomic_dec(&dsa_emu_submit_inflight);
	} else {
		atomic_dec(&dsa_emu_submit_inflight);
		dsa_emu_process_one_req(req);
	}
}
EXPORT_SYMBOL(dsa_emu_submit_in_drain);

static int dsa_emu_submit_pending_ents(struct dsa_emu_memcpy_req *req)
{
	int pending_ents = req->n_ents;

	if (unlikely(WARN_ON_ONCE(pending_ents <= 0)))
		pending_ents = 1;

	return pending_ents;
}

static void dsa_emu_submit_batch_common(struct dsa_emu_memcpy_req **reqs,
					unsigned int nr, struct inode *inode,
					bool draining)
{
	struct llist_node *first;
	struct llist_node *last;
	int target_wq_idx;
	int total_pending_ents = 0;
	int pending_after;
	uint32_t threshold;
	unsigned int i;

	if (!nr)
		return;
	if (nr == 1) {
		if (draining)
			dsa_emu_submit_in_drain(reqs[0], inode);
		else
			dsa_emu_submit(reqs[0], inode);
		return;
	}

	target_wq_idx = reqs[0]->target_wq_idx;
	for (i = 0; i < nr; i++) {
		struct dsa_emu_memcpy_req *req = reqs[i];
		int pending_ents;

		if (unlikely(!req))
			return;
		if (unlikely(req->target_wq_idx != target_wq_idx)) {
			WARN_ON_ONCE(1);
			for (i = 0; i < nr; i++) {
				if (!reqs[i])
					continue;
				if (draining)
					dsa_emu_submit_in_drain(reqs[i], inode);
				else
					dsa_emu_submit(reqs[i], inode);
			}
			return;
		}
		WARN_ONCE(req->lnode.next,
			  "dsa_emu: req %px resubmitted while still chained\n",
			  req);
		pending_ents = dsa_emu_submit_pending_ents(req);
		total_pending_ents += pending_ents;
	}

	if (draining)
		WARN_ON_ONCE(!atomic_read(&inode->i_draining));

retry:
	if (!draining && unlikely(atomic_read_acquire(&inode->i_draining)))
		wait_var_event(&inode->i_draining, !atomic_read(&inode->i_draining));

	pending_after = atomic_add_return_relaxed(total_pending_ents,
						  &inode->i_bg_pending);

	if (!draining && unlikely(atomic_read(&inode->i_draining))) {
		if (atomic_sub_and_test(total_pending_ents, &inode->i_bg_pending))
			complete_all(&inode->i_bg_drained);
		goto retry;
	}

	dsa_emu_record_pending_submit_depth(pending_after, total_pending_ents);

	threshold = READ_ONCE(dsa_emu_sync_fallback_threshold);
	if (!draining && likely(READ_ONCE(wq_inited)) &&
	    unlikely(threshold && pending_after > (int)threshold)) {
		dsa_emu_enter_submit_backoff(target_wq_idx);
		for (i = 0; i < nr; i++) {
			struct dsa_emu_memcpy_req *req = reqs[i];

			dsa_emu_process_req_sync_accounted(
				req, dsa_emu_submit_pending_ents(req));
		}
		return;
	}

	atomic_inc(&dsa_emu_submit_inflight);
	if (likely(READ_ONCE(wq_inited))) {
		first = &reqs[nr - 1]->lnode;
		for (i = nr - 1; i > 0; i--)
			reqs[i]->lnode.next = &reqs[i - 1]->lnode;
		last = &reqs[0]->lnode;
		last->next = NULL;
		if (llist_add_batch(first, last,
				    &kthread_queue[target_wq_idx].head) &&
		    READ_ONCE(dsa_emu_sleep_policy))
			wake_up(&kthread_queue[target_wq_idx].wait);
		atomic_dec(&dsa_emu_submit_inflight);
	} else {
		atomic_dec(&dsa_emu_submit_inflight);
		/* No BG workers: pending already accounted, process inline. */
		for (i = 0; i < nr; i++)
			dsa_emu_process_one_req(reqs[i]);
	}
}

void dsa_emu_submit_batch(struct dsa_emu_memcpy_req **reqs, unsigned int nr,
			  struct inode *inode)
{
	unsigned int i;
	int total_pending_ents = 0;

	for (i = 0; i < nr; i++) {
		if (!reqs[i])
			break;
		total_pending_ents += dsa_emu_submit_pending_ents(reqs[i]);
	}

	DSA_EMU_STAT_INC(async_submit_batch_calls);
	DSA_EMU_STAT_ADD(async_submit_batch_reqs, i);
	DSA_EMU_STAT_ADD(async_submit_batch_entries, total_pending_ents);

	dsa_emu_submit_batch_common(reqs, nr, inode, false);
}
EXPORT_SYMBOL(dsa_emu_submit_batch);

void dsa_emu_submit_batch_in_drain(struct dsa_emu_memcpy_req **reqs,
				   unsigned int nr, struct inode *inode)
{
	dsa_emu_submit_batch_common(reqs, nr, inode, true);
}
EXPORT_SYMBOL(dsa_emu_submit_batch_in_drain);

int dsa_emu_init_kthreads(uint32_t num_kthreads)
{
	if (num_kthreads > DSA_EMU_NUM_THREADS_MAX) {
		pr_err("Invalid number of kthreads\n");
		return -EINVAL;
	}

	if (wq_inited) {
		pr_err("Threads already inited\n");
		return -EINVAL;
	}

	/*
	 * Refresh the online-node list so thread_numa == -1 can spread
	 * kthreads round-robin across every NUMA node.
	 */
	dsa_emu_init_numa_nodes(num_kthreads);
	WRITE_ONCE(dsa_emu_orig_fallback_active, 0);

	for (int i = 0; i < num_kthreads; i++) {
		int target_nid = READ_ONCE(dsa_emu_thread_numa);
		struct sched_param sched_idle_param = { .sched_priority = 0 };
		int cpu = -1;
		int nid;
		int ret;

		if (target_nid < 0) {
			int ncount = READ_ONCE(dsa_emu_numa_node_count);
			int rnid;

			if (ncount < 1)
				ncount = 1;
			rnid = dsa_emu_numa_nodes[i % ncount];
			cpu = dsa_emu_pick_node_cpu(rnid, i / ncount);
		} else if ((unsigned int)target_nid < nr_node_ids &&
			   node_online(target_nid)) {
			cpu = dsa_emu_pick_node_cpu(target_nid, i);
		} else {
			cpu = i;
		}

		if (cpu < 0 || cpu >= nr_cpu_ids || !cpu_online(cpu))
			cpu = cpumask_first(cpu_online_mask);
		nid = cpu_to_node(cpu);

		/* destroy_kthreads drains queues; anything here is a bug. */
		WARN_ON_ONCE(!llist_empty(&kthread_queue[i].head));
		init_llist_head(&kthread_queue[i].head);
		init_waitqueue_head(&kthread_queue[i].wait);
		WRITE_ONCE(kthread_queue[i].backoff, 0);
		kthread_queue[i].backoff_until_ns = 0;
		kthread_queue[i].stall_streak = 0;

		kthreads[i] = kthread_create(dsa_emu_kthread_fn,
					     (void *)(long)i,
					     "dsa_emu_kt_%d", i);
		if (IS_ERR(kthreads[i])) {
			pr_err("Failed to create kthread for DSA EMU\n");
			return PTR_ERR(kthreads[i]);
		}

		kthread_bind(kthreads[i], cpu);
		if (READ_ONCE(dsa_emu_worker_sched_idle)) {
			ret = sched_setscheduler_nocheck(kthreads[i], SCHED_IDLE,
							 &sched_idle_param);
			if (ret)
				pr_warn("dsa_emu: failed to set kthread%d SCHED_IDLE: %d\n",
					i, ret);
		}
		wq_cpu[i] = cpu;

		wake_up_process(kthreads[i]);
		pr_info("kthread%d: bound to CPU %d (node %d)\n", i, cpu, nid);
	}

	dsa_emu_rebuild_cpu_to_wq(num_kthreads);
	wq_inited = true;
	return 0;
}

void dsa_emu_destroy_kthreads(void)
{
	if (!wq_inited) {
		pr_err("Threads not inited\n");
		return;
	}

	/*
	 * Cut off queueing first: submitters that observe wq_inited==false
	 * process their requests inline; ones already past the check are
	 * inside the inflight window - wait them out so every queued
	 * request is visible to the drain below.
	 */
	WRITE_ONCE(wq_inited, false);
	smp_mb();
	while (atomic_read(&dsa_emu_submit_inflight))
		cpu_relax();

	for (int i = 0; i < num_threads; i++) {
		if (!IS_ERR_OR_NULL(kthreads[i])) {
			kthread_stop(kthreads[i]);
			kthreads[i] = NULL;
			pr_info("Stopped kthread %d\n", i);
		}
	}

	/*
	 * Process anything still queued.  Without this, init_llist_head on
	 * the next init silently discarded these requests, leaking their
	 * file references (unmountable fs), i_bg_pending counts (drain
	 * hangs) and o-list entries.
	 */
	for (int i = 0; i < num_threads; i++) {
		struct llist_node *node =
			llist_del_all(&kthread_queue[i].head);
		struct dsa_emu_memcpy_req *req, *tmp;
		int n = 0;

		node = llist_reverse_order(node);
		llist_for_each_entry_safe(req, tmp, node, lnode) {
			dsa_emu_process_one_req(req);
			n++;
		}
		if (n)
			pr_info("dsa_emu: drained %d leftover request(s) from queue %d on teardown\n",
				n, i);
	}
}

int dsa_emu_thread_init(void)
{
	int ret;
	bool flush_worker_created;

	ret = dsa_emu_init_flush_worker(&flush_worker_created);
	if (ret)
		return ret;

	ongoing_node_cache_init();

	ret = dsa_emu_init_kthreads(num_threads);
	if (ret && flush_worker_created) {
		dsa_emu_destroy_flush_worker();
	}
	if (ret)
		return ret;

#ifdef DSA_EMU_BG_ALLOC_THREAD
	init_folio_alloc_task();
#endif

#ifdef DSA_EMU_STATIC_FOLIO_POOL
	init_static_folio_pool();
#endif

	init_req_pool();

	return 0;
}

int dsa_emu_thread_stop(void)
{
	dsa_emu_destroy_kthreads();
	dsa_emu_destroy_flush_worker();
	return 0;
}
