#include <asm/io.h>
#include <linux/types.h>
#include <linux/module.h>
#include <linux/cpufeature.h>
#include <linux/pci.h>
#include <linux/iommu.h>
#include <linux/io-64-nonatomic-lo-hi.h>
#include <linux/idxd.h>
#include <linux/delay.h>
#include <linux/dsa_sl.h>
#include "dsa.h"

struct dsa_device_info *dsa;

#ifndef CONFIG_INTEL_DSA_SL_EMU
#include "dump.h"
#include "ctrl.h"
#include "irq.h"
#include "ioctl.h"

static const struct pci_device_id dsa_sl_pci_tbl[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_DSA_SPR0) },
	{
		0,
	}
};

static int dsa_get_wq_info(void)
{
	struct pci_dev *pdev = dsa->pdev;
	struct device *dev = &pdev->dev;
	resource_size_t start;

	// Map the BAR2 for submission
	start = pci_resource_start(pdev, 2);

	// Setup
	union WQCAP wqcap;
	wqcap.bits = ioread64(&dsa->bar_0->wq_cap);

	dsa->num_wqs = wqcap.num_wq;
	dsa->num_kernel_wqs = wqcap.num_wq > 4 ? 2 : 1;
	dsa->wq_reserved = dsa->num_wqs;
	dsa->wqs = kcalloc_node(dsa->num_wqs, sizeof(struct dsa_workqueue),
				GFP_KERNEL, dev_to_node(dev));
	dsa->wqcfg_size = int_pow(2, 5 + wqcap.wqcfg_size);
	dev_info(dev, "WQ CFG size is %d\n", dsa->wqcfg_size);

	// Configure individual workqueues
	union WQCFG wqcfg;
	for (int i = 0; i < wqcap.num_wq; i++) {
		resource_size_t wq_offset = start + WQ_PORTAL_OFFSET(i, 1);
		dsa->wqs[i].dsa = dsa;
		dsa->wqs[i].wq_idx = i;
		dsa->wqs[i].wq_cfg = WQ_CONFIGURATION(dsa->bar_0, i);
		dsa->wqs[i].wq_portal =
			devm_ioremap(dev, wq_offset, (1UL << 12));
		dsa->wqs[i].wq_phyaddr = wq_offset;
		pr_info("WQ %d phys addr: %p\n", i, (void *)wq_offset);
		if (!dsa->wqs[i].wq_portal) {
			dev_err(dev, "Failed to map WQ portal\n");
			return -ENOMEM;
		} else {
			dev_info(dev, "Mapped WQ portal for %d\n", i);
		}
		dsa_read_wqcfg(&wqcfg, dsa, i);
		dev_info(dev, "==> WQ %d is %s\n", i,
			 wqcfg.wq_state == 0 ? "disabled" :
			 wqcfg.wq_state == 1 ? "enabled" :
			 wqcfg.wq_state == 2 ? "progress" :
					       "???");
	}

	return 0;
}

static int dsa_get_engine_info(void)
{
	union ENGCAP engcap;
	engcap.bits = ioread64(&dsa->bar_0->engine_cap);

	dump_engcap(&dsa->bar_0->engine_cap);

	dsa->num_engines = engcap.num_engines;

	return 0;
}

static int dsa_get_group_info(void)
{
	struct device *dev = &dsa->pdev->dev;

	// Setup
	union GRPCAP grpcap;
	grpcap.bits = ioread64(&dsa->bar_0->group_cap);

	dump_grpcap(&dsa->bar_0->group_cap);

	dsa->num_groups = grpcap.num_groups;
	dsa->groups = kcalloc_node(dsa->num_groups, sizeof(struct dsa_group),
				   GFP_KERNEL, dev_to_node(dev));

	dump_grpcap(&dsa->bar_0->group_cap);

	for (int i = 0; i < grpcap.num_groups; i++) {
		dsa->groups[i].group_idx = i;
		dsa->groups[i].group_cfg = GROUP_CONFIGURATION(dsa->bar_0, i);
	}

	return 0;
}

static int dsa_setup_internal(struct dsa_device_info *dsa)
{
	struct device *dev = &dsa->pdev->dev;

	// Gather WQ, Group and Engine info
	dsa_get_wq_info();
	dsa_get_group_info();
	dsa_get_engine_info();

	// Check if the device supports requesting interrupt handle
	dev_info(dev, "DSA supported cmds: %#x\n", dsa->bar_0->cmd_caps);
	if (dsa->bar_0->cmd_caps & BIT(13)) {
		dev_err(dev,
			"DSA supports requesting int handle, but our driver does not implement this!");
	}

	// Add engine to group
	for (int i = 0; i < dsa->num_engines; i++) {
		dev_info(dev, "Adding Engine %d to group 0\n", i);

		add_engine_to_group(dsa->bar_0, 0, i);
	}

	// Add workqueue to group
	for (int i = 0; i < dsa->num_wqs; i++) {
		dev_info(dev, "Adding Workqueue %d to group 0\n", i);

		add_wq_to_group(dsa->bar_0, 0, i);
	}

	// Set up DSA interrupt
	dsa_setup_interrupt(dsa);

	return 0;
}

static int dsa_sl_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	int rc;
	int ret;
	struct device *dev = &pdev->dev;

	// Let's only care about node 0 for now
	if (dev_to_node(dev) != 0) {
		return 0;
	}

	printk(KERN_INFO "DSA Standalone driver probed\n");

	rc = pci_enable_device(pdev);
	if (rc) {
		return rc;
	}
	dev_info(dev, "PCI device enabled\n");

	// Allocate and initialize dsa structure
	dsa = kmalloc(sizeof(struct dsa_device_info), GFP_KERNEL);
	memset(dsa, 0, sizeof(struct dsa_device_info));
	if (!dsa) {
		dev_err(dev, "Failed to allocate memory for dsa_device_info\n");
		goto err_alloc;
	}
	spin_lock_init(&dsa->cmd_lock);
	dev_info(dev, "Memory allocated for dsa_device_info\n");

	dsa->pdev = pdev;
	dsa->bar_0 = pci_iomap(pdev, 0, 0);
	if (!dsa->bar_0) {
		dev_err(dev, "Failed to map BAR0\n");
		goto err_iomap;
	}
	dev_info(dev, "BAR0 mapped\n");

	rc = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (rc) {
		dev_err(dev, "Failed to set DMA mask\n");
		goto err_dma_mask;
	}
	dev_info(dev, "DMA mask set\n");

	pci_set_master(pdev);
	pci_set_drvdata(pdev, dsa);
	dev_info(dev, "Master set\n");

	// Let's try dumping
	dev_info(dev, "DSA version: %#x\n", dsa->bar_0->version);
	dev_info(dev, " -- Current device state: %#x\n",
		 dsa_get_device_state(dsa->bar_0));
	if (dsa_get_device_state(dsa->bar_0) == HALTED) {
		dev_warn(dev, "Device is halted\n");
		return -ENXIO;
	}
	ret = dsa_send_cmd(dsa, DSA_CMD_RESET_DEVICE, 0);
	dev_info(dev, "Reset device errcode: %d\n", ret);

	// Enable SVA feature
	ret = iommu_dev_enable_feature(dev, IOMMU_DEV_FEAT_IOPF);
	if (ret) {
		dev_info(dev, "Failed to enable IOPF IOMMU feature\n");
		return ret;
	}

	ret = iommu_dev_enable_feature(dev, IOMMU_DEV_FEAT_SVA);
	if (ret) {
		iommu_dev_disable_feature(dev, IOMMU_DEV_FEAT_IOPF);
		dev_info(dev, "Enable SVA failed!\n");
		return ret;
	}

	// PASID
	struct iommu_domain *domain;
	ioasid_t pasid;
	domain = iommu_get_domain_for_dev(dev);
	if (!domain) {
		dev_info(dev, "Failed to get domain for DSA\n");
		return -EPERM;
	}

	pasid = iommu_alloc_global_pasid(dev);
	if (pasid == IOMMU_PASID_INVALID) {
		dev_info(dev, "Failed to allocate global PASID\n");
		return -ENOSPC;
	}

	ret = iommu_attach_device_pasid(domain, dev, pasid);
	if (ret) {
		dev_err(dev,
			"Failed to attach device pasid %d, domain type %d\n",
			pasid, domain->type);
		iommu_free_global_pasid(pasid);
		return ret;
	}

	dsa_set_user_int(dsa, 0);
	dev_info(dev, "Disabled User Mode Interrupt\n");

	dsa->pasid = pasid;
	dev_info(dev, "Enabled PASID!\n");

	// TODO: Setup WQs, engines, groups, etc. See function `idxd_setup_internals`
	dsa_setup_internal(dsa);
	dev_info(dev, "Internal setup done\n");

	// Do A Test Memcpy
	// dev_info(dev, "Begin memcpy test\n");
	// try_memcpy(dsa);
	// dev_info(dev, "End memcpy test\n");

	dsa_register_chardev(dsa);

	return 0;

err_dma_mask:
	pci_iounmap(pdev, dsa->bar_0);
err_iomap:
	kfree(dsa);
err_alloc:
	pci_disable_device(pdev);
	return -ENOMEM;
}

static void dsa_sl_remove(struct pci_dev *pdev)
{
	if (dev_to_node(&pdev->dev) != 0) {
		// Continue
		return;
	}
	dsa_unregister_chardev(pci_get_drvdata(pdev));
	struct dsa_device_info *dsa = pci_get_drvdata(pdev);
	struct device *dev = &pdev->dev;
	printk(KERN_INFO "DSA Standalone driver removed\n");

	dsa_release_interrupt(dsa);

	// Disable WQs
	for (int i = 0; i < dsa->num_wqs; i++) {
		dsa_wq_enable(dsa, i, false);
	}

	// Disable device
	dsa_device_enable(dsa, false);

	// Disable user_int
	dsa_set_user_int(dsa, 0);

	// Disable PASID
	struct iommu_domain *domain;
	domain = iommu_get_domain_for_dev(dev);
	if (!domain) {
		dev_warn(dev,
			 "Failed to get the domain for device on remove!\n");
	} else {
		iommu_detach_device_pasid(domain, dev, dsa->pasid);
		iommu_free_global_pasid(dsa->pasid);

		dsa_set_user_int(dsa, 0);
		dev_info(dev, "On remove: Deallocated PASID, etc.\n");
	}

	// Disable SVA
	iommu_dev_disable_feature(dev, IOMMU_DEV_FEAT_SVA);
	iommu_dev_disable_feature(dev, IOMMU_DEV_FEAT_IOPF);

	pci_iounmap(pdev, dsa->bar_0);
	if (dsa->wqs) {
		kfree(dsa->wqs);
	}
	if (dsa->groups) {
		kfree(dsa->groups);
	}
	kfree(dsa);
	pci_disable_device(pdev);
}

static void dsa_sl_shutdown(struct pci_dev *pdev)
{
	printk(KERN_INFO "DSA Standalone driver shutdown\n");
}

static struct pci_driver dsa_sl_pci_driver = {
	.name = "dsa_sl",
	.id_table = dsa_sl_pci_tbl,
	.probe = dsa_sl_probe,
	.remove = dsa_sl_remove,
	.shutdown = dsa_sl_shutdown,
};
#endif

static int __init dsa_sl_init(void)
{
	printk(KERN_INFO "DSA Standalone driver loaded\n");

#ifndef CONFIG_INTEL_DSA_SL_EMU
	int err;
	if (!cpu_feature_enabled(X86_FEATURE_MOVDIR64B)) {
		printk(KERN_ERR "MOVDIR64B not supported\n");
		return -ENODEV;
	}

	if (!cpu_feature_enabled(X86_FEATURE_ENQCMD)) {
		printk(KERN_ERR "MOVCMD not supported\n");
		return -ENODEV;
	}

	err = pci_register_driver(&dsa_sl_pci_driver);

	if (err) {
		goto err_pci_register;
	}

	return 0;

err_pci_register:
	return err;
#else
#include "sysfs_emu.h"
	int ret = dsa_emu_init_sysfs();
	if (ret) {
		pr_err("Failed to init sysfs\n");
		return ret;
	}

	pr_info("Running in emu mode!\n");

	return 0;
#endif
}

static void __exit dsa_sl_exit(void)
{
	printk(KERN_INFO "DSA Standalone driver unloaded\n");
#ifndef CONFIG_INTEL_DSA_SL_EMU
	pci_unregister_driver(&dsa_sl_pci_driver);
#else
	dsa_emu_thread_stop();
#endif
}

late_initcall(dsa_sl_init);
module_exit(dsa_sl_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("NA");
MODULE_DESCRIPTION("DSA Standalone driver");
MODULE_VERSION("1.0");
