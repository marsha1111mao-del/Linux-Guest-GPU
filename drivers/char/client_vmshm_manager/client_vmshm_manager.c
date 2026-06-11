// SPDX-License-Identifier: GPL-2.0
/*
 * client_vmshm_manager - client VM view of proxy-managed vmshm objects.
 *
 * The client VM never allocates or frees from this shared window. It only maps
 * the window, asks the proxy VM for object descriptors, and translates returned
 * offsets into local kernel mappings for other client-side drivers.
 */

#define CLIENT_VMSHM_MANAGER_BUILD

#include <linux/atomic.h>
#include <linux/cdev.h>
#include <linux/client_vmshm.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/refcount.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/vmshm_comm.h>

#define CLIENT_VMSHM_MANAGER_NAME	"client_vmshm_manager"

struct client_vmshm_object {
	struct vmshm_manager_desc desc;
	void *kva;
	phys_addr_t gpa;
	refcount_t refcnt;
};

struct client_vmshm_manager_dev {
	void *base;
	phys_addr_t gpa;
	resource_size_t size;
	u32 client_vmid;
	dev_t devt;
	struct cdev cdev;
	struct class *class;
	struct device *dev;
	struct mutex lock;
	atomic_t open_cnt;
};

static struct client_vmshm_manager_dev client_vmshm_mgr = {
	.lock = __MUTEX_INITIALIZER(client_vmshm_mgr.lock),
};

static bool client_vmshm_range_valid(struct client_vmshm_manager_dev *d,
				     u64 offset, u64 size)
{
	if (!d->base)
		return false;
	if (offset > d->size)
		return false;

	return size <= d->size - offset;
}

bool client_vmshm_manager_ready(void)
{
	return client_vmshm_mgr.base != NULL;
}
EXPORT_SYMBOL_GPL(client_vmshm_manager_ready);

int client_vmshm_manager_map_offset(u64 offset, size_t size,
				    void **kva, phys_addr_t *gpa)
{
	struct client_vmshm_manager_dev *d = &client_vmshm_mgr;

	if (!kva && !gpa)
		return -EINVAL;

	mutex_lock(&d->lock);
	if (!client_vmshm_range_valid(d, offset, size)) {
		mutex_unlock(&d->lock);
		return -ERANGE;
	}

	if (kva)
		*kva = (void *)((u8 *)d->base + offset);
	if (gpa)
		*gpa = d->gpa + offset;
	mutex_unlock(&d->lock);
	return 0;
}
EXPORT_SYMBOL_GPL(client_vmshm_manager_map_offset);

static int client_vmshm_validate_desc(struct client_vmshm_manager_dev *d,
				      const struct vmshm_manager_desc *desc)
{
	if (!(desc->flags & VMSHM_MANAGER_DESC_F_CONTIG))
		return -EOPNOTSUPP;
	if (desc->nr_segments != 1)
		return -EOPNOTSUPP;
	if (desc->size > desc->alloc_size)
		return -EINVAL;
	if (!client_vmshm_range_valid(d, desc->offset, desc->alloc_size))
		return -ERANGE;

	return 0;
}

int client_vmshm_manager_get(const struct client_vmshm_lookup_params *params,
			     struct client_vmshm_object **out)
{
	struct client_vmshm_manager_dev *d = &client_vmshm_mgr;
	struct vmshm_manager_get_object_req req;
	struct vmshm_manager_get_object_rsp rsp;
	struct vmshm_comm_rx rx;
	struct client_vmshm_object *obj;
	int ret;

	if (!params || !out)
		return -EINVAL;
	if (params->lookup != VMSHM_MANAGER_LOOKUP_HANDLE &&
	    params->lookup != VMSHM_MANAGER_LOOKUP_GRANT)
		return -EINVAL;

	*out = NULL;
	memset(&req, 0, sizeof(req));
	req.handle = params->handle;
	req.grant_id = params->grant_id;
	req.lookup = params->lookup;
	req.requester_vmid = params->requester_vmid ?: d->client_vmid;
	req.required_perms = params->required_perms;
	req.flags = params->flags;

	ret = client_comm_vmshm_call(VMSHM_MANAGER_MSG_GET_OBJECT_REQ, 0,
				     &req, sizeof(req),
				     VMSHM_MANAGER_MSG_GET_OBJECT_RSP,
				     &rsp, sizeof(rsp), &rx);
	if (ret)
		return ret;
	if (rx.len != sizeof(rsp))
		return -EPROTO;
	if (rsp.ret)
		return rsp.ret;

	mutex_lock(&d->lock);
	ret = client_vmshm_validate_desc(d, &rsp.desc);
	mutex_unlock(&d->lock);
	if (ret)
		return ret;

	obj = kzalloc(sizeof(*obj), GFP_KERNEL);
	if (!obj)
		return -ENOMEM;

	obj->desc = rsp.desc;
	obj->desc.gpa = d->gpa + rsp.desc.offset;
	obj->kva = (void *)((u8 *)d->base + rsp.desc.offset);
	obj->gpa = d->gpa + rsp.desc.offset;
	refcount_set(&obj->refcnt, 1);
	*out = obj;
	return 0;
}
EXPORT_SYMBOL_GPL(client_vmshm_manager_get);

void client_vmshm_manager_put(struct client_vmshm_object *obj)
{
	if (obj && refcount_dec_and_test(&obj->refcnt))
		kfree(obj);
}
EXPORT_SYMBOL_GPL(client_vmshm_manager_put);

const struct vmshm_manager_desc *
client_vmshm_object_desc(const struct client_vmshm_object *obj)
{
	return obj ? &obj->desc : NULL;
}
EXPORT_SYMBOL_GPL(client_vmshm_object_desc);

void *client_vmshm_object_kva(const struct client_vmshm_object *obj)
{
	return obj ? obj->kva : NULL;
}
EXPORT_SYMBOL_GPL(client_vmshm_object_kva);

phys_addr_t client_vmshm_object_gpa(const struct client_vmshm_object *obj)
{
	return obj ? obj->gpa : 0;
}
EXPORT_SYMBOL_GPL(client_vmshm_object_gpa);

static int client_vmshm_open(struct inode *inode, struct file *filp)
{
	if (atomic_inc_return(&client_vmshm_mgr.open_cnt) < 0) {
		atomic_dec(&client_vmshm_mgr.open_cnt);
		return -EMFILE;
	}

	filp->private_data = &client_vmshm_mgr;
	return 0;
}

static int client_vmshm_release(struct inode *inode, struct file *filp)
{
	atomic_dec(&client_vmshm_mgr.open_cnt);
	return 0;
}

static loff_t client_vmshm_llseek(struct file *filp, loff_t offset, int whence)
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
		newpos = client_vmshm_mgr.size + offset;
		break;
	default:
		return -EINVAL;
	}

	if (newpos < 0 || newpos > client_vmshm_mgr.size)
		return -EINVAL;

	filp->f_pos = newpos;
	return newpos;
}

static ssize_t client_vmshm_read(struct file *filp, char __user *buf,
				 size_t count, loff_t *ppos)
{
	return -EPERM;
}

static ssize_t client_vmshm_write(struct file *filp, const char __user *buf,
				  size_t count, loff_t *ppos)
{
	return -EPERM;
}

static int client_vmshm_mmap(struct file *filp, struct vm_area_struct *vma)
{
	return -EPERM;
}

static long client_vmshm_ioctl(struct file *filp, unsigned int cmd,
			       unsigned long arg)
{
	switch (cmd) {
	case CLIENT_VMSHM_MANAGER_IOC_GET_INFO: {
		struct client_vmshm_manager_info info = {
			.gpa = client_vmshm_mgr.gpa,
			.size = client_vmshm_mgr.size,
		};

		if (copy_to_user((void __user *)arg, &info, sizeof(info)))
			return -EFAULT;
		return 0;
	}
	case CLIENT_VMSHM_MANAGER_IOC_GET_OBJECT: {
		struct client_vmshm_manager_user_lookup user;
		struct client_vmshm_lookup_params params;
		struct client_vmshm_object *obj;
		int ret;

		if (copy_from_user(&user, (void __user *)arg, sizeof(user)))
			return -EFAULT;

		params.handle = user.handle;
		params.grant_id = user.grant_id;
		params.lookup = user.lookup;
		params.requester_vmid = user.requester_vmid;
		params.required_perms = user.required_perms;
		params.flags = user.flags;

		ret = client_vmshm_manager_get(&params, &obj);
		if (ret)
			return ret;

		user.desc = *client_vmshm_object_desc(obj);
		client_vmshm_manager_put(obj);

		if (copy_to_user((void __user *)arg, &user, sizeof(user)))
			return -EFAULT;
		return 0;
	}
	default:
		return -ENOTTY;
	}
}

static const struct file_operations client_vmshm_fops = {
	.owner = THIS_MODULE,
	.open = client_vmshm_open,
	.release = client_vmshm_release,
	.read = client_vmshm_read,
	.write = client_vmshm_write,
	.llseek = client_vmshm_llseek,
	.mmap = client_vmshm_mmap,
	.unlocked_ioctl = client_vmshm_ioctl,
};

static int client_vmshm_probe(struct platform_device *pdev)
{
	struct resource *res;
	int ret;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev, "missing 'reg' resource in DT node\n");
		return -ENODEV;
	}

	client_vmshm_mgr.gpa = res->start;
	client_vmshm_mgr.size = resource_size(res);
	if (pdev->dev.of_node)
		of_property_read_u32(pdev->dev.of_node, "vmshm-client-vmid",
				     &client_vmshm_mgr.client_vmid);
	if (!client_vmshm_mgr.client_vmid)
		client_vmshm_mgr.client_vmid = 1;
	mutex_init(&client_vmshm_mgr.lock);
	atomic_set(&client_vmshm_mgr.open_cnt, 0);

	if (!IS_ALIGNED((u64)client_vmshm_mgr.gpa, PAGE_SIZE) ||
	    !IS_ALIGNED((u64)client_vmshm_mgr.size, PAGE_SIZE)) {
		dev_err(&pdev->dev, "shared window is not page aligned\n");
		return -EINVAL;
	}

	client_vmshm_mgr.base = memremap(client_vmshm_mgr.gpa,
					client_vmshm_mgr.size, MEMREMAP_WB);
	if (!client_vmshm_mgr.base) {
		dev_err(&pdev->dev, "memremap failed\n");
		return -ENOMEM;
	}

	ret = alloc_chrdev_region(&client_vmshm_mgr.devt, 0, 1,
				  CLIENT_VMSHM_MANAGER_NAME);
	if (ret)
		goto err_unmap;

	cdev_init(&client_vmshm_mgr.cdev, &client_vmshm_fops);
	client_vmshm_mgr.cdev.owner = THIS_MODULE;
	ret = cdev_add(&client_vmshm_mgr.cdev, client_vmshm_mgr.devt, 1);
	if (ret)
		goto err_unregister_chrdev;

	client_vmshm_mgr.class = class_create(CLIENT_VMSHM_MANAGER_NAME);
	if (IS_ERR(client_vmshm_mgr.class)) {
		ret = PTR_ERR(client_vmshm_mgr.class);
		client_vmshm_mgr.class = NULL;
		goto err_cdev_del;
	}

	client_vmshm_mgr.dev = device_create(client_vmshm_mgr.class,
					     &pdev->dev,
					     client_vmshm_mgr.devt, NULL,
					     CLIENT_VMSHM_MANAGER_NAME);
	if (IS_ERR(client_vmshm_mgr.dev)) {
		ret = PTR_ERR(client_vmshm_mgr.dev);
		client_vmshm_mgr.dev = NULL;
		goto err_class_destroy;
	}

	dev_info(&pdev->dev, "/dev/%s registered GPA 0x%pa size 0x%pa client_vmid=%u\n",
		 CLIENT_VMSHM_MANAGER_NAME, &client_vmshm_mgr.gpa,
		 &client_vmshm_mgr.size, client_vmshm_mgr.client_vmid);
	return 0;

err_class_destroy:
	class_destroy(client_vmshm_mgr.class);
	client_vmshm_mgr.class = NULL;
err_cdev_del:
	cdev_del(&client_vmshm_mgr.cdev);
err_unregister_chrdev:
	unregister_chrdev_region(client_vmshm_mgr.devt, 1);
err_unmap:
	memunmap(client_vmshm_mgr.base);
	client_vmshm_mgr.base = NULL;
	return ret;
}

static const struct of_device_id client_vmshm_of_match[] = {
	{ .compatible = "client-vmshm-manager" },
	{ }
};

static struct platform_driver client_vmshm_driver = {
	.probe = client_vmshm_probe,
	.driver = {
		.name = CLIENT_VMSHM_MANAGER_NAME,
		.of_match_table = client_vmshm_of_match,
	},
};

builtin_platform_driver(client_vmshm_driver);
