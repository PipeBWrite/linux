#include "dsa.h"
#include "ctrl.h"
#include "dump.h"
#include <linux/pci.h>
#include <linux/io-64-nonatomic-lo-hi.h>

void dsa_configure_wqs_user(struct dsa_device_info *dsa, unsigned int pasid)
{
	struct device *dev = &dsa->pdev->dev;

	for (int i = 0; i < dsa->num_wqs; i++) {
		dev_info(dev, "Configuring WQ %d\n", i);

		union WQCFG wqcfg;
		dsa_read_wqcfg(&wqcfg, dsa, i);
		wqcfg.wq_mode = WQ_MODE_DEDICATED;
		wqcfg.wq_size = 16;
		wqcfg.wq_threashold = 16;
		wqcfg.wq_priority = 1;
		wqcfg.bof_enable = 1;
		wqcfg.max_batch_size = 1;
		wqcfg.max_xfer_size = 21;
		wqcfg.pasid = pasid;
		wqcfg.pasid_enable = 1;
		wqcfg.priv = 1;
		dsa_write_wqcfg(&wqcfg, dsa, i);

		if (i == 0) {
			dev_info(dev, "==> Dumping WQCFG for WQ %d", i);
			dump_wqcfg(dsa, i);
		}
	}
}

void dsa_configure_wqs(struct dsa_device_info *dsa)
{
	struct device *dev = &dsa->pdev->dev;

	for (int i = 0; i < dsa->num_wqs; i++) {
		dev_info(dev, "Configuring WQ %d\n", i);

		union WQCFG wqcfg;
		dsa_read_wqcfg(&wqcfg, dsa, i);
		wqcfg.wq_state = 1;
		wqcfg.wq_mode = WQ_MODE_DEDICATED;
		wqcfg.wq_size = 16;
		wqcfg.wq_priority = 1;
		wqcfg.bof_enable = 0;
		wqcfg.max_batch_size = 1;
		wqcfg.max_xfer_size = 21;
		wqcfg.pasid = dsa->pasid;
		wqcfg.pasid_enable = 1;
		dsa_write_wqcfg(&wqcfg, dsa, i);

		// dev_info(dev, "==> Dumping WQCFG for WQ %d", i);
		// dump_wqcfg(dsa, i);
	}
}

int dsa_device_enable(struct dsa_device_info *dsa, bool enable)
{
	struct device *dev = &dsa->pdev->dev;
	int ret = 0;
	dev_info(dev, "Before: Current DSA enable status: %d\n",
		 dsa_get_device_state(dsa->bar_0));

	if (enable && !dsa->enabled) {
		ret = dsa_send_cmd(dsa, DSA_CMD_ENABLE_DEVICE, 0);
		dsa->enabled = true;
	} else if (!enable && dsa->enabled) {
		ret = dsa_send_cmd(dsa, DSA_CMD_DISABLE_DEVICE, 0);
		dsa->enabled = false;
	}

	dev_info(dev, "Enable device errcode: %d\n", ret);

	// Verify
	dev_info(dev, "After: Current DSA enable status: %d\n",
		 dsa_get_device_state(dsa->bar_0));

	return ret;
}

int dsa_wq_enable(struct dsa_device_info *dsa, int idx, bool enable)
{
	struct device *dev = &dsa->pdev->dev;
	int ret = 0;
	union WQCFG wqcfg;
	// dev_info(dev, "Try to %s WQ %d\n", enable ? "enable" : "disable", idx);

	if (enable && !dsa->wqs[idx].enabled) {
		ret = dsa_send_cmd(dsa, DSA_CMD_ENABLE_WQ, idx);
		dsa->wqs[idx].enabled = true;
	} else if (!enable && dsa->wqs[idx].enabled) {
		ret = dsa_send_cmd(dsa, DSA_CMD_DISABLE_WQ, idx);
		dsa->wqs[idx].enabled = false;
	}

	dsa_read_wqcfg(&wqcfg, dsa, idx);
	dev_info(dev, "WQ %d is_enabled: %d\n", idx, wqcfg.wq_state);

	dev_info(dev, "%s WQ %d errcode: %d\n", enable ? "Enable" : "Disable",
		 idx, ret);
	// dump_wqcfg(dsa, idx);

	return ret;
}

void dsa_read_wqcfg(union WQCFG *out, struct dsa_device_info *dsa, int idx)
{
	void *addr = dsa->wqs[idx].wq_cfg;
	memset(out, 0, sizeof(union WQCFG));
	for (int i = 0; i < (dsa->wqcfg_size / 8); i++) {
		out->bits[i] = ioread64(
			(void *)((uintptr_t)addr + i * sizeof(uint64_t)));
	}
}

void dsa_write_wqcfg(union WQCFG *in, struct dsa_device_info *dsa, int idx)
{
	void *addr = dsa->wqs[idx].wq_cfg;
	for (int i = 0; i < (dsa->wqcfg_size / 8); i++) {
		iowrite64(in->bits[i],
			  (void *)((uintptr_t)addr + i * sizeof(uint64_t)));
	}
}

void dsa_dump_swerr(struct dsa_device_info *dsa)
{
	union SWERR swerr;
	void *addr = &dsa->bar_0->swerr;
	struct device *dev = &dsa->pdev->dev;

	memset(&swerr, 0, sizeof(union SWERR));
	for (int i = 0; i < 4; i++) {
		swerr.bits[i] = ioread64(
			(void *)((uintptr_t)addr + i * sizeof(uint64_t)));
	}

	dev_err(dev, "SWERR dump begin\n");
	dev_err(dev, "valid: 0x%x\n", swerr.valid);
	dev_err(dev, "overflow: 0x%x\n", swerr.overflow);
	dev_err(dev, "desc_valid: 0x%x\n", swerr.desc_valid);
	dev_err(dev, "wq_idx_valid: 0x%x\n", swerr.wq_idx_valid);
	dev_err(dev, "batch_member: 0x%x\n", swerr.batch_member);
	dev_err(dev, "rw: 0x%x\n", swerr.rw);
	dev_err(dev, "priv: 0x%x\n", swerr.priv);
	dev_err(dev, "err_info_valid: %x\n", swerr.err_info_valid);
	dev_err(dev, "errcode: 0x%x\n", swerr.errcode);
	dev_err(dev, "wq_idx: 0x%x\n", swerr.wq_idx);
	dev_err(dev, "operation: 0x%x\n", swerr.operation);
	dev_err(dev, "pasid: 0x%x\n", swerr.pasid);
	dev_err(dev, "batch_idx: 0x%x\n", swerr.batch_idx);
	dev_err(dev, "err_info: 0x%x\n", swerr.err_info);
	dev_err(dev, "addr: %llx\n", swerr.addr);
	dev_err(dev, "SWERR dump end\n");
}
