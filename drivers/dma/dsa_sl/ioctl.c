#include "asm/current.h"
#include "dump.h"
#include "ioctl.h"
#include <uapi/linux/dsa_sl.h>
#include "asm/page_types.h"
#include "asm/pgtable_types.h"
#include "ctrl.h"
#include "dsa.h"
#include "linux/device.h"
#include "linux/fs.h"
#include "linux/kdev_t.h"
#include "linux/printk.h"
#include "linux/types.h"
#include "linux/uaccess.h"
#include "memcpy.h"
#include "thread.h"
#include "irq.h"
#include <linux/iommu.h>
#include <linux/pci.h>

int wq_open(struct inode *inode, struct file *file)
{
	struct dsa_workqueue *wq =
		container_of(inode->i_cdev, struct dsa_workqueue, cdev);
	if (wq->enabled == 0) {
		return -EBUSY;
	}
	file->private_data = wq;
	return 0;
}

static int wq_close(struct inode *inode, struct file *file)
{
	return 0;
}

int wq_mmap(struct file *flip, struct vm_area_struct *vma)
{
	struct dsa_workqueue *wq = flip->private_data;
	unsigned long pfn = wq->wq_phyaddr >> PAGE_SHIFT;
	vm_flags_set(vma, VM_DONTCOPY);
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	if (io_remap_pfn_range(vma, vma->vm_start, pfn, PAGE_SIZE,
			       vma->vm_page_prot))
		return -EAGAIN;
	return 0;
}

static struct file_operations wq_fops = { .open = wq_open,
					  .release = wq_close,
					  .mmap = wq_mmap };

void wq_register_chardev(struct dsa_workqueue *wq, int index)
{
	cdev_init(&wq->cdev, &wq_fops);
	pr_info("Registering chardev for wq %d\n", index);
	dev_t dev = MKDEV(wq->dsa->major, index + 1);
	wq->minor = index + 1;
	if (cdev_add(&wq->cdev, dev, 1) < 0) {
		dev_err(&wq->dsa->pdev->dev, "Failed to add cdev\n");
		device_destroy(wq->dsa->charClass, dev);
		return;
	}
	pr_info("Added cdev for wq %d\n", index);
	if (IS_ERR(device_create(wq->dsa->charClass, NULL, dev, NULL, "wq%d",
				 index))) {
		dev_err(&wq->dsa->pdev->dev, "Failed to create device\n");
		device_destroy(wq->dsa->charClass, dev);
		return;
	};
}

void wq_unregister_chardev(struct dsa_workqueue *wq)
{
	dev_t dev = MKDEV(wq->dsa->major, wq->minor);
	device_destroy(wq->dsa->charClass, dev);
	cdev_del(&wq->cdev);
}

static inline struct dsa_device_info *file_to_dsa(struct file *file)
{
	return container_of(file->f_inode->i_cdev, struct dsa_device_info,
			    cdev);
}

long dsa_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	if (_IOC_TYPE(cmd) != DSA_IOCTL_MAGIC) {
		return -ENOTTY;
	}

	if (_IOC_NR(cmd) > DSA_IOCTL_MAX) {
		return -ENOTTY;
	}

	struct dsa_device_info *dsa = file_to_dsa(file);

	switch (cmd) {
	case DSA_IOCTL_MEMCPY: {
		struct dsa_memcpy_args args;
		if (copy_from_user(&args, (void __user *)arg, sizeof(args))) {
			return -EFAULT;
		}
		if (dsa) {
			dsa_memcpy_user((void *)args.dst, (void *)args.src,
					args.size);
		} else {
			pr_info("MEMCPY CALLED, dsa = NULL\n");
		}
		break;
	}
	case DSA_IOCTL_MEMCPY_PPL: {
		struct dsa_memcpy_args args;
		if (copy_from_user(&args, (void __user *)arg, sizeof(args))) {
			return -EFAULT;
		}
		if (dsa) {
			dsa_copy_from_user((void *)args.dst, (void *)args.src,
					   args.size);
		} else {
			pr_info("MEMCPY_PPL CALLED, dsa = NULL\n");
		}
		break;
	}
	case DSA_IOCTL_COPY_TO_USER: {
		void *user_buf;
		user_buf = (void *)arg;
		if (user_buf == NULL) {
			pr_info("User buffer is NULL\n");
			return -EFAULT;
		}
		void *kernel_buf = vmalloc((1 << 30));
		if (kernel_buf == NULL) {
			pr_info("Failed to allocate kernel buffer\n");
			return -ENOMEM;
		}
		memset(kernel_buf, 0x51, (1 << 30));
		pr_info("Kernel buffer allocated\n");
		struct timespec64 start, end;
		ktime_get_real_ts64(&start);
		int blk_size = (32 << 10);
		for (int i = 0; i < 10; i++) {
			int ret = copy_to_user(user_buf + blk_size * i,
					       kernel_buf + blk_size * i,
					       blk_size);
			if (ret < 0) {
				pr_info("Failed to copy to user\n");
				vfree(kernel_buf);
				return -EFAULT;
			}
		}
		// dsa_copy_to_user(user_buf, kernel_buf, (32 << 10));
		// pr_info("Copied to user\n");
		ktime_get_real_ts64(&end);
		dev_info(&dsa->pdev->dev, "Bandwidth: %lldGB/s\n",
			 100000000000 / ((end.tv_sec - start.tv_sec) +
					 (end.tv_nsec - start.tv_nsec)));
		vfree(kernel_buf);
		break;
	}
	case DSA_IOCTL_CHECK_BUF: {
#ifdef CONFIG_DSA_SL_ENABLE_USER_THREAD
		struct dsa_user_ctx *ctx = file->private_data;
		dsa_dumpbuf(ctx->reqs);
		break;
#else
		break;
#endif
	}
	default:
		return -EINVAL;
	}

	return 0;
}

int dsa_open(struct inode *inode, struct file *file)
{
	struct dsa_device_info *dsa =
		container_of(inode->i_cdev, struct dsa_device_info, cdev);
	struct device *dev = &dsa->pdev->dev;

	dev_info(dev, "Opened device %s: pid: %d\n", dsa->name, current->pid);

	struct dsa_user_ctx *ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	dsa->pid = current->pid;
	ctx->pid = current->pid;
	file->private_data = ctx;

	struct iommu_sva *sva = iommu_sva_bind_device(dev, current->mm);
	if (IS_ERR(sva)) {
		dev_err(dev, "Failed to bind device to process\n");
		kfree(ctx);
		return PTR_ERR(sva);
	}

	unsigned int pasid = iommu_sva_get_pasid(sva);
	if (pasid == IOMMU_PASID_INVALID) {
		dev_err(dev, "Failed to get PASID\n");
		iommu_sva_unbind_device(sva);
		kfree(ctx);
		return -EINVAL;
	}

	ctx->sva = sva;
	ctx->pasid = pasid;
	ctx->mm = current->mm;

#ifdef CONFIG_DSA_SL_ENABLE_USER_THREAD
	ctx->reqs = kzalloc(sizeof(*ctx->reqs), GFP_KERNEL);
	if (!ctx->reqs) {
		dev_err(dev, "Failed to allocate reqs\n");
		iommu_sva_unbind_device(sva);
		kfree(ctx);
		return -ENOMEM;
	}
	ctx->reqs->head = 0;
	ctx->reqs->tail = 0;
#endif

	dsa_configure_wqs_user(dsa, pasid);

	dsa_device_enable(dsa, true);

	for (int i = 0; i < dsa->num_wqs; i++) {
		dsa->wqs[i].pasid = pasid;
		dsa_wq_setup_interrupt(dsa, &dsa->wqs[i]);
		dsa_wq_enable(dsa, i, true);
	}

#ifdef CONFIG_DSA_SL_ENABLE_USER_THREAD
	// Start thread
	dsa_start_thread(dsa, ctx->reqs);
#endif

	return 0;
}

static int dsa_close(struct inode *inode, struct file *file)
{
	struct dsa_device_info *dsa =
		container_of(inode->i_cdev, struct dsa_device_info, cdev);
	struct device *dev = &dsa->pdev->dev;

	dev_info(dev, "Closed device %s\n", dsa->name);

	struct dsa_user_ctx *ctx = file->private_data;

	iommu_sva_unbind_device(ctx->sva);
	kfree(ctx);

	for (int i = 0; i < dsa->num_wqs; i++) {
		dsa_wq_enable(dsa, i, false);
		dsa_wq_free_interrupt(dsa, &dsa->wqs[i]);
	}

	dsa_device_enable(dsa, false);

	// Stop threads
	dsa_stop_thread(dsa);

	return 0;
}

static int dsa_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct dsa_device_info *dsa = file_to_dsa(filp);
	struct dsa_user_ctx *ctx = filp->private_data;
	int ret;

	ret = remap_pfn_range(vma, vma->vm_start,
			      virt_to_phys(ctx->reqs) >> PAGE_SHIFT,
			      vma->vm_end - vma->vm_start, vma->vm_page_prot);

	if (ret < 0) {
		dev_err(&dsa->pdev->dev, "Failed to mmap\n");
		return ret;
	}

	return 0;
}

static struct file_operations fops = {
	.open = dsa_open,
	.release = dsa_close,
	.unlocked_ioctl = dsa_ioctl,
	.mmap = dsa_mmap,
};

void dsa_register_chardev(struct dsa_device_info *dsa)
{
	dev_t dev;
	alloc_chrdev_region(&dev, 0, 3, "dsa");
	dsa->major = MAJOR(dev);
	sprintf(dsa->name, "dsa");
	if (dsa->major < 0) {
		dev_err(&dsa->pdev->dev, "Failed to register chardev\n");
		return;
	}
	dev_info(&dsa->pdev->dev, "Registered chardev with major %d\n",
		 dsa->major);

	cdev_init(&dsa->cdev, &fops);
	if (cdev_add(&dsa->cdev, MKDEV(dsa->major, 0), 1) < 0) {
		dev_err(&dsa->pdev->dev, "Failed to add cdev\n");
		device_destroy(dsa->charClass, MKDEV(dsa->major, 0));
		unregister_chrdev(dsa->major, "dsa");
		return;
	}

	dsa->charClass = class_create("dsa");
	if (IS_ERR(dsa->charClass)) {
		dev_err(&dsa->pdev->dev, "Failed to create class\n");
		unregister_chrdev(dsa->major, "dsa");
		return;
	}
	dev_info(&dsa->pdev->dev, "Created class\n");

	if (IS_ERR(device_create(dsa->charClass, NULL, MKDEV(dsa->major, 0),
				 NULL, "dsa"))) {
		dev_err(&dsa->pdev->dev, "Failed to create device\n");
		device_destroy(dsa->charClass, MKDEV(dsa->major, 0));
		class_destroy(dsa->charClass);
		unregister_chrdev(dsa->major, "dsa");
		return;
	}
	dev_info(&dsa->pdev->dev, "Created device\n");
	// pr_info("dsa->num_wqs: %d\n", dsa->num_wqs);
	for (int i = dsa->num_wqs - 2; i < dsa->num_wqs; i++) {
		wq_register_chardev(&dsa->wqs[i], i - (dsa->num_wqs - 2));
	}
	return;
}

void dsa_unregister_chardev(struct dsa_device_info *dsa)
{
	pr_info("Unregistering chardev\n");
	device_destroy(dsa->charClass, MKDEV(dsa->major, 0));
	cdev_del(&dsa->cdev);
	for (int i = dsa->num_wqs - 2; i < dsa->num_wqs; i++) {
		wq_unregister_chardev(&dsa->wqs[i]);
	}
	class_destroy(dsa->charClass);
	unregister_chrdev_region(MKDEV(dsa->major, 0), 3);
}
