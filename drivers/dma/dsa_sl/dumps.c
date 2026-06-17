#include "dsa.h"
#include "ctrl.h"
#include "dump.h"
#include <linux/printk.h>

void dump_gencap(void *addr)
{
	struct GENCAP *gencap = (struct GENCAP *)addr;

	pr_info("==> GENCAP\n");
	pr_info("bof_support: %u\n", gencap->bof_support);
	pr_info("overlapping_copy_support: %u\n",
		gencap->overlapping_copy_support);
	pr_info("cache_ctrl_support: %u\n", gencap->cache_ctrl_support);
	pr_info("command_cap_support: %u\n", gencap->command_cap_support);
	pr_info("inter_domain_support: %u\n", gencap->inter_domain_support);
	pr_info("trans_fetch_support: %u\n", gencap->trans_fetch_support);
	pr_info("dest_readback_support: %u\n", gencap->dest_readback_support);
	pr_info("drain_readback_support: %u\n", gencap->drain_readback_support);
	pr_info("fill16_support: %u\n", gencap->fill16_support);
	pr_info("crc64_support: %u\n", gencap->crc64_support);
	pr_info("cr_fault_info_support: %u\n", gencap->cr_fault_info_support);
	pr_info("event_log_support: %u\n", gencap->event_log_support);
	pr_info("batch_continuation_support: %u\n",
		gencap->batch_continuation_support);
	pr_info("max_xfer_size: %u\n", gencap->max_xfer_size);
	pr_info("max_batch_size: %u\n", gencap->max_batch_size);
	pr_info("interrupt_msg_storage_size: %u\n",
		gencap->interrupt_msg_storage_size);
	pr_info("config_support: %u\n", gencap->config_support);
	pr_info("event_log_overflow_support: %u\n",
		gencap->event_log_overflow_support);
	pr_info("strict_ordering_limitation_for_memory_destinations_support: %u\n",
		gencap->strict_ordering_limitation_for_memory_destinations_support);
	pr_info("strict_ordering_limitation_for_peer_destinations_support: %u\n",
		gencap->strict_ordering_limitation_for_peer_destinations_support);
	pr_info("\n");
}

void dump_wqcap(void *addr)
{
	union WQCAP *wqcap = (union WQCAP *)addr;

	pr_info("==> WQCAP\n");
	pr_info("total_wq_size: %u\n", wqcap->total_wq_size);
	pr_info("num_wq: %u\n", wqcap->num_wq);
	pr_info("wqcfg_size: %u\n", wqcap->wqcfg_size);
	pr_info("shared_mode_support: %u\n", wqcap->shared_mode_support);
	pr_info("dedicated_mode_support: %u\n", wqcap->dedicated_mode_support);
	pr_info("wq_ats_support: %u\n", wqcap->wq_ats_support);
	pr_info("wq_priority_support: %u\n", wqcap->wq_priority_support);
	pr_info("wq_occupancy_support: %u\n", wqcap->wq_occupancy_support);
	pr_info("wq_occupancy_interrupt_support: %u\n",
		wqcap->wq_occupancy_interrupt_support);
	pr_info("wq_op_configration_support: %u\n",
		wqcap->wq_op_configration_support);
	pr_info("wq_prs_support: %u\n", wqcap->wq_prs_support);
	pr_info("\n");
}

void dump_grpcap(void *addr)
{
	union GRPCAP *grpcap = (union GRPCAP *)addr;

	pr_info("==> GRPCAP\n");
	pr_info("num_groups: %u\n", grpcap->num_groups);
	pr_info("total_read_buffers: %u\n", grpcap->total_read_buffers);
	pr_info("read_buffer_controls_support: %u\n",
		grpcap->read_buffer_controls_support);
	pr_info("global_read_buffer_limit_support: %u\n",
		grpcap->global_read_buffer_limit_support);
	pr_info("desc_inprogress_limit_support: %u\n",
		grpcap->desc_inprogress_limit_support);
	pr_info("\n");
}

void dump_engcap(void *addr)
{
	union ENGCAP *engcap = (union ENGCAP *)addr;

	pr_info("==> ENGCAP\n");
	pr_info("num_engines: %u\n", engcap->num_engines);
	pr_info("max_work_desc_in_progress: %u\n",
		engcap->max_work_desc_in_progress);
	pr_info("max_batch_desc_in_progress: %u\n",
		engcap->max_batch_desc_in_progress);
	pr_info("\n");
}

void dump_tableoffsets(void *addr)
{
	struct TABLEOFFSETS *tableoffsets = (struct TABLEOFFSETS *)addr;

	pr_info("==> TABLEOFFSETS\n");
	pr_info("group_configuration_offset: %u\n",
		tableoffsets->group_configuration_offset);
	pr_info("wq_configuration_offset: %u\n",
		tableoffsets->wq_configuration_offset);
	pr_info("msix_permission_offset: %u\n",
		tableoffsets->msix_permission_offset);
	pr_info("ims_offset: %u\n", tableoffsets->ims_offset);
	pr_info("perfmon_offset: %u\n", tableoffsets->perfmon_offset);
	pr_info("interdomain_permissions_table_offset: %u\n",
		tableoffsets->interdomain_permissions_table_offset);
	pr_info("\n");
}

void dump_wqcfg(struct dsa_device_info *dsa, int idx)
{
	union WQCFG wqcfg;
	dsa_read_wqcfg(&wqcfg, dsa, idx);

	pr_info("==> WQCFG\n");
	pr_info("wq_size: %u\n", wqcfg.wq_size);
	pr_info("wq_threashold: %u\n", wqcfg.wq_threashold);
	pr_info("wq_mode: %u\n", wqcfg.wq_mode);
	pr_info("bof_enable: %u\n", wqcfg.bof_enable);
	pr_info("ats_disable: %u\n", wqcfg.ats_disable);
	pr_info("prs_disable: %u\n", wqcfg.prs_disable);
	pr_info("wq_priority: %u\n", wqcfg.wq_priority);
	pr_info("pasid: %u\n", wqcfg.pasid);
	pr_info("pasid_enable: %u\n", wqcfg.pasid_enable);
	pr_info("priv: %u\n", wqcfg.priv);
	pr_info("max_xfer_size: %u\n", wqcfg.max_xfer_size);
	pr_info("max_batch_size: %u\n", wqcfg.max_batch_size);
	pr_info("wq_occupancy_interrupt_handle: %u\n",
		wqcfg.wq_occupancy_interrupt_handle);
	pr_info("wq_occupancy_interrupt_table: %u\n",
		wqcfg.wq_occupancy_interrupt_table);
	pr_info("wq_occupancy_limit: %u\n", wqcfg.wq_occupancy_limit);
	pr_info("wq_occupancy_interrupt_enable: %u\n",
		wqcfg.wq_occupancy_interrupt_enable);
	pr_info("wq_occupancy: %u\n", wqcfg.wq_occupancy);
	pr_info("wq_occupancy_interrupt_generated: %u\n",
		wqcfg.wq_occupancy_interrupt_generated);
	pr_info("wq_mode_support: %u\n", wqcfg.wq_mode_support);
	pr_info("wq_state: %u\n", wqcfg.wq_state);
	pr_info("\n");
}

#ifdef CONFIG_DSA_SL_ENABLE_USER_THREAD
void dsa_dumpbuf(struct dsa_memcpy_reqs *reqs)
{
	pr_info("==> DSA_MEMCPY_REQS\n");

	if (reqs->head == reqs->tail) {
		pr_info("Empty\n");
		return;
	}

	for (int i = reqs->head; i != reqs->tail; i = (i + 1) % RB_SIZE) {
		pr_info("idx: %d\n", i);
		pr_info("src: %#llx\n", reqs->memcpys[i].src);
		pr_info("dst: %#llx\n", reqs->memcpys[i].dst);
		pr_info("size: %llu\n", reqs->memcpys[i].size);
		pr_info("--\n");
	}
}
#else

#define dsa_dumpbuf(reqs) do {} while (0)

#endif
