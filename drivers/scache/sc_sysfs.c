// SPDX-License-Identifier: GPL-2.0
/*
 * StreamCache memory pool sysfs interface.
 *
 * Exposed under /sys/fs/sc_memory/:
 *   enabled           - "0" or "1"; writing "0" destroys the pool and frees
 *                       all regions; writing "1" (re)creates with current
 *                       nr_regions / pages_per_region.
 *   nr_regions        - number of per-CPU regions (default: num_possible_cpus)
 *   pages_per_region  - pages per region (default: 524288 = 2GB)
 *   threshold_low     - low memory threshold percent (default: 20)
 *   threshold_urgent  - urgent memory threshold percent (default: 5)
 *   low_mark          - per-file cache refill batch size (default: 50)
 *   high_mark         - per-file cache max pages (default: 100)
 *   memory_flag       - current flag (read-only): normal / low / urgent
 *   pool_free_pages   - total free pages across all regions (read-only)
 */

#define pr_fmt(fmt) "sc_memory: " fmt

#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/module.h>
#include <linux/sc_memory.h>

/*
 * Tunable defaults.  The sysfs writes update these, and they take effect
 * on the next pool init (writing "1" to enabled).
 */
static unsigned int sc_cfg_nr_regions;
static unsigned long sc_cfg_pages_per_region = 524288; /* 2GB per region */
static unsigned int sc_cfg_low_mark = SC_LOW_MARK_DEFAULT;
static unsigned int sc_cfg_high_mark = SC_HIGH_MARK_DEFAULT;

/* -- enabled -- */

static ssize_t enabled_show(struct kobject *kobj, struct kobj_attribute *attr,
			    char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n", sc_pool.enabled ? 1 : 0);
}

static ssize_t enabled_store(struct kobject *kobj, struct kobj_attribute *attr,
			     const char *buf, size_t count)
{
	unsigned int val;
	int ret;

	ret = kstrtouint(buf, 0, &val);
	if (ret)
		return ret;

	if (val && !sc_pool.enabled) {
		ret = sc_memory_pool_init(&sc_pool, sc_cfg_nr_regions,
					  sc_cfg_pages_per_region);
		if (ret)
			return ret;
	} else if (!val && sc_pool.enabled) {
		sc_memory_pool_destroy(&sc_pool);
	}

	return count;
}
static struct kobj_attribute enabled_attr =
	__ATTR(enabled, 0660, enabled_show, enabled_store);

/* -- nr_regions -- */

static ssize_t nr_regions_show(struct kobject *kobj,
			       struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u\n", sc_cfg_nr_regions);
}

static ssize_t nr_regions_store(struct kobject *kobj,
				struct kobj_attribute *attr,
				const char *buf, size_t count)
{
	unsigned int val;
	int ret;

	ret = kstrtouint(buf, 0, &val);
	if (ret)
		return ret;
	if (val == 0 || val > 4096)
		return -EINVAL;
	if (sc_pool.enabled) {
		pr_err("disable pool before changing nr_regions\n");
		return -EBUSY;
	}
	sc_cfg_nr_regions = val;
	return count;
}
static struct kobj_attribute nr_regions_attr =
	__ATTR(nr_regions, 0660, nr_regions_show, nr_regions_store);

/* -- pages_per_region -- */

static ssize_t pages_per_region_show(struct kobject *kobj,
				     struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%lu\n", sc_cfg_pages_per_region);
}

static ssize_t pages_per_region_store(struct kobject *kobj,
				      struct kobj_attribute *attr,
				      const char *buf, size_t count)
{
	unsigned long val;
	int ret;

	ret = kstrtoul(buf, 0, &val);
	if (ret)
		return ret;
	if (val == 0)
		return -EINVAL;
	if (sc_pool.enabled) {
		pr_err("disable pool before changing pages_per_region\n");
		return -EBUSY;
	}
	sc_cfg_pages_per_region = val;
	return count;
}
static struct kobj_attribute pages_per_region_attr =
	__ATTR(pages_per_region, 0660, pages_per_region_show,
	       pages_per_region_store);

/* -- threshold_low -- */

static ssize_t threshold_low_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u\n",
			 sc_pool.enabled ? sc_pool.threshold_low :
					   SC_THRESHOLD_LOW_DEFAULT);
}

static ssize_t threshold_low_store(struct kobject *kobj,
				   struct kobj_attribute *attr,
				   const char *buf, size_t count)
{
	unsigned int val;
	int ret;

	ret = kstrtouint(buf, 0, &val);
	if (ret)
		return ret;
	if (val > 100)
		return -EINVAL;
	if (sc_pool.enabled)
		sc_pool.threshold_low = val;
	return count;
}
static struct kobj_attribute threshold_low_attr =
	__ATTR(threshold_low, 0660, threshold_low_show, threshold_low_store);

/* -- threshold_urgent -- */

static ssize_t threshold_urgent_show(struct kobject *kobj,
				     struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u\n",
			 sc_pool.enabled ? sc_pool.threshold_urgent :
					   SC_THRESHOLD_URGENT_DEFAULT);
}

static ssize_t threshold_urgent_store(struct kobject *kobj,
				      struct kobj_attribute *attr,
				      const char *buf, size_t count)
{
	unsigned int val;
	int ret;

	ret = kstrtouint(buf, 0, &val);
	if (ret)
		return ret;
	if (val > 100)
		return -EINVAL;
	if (sc_pool.enabled)
		sc_pool.threshold_urgent = val;
	return count;
}
static struct kobj_attribute threshold_urgent_attr =
	__ATTR(threshold_urgent, 0660, threshold_urgent_show,
	       threshold_urgent_store);

/* -- low_mark -- */

static ssize_t low_mark_show(struct kobject *kobj,
			     struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u\n", sc_cfg_low_mark);
}

static ssize_t low_mark_store(struct kobject *kobj,
			      struct kobj_attribute *attr,
			      const char *buf, size_t count)
{
	unsigned int val;
	int ret;

	ret = kstrtouint(buf, 0, &val);
	if (ret)
		return ret;
	if (val == 0)
		return -EINVAL;
	sc_cfg_low_mark = val;
	return count;
}
static struct kobj_attribute low_mark_attr =
	__ATTR(low_mark, 0660, low_mark_show, low_mark_store);

/* -- high_mark -- */

static ssize_t high_mark_show(struct kobject *kobj,
			      struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u\n", sc_cfg_high_mark);
}

static ssize_t high_mark_store(struct kobject *kobj,
			       struct kobj_attribute *attr,
			       const char *buf, size_t count)
{
	unsigned int val;
	int ret;

	ret = kstrtouint(buf, 0, &val);
	if (ret)
		return ret;
	if (val == 0)
		return -EINVAL;
	sc_cfg_high_mark = val;
	return count;
}
static struct kobj_attribute high_mark_attr =
	__ATTR(high_mark, 0660, high_mark_show, high_mark_store);

/* -- memory_flag (read-only) -- */

static const char * const flag_names[] = { "normal", "low", "urgent" };

static ssize_t memory_flag_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	int flag;

	if (!sc_pool.enabled)
		return scnprintf(buf, PAGE_SIZE, "disabled\n");

	flag = atomic_read(&sc_pool.memory_flag);
	if (flag < 0 || flag > SC_FLAG_URGENT)
		flag = SC_FLAG_NORMAL;
	return scnprintf(buf, PAGE_SIZE, "%s\n", flag_names[flag]);
}
static struct kobj_attribute memory_flag_attr =
	__ATTR(memory_flag, 0440, memory_flag_show, NULL);

/* -- pool_free_pages (read-only) -- */

static ssize_t pool_free_pages_show(struct kobject *kobj,
				    struct kobj_attribute *attr, char *buf)
{
	unsigned long total_free = 0;
	unsigned int i;

	if (!sc_pool.enabled)
		return scnprintf(buf, PAGE_SIZE, "0\n");

	for (i = 0; i < sc_pool.nr_regions; i++)
		total_free += READ_ONCE(sc_pool.regions[i].nr_free);

	return scnprintf(buf, PAGE_SIZE, "%lu\n", total_free);
}
static struct kobj_attribute pool_free_pages_attr =
	__ATTR(pool_free_pages, 0440, pool_free_pages_show, NULL);

/* -- attribute group -- */

static struct attribute *sc_memory_attrs[] = {
	&enabled_attr.attr,
	&nr_regions_attr.attr,
	&pages_per_region_attr.attr,
	&threshold_low_attr.attr,
	&threshold_urgent_attr.attr,
	&low_mark_attr.attr,
	&high_mark_attr.attr,
	&memory_flag_attr.attr,
	&pool_free_pages_attr.attr,
	NULL,
};

static const struct attribute_group sc_memory_attr_group = {
	.attrs = sc_memory_attrs,
};

static struct kobject *sc_memory_kobj;

int sc_memory_init_sysfs(void)
{
	int ret;

	/* Default nr_regions to number of possible CPUs */
	if (sc_cfg_nr_regions == 0)
		sc_cfg_nr_regions = num_possible_cpus();

	sc_memory_kobj = kobject_create_and_add("sc_memory", fs_kobj);
	if (!sc_memory_kobj)
		return -ENOMEM;

	ret = sysfs_create_group(sc_memory_kobj, &sc_memory_attr_group);
	if (ret) {
		kobject_put(sc_memory_kobj);
		sc_memory_kobj = NULL;
		return ret;
	}

	return 0;
}
EXPORT_SYMBOL(sc_memory_init_sysfs);

void sc_memory_exit_sysfs(void)
{
	if (sc_memory_kobj) {
		sysfs_remove_group(sc_memory_kobj, &sc_memory_attr_group);
		kobject_put(sc_memory_kobj);
		sc_memory_kobj = NULL;
	}
}
EXPORT_SYMBOL(sc_memory_exit_sysfs);
