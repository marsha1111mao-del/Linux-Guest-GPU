// SPDX-License-Identifier: GPL-2.0
/*
 * proxy_vmshm_manager - Character device driver for KVM memslot shared memory
 *
 * Manages a shared memory region described in the device tree as a
 * "proxy-vmshm-manager" compatible node. The region is mapped via KVM memslot
 * in Firecracker microVMs and backed by a host-side memfd. Provides
 * /dev/proxy_vmshm_manager for user-space read/write/mmap and ioctl access.
 */

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/ioctl.h>
#include <linux/mm.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "proxy_vmshm_manager.h"

/* ---------- ioctl definitions ---------- */

#define PROXY_VMSHM_MANAGER_IOC_MAGIC 'P'

#define PROXY_VMSHM_MANAGER_IOC_GET_SIZE \
	_IOR(PROXY_VMSHM_MANAGER_IOC_MAGIC, 1, __u64)
#define PROXY_VMSHM_MANAGER_IOC_GET_BASE \
	_IOR(PROXY_VMSHM_MANAGER_IOC_MAGIC, 2, __u64)
#define PROXY_VMSHM_MANAGER_IOC_SELFTEST \
	_IOR(PROXY_VMSHM_MANAGER_IOC_MAGIC, 3, __s32)

#define VMSHM_IOC_GET_SIZE	PROXY_VMSHM_MANAGER_IOC_GET_SIZE
#define VMSHM_IOC_GET_BASE	PROXY_VMSHM_MANAGER_IOC_GET_BASE
#define VMSHM_IOC_SELFTEST	PROXY_VMSHM_MANAGER_IOC_SELFTEST

/* ---------- device state ---------- */

static struct {
	void *base;		/* memremap'd kernel VA (WB RAM) */
	phys_addr_t gpa;	/* guest physical base */
	resource_size_t size;	/* region size in bytes */
	u32 owner_vmid;		/* security owner for the active debug window */
	dev_t devt;		/* major:minor */
	struct cdev cdev;
	struct class *class;
	struct device *dev;	/* char device under /sys/class/proxy_vmshm_manager */
	struct mutex lock;	/* protects read/write fops */
	atomic_t open_cnt;	/* open reference count */
	bool chardev_registered;
} vmshm_dev;

#define PROXY_VMSHM_MANAGER_NAME "proxy_vmshm_manager"

/* ---------- self-test ---------- */

/* Write magic, read back, verify, then restore the old value. Returns 0 on pass. */
static int vmshm_selftest(void)
{
	u32 *p = vmshm_dev.base;
	const u32 magic = 0xDEADBEEF;
	u32 old, val;

	old = *p;
	*p = magic;
	val = *p;
	*p = old;

	if (val != magic) {
		pr_warn("proxy_manager_vmshm: self-test FAILED: wrote 0x%08X read 0x%08X\n",
			magic, val);
		return -EIO;
	}

	pr_info("proxy_manager_vmshm: self-test passed (magic 0x%08X)\n",
		magic);
	return 0;
}

/* ---------- file_operations ---------- */

static int vmshm_open(struct inode *inode, struct file *filp)
{
	if (atomic_inc_return(&vmshm_dev.open_cnt) < 0) {
		atomic_dec(&vmshm_dev.open_cnt);
		return -EMFILE;
	}
	filp->private_data = &vmshm_dev;
	return 0;
}

static int vmshm_release(struct inode *inode, struct file *filp)
{
	atomic_dec(&vmshm_dev.open_cnt);
	return 0;
}

static ssize_t vmshm_read(struct file *filp, char __user *buf,
			   size_t count, loff_t *ppos)
{
	loff_t pos = *ppos;
	size_t avail;

	if (pos >= vmshm_dev.size)
		return 0;

	avail = vmshm_dev.size - pos;
	if (count > avail)
		count = avail;

	mutex_lock(&vmshm_dev.lock);
	if (copy_to_user(buf, vmshm_dev.base + pos, count))
		count = -EFAULT;
	else
		*ppos = pos + count;
	mutex_unlock(&vmshm_dev.lock);

	return count;
}

static ssize_t vmshm_write(struct file *filp, const char __user *buf,
			    size_t count, loff_t *ppos)
{
	loff_t pos = *ppos;
	size_t avail;

	if (pos >= vmshm_dev.size)
		return -ENOSPC;

	avail = vmshm_dev.size - pos;
	if (count > avail)
		count = avail;

	mutex_lock(&vmshm_dev.lock);
	if (copy_from_user(vmshm_dev.base + pos, buf, count))
		count = -EFAULT;
	else
		*ppos = pos + count;
	mutex_unlock(&vmshm_dev.lock);

	return count;
}

static loff_t vmshm_llseek(struct file *filp, loff_t offset, int whence)
{
	loff_t newpos;

	switch (whence) {
	case SEEK_SET:
		newpos = offset;
		break;
	case SEEK_CUR:
		newpos = filp->f_pos + offset;
		break;
	case SEEK_END:
		newpos = vmshm_dev.size + offset;
		break;
	default:
		return -EINVAL;
	}

	if (newpos < 0 || newpos > vmshm_dev.size)
		return -EINVAL;

	filp->f_pos = newpos;
	return newpos;
}

static int vmshm_mmap(struct file *filp, struct vm_area_struct *vma)
{
	unsigned long vma_size = vma->vm_end - vma->vm_start;
	unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;

	if (offset >= vmshm_dev.size)
		return -EINVAL;
	if (vma_size > vmshm_dev.size - offset)
		return -EINVAL;

	/* Map with WB attributes (vma->vm_page_prot default) */
	if (remap_pfn_range(vma, vma->vm_start,
			    (vmshm_dev.gpa >> PAGE_SHIFT) + vma->vm_pgoff,
			    vma_size, vma->vm_page_prot))
		return -EAGAIN;

	return 0;
}

static long vmshm_ioctl(struct file *filp, unsigned int cmd,
			 unsigned long arg)
{
	switch (cmd) {
	case VMSHM_IOC_GET_SIZE: {
		__u64 sz = vmshm_dev.size;
		if (copy_to_user((__u64 __user *)arg, &sz, sizeof(sz)))
			return -EFAULT;
		return 0;
	}
	case VMSHM_IOC_GET_BASE: {
		__u64 base = vmshm_dev.gpa;
		if (copy_to_user((__u64 __user *)arg, &base, sizeof(base)))
			return -EFAULT;
		return 0;
	}
	case VMSHM_IOC_SELFTEST: {
		__s32 result = vmshm_selftest();
		if (copy_to_user((__s32 __user *)arg, &result, sizeof(result)))
			return -EFAULT;
		return 0;
	}
	default:
		return -ENOTTY;
	}
}

static const struct file_operations vmshm_fops = {
	.owner		= THIS_MODULE,
	.open		= vmshm_open,
	.release	= vmshm_release,
	.read		= vmshm_read,
	.write		= vmshm_write,
	.llseek		= vmshm_llseek,
	.mmap		= vmshm_mmap,
	.unlocked_ioctl = vmshm_ioctl,
};

/* ---------- platform driver ---------- */

static int vmshm_probe(struct platform_device *pdev)
{
	void *base;
	phys_addr_t gpa;
	resource_size_t size;
	u32 owner_vmid = 0;
	struct resource *res;
	int ret;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev, "missing 'reg' resource in DT node\n");
		return -ENODEV;
	}

	gpa = res->start;
	size = resource_size(res);
	if (pdev->dev.of_node)
		of_property_read_u32(pdev->dev.of_node, "vmshm-client-vmid",
				     &owner_vmid);
	if (!owner_vmid)
		owner_vmid = 1;

	dev_info(&pdev->dev, "GPA 0x%pa size 0x%pa owner_vmid=%u\n",
		 &gpa, &size, owner_vmid);

	/* Map the shared memory region as Write-Back RAM */
	base = memremap(gpa, size, MEMREMAP_WB);
	if (!base) {
		dev_err(&pdev->dev, "memremap failed\n");
		return -ENOMEM;
	}

	dev_info(&pdev->dev, "mapped -> kernel VA %p\n", base);

	ret = proxy_vmshm_manager_register_domain(owner_vmid, base, gpa, size);
	if (ret) {
		dev_err(&pdev->dev, "proxy_manager_vmshm domain init failed (%d)\n", ret);
		goto err_unmap;
	}

	ret = proxy_vmshm_manager_selftest_run();
	if (ret) {
		dev_err(&pdev->dev, "proxy_manager_vmshm selftest failed (%d)\n", ret);
		goto err_manager_destroy;
	}

	if (vmshm_dev.chardev_registered) {
		dev_info(&pdev->dev, "registered allocator-only domain owner_vmid=%u\n",
			 owner_vmid);
		return 0;
	}

	vmshm_dev.base = base;
	vmshm_dev.gpa = gpa;
	vmshm_dev.size = size;
	vmshm_dev.owner_vmid = owner_vmid;

	/* Allocate character device number */
	ret = alloc_chrdev_region(&vmshm_dev.devt, 0, 1,
				  PROXY_VMSHM_MANAGER_NAME);
	if (ret < 0) {
		dev_err(&pdev->dev, "alloc_chrdev_region failed (%d)\n", ret);
		goto err_manager_destroy;
	}

	/* Register cdev */
	cdev_init(&vmshm_dev.cdev, &vmshm_fops);
	vmshm_dev.cdev.owner = THIS_MODULE;
	ret = cdev_add(&vmshm_dev.cdev, vmshm_dev.devt, 1);
	if (ret < 0) {
		dev_err(&pdev->dev, "cdev_add failed (%d)\n", ret);
		goto err_unregister_chrdev;
	}

	/* Create class and device node /dev/proxy_vmshm_manager */
	vmshm_dev.class = class_create(PROXY_VMSHM_MANAGER_NAME);
	if (IS_ERR(vmshm_dev.class)) {
		dev_err(&pdev->dev, "class_create failed\n");
		ret = PTR_ERR(vmshm_dev.class);
		goto err_cdev_del;
	}

	vmshm_dev.dev = device_create(vmshm_dev.class, &pdev->dev,
				       vmshm_dev.devt, NULL,
				       PROXY_VMSHM_MANAGER_NAME);
	if (IS_ERR(vmshm_dev.dev)) {
		dev_err(&pdev->dev, "device_create failed\n");
		ret = PTR_ERR(vmshm_dev.dev);
		goto err_class_destroy;
	}

	mutex_init(&vmshm_dev.lock);
	atomic_set(&vmshm_dev.open_cnt, 0);
	vmshm_dev.chardev_registered = true;

	dev_info(&pdev->dev, "/dev/%s registered (major %d)\n",
		 PROXY_VMSHM_MANAGER_NAME, MAJOR(vmshm_dev.devt));
	return 0;

err_class_destroy:
	class_destroy(vmshm_dev.class);
err_cdev_del:
	cdev_del(&vmshm_dev.cdev);
err_unregister_chrdev:
	unregister_chrdev_region(vmshm_dev.devt, 1);
err_manager_destroy:
	proxy_vmshm_manager_destroy();
err_unmap:
	memunmap(base);
	if (vmshm_dev.base == base)
		vmshm_dev.base = NULL;
	return ret;
}

static const struct of_device_id vmshm_of_match[] = {
	{ .compatible = "proxy-vmshm-manager" },
	{ }
};

static struct platform_driver vmshm_driver = {
	.probe = vmshm_probe,
	.driver = {
		.name = PROXY_VMSHM_MANAGER_NAME,
		.of_match_table = vmshm_of_match,
	},
};

builtin_platform_driver(vmshm_driver);
