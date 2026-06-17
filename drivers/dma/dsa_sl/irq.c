#include <linux/irq.h>
#include <linux/types.h>
#include <linux/pci.h>
#include "dsa.h"
#include "irq.h"

static irqreturn_t dsa_interrupt_thread(int vec, void *data)
{
	struct dsa_workqueue *wq = data;
	struct dsa_device_info *dsa = wq->dsa;
	struct device *dev = &dsa->pdev->dev;

	dev_info(dev, "dsa interrupt on MSI-X vector %d\n", vec);
	return IRQ_HANDLED;
}

static irqreturn_t dsa_wq_interrupt_thread(int vec, void *data)
{
	struct dsa_workqueue *wq = data;
	struct dsa_device_info *dsa = wq->dsa;
	struct device *dev = &dsa->pdev->dev;

	dev_info(dev, "wq interrupt on MSI-X vector %d\n", vec);
	return IRQ_HANDLED;
}

int dsa_wq_setup_interrupt(struct dsa_device_info *dsa,
			   struct dsa_workqueue *wq)
{
	struct pci_dev *pdev = dsa->pdev;
	struct device *dev = &pdev->dev;
	int pasid = wq->pasid;
	struct dsa_int_entry *int_ent = &wq->int_ent;
	int rc;

	int_ent->idx = wq->wq_idx + 1;
	int_ent->vector = pci_irq_vector(pdev, int_ent->idx);

	dsa_set_msix_perm(dsa, pasid, int_ent->idx);
	rc = request_threaded_irq(int_ent->vector, NULL,
				  dsa_wq_interrupt_thread, 0,
				  "dsa_wq_interrupt", wq);

	if (rc < 0) {
		dev_err(dev, "Failed to request wq irq on MSI-X vector %d\n",
			int_ent->vector);
		goto err_wq_irq;
	}

	dev_info(dev, "Requested wq irq on MSI-X vector %d\n", int_ent->vector);

	return 0;

err_wq_irq:
	return rc;
}

int dsa_wq_free_interrupt(struct dsa_device_info *dsa, struct dsa_workqueue *wq)
{
	struct dsa_int_entry *int_ent = &wq->int_ent;
	struct device *dev = &dsa->pdev->dev;
	if (int_ent->vector != 0) {
		free_irq(int_ent->vector, wq);
		dev_info(dev, "Released wq irq on MSI-X vector %d\n",
			 int_ent->vector);
	}

	return 0;
}

int dsa_setup_interrupt(struct dsa_device_info *dsa)
{
	struct pci_dev *pdev = dsa->pdev;
	struct device *dev = &pdev->dev;

	int msixcnt = pci_msix_vec_count(pdev);
	if (msixcnt < 0) {
		dev_err(dev, "Not MSI-X interrupt capable.\n");
		return -ENOSPC;
	}
	dsa->irq_cnt = msixcnt;

	int rc = pci_alloc_irq_vectors(pdev, msixcnt, msixcnt, PCI_IRQ_MSIX);
	if (rc != msixcnt) {
		dev_err(dev, "Failed to allocate %d MSI-X interrupts: %d\n",
			msixcnt, rc);
		return -ENOSPC;
	}
	dev_info(dev, "Enabled %d MSI-X vectors\n", msixcnt);

	dsa->int_ent.vector = pci_irq_vector(pdev, 0);
	dsa->int_ent.idx = 0;
	rc = request_threaded_irq(dsa->int_ent.vector, NULL,
				  dsa_interrupt_thread, 0, "dsa_interrupt",
				  dsa);

	if (rc < 0) {
		dev_err(dev, "Failed to allocate interrupt: %d\n", rc);
		goto err_dsa_irq;
	}
	dev_info(dev, "Requested dsa threaded irq on MSI-X vector %d\n",
		 dsa->int_ent.vector);

	return 0;

err_dsa_irq:
	pci_free_irq_vectors(pdev);
	return rc;
}

int dsa_release_interrupt(struct dsa_device_info *dsa)
{
	struct pci_dev *pdev = dsa->pdev;
	struct device *dev = &pdev->dev;

	free_irq(dsa->int_ent.vector, dsa);
	dev_info(dev, "Released dsa threaded irq on MSI-X vector %d\n",
		 dsa->int_ent.vector);

	pci_free_irq_vectors(pdev);
	dev_info(dev, "Freed %d MSI-X vectors\n", dsa->irq_cnt);

	return 0;
}
