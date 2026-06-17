#ifndef CUSTOM_STATS_H
#define CUSTOM_STATS_H

#include <linux/types.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include <linux/sched/clock.h>
#include <linux/percpu.h>
#include <asm/msr.h>

#define _MISC_STATS_BUCKET_NUM 20

#ifdef CONFIG_MISC_STATS

extern uint64_t _t_1;
extern uint64_t _t_2;

#define STATS_DEF_EXT_TIME_COUNTER(name)                    \
	DECLARE_PER_CPU(uint64_t, stats_total_time_##name); \
	DECLARE_PER_CPU(uint64_t, stats_##name##_count);

#define STATS_DEF_EXT_COUNTER_ONLY(name) \
	DECLARE_PER_CPU(uint64_t, stats_##name##_count);

#define STATS_DEF_EXT_TIME_BUCKET(name)                   \
	DECLARE_PER_CPU(uint64_t[_MISC_STATS_BUCKET_NUM], \
			stats_##name##_bucket);

extern const unsigned long bucket_size[_MISC_STATS_BUCKET_NUM + 1];

extern uint64_t _stat_pid_max;
extern uint64_t _stat_pid_min;
extern unsigned int _stat_allowed_disk_major;
extern unsigned int _stat_allowed_disk_minor;
extern unsigned long _stat_latency;

#define SYSFS_UINT_ATTR(_name, _var)                                        \
	static ssize_t _name##_show(struct kobject *kobj,                   \
				    struct kobj_attribute *attr, char *buf) \
	{                                                                   \
		return sprintf(buf, "%u\n", (unsigned int)(_var));          \
	}                                                                   \
	static ssize_t _name##_store(struct kobject *kobj,                  \
				     struct kobj_attribute *attr,           \
				     const char *buf, size_t count)         \
	{                                                                   \
		unsigned int val;                                           \
		sscanf(buf, "%u", &val);                                    \
		_var = val;                                                 \
		return count;                                               \
	}                                                                   \
	static struct kobj_attribute _name##_attr =                         \
		__ATTR(_name, 0660, _name##_show, _name##_store)

#define SYSFS_UINT_ATTR_CUSTOM(_name, _var, _storefn)                       \
	static ssize_t _name##_show(struct kobject *kobj,                   \
				    struct kobj_attribute *attr, char *buf) \
	{                                                                   \
		return sprintf(buf, "%u\n", (unsigned int)(_var));          \
	}                                                                   \
	static ssize_t _name##_store(struct kobject *kobj,                  \
				     struct kobj_attribute *attr,           \
				     const char *buf, size_t count)         \
	{                                                                   \
		return _storefn(buf, count);                                \
	}                                                                   \
	static struct kobj_attribute _name##_attr =                         \
		__ATTR(_name, 0660, _name##_show, _name##_store)

#define SYSFS_UINT2_ATTR(_name, _var1, _var2)                                       \
	static ssize_t _name##_show(struct kobject *kobj,                           \
				    struct kobj_attribute *attr, char *buf)         \
	{                                                                           \
		return sprintf(buf, "%u %u\n", (unsigned int)(_var1),               \
			       (unsigned int)(_var2));                              \
	}                                                                           \
	static ssize_t _name##_store(struct kobject *kobj,                          \
				     struct kobj_attribute *attr,                   \
				     const char *buf, size_t count)                 \
	{                                                                           \
		unsigned int v1, v2;                                                \
		sscanf(buf, "%u %u", &v1, &v2);                                     \
		if (v1 == _var1 && v2 == _var2) {                                   \
			pr_info("min(%u) == %s_min(%u) && max(%u) == %s_max(%u)\n", \
				v1, #_name, _var1, v2, #_name, _var2);              \
			return count;                                               \
		}                                                                   \
		_var1 = v1;                                                         \
		_var2 = v2;                                                         \
		return count;                                                       \
	}                                                                           \
	static struct kobj_attribute _name##_attr =                                 \
		__ATTR(_name, 0660, _name##_show, _name##_store)

#define SYSFS_STRING_ARRAY(_name, _var, ...)                                  \
	static const char *const _var##_names[] = { __VA_ARGS__ };            \
                                                                              \
	static ssize_t _name##_show(struct kobject *kobj,                     \
				    struct kobj_attribute *attr, char *buf)   \
	{                                                                     \
		size_t count = ARRAY_SIZE(_var##_names);                      \
		ssize_t len = 0;                                              \
		size_t i;                                                     \
		for (i = 0; i < count; i++) {                                 \
			if (i == _var)                                        \
				len += scnprintf(buf + len, PAGE_SIZE - len,  \
						 "[%s] ", _var##_names[i]);   \
			else                                                  \
				len += scnprintf(buf + len, PAGE_SIZE - len,  \
						 "%s ", _var##_names[i]);     \
		}                                                             \
		len += scnprintf(buf + len, PAGE_SIZE - len, "\n");           \
		return len;                                                   \
	}                                                                     \
                                                                              \
	static ssize_t _name##_store(struct kobject *kobj,                    \
				     struct kobj_attribute *attr,             \
				     const char *buf, size_t count)           \
	{                                                                     \
		size_t num_names = ARRAY_SIZE(_var##_names);                  \
		size_t i;                                                     \
		for (i = 0; i < num_names; i++) {                             \
			if (sysfs_streq(buf, _var##_names[i])) {              \
				_var = i;                                     \
				return count;                                 \
			}                                                     \
		}                                                             \
		if (isdigit(buf[0])) {                                        \
			unsigned long val;                                    \
			if (kstrtoul(buf, 0, &val) == 0 && val < num_names) { \
				_var = val;                                   \
				return count;                                 \
			}                                                     \
		}                                                             \
		return -EINVAL;                                               \
	}                                                                     \
                                                                              \
	static struct kobj_attribute _name##_attr =                           \
		__ATTR(_name, 0664, _name##_show, _name##_store)

#define STATS_TIME_COUNTER_VARS \
	X(perform_write);       \
	X(fg_olist_lookup);     \
	X(fg_olist_insert);     \
	X(fg_submit);           \
	X(bg_process_one_req);

#define STATS_COUNTER_ONLY_VARS          \
	X(fsync_bg_compatible);          \
	X(fsync_drain_called);           \
	X(fsync_drain_pending);          \
	X(fsync_drain_empty);            \
	X(fsync_fb_entry_on);            \
	X(fsync_fb_entry_off);           \
	X(fsync_fb_eval);                \
	X(fsync_fb_disabled);            \
	X(fsync_fb_no_writes);           \
	X(fsync_fb_pending_zero);        \
	X(fsync_fb_threshold_hit);       \
	X(fsync_fb_threshold_miss);      \
	X(fsync_fb_set);                 \
	X(fsync_fb_keep_on);             \
	X(fsync_fb_clear);               \
	X(fsync_fb_keep_off);            \
	X(fsync_fb_writes_sum);          \
	X(fsync_fb_pending_sum);         \
	X(bg_write_total_req);           \
	X(bg_write_total_ent);           \
	X(bg_write_fsync_fb_sync_req);   \
	X(bg_write_fsync_fb_sync_ent);   \
	X(bg_write_async_req);           \
	X(bg_write_async_ent);           \
	X(bg_write_draining_async_req);  \
	X(bg_write_draining_async_ent);  \
	X(bg_write_backoff_sync_req);    \
	X(bg_write_backoff_sync_ent);    \
	X(bg_write_threshold_sync_req);  \
	X(bg_write_threshold_sync_ent);  \
	X(submit_pressure_sync_req);     \
	X(submit_pressure_sync_ent);     \
	X(bg_pending_submit_depth_sum);  \
	X(bg_pending_submit_depth_samples); \
	X(get_folio_calls);              \
	X(get_folio_entries);            \
	X(pool_hit_calls);               \
	X(pool_hit_entries);             \
	X(fg_alloc_fallback_calls);      \
	X(fg_alloc_fallback_entries);    \
	X(foreground_sync_fallback_calls); \
	X(foreground_sync_fallback_entries); \
	X(backoff_enter);                \
	X(backoff_stall_ns);             \
	X(backoff_wall_ns);              \
	X(backoff_cpu_ns);               \
	X(async_submit_single_calls);    \
	X(async_submit_single_entries);  \
	X(async_submit_batch_calls);     \
	X(async_submit_batch_reqs);      \
	X(async_submit_batch_entries);

#define STATS_TIME_BUCKET_VARS \
	X(dummy);              \
	X(inner);

#define X STATS_DEF_EXT_TIME_COUNTER
STATS_TIME_COUNTER_VARS
#undef X

#define X STATS_DEF_EXT_COUNTER_ONLY
STATS_COUNTER_ONLY_VARS
#undef X

#define X STATS_DEF_EXT_TIME_BUCKET
STATS_TIME_BUCKET_VARS
#undef X

#define MIN_RECORD_SIZE 8192

/* Define DISABLE_ALWAYS_STATS to turn off ALWAYS timing. */

static inline bool is_pid_valid(void)
{
	return current->pid >= _stat_pid_min && current->pid <= _stat_pid_max;
}

#define TEST_BEGIN(count)                                 \
	bool should_test;                                 \
	if (!is_pid_valid() || count < MIN_RECORD_SIZE) { \
		should_test = 0;                          \
	} else {                                          \
		should_test = 1;                          \
	}

#ifdef DISABLE_ALWAYS_STATS
#define TEST_BEGIN_BY_INODE_ALWAYS(inode) bool should_test = 0;
#define TEST_BEGIN_ALWAYS()               bool should_test = 0;
#define TEST_BEGIN_AALWAYS()              bool should_test = 0;
#define TIME_BEGIN_ALWAYS(name)
#define TIME_END_ALWAYS(name)
#define TIME_END_ALWAYS_COND(name, apply)
#define INCR_COUNT_ALWAYS(name)
#define INCR_COUNT_BY_ALWAYS(name, count)
#else /* !DISABLE_ALWAYS_STATS */
#define TEST_BEGIN_BY_INODE_ALWAYS(inode)                       \
	bool should_test = 0;                                   \
	if (stats_is_inode_bdev_valid(inode)) {                 \
		should_test = 1;                                \
	}                                                       \
	if (inode && inode->i_ino >= _stat_allowed_inode_min && \
	    inode->i_ino <= _stat_allowed_inode_max) {          \
		should_test = 1;                                \
	}

#define TEST_BEGIN_ALWAYS() TEST_BEGIN(1000000)

#define TEST_BEGIN_AALWAYS() bool should_test = 1;

static inline uint64_t _stat_local_get_time_func(void)
{
	return rdtsc();
}

#define TIME_BEGIN_ALWAYS(name)                             \
	uint64_t name##_start;                              \
	uint64_t name##_end;                                \
	if (should_test) {                                  \
		name##_start = _stat_local_get_time_func(); \
	}

#define TIME_END_ALWAYS_COND(name, apply)                     \
	if (should_test) {                                    \
		name##_end = _stat_local_get_time_func();     \
		uint64_t diff = name##_end - name##_start;    \
		this_cpu_add(stats_total_time_##apply, diff); \
		this_cpu_inc(stats_##apply##_count);          \
	}

#define TIME_END_ALWAYS(name) TIME_END_ALWAYS_COND(name, name)

#define INCR_COUNT_ALWAYS(name)                     \
	do {                                        \
		this_cpu_inc(stats_##name##_count); \
	} while (0);

#define INCR_COUNT_BY_ALWAYS(name, count)                  \
	do {                                               \
		this_cpu_add(stats_##name##_count, count); \
	} while (0);
#endif /* !DISABLE_ALWAYS_STATS */

#define DISABLE_COSTLY_STATS

#ifndef DISABLE_COSTLY_STATS

#define TEST_BEGIN_BY_INODE(inode)                              \
	bool should_test = 0;                                   \
	if (stats_is_inode_bdev_valid(inode)) {                 \
		should_test = 1;                                \
	}                                                       \
	if (inode && inode->i_ino >= _stat_allowed_inode_min && \
	    inode->i_ino <= _stat_allowed_inode_max) {          \
		should_test = 1;                                \
	}

#define TIME_BEGIN(name)                                    \
	uint64_t name##_start;                              \
	uint64_t name##_end;                                \
	if (should_test) {                                  \
		name##_start = _stat_local_get_time_func(); \
	}

#define TIME_END_NO_COUNT(name)                           \
	if (should_test) {                                \
		name##_end = _stat_local_get_time_func(); \
		this_cpu_add(stats_total_time_##name,     \
			     name##_end - name##_start);  \
	}

#define TIME_END_BUCKET(name)                                           \
	if (should_test) {                                              \
		name##_end = _stat_local_get_time_func();               \
		uint64_t diff = name##_end - name##_start;              \
		uint64_t *bucket = this_cpu_ptr(stats_##name##_bucket); \
		for (int i = 0; i < _MISC_STATS_BUCKET_NUM; i++) {      \
			if (diff <= bucket_size[i]) {                   \
				bucket[i]++;                            \
				break;                                  \
			}                                               \
		}                                                       \
	}

#define INCR_COUNT(name) INCR_COUNT_BY(name, 1)

#define INCR_COUNT_BY(name, count)                         \
	if (should_test) {                                 \
		this_cpu_add(stats_##name##_count, count); \
	}

#define TIME_END(name)                                   \
	{                                                \
		TIME_END_NO_COUNT(name) INCR_COUNT(name) \
	}

#else

#define TEST_BEGIN_BY_INODE(inode)
#define TIME_BEGIN(name)
#define TIME_END_NO_COUNT(name)
#define TIME_END_BUCKET(name)

#define TIME_END(name)
#define INCR_COUNT(name)
#define INCR_COUNT_BY(name, count)
#endif

#else

#define TEST_BEGIN(count)
#define TEST_BEGIN_ALWAYS()
#define TEST_BEGIN_BY_INODE(inode)
#define TEST_BEGIN_AALWAYS()
#define TIME_BEGIN(name)
#define TIME_END_NO_COUNT(name)
#define INCR_COUNT(name)
#define INCR_COUNT_BY(name, count)
#define TIME_END(name)
#define TIME_END_BUCKET(name)

#endif

extern unsigned long _bg_allowed_inode_min;
extern unsigned long _bg_allowed_inode_max;
extern unsigned int _bg_allowed_disk_major;
extern unsigned int _bg_allowed_disk_minor;
extern unsigned long _stat_allowed_inode_min;
extern unsigned long _stat_allowed_inode_max;

#ifdef CONFIG_INTEL_DSA_SL_EMU
extern volatile unsigned long _stat_dsa_emu_retry_count;
#endif

#include <linux/blkdev.h>
static inline bool stats_is_inode_bdev_valid(struct inode *inode)
{
	if (!inode)
		return false;

	if (inode->i_ino < 12)
		return false;

	if (inode->i_sb && inode->i_sb->s_bdev && inode->i_sb->s_bdev->bd_dev) {
		if (MAJOR(inode->i_sb->s_bdev->bd_dev) ==
			    _stat_allowed_disk_major &&
		    MINOR(inode->i_sb->s_bdev->bd_dev) ==
			    _stat_allowed_disk_minor) {
			return true;
		}
	}
	return false;
}

#endif
