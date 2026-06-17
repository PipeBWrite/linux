#ifndef SYSFS_EMU_H
#define SYSFS_EMU_H

#include <linux/sysfs.h>

int dsa_emu_init_sysfs(void);

extern uint32_t num_threads;
extern bool wq_inited;
extern bool dsa_emu_enable_bdp;
extern uint32_t dsa_emu_force_node;
extern uint32_t dsa_emu_force_node_nid;
extern uint32_t dsa_emu_prefetch;
extern uint32_t dsa_emu_folio_pool_order_max;

#endif
