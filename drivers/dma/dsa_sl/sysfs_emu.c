#include <linux/kobject.h>
#include <linux/fs.h>
#include <linux/dsa_emu.h>
#include <linux/fbg.h>
#include <linux/ctype.h>
#include <linux/cache.h>

#include "dsa_emu.h"
#include "folio_pool.h"
#include "sysfs_emu.h"

#define NUM_THREADS_DEFAULT DSA_EMU_BG_WQ_COUNT

bool wq_inited __read_mostly = false;
uint32_t num_threads __read_mostly = NUM_THREADS_DEFAULT;
bool dsa_emu_enable_bdp __read_mostly = 0;
uint32_t dsa_emu_force_node __read_mostly = 0;
uint32_t dsa_emu_force_node_nid __read_mostly = 0;
uint32_t dsa_emu_prefetch __read_mostly = 0;
uint32_t dsa_emu_nt_store __read_mostly = 1;
uint32_t dsa_emu_nt_store_min_bytes __read_mostly = 256 * 1024;
uint32_t dsa_emu_no_zero_alloc __read_mostly = 0;
uint32_t dsa_emu_ignored_inode_min __read_mostly = 0;
uint32_t dsa_emu_ignored_inode_max __read_mostly = 0;
uint32_t dsa_emu_debug_origin_bdp_mode __read_mostly = 0;
uint32_t dsa_emu_debug_origin_bdp_fg_wb_nrpages __read_mostly = 0;
uint32_t dsa_emu_debug_bg_fsync __read_mostly = 0;
uint32_t dsa_emu_debug_defer_blk_write_begin __read_mostly = 0;
/*
 * Run a_ops->blk_write_begin in the foreground for ALL entries, not just
 * unsafe ones.  FG iterates entries in file order, so delayed-allocation
 * reservations land in the extent-status tree in order and merge into large
 * delayed extents.  With safe-entry begins on the concurrent BG workers the
 * reservations interleave and the es tree fragments; writeback/fsync then
 * pays one journal handle + ext4_map_blocks per fragment.
 */
uint32_t dsa_emu_fg_bwb_all __read_mostly = 0;
/*
 * Share one jbd2 handle across all write_end calls of a BG request
 * (a_ops->bg_batch_begin/end) instead of one handle per entry.
 */
uint32_t dsa_emu_bg_batch_handle __read_mostly = 0;
/*
 * Drain the inode's BG backlog before acting on a sync_file_range
 * write hint, so the hint covers the full contiguous dirty range
 * instead of leaving per-request islands for the next fsync.
 */
uint32_t dsa_emu_sfr_drain __read_mostly = 0;
/* -1 = spread BG kthreads round-robin across NUMA nodes; N = pin all to
 * node N. */
int dsa_emu_thread_numa __read_mostly = -1;
uint32_t dsa_emu_debug_dirty_threshold __read_mostly = DSA_EMU_DEBUG_DIRTY_THRESHOLD_DEFAULT;
uint32_t dsa_emu_fsync_skip __read_mostly = 0;
uint32_t dsa_emu_naive_mode __read_mostly = 0;
uint32_t dsa_emu_folio_pool_order_max __read_mostly = 4;
uint32_t dsa_emu_disable_batching __read_mostly = 0;
uint32_t dsa_emu_fg_alloc_threshold __read_mostly =
	DSA_EMU_FG_ALLOC_THRESHOLD_DEFAULT;
uint32_t dsa_emu_sync_fallback_threshold __read_mostly = 0;
uint32_t dsa_emu_fsync_fallback_numerator __read_mostly = 1;
uint32_t dsa_emu_fsync_fallback_denominator __read_mostly = 2;
uint32_t dsa_emu_fsync_fallback_recovery_writes __read_mostly = 64;
uint32_t dsa_emu_backoff_threshold_ns __read_mostly = 0;
uint32_t dsa_emu_backoff_duration_ns __read_mostly = 1000000; /* 1ms */
uint32_t dsa_emu_worker_sched_idle __read_mostly = 0;

static ssize_t folio_pool_order_max_store_fn(const char *buf, size_t count)
{
	unsigned int order;

	sscanf(buf, "%u", &order);
	if (order > DSA_EMU_FOLIO_POOL_ORDER_MAX)
		order = DSA_EMU_FOLIO_POOL_ORDER_MAX;
	dsa_emu_folio_pool_order_max = order;

	return count;
}

static ssize_t static_folio_pool_size_store_fn(const char *buf, size_t count)
{
	unsigned int size;
	int ret;

	if (kstrtouint(buf, 0, &size))
		return -EINVAL;

	ret = dsa_emu_static_folio_pool_resize(size);
	if (ret)
		return ret;

	return count;
}

static ssize_t num_threads_store_fn(const char *buf, size_t count)
{
	unsigned int val;

	sscanf(buf, "%u", &val);
	if (val > DSA_EMU_NUM_THREADS_MAX)
		return count;

	dsa_emu_destroy_kthreads();

	/*
	 * Publish the new count before workers come up: queueing is cut off
	 * (wq_inited false, submissions run inline) until init completes, so
	 * dsa_emu_select_wq can never hand out an index in the old range
	 * that the new worker pool does not poll.
	 */
	num_threads = val;

	if (val > 0) {
		if (dsa_emu_init_kthreads(val)) {
			pr_err("Failed to init kthreads\n");
			return -EINVAL;
		}
	}

	return count;
}

SYSFS_UINT_ATTR_CUSTOM(num_threads, num_threads, num_threads_store_fn);
SYSFS_UINT_ATTR(poll_usecs, dsa_emu_poll_usecs);
SYSFS_UINT_ATTR(force_node_nid, dsa_emu_force_node_nid);
SYSFS_UINT_ATTR(nt_store_min_bytes, dsa_emu_nt_store_min_bytes);
SYSFS_UINT2_ATTR(ignored_inode, dsa_emu_ignored_inode_min,
		 dsa_emu_ignored_inode_max);
SYSFS_UINT_ATTR(debug_dirty_threshold, dsa_emu_debug_dirty_threshold);
SYSFS_UINT_ATTR_CUSTOM(folio_pool_order_max, dsa_emu_folio_pool_order_max,
		       folio_pool_order_max_store_fn);
SYSFS_UINT_ATTR_CUSTOM(static_folio_pool_size,
		       dsa_emu_static_folio_pool_size,
		       static_folio_pool_size_store_fn);

SYSFS_STRING_ARRAY(enable_bdp, dsa_emu_enable_bdp, "bdp_off", "bdp_on");
SYSFS_STRING_ARRAY(force_node, dsa_emu_force_node, "off", "prefer", "force");
SYSFS_STRING_ARRAY(prefetch, dsa_emu_prefetch, "off", "on");
SYSFS_STRING_ARRAY(nt_store, dsa_emu_nt_store, "off", "on");
SYSFS_STRING_ARRAY(no_zero_alloc, dsa_emu_no_zero_alloc, "off", "pbw", "all");
SYSFS_STRING_ARRAY(debug_bg_fsync, dsa_emu_debug_bg_fsync, "off", "on", "auto");
SYSFS_STRING_ARRAY(debug_defer_blk_write_begin,
		   dsa_emu_debug_defer_blk_write_begin, "off", "on");
SYSFS_STRING_ARRAY(fg_bwb_all, dsa_emu_fg_bwb_all, "off", "on");
SYSFS_STRING_ARRAY(bg_batch_handle, dsa_emu_bg_batch_handle, "off", "on");
SYSFS_STRING_ARRAY(sfr_drain, dsa_emu_sfr_drain, "off", "on");

static ssize_t dsa_emu_thread_numa_show(struct kobject *kobj,
					struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", dsa_emu_thread_numa);
}

static ssize_t dsa_emu_thread_numa_store(struct kobject *kobj,
					 struct kobj_attribute *attr,
					 const char *buf, size_t count)
{
	int val;

	if (sscanf(buf, "%d", &val) != 1)
		return -EINVAL;
	dsa_emu_thread_numa = val;
	return count;
}

static struct kobj_attribute dsa_emu_thread_numa_attr =
	__ATTR(dsa_emu_thread_numa, 0660, dsa_emu_thread_numa_show,
	       dsa_emu_thread_numa_store);
SYSFS_STRING_ARRAY(debug_fsync_skip, dsa_emu_fsync_skip, "off", "on");
SYSFS_STRING_ARRAY(naive_mode, dsa_emu_naive_mode, "off", "on");
SYSFS_STRING_ARRAY(disable_batching, dsa_emu_disable_batching, "off", "on");
SYSFS_UINT_ATTR(fg_alloc_threshold, dsa_emu_fg_alloc_threshold);
SYSFS_UINT_ATTR(sync_fallback_threshold, dsa_emu_sync_fallback_threshold);
SYSFS_UINT_ATTR(fsync_fallback_numerator,
		dsa_emu_fsync_fallback_numerator);
SYSFS_UINT_ATTR(fsync_fallback_denominator,
		dsa_emu_fsync_fallback_denominator);
SYSFS_UINT_ATTR(fsync_fallback_recovery_writes,
		dsa_emu_fsync_fallback_recovery_writes);
SYSFS_UINT_ATTR(backoff_threshold_ns, dsa_emu_backoff_threshold_ns);
SYSFS_UINT_ATTR(backoff_duration_ns, dsa_emu_backoff_duration_ns);
SYSFS_UINT_ATTR(worker_sched_idle, dsa_emu_worker_sched_idle);

SYSFS_STRING_ARRAY(debug_origin_bdp_mode, dsa_emu_debug_origin_bdp_mode,
		   "normal", "no_wb", "fg_wb");
SYSFS_UINT_ATTR(debug_origin_bdp_fg_wb_nrpages,
		dsa_emu_debug_origin_bdp_fg_wb_nrpages);

static struct attribute *dsa_emu_attrs[] = {
	&num_threads_attr.attr,
	&poll_usecs_attr.attr,
	&enable_bdp_attr.attr,
	&force_node_attr.attr,
	&force_node_nid_attr.attr,
	&prefetch_attr.attr,
	&nt_store_attr.attr,
	&nt_store_min_bytes_attr.attr,
	&no_zero_alloc_attr.attr,
	&ignored_inode_attr.attr,
	&debug_origin_bdp_mode_attr.attr,
	&debug_origin_bdp_fg_wb_nrpages_attr.attr,
	&debug_bg_fsync_attr.attr,
	&debug_defer_blk_write_begin_attr.attr,
	&fg_bwb_all_attr.attr,
	&bg_batch_handle_attr.attr,
	&sfr_drain_attr.attr,
	&dsa_emu_thread_numa_attr.attr,
	&debug_dirty_threshold_attr.attr,
	&debug_fsync_skip_attr.attr,
	&naive_mode_attr.attr,
	&disable_batching_attr.attr,
	&fg_alloc_threshold_attr.attr,
	&sync_fallback_threshold_attr.attr,
	&fsync_fallback_numerator_attr.attr,
	&fsync_fallback_denominator_attr.attr,
	&fsync_fallback_recovery_writes_attr.attr,
	&backoff_threshold_ns_attr.attr,
	&backoff_duration_ns_attr.attr,
	&worker_sched_idle_attr.attr,
	&folio_pool_order_max_attr.attr,
	&static_folio_pool_size_attr.attr,
	NULL
};

static const struct attribute_group dsa_emu_attr_group = {
	.attrs = dsa_emu_attrs,
};

static struct kobject *dsa_emu_kobj;
int dsa_emu_init_sysfs(void)
{
	dsa_emu_kobj = kobject_create_and_add("dsa_emu", fs_kobj);
	if (!dsa_emu_kobj) {
		pr_err("Failed to create kobject\n");
		return -ENOMEM;
	}

	int ret = sysfs_create_group(dsa_emu_kobj, &dsa_emu_attr_group);
	if (ret) {
		pr_err("Failed to create sysfs group\n");
		kobject_put(dsa_emu_kobj);
		return ret;
	}

	return 0;
}
