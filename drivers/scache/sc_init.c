// SPDX-License-Identifier: GPL-2.0
/*
 * StreamCache Two-Layer Memory Management - module init/exit
 */

#define pr_fmt(fmt) "sc_memory: " fmt

#include <linux/module.h>
#include <linux/sc_memory.h>

static int __init sc_memory_module_init(void)
{
	int ret;

	ret = sc_memory_init_sysfs();
	if (ret) {
		pr_err("failed to initialize sysfs: %d\n", ret);
		return ret;
	}

	pr_info("module loaded (pool disabled, configure via "
		"/sys/fs/sc_memory/ then write 1 to enabled)\n");
	return 0;
}

static void __exit sc_memory_module_exit(void)
{
	if (sc_pool.enabled)
		sc_memory_pool_destroy(&sc_pool);

	sc_memory_exit_sysfs();
	pr_info("module unloaded\n");
}

module_init(sc_memory_module_init);
module_exit(sc_memory_module_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("StreamCache Two-Layer Memory Management");
MODULE_AUTHOR("StreamCache");
