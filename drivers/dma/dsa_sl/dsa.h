#include "asm/vdso/processor.h"
#include "linux/spinlock_types.h"
#include <linux/types.h>
#include <linux/string.h>
#include <linux/iomap.h>
#include <linux/io-64-nonatomic-lo-hi.h>
#include <linux/cdev.h>

#ifndef DSA_H
#define DSA_H

#define PCI_DEVICE_ID_INTEL_DSA_SPR0 0x0b25

struct __attribute__((packed)) GENCAP {
	// Byte 1
	uint32_t bof_support : 1;
	uint32_t overlapping_copy_support : 1;
	uint32_t cache_ctrl_support : 1;
	uint32_t : 1;
	uint32_t command_cap_support : 1;
	uint32_t : 1;
	uint32_t inter_domain_support : 1;
	uint32_t trans_fetch_support : 1;

	// Byte 2
	uint32_t dest_readback_support : 1;
	uint32_t drain_readback_support : 1;
	uint32_t fill16_support : 1;
	uint32_t crc64_support : 1;
	uint32_t cr_fault_info_support : 1;
	uint32_t event_log_support : 2;
	uint32_t batch_continuation_support : 1;

	// Byte 3 - 4
	uint32_t max_xfer_size : 5;
	uint32_t max_batch_size : 4;
	uint32_t interrupt_msg_storage_size : 6;
	uint32_t config_support : 1;

	// Byte 5 - 8
	uint32_t event_log_overflow_support : 1;
	uint32_t : 20;
	uint32_t strict_ordering_limitation_for_memory_destinations_support : 1;
	uint32_t strict_ordering_limitation_for_peer_destinations_support : 1;
	uint32_t : 9;
};

union __attribute__((packed)) WQCAP {
	struct {
		uint32_t total_wq_size : 16;
		uint32_t num_wq : 8;
		uint32_t wqcfg_size : 4;
		uint32_t : 20;
		uint32_t shared_mode_support : 1;
		uint32_t dedicated_mode_support : 1;
		uint32_t wq_ats_support : 1;
		uint32_t wq_priority_support : 1;
		uint32_t wq_occupancy_support : 1;
		uint32_t wq_occupancy_interrupt_support : 1;
		uint32_t wq_op_configration_support : 1;
		uint32_t wq_prs_support : 1;
		uint32_t : 8;
	};
	uint64_t bits;
};

union __attribute__((packed)) GRPCAP {
	struct {
		uint32_t num_groups : 8;
		uint32_t total_read_buffers : 8;
		uint32_t read_buffer_controls_support : 1;
		uint32_t global_read_buffer_limit_support : 1;
		uint32_t desc_inprogress_limit_support : 1;
		uint64_t : 45;
	};
	uint64_t bits;
};

union __attribute__((packed)) ENGCAP {
	struct {
		uint32_t num_engines : 8;
		uint32_t max_work_desc_in_progress : 8;
		uint32_t max_batch_desc_in_progress : 8;
		uint64_t : 40;
	};
	uint64_t bits;
};

struct __attribute__((packed)) TABLEOFFSETS {
	uint32_t group_configuration_offset : 16;
	uint32_t wq_configuration_offset : 16;
	uint32_t msix_permission_offset : 16;
	uint32_t ims_offset : 16;
	uint32_t perfmon_offset : 16;
	uint32_t interdomain_permissions_table_offset : 16;
	uint32_t : 32;
};

union __attribute__((packed)) CMD {
	struct {
		uint32_t operand : 20;
		uint32_t cmd : 5;
		uint32_t : 6;
		uint32_t req_completion_interrupt : 1;
	};
	uint32_t bits;
};

struct __attribute__((packed)) CMD_STATUS {
	uint32_t errcode : 8;
	uint32_t result : 16;
	uint32_t : 7;
	uint32_t active : 1;
};

union __attribute__((packed)) WQCFG {
	struct {
		uint32_t wq_size : 16;
		uint32_t : 16;
		uint32_t wq_threashold : 16;
		uint32_t : 16;
		uint32_t wq_mode : 1;
		uint32_t bof_enable : 1;
		uint32_t ats_disable : 1;
		uint32_t prs_disable : 1;
		uint32_t wq_priority : 4;
		uint32_t pasid : 20;
		uint32_t pasid_enable : 1;
		uint32_t priv : 1;
		uint32_t : 2;
		uint32_t max_xfer_size : 5;
		uint32_t max_batch_size : 4;
		uint32_t : 23;
		uint32_t wq_occupancy_interrupt_handle : 16;
		uint32_t wq_occupancy_interrupt_table : 1;
		uint32_t : 15;
		uint32_t wq_occupancy_limit : 16;
		uint32_t wq_occupancy_interrupt_enable : 1;
		uint32_t : 15;
		uint32_t wq_occupancy : 16;
		uint32_t wq_occupancy_interrupt_generated : 1;
		uint32_t : 12;
		uint32_t wq_mode_support : 1;
		uint32_t wq_state : 2;
		uint32_t : 32;
		uint8_t wq_op_support[32];
	};

	uint64_t bits[4];
};

struct __attribute__((packed)) GROUPCFG {
	// bitmap
	uint8_t wqs[32];

	// bitmap
	uint8_t engines[8];

	uint32_t tc_a : 3;
	uint32_t tc_b : 3;
	uint32_t : 1;
	uint32_t use_global_read_buffer_limit : 1;
	uint32_t read_buffers_reserved : 8;
	uint32_t : 4;
	uint32_t read_buffers_allowed : 8;
	uint32_t : 4;
	uint32_t work_desc_in_progress_limit : 2;
	uint32_t : 2;
	uint32_t batch_desc_in_progress_limit : 2;
	uint32_t : 26;
};

union __attribute__((packed)) GENSTS {
	struct {
		uint32_t state : 2;
		uint32_t reset_type_required : 2;
		uint32_t : 28;
	};
	uint32_t bits;
};

union __attribute__((packed)) GENCFG {
	struct {
		uint32_t glb_read_buffer_limit : 8;
		uint32_t : 4;
		uint32_t user_mode_int_enable : 1;
		uint32_t event_log_enable : 1;
		uint32_t : 18;
	};
	uint32_t bits;
};

union __attribute__((packed)) SWERR {
	struct {
		// Byte 0-7
		uint32_t valid : 1;
		uint32_t overflow : 1;
		uint32_t desc_valid : 1;
		uint32_t wq_idx_valid : 1;
		uint32_t batch_member : 1;
		uint32_t rw : 1;
		uint32_t priv : 1;
		uint32_t err_info_valid : 1;
		uint32_t errcode : 8;
		uint32_t wq_idx : 8;
		uint32_t : 8;
		uint32_t operation : 8;
		uint32_t pasid : 20;
		uint32_t : 4;

		// BYte 8 - 15
		uint32_t batch_idx : 16;
		uint32_t : 16;
		uint32_t err_info : 32;

		// Byte 23 - 16
		uint64_t addr;

		// Byte 31 - 24
		uint64_t : 64;
	};
	uint64_t bits[4];
};

struct __attribute__((packed)) BAR_0 {
	uint32_t version;
	uint32_t : 32;
	uint64_t : 64;
	uint64_t gencap;
	uint64_t : 64;
	uint64_t wq_cap;
	uint64_t : 64;
	uint64_t group_cap;
	uint64_t engine_cap;
	uint8_t op_caps[32];
	uint64_t table_offsets[2];
	uint64_t : 64;
	uint64_t : 64;
	uint32_t gen_config;
	uint32_t : 32;
	uint32_t gen_ctrl;
	uint32_t : 32;
	uint32_t gen_status;
	uint32_t : 32;
	uint32_t int_cause;
	uint32_t : 32;
	uint32_t cmd;
	uint32_t : 32;
	uint32_t cmd_status;
	uint32_t : 32;
	uint32_t cmd_caps;
	uint32_t : 32;
	uint64_t : 64;
	uint8_t swerr[32];
	uint8_t event_log_config[16];
	uint64_t event_log_status;
	uint64_t : 64;
	uint64_t inter_pasid_caps;
	uint32_t inter_pasid_bitmap_reg;
	uint32_t : 32;

	// The reset is determined by the offset table
};

// Interrupt entries
struct dsa_int_entry {
	int vector;
	int idx;
};

struct dsa_workqueue {
	struct dsa_device_info *dsa;
	int wq_idx;

	union WQCFG *wq_cfg;

	unsigned int pasid;

	bool enabled;

	void *wq_portal;
	phys_addr_t wq_phyaddr;

	struct cdev cdev;

	int minor;

	struct dsa_int_entry int_ent;
};

struct dsa_group {
	struct dsa_device_info *dsa;
	int group_idx;

	struct GROUPCFG *group_cfg;
};

struct dsa_device_info {
	struct pci_dev *pdev;
	struct BAR_0 *bar_0;

	unsigned int pasid;
	unsigned int pid;

	bool enabled;

	// Workqueues
	int num_wqs;
	int wqcfg_size;
	int num_kernel_wqs;
	struct dsa_workqueue *wqs;
	int wq_reserved;

	// Groups
	int num_groups;
	struct dsa_group *groups;

	// Engines
	int num_engines;

	// CharDev and ioctl
	char name[20];
	struct cdev cdev;
	struct class *charClass;
	int major;

	spinlock_t cmd_lock;

	// Kernel thread
	struct task_struct *kth;

	// Interrupts
	int irq_cnt;
	struct dsa_int_entry int_ent;
};

union MSIX_PERM {
	struct {
		uint32_t : 2;
		uint32_t : 1;
		uint32_t pasid_en : 1;
		uint32_t : 8;
		uint32_t pasid : 20;
	};
	uint32_t bits;
} __packed;

extern struct dsa_device_info *dsa;

enum DSA_CMD {
	DSA_CMD_ENABLE_DEVICE = 1,
	DSA_CMD_DISABLE_DEVICE = 2,
	DSA_CMD_DRAIN_ALL = 3,
	DSA_CMD_ABORT_ALL = 4,
	DSA_CMD_RESET_DEVICE = 5,
	DSA_CMD_ENABLE_WQ = 6,
	DSA_CMD_DISABLE_WQ = 7,
	DSA_CMD_DRAIN_WQ = 8,
	DSA_CMD_ABORT_WQ = 9,
	DSA_CMD_RESET_WQ = 10,
	DSA_CMD_DRAIN_PASID = 11,
	DSA_CMD_ABORT_PASID = 12,
	DSA_CMD_REQ_INT_HANDLE = 13,
	DSA_CMD_RELEASE_INT_HANDLE = 14,
	DSA_CMD_REQ_IDPT_HANDLE = 15,
	DSA_CMD_REL_IDPT_HANDLE = 16,
	DSA_CMD_INV_SUBMITTER_BITMAP_CACHE = 17
};

#define WQ_CONFIGURATION(bar_0, wq_idx)                                        \
	((union WQCFG *)((uintptr_t)bar_0 +                                    \
			 (((struct TABLEOFFSETS *)(&bar_0->table_offsets))     \
				  ->wq_configuration_offset *                  \
			  0x100) +                                             \
			 wq_idx * (long)int_pow(                               \
					  2, ((union WQCAP *)(&bar_0->wq_cap)) \
							     ->wqcfg_size +    \
						     5)))

#define GROUP_CONFIGURATION(bar_0, group_idx)                                  \
	((struct GROUPCFG *)((uintptr_t)bar_0 +                                \
			     (((struct TABLEOFFSETS *)(&bar_0->table_offsets)) \
				      ->group_configuration_offset *           \
			      0x100) +                                         \
			     group_idx * 64))

#define MSIX_PERM_CONFIGURATION(bar_0, idx)                                    \
	((union MSIX_PERM *)((uintptr_t)bar_0 +                                \
			     (((struct TABLEOFFSETS *)(&bar_0->table_offsets)) \
				      ->msix_permission_offset *               \
			      0x100) +                                         \
			     idx * 8))

#define GENCAP(bar_0) ((struct GENCAP *)(&bar_0->gencap))
#define WQCAP(bar_0) ((union WQCAP *)(&bar_0->wq_cap))
#define GRPCAP(bar_0) ((struct GRPCAP *)(&bar_0->group_cap))
#define ENGINECAP(bar_0) ((struct ENGCAP *)(&bar_0->engine_cap))
#define CMD(bar_0) ((union CMD *)(&bar_0->cmd))
#define CMD_STATUS(bar_0) ((struct CMD_STATUS *)(&bar_0->cmd_status))
#define GENSTS(bar_0) ((struct GENSTS *)(&bar_0->gen_status))
#define GENCFG(bar_0) ((union GENCFG *)(&bar_0->gen_config))

#define GET_GENCAP_VALUE(bar_0, field) (GENCAP(bar_0)->field)
#define GET_WQCAP_VALUE(bar_0, field) (WQCAP(bar_0)->field)
#define GET_GRPCAP_VALUE(bar_0, field) (GRPCAP(bar_0)->field)
#define GET_ENGINECAP_VALUE(bar_0, field) (ENGINECAP(bar_0)->field)
#define GET_GENSTS_VALUE(bar_0, field) (GENSTS(bar_0)->field)

#define GET_WQCFG_VALUE(bar_0, wq_idx, field) \
	(WQ_CONFIGURATION(bar_0, wq_idx)->field)

#define SET_WQCFG_VALUE(bar_0, wq_idx, field, value) \
	(WQ_CONFIGURATION(bar_0, wq_idx)->field = value)

// idxd_get_wq_portal_full_offset
#define WQ_PORTAL_OFFSET(idx, prot) (((idx * 4) << PAGE_SHIFT) + prot * 0x1000)

enum WQ_MODE {
	WQ_MODE_SHARED = 0,
	WQ_MODE_DEDICATED = 1,
};

enum WQ_STATE {
	WQ_STATE_DISABLED = 0,
	WQ_STATE_ENABLED = 1,
	WQ_STATE_INPROGRESS = 2,
	WQ_STATE_RESERVED = 3,
};

enum DEVICE_STATE {
	DISABLED = 0,
	ENABLED = 1,
	INPROGRESS = 2,
	HALTED = 3,
};

static inline bool is_cap_supported(struct BAR_0 *bar_0, int opcode)
{
	return bar_0->op_caps[opcode / 8] & (1 << (opcode % 8));
}

static inline bool is_engine_in_group(struct BAR_0 *bar_0, int group_idx,
				      int engine_idx)
{
	struct GROUPCFG *group_cfg = GROUP_CONFIGURATION(bar_0, group_idx);
	return group_cfg->engines[engine_idx / 8] & (1 << (engine_idx % 8));
}

static inline bool add_engine_to_group(struct BAR_0 *bar_0, int group_idx,
				       int engine_idx)
{
	struct GROUPCFG *group_cfg = GROUP_CONFIGURATION(bar_0, group_idx);
	group_cfg->engines[engine_idx / 8] |= 1 << (engine_idx % 8);

	return is_engine_in_group(bar_0, group_idx, engine_idx);
}

static inline bool is_wq_in_group(struct BAR_0 *bar_0, int group_idx,
				  int wq_idx)
{
	struct GROUPCFG *group_cfg = GROUP_CONFIGURATION(bar_0, group_idx);
	return group_cfg->wqs[wq_idx / 8] & (1 << (wq_idx % 8));
}

static inline bool add_wq_to_group(struct BAR_0 *bar_0, int group_idx,
				   int wq_idx)
{
	struct GROUPCFG *group_cfg = GROUP_CONFIGURATION(bar_0, group_idx);
	group_cfg->wqs[wq_idx / 8] |= 1 << (wq_idx % 8);

	return is_wq_in_group(bar_0, group_idx, wq_idx);
}

static inline bool reset_group(struct BAR_0 *bar_0, int group_idx)
{
	struct GROUPCFG *group_cfg = GROUP_CONFIGURATION(bar_0, group_idx);
	for (int i = 0; i < 32; i++) {
		group_cfg->wqs[i] = 0;
	}

	for (int i = 0; i < 8; i++) {
		group_cfg->engines[i] = 0;
	}

	return true;
}

static inline bool dsa_is_cmd_active(struct BAR_0 *bar_0)
{
	return ioread32(&bar_0->cmd_status) & 0x1;
}

static inline uint32_t dsa_send_cmd(struct dsa_device_info *dsa, uint32_t cmd,
				    uint32_t operand)
{
	union CMD cmd_tmp;
	union CMD *cmd_reg;
	struct BAR_0 *bar_0 = dsa->bar_0;

	memset(&cmd_tmp, 0, sizeof(union CMD));
	cmd_tmp.cmd = cmd;
	cmd_tmp.operand = operand;

	cmd_reg = CMD(bar_0);

	spin_lock(&dsa->cmd_lock);
	iowrite32(cmd_tmp.bits, &bar_0->cmd);
	while (dsa_is_cmd_active(bar_0))
		cpu_relax();

	spin_unlock(&dsa->cmd_lock);

	return CMD_STATUS(bar_0)->errcode;
}

static inline enum DEVICE_STATE dsa_get_device_state(struct BAR_0 *bar_0)
{
	union GENSTS gensts;
	gensts.bits = ioread32(&bar_0->gen_status);
	return gensts.state;
}

static inline void dsa_set_user_int(struct dsa_device_info *dsa, bool enable)
{
	union GENCFG gencfg;
	gencfg.bits = ioread32(&dsa->bar_0->gen_config);
	gencfg.user_mode_int_enable = enable;
	iowrite32(gencfg.bits, &dsa->bar_0->gen_config);
}

static inline void dsa_set_msix_perm(struct dsa_device_info *dsa, int pasid,
				     int idx)
{
	union MSIX_PERM perm;

	if (pasid == 0)
		return;

	perm.bits = 0;
	perm.pasid = pasid;
	perm.pasid_en = 1;
	iowrite32(perm.bits, MSIX_PERM_CONFIGURATION(dsa->bar_0, idx));

	perm.bits = ioread32(MSIX_PERM_CONFIGURATION(dsa->bar_0, idx));
	pr_info("MSIX PASID: %d, IDX: %d, PASID_EN: %d\n", perm.pasid, idx,
		perm.pasid_en);
}

#endif
