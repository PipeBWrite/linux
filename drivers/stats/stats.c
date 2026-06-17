#define STATS_USE_BUCKET

#include "linux/stats.h"
#include <linux/module.h>
#include <linux/init.h>
#include <linux/dsa_sl.h>
#include <linux/blkdev.h>
#include <linux/percpu.h>
#include <linux/smp.h>

// Those will be exported to other modules

const unsigned long bucket_size[_MISC_STATS_BUCKET_NUM + 1] = {
	1,     2,     4,     8,	     16,     32,     64,
	128,   256,   512,   1024,   2048,   4096,   8192,
	16384, 32768, 65536, 131072, 262144, 524288, UINT_MAX
};

uint64_t _t_1;
uint64_t _t_2;

static uint64_t stats_sum_u64_percpu(const uint64_t __percpu *stat)
{
	uint64_t sum = 0;
	int cpu;

	for_each_possible_cpu(cpu) {
		sum += *per_cpu_ptr(stat, cpu);
	}

	return sum;
}

#define STRING_TIME_COUNTER(name) \
	"==> " #name "_time: %llu, " #name "_count: %llu\n"

#define SPRINTF_TIME_COUNTER(name)                                            \
	do {                                                                  \
		uint64_t count = stats_sum_u64_percpu(&stats_##name##_count); \
		if (count) {                                                  \
			uint64_t total_time = stats_sum_u64_percpu(           \
				&stats_total_time_##name);                    \
			total += sprintf(buf + total,                         \
					 STRING_TIME_COUNTER(name),           \
					 total_time, count);                  \
		}                                                             \
	} while (0)

#define STATS_DEF_TIME_COUNTER(name)                       \
	DEFINE_PER_CPU(uint64_t, stats_total_time_##name); \
	DEFINE_PER_CPU(uint64_t, stats_##name##_count);    \
	EXPORT_PER_CPU_SYMBOL(stats_total_time_##name);    \
	EXPORT_PER_CPU_SYMBOL(stats_##name##_count);

#define STATS_DEF_COUNTER_ONLY(name)                    \
	DEFINE_PER_CPU(uint64_t, stats_##name##_count); \
	EXPORT_PER_CPU_SYMBOL(stats_##name##_count);

#define STATS_DEF_BUCKET_ONLY(name)                      \
	DEFINE_PER_CPU(uint64_t[_MISC_STATS_BUCKET_NUM], \
		       stats_##name##_bucket);           \
	EXPORT_PER_CPU_SYMBOL(stats_##name##_bucket);

#define STATS_CLEAR_TIME_COUNTER(name)                                   \
	do {                                                             \
		int cpu;                                                 \
		for_each_possible_cpu(cpu) {                             \
			*per_cpu_ptr(&stats_total_time_##name, cpu) = 0; \
			*per_cpu_ptr(&stats_##name##_count, cpu) = 0;    \
		}                                                        \
	} while (0)

#ifdef CONFIG_MISC_STATS

uint64_t _stat_pid_min = 0;
uint64_t _stat_pid_max = 0;

#define X STATS_DEF_TIME_COUNTER
STATS_TIME_COUNTER_VARS
#undef X

#define X STATS_DEF_COUNTER_ONLY
STATS_COUNTER_ONLY_VARS
#undef X

#define X STATS_DEF_BUCKET_ONLY
STATS_TIME_BUCKET_VARS
#undef X

static ssize_t stats_show(struct kobject *kobj, struct kobj_attribute *attr,
			  char *buf)
{
	ssize_t total = 0;

#define X SPRINTF_TIME_COUNTER
	STATS_TIME_COUNTER_VARS
#undef X

	return total;
}

static ssize_t stats_store(struct kobject *kobj, struct kobj_attribute *attr,
			   const char *buf, size_t count)
{
	// Reset all

#define X STATS_CLEAR_TIME_COUNTER
	STATS_TIME_COUNTER_VARS
#undef X

	return count;
}

static struct kobj_attribute statis_attribute =
	__ATTR(stats, 0660, stats_show, stats_store);

static ssize_t stats_pid_show(struct kobject *kobj, struct kobj_attribute *attr,
			      char *buf)
{
	return sprintf(buf, "%llu - %llu\n", _stat_pid_min, _stat_pid_max);
}

static ssize_t stats_pid_store(struct kobject *kobj,
			       struct kobj_attribute *attr, const char *buf,
			       size_t count)
{
	_stat_pid_min = 0;
	_stat_pid_max = 0;
	sscanf(buf, "%llu - %llu", &_stat_pid_min, &_stat_pid_max);
	return count;
}

static struct kobj_attribute statis_pid_attribute =
	__ATTR(pid, 0660, stats_pid_show, stats_pid_store);

#endif

// TODO: For convience, we add the allowed inode variable here.
//       However, it should be moved to a saparate place later

unsigned long _bg_allowed_inode_min;
unsigned long _bg_allowed_inode_max;
unsigned int _bg_allowed_disk_major;
unsigned int _bg_allowed_disk_minor;

static ssize_t bg_allowed_inode_show(struct kobject *kobj,
				     struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%lu - %lu\n", _bg_allowed_inode_min,
		       _bg_allowed_inode_max);
}

static ssize_t bg_allowed_inode_store(struct kobject *kobj,
				      struct kobj_attribute *attr,
				      const char *buf, size_t count)
{
	sscanf(buf, "%lu - %lu\n", &_bg_allowed_inode_min,
	       &_bg_allowed_inode_max);
	_bg_allowed_disk_major = 0;
	_bg_allowed_disk_minor = 0;

	return count;
}

static struct kobj_attribute bg_allowed_inode_attribute = __ATTR(
	bg_allowed_inode, 0660, bg_allowed_inode_show, bg_allowed_inode_store);

static ssize_t bg_allowed_dev_name_show(struct kobject *kobj,
					struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u:%u\n", _bg_allowed_disk_major,
		       _bg_allowed_disk_minor);
}

static ssize_t bg_allowed_dev_name_store(struct kobject *kobj,
					 struct kobj_attribute *attr,
					 const char *buf, size_t count)
{
	sscanf(buf, "%u:%u", &_bg_allowed_disk_major, &_bg_allowed_disk_minor);
	_bg_allowed_inode_min = 0;
	_bg_allowed_inode_max = 0;
	return count;
}

static struct kobj_attribute bg_allowed_dev_name_attribute =
	__ATTR(bg_allowed_dev_name, 0660, bg_allowed_dev_name_show,
	       bg_allowed_dev_name_store);

static struct kobject *stats_kobj;

// TODO: Also for convinience, we put the init thread code here
#ifdef CONFIG_INTEL_DSA_SL_EMU
unsigned int _stats_is_dsa_emu_thread_inited = 0;

static ssize_t stats_thread_init_show(struct kobject *kobj,
				      struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", _stats_is_dsa_emu_thread_inited);
}

static ssize_t stats_thread_init_store(struct kobject *kobj,
				       struct kobj_attribute *attr,
				       const char *buf, size_t count)
{
	unsigned int enabled;
	sscanf(buf, "%u", &enabled);

	if (enabled != 0 && enabled != 1) {
		pr_info("ERR: enabled: %d\n", enabled);
		return -EINVAL;
	}

	if (enabled != _stats_is_dsa_emu_thread_inited) {
		if (enabled) {
			dsa_emu_thread_init();
		} else {
			dsa_emu_thread_stop();
		}
		_stats_is_dsa_emu_thread_inited = enabled;
	} else {
		pr_info("ERR: enabled: %d, _stats_is_dsa_emu_thread_inited: %d\n",
			enabled, _stats_is_dsa_emu_thread_inited);
	}

	return count;
}

static struct kobj_attribute stats_thread_init_attribute = __ATTR(
	thread_init, 0660, stats_thread_init_show, stats_thread_init_store);

volatile unsigned long _stat_dsa_emu_retry_count;

static ssize_t stats_dsa_emu_retry_count_show(struct kobject *kobj,
					      struct kobj_attribute *attr,
					      char *buf)
{
	return sprintf(buf, "%lu\n", _stat_dsa_emu_retry_count);
}

static ssize_t stats_dsa_emu_retry_count_store(struct kobject *kobj,
					       struct kobj_attribute *attr,
					       const char *buf, size_t count)
{
	_stat_dsa_emu_retry_count = 0;
	return count;
}

static struct kobj_attribute stats_dsa_emu_retry_count_attribute =
	__ATTR(dsa_emu_retry_count, 0660, stats_dsa_emu_retry_count_show,
	       stats_dsa_emu_retry_count_store);

unsigned long _stat_allowed_inode_min;
unsigned long _stat_allowed_inode_max;
unsigned int _stat_allowed_disk_major;
unsigned int _stat_allowed_disk_minor;

static ssize_t stats_allowed_inode_show(struct kobject *kobj,
					struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%lu - %lu\n", _stat_allowed_inode_min,
		       _stat_allowed_inode_max);
}

static ssize_t stats_allowed_inode_store(struct kobject *kobj,
					 struct kobj_attribute *attr,
					 const char *buf, size_t count)
{
	sscanf(buf, "%lu - %lu", &_stat_allowed_inode_min,
	       &_stat_allowed_inode_max);
	_stat_allowed_disk_major = 0;
	_stat_allowed_disk_minor = 0;
	return count;
}

static struct kobj_attribute stats_allowed_inode_attribute =
	__ATTR(stats_allowed_inode, 0660, stats_allowed_inode_show,
	       stats_allowed_inode_store);

static ssize_t stats_allowed_dev_name_show(struct kobject *kobj,
					   struct kobj_attribute *attr,
					   char *buf)
{
	return sprintf(buf, "%u:%u\n", _stat_allowed_disk_major,
		       _stat_allowed_disk_minor);
}

static ssize_t stats_allowed_dev_name_store(struct kobject *kobj,
					    struct kobj_attribute *attr,
					    const char *buf, size_t count)
{
	sscanf(buf, "%u:%u", &_stat_allowed_disk_major,
	       &_stat_allowed_disk_minor);
	_stat_allowed_inode_min = 0;
	_stat_allowed_inode_max = 0;
	return count;
}

static struct kobj_attribute stats_allowed_dev_name_attribute =
	__ATTR(stats_allowed_dev_name, 0660, stats_allowed_dev_name_show,
	       stats_allowed_dev_name_store);

unsigned long _stat_latency;

static ssize_t stats_latency_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", _stat_latency);
}

static ssize_t stats_latency_store(struct kobject *kobj,
				   struct kobj_attribute *attr, const char *buf,
				   size_t count)
{
	sscanf(buf, "%lu", &_stat_latency);
	return count;
}

static struct kobj_attribute stats_latency_attribute =
	__ATTR(stats_latency, 0660, stats_latency_show, stats_latency_store);

#endif

static int __init stats_init(void)
{
	int error;
	pr_debug("Stats module loaded\n");

	// create a sysfs node
	stats_kobj = kobject_create_and_add("stats", kernel_kobj);
	if (!stats_kobj) {
		pr_err("Failed to create stats kobject\n");
		return -ENOMEM;
	}

#ifdef CONFIG_MISC_STATS
	error = sysfs_create_file(stats_kobj, &statis_attribute.attr);
	if (error) {
		pr_err("Failed to create stats sysfs file\n");
		kobject_put(stats_kobj);
		return error;
	}

	error = sysfs_create_file(stats_kobj, &statis_pid_attribute.attr);
	if (error) {
		pr_err("Failed to create stats sysfs file\n");
		kobject_put(stats_kobj);
		return error;
	}

	error = sysfs_create_file(stats_kobj,
				  &stats_allowed_inode_attribute.attr);
	if (error) {
		pr_err("Failed to create stats sysfs file\n");
		kobject_put(stats_kobj);
		return error;
	}

	error = sysfs_create_file(stats_kobj,
				  &stats_allowed_dev_name_attribute.attr);
	if (error) {
		pr_err("Failed to create stats sysfs file\n");
		kobject_put(stats_kobj);
		return error;
	} else {
		pr_info("stats_allowed_dev_name: %u:%u\n",
			_stat_allowed_disk_major, _stat_allowed_disk_minor);
	}
#endif

	error = sysfs_create_file(stats_kobj, &bg_allowed_inode_attribute.attr);
	if (error) {
		pr_err("Failed to create stats sysfs file\n");
		kobject_put(stats_kobj);
		return error;
	}

	error = sysfs_create_file(stats_kobj,
				  &bg_allowed_dev_name_attribute.attr);
	if (error) {
		pr_err("Failed to create stats sysfs file\n");
		kobject_put(stats_kobj);
		return error;
	} else {
		pr_info("bg_allowed_dev_name: %u:%u\n", _bg_allowed_disk_major,
			_bg_allowed_disk_minor);
	}

	error = sysfs_create_file(stats_kobj, &stats_latency_attribute.attr);
	if (error) {
		pr_err("Failed to create stats sysfs file\n");
		kobject_put(stats_kobj);
		return error;
	}

#ifdef CONFIG_INTEL_DSA_SL_EMU
	error = sysfs_create_file(stats_kobj,
				  &stats_thread_init_attribute.attr);
	if (error) {
		pr_err("Failed to create thread init sysfs file\n");
		kobject_put(stats_kobj);
		return error;
	}

	error = sysfs_create_file(stats_kobj,
				  &stats_dsa_emu_retry_count_attribute.attr);
	if (error) {
		pr_err("Failed to create dsa emu retry count sysfs file\n");
		kobject_put(stats_kobj);
		return error;
	}
#endif

	return 0;
}

static void __exit stats_exit(void)
{
	pr_debug("Stats module unloaded\n");
	kobject_put(stats_kobj);
}

module_init(stats_init);
module_exit(stats_exit);
MODULE_LICENSE("GPL");
