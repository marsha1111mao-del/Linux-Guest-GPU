// SPDX-License-Identifier: GPL-2.0 or MIT
/*
 * Panthor client VM DRM frontend.
 *
 * This frontend exposes the Panthor DEV_QUERY ioctl in the client VM and
 * forwards the actual query to the proxy VM over client_vmshm_comm.
 */

#include <linux/atomic.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/ktime.h>
#include <linux/limits.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/vmshm_comm.h>
#include <linux/panthor_vmshm.h>

#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_ioctl.h>
#include <drm/panthor_drm.h>

#define DRIVER_NAME	"panthor"
#define PLATFORM_NAME	"panthor-client"
#define DRIVER_DESC	"Panthor client VM DRM frontend"
#define DRIVER_DATE	"20260528"
#define DRIVER_MAJOR	1
#define DRIVER_MINOR	0
#define PANTHOR_CLIENT_DEV_QUERY_PERF_RTT_ITERS	1000

struct panthor_client_device {
	struct drm_device drm;
	struct platform_device *platform;
	dev_t devt;
	struct cdev cdev;
	struct class *class;
	struct device *chardev;
	atomic_t open_cnt;
};

static struct panthor_client_device *panthor_client;

static int
panthor_client_rpc_dev_query(const struct panthor_vmshm_dev_query_req *req,
			     struct panthor_vmshm_dev_query_rsp *rsp)
{
	struct vmshm_comm_rx rx;
	int ret;

	ret = client_comm_vmshm_call(PANTHOR_VMSHM_MSG_DEV_QUERY_REQ, 0,
				     req, sizeof(*req),
				     PANTHOR_VMSHM_MSG_DEV_QUERY_RSP,
				     rsp, sizeof(*rsp), &rx);
	if (ret)
		return ret;

	if (rx.len < offsetof(struct panthor_vmshm_dev_query_rsp, data) ||
	    rsp->data_len > sizeof(rsp->data) ||
	    rsp->data_len >
		    rx.len - offsetof(struct panthor_vmshm_dev_query_rsp, data))
		return -EPROTO;

	return 0;
}

static int panthor_client_dev_query(struct drm_panthor_dev_query *args)
{
	struct panthor_vmshm_dev_query_req req = {
		.type = args->type,
		.size = args->size,
		.flags = args->pointer ? PANTHOR_VMSHM_DEV_QUERY_F_DATA : 0,
	};
	struct panthor_vmshm_dev_query_rsp rsp;
	u32 user_size = args->size;
	void __user *user_ptr = u64_to_user_ptr(args->pointer);
	int ret;

	ret = panthor_client_rpc_dev_query(&req, &rsp);
	if (ret)
		return ret;
	if (rsp.ret)
		return rsp.ret;
	if (rsp.type != args->type || rsp.data_len > user_size)
		return -EPROTO;

	if (!args->pointer) {
		args->size = rsp.size;
		return 0;
	}

	if (rsp.data_len &&
	    copy_to_user(user_ptr, rsp.data, rsp.data_len))
		return -EFAULT;

	if (user_size > rsp.data_len &&
	    clear_user(u64_to_user_ptr(args->pointer + rsp.data_len),
		       user_size - rsp.data_len))
		return -EFAULT;

	return 0;
}

static int panthor_client_ioctl_dev_query(struct drm_device *ddev, void *data,
					  struct drm_file *file)
{
	return panthor_client_dev_query(data);
}

static const struct drm_ioctl_desc panthor_client_ioctls[] = {
	DRM_IOCTL_DEF_DRV(PANTHOR_DEV_QUERY, panthor_client_ioctl_dev_query,
			  DRM_RENDER_ALLOW),
};

static const struct file_operations panthor_client_fops = {
	.owner = THIS_MODULE,
	.open = drm_open,
	.release = drm_release,
	.unlocked_ioctl = drm_ioctl,
	.compat_ioctl = drm_compat_ioctl,
	.poll = drm_poll,
	.read = drm_read,
	.llseek = noop_llseek,
};

static const struct drm_driver panthor_client_driver = {
	.driver_features = DRIVER_RENDER,
	.ioctls = panthor_client_ioctls,
	.num_ioctls = ARRAY_SIZE(panthor_client_ioctls),
	.fops = &panthor_client_fops,
	.name = DRIVER_NAME,
	.desc = DRIVER_DESC,
	.date = DRIVER_DATE,
	.major = DRIVER_MAJOR,
	.minor = DRIVER_MINOR,
};

#ifdef CONFIG_DRM_PANTHOR_CLIENT_DEV_QUERY_SELFTEST
static int panthor_client_selftest_query_size(u32 type, u32 *size)
{
	struct panthor_vmshm_dev_query_req req = {
		.type = type,
	};
	struct panthor_vmshm_dev_query_rsp rsp;
	int ret;

	ret = panthor_client_rpc_dev_query(&req, &rsp);
	if (ret)
		return ret;
	if (rsp.ret)
		return rsp.ret;
	if (rsp.type != type)
		return -EPROTO;

	*size = rsp.size;
	return 0;
}

static int panthor_client_selftest_query_data(u32 type, u32 size,
					     void *data, u32 data_size)
{
	struct panthor_vmshm_dev_query_req req = {
		.type = type,
		.size = size,
		.flags = PANTHOR_VMSHM_DEV_QUERY_F_DATA,
	};
	struct panthor_vmshm_dev_query_rsp rsp;
	int ret;

	if (size > data_size)
		return -EINVAL;

	ret = panthor_client_rpc_dev_query(&req, &rsp);
	if (ret)
		return ret;
	if (rsp.ret)
		return rsp.ret;
	if (rsp.type != type || rsp.size != size || rsp.data_len > data_size)
		return -EPROTO;

	memcpy(data, rsp.data, rsp.data_len);
	return 0;
}

static int panthor_client_dev_query_selftest_run(void)
{
	struct drm_panthor_gpu_info gpu_info;
	struct drm_panthor_csif_info csif_info;
	u32 gpu_info_size, csif_info_size;
	int ret;

	ret = panthor_client_selftest_query_size(DRM_PANTHOR_DEV_QUERY_GPU_INFO,
						&gpu_info_size);
	if (ret)
		return ret;

	memset(&gpu_info, 0, sizeof(gpu_info));
	ret = panthor_client_selftest_query_data(DRM_PANTHOR_DEV_QUERY_GPU_INFO,
						gpu_info_size, &gpu_info,
						sizeof(gpu_info));
	if (ret)
		return ret;

	pr_info("panthor-client: DEV_QUERY GPU_INFO size=%u gpu_id=0x%08x gpu_rev=0x%08x csf_id=0x%08x shader_present=0x%llx l2_present=0x%llx tiler_present=0x%llx as_present=0x%x\n",
		gpu_info_size, gpu_info.gpu_id, gpu_info.gpu_rev,
		gpu_info.csf_id, gpu_info.shader_present, gpu_info.l2_present,
		gpu_info.tiler_present, gpu_info.as_present);

	ret = panthor_client_selftest_query_size(DRM_PANTHOR_DEV_QUERY_CSIF_INFO,
						&csif_info_size);
	if (ret)
		return ret;

	memset(&csif_info, 0, sizeof(csif_info));
	ret = panthor_client_selftest_query_data(DRM_PANTHOR_DEV_QUERY_CSIF_INFO,
						csif_info_size, &csif_info,
						sizeof(csif_info));
	if (ret)
		return ret;

	pr_info("panthor-client: DEV_QUERY CSIF_INFO size=%u csg_slots=%u cs_slots=%u cs_regs=%u scoreboard_slots=%u unpreserved_cs_regs=%u\n",
		csif_info_size, csif_info.csg_slot_count,
		csif_info.cs_slot_count, csif_info.cs_reg_count,
		csif_info.scoreboard_slot_count,
		csif_info.unpreserved_cs_reg_count);

	pr_info("panthor-client: DEV_QUERY selftest passed\n");
	return 0;
}
#else
static int panthor_client_dev_query_selftest_run(void)
{
	return 0;
}
#endif

#ifdef CONFIG_DRM_PANTHOR_CLIENT_DEV_QUERY_PERF_SELFTEST
static int panthor_client_dev_query_perf_rtt(void)
{
	u64 min_ns = U64_MAX, max_ns = 0, total_ns = 0, elapsed_ns;
	u32 iters = PANTHOR_CLIENT_DEV_QUERY_PERF_RTT_ITERS;
	struct drm_panthor_gpu_info gpu_info;
	u32 gpu_info_size;
	int ret, i;

	ret = panthor_client_selftest_query_size(DRM_PANTHOR_DEV_QUERY_GPU_INFO,
						&gpu_info_size);
	if (ret)
		return ret;
	if (gpu_info_size > sizeof(gpu_info))
		return -EOVERFLOW;

	for (i = 0; i < iters; i++) {
		u64 start_ns;

		memset(&gpu_info, 0, sizeof(gpu_info));
		start_ns = ktime_get_ns();
		ret = panthor_client_selftest_query_data(
			DRM_PANTHOR_DEV_QUERY_GPU_INFO, gpu_info_size,
			&gpu_info, sizeof(gpu_info));
		if (ret) {
			pr_warn("panthor-client: DEV_QUERY perf gpu_info_rtt failed iter=%d ret=%d\n",
				i, ret);
			return ret;
		}

		elapsed_ns = ktime_get_ns() - start_ns;
		if (elapsed_ns < min_ns)
			min_ns = elapsed_ns;
		if (elapsed_ns > max_ns)
			max_ns = elapsed_ns;
		total_ns += elapsed_ns;
	}

	pr_info("panthor-client: DEV_QUERY perf gpu_info_rtt iters=%u size=%u min_ns=%llu avg_ns=%llu max_ns=%llu total_ns=%llu\n",
		iters, gpu_info_size, min_ns, div64_u64(total_ns, iters),
		max_ns, total_ns);
	pr_info("panthor-client: DEV_QUERY perf selftest passed\n");
	return 0;
}
#else
static int panthor_client_dev_query_perf_rtt(void)
{
	return 0;
}
#endif

static int panthor_client_chr_open(struct inode *inode, struct file *filp)
{
	if (!panthor_client)
		return -ENODEV;

	if (atomic_inc_return(&panthor_client->open_cnt) < 0) {
		atomic_dec(&panthor_client->open_cnt);
		return -EMFILE;
	}

	filp->private_data = panthor_client;
	return 0;
}

static int panthor_client_chr_release(struct inode *inode, struct file *filp)
{
	struct panthor_client_device *pcdev = filp->private_data;

	if (pcdev)
		atomic_dec(&pcdev->open_cnt);
	return 0;
}

static long panthor_client_chr_ioctl(struct file *filp, unsigned int cmd,
				     unsigned long arg)
{
	struct drm_panthor_dev_query args;
	int ret;

	if (cmd != DRM_IOCTL_PANTHOR_DEV_QUERY)
		return -ENOTTY;

	if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
		return -EFAULT;

	ret = panthor_client_dev_query(&args);
	if (ret)
		return ret;

	if (copy_to_user((void __user *)arg, &args, sizeof(args)))
		return -EFAULT;

	return 0;
}

static const struct file_operations panthor_client_chr_fops = {
	.owner = THIS_MODULE,
	.open = panthor_client_chr_open,
	.release = panthor_client_chr_release,
	.unlocked_ioctl = panthor_client_chr_ioctl,
	.compat_ioctl = panthor_client_chr_ioctl,
	.llseek = noop_llseek,
};

static void panthor_client_chrdev_unregister(struct panthor_client_device *pcdev)
{
	if (!pcdev)
		return;

	if (pcdev->chardev && !IS_ERR(pcdev->chardev))
		device_destroy(pcdev->class, pcdev->devt);
	pcdev->chardev = NULL;

	if (pcdev->class && !IS_ERR(pcdev->class))
		class_destroy(pcdev->class);
	pcdev->class = NULL;

	if (pcdev->devt) {
		cdev_del(&pcdev->cdev);
		unregister_chrdev_region(pcdev->devt, 1);
		pcdev->devt = 0;
	}
}

static int panthor_client_chrdev_register(struct panthor_client_device *pcdev)
{
	struct device *parent = &pcdev->platform->dev;
	int ret;

	ret = alloc_chrdev_region(&pcdev->devt, 0, 1, DRIVER_NAME);
	if (ret)
		return ret;

	cdev_init(&pcdev->cdev, &panthor_client_chr_fops);
	pcdev->cdev.owner = THIS_MODULE;
	ret = cdev_add(&pcdev->cdev, pcdev->devt, 1);
	if (ret)
		goto err_unregister_chrdev;

	pcdev->class = class_create(PLATFORM_NAME);
	if (IS_ERR(pcdev->class)) {
		ret = PTR_ERR(pcdev->class);
		pcdev->class = NULL;
		goto err_cdev_del;
	}

	pcdev->chardev = device_create(pcdev->class, parent, pcdev->devt,
				       NULL, DRIVER_NAME);
	if (IS_ERR(pcdev->chardev)) {
		ret = PTR_ERR(pcdev->chardev);
		pcdev->chardev = NULL;
		goto err_class_destroy;
	}

	return 0;

err_class_destroy:
	class_destroy(pcdev->class);
	pcdev->class = NULL;
err_cdev_del:
	cdev_del(&pcdev->cdev);
err_unregister_chrdev:
	unregister_chrdev_region(pcdev->devt, 1);
	pcdev->devt = 0;
	return ret;
}

static int __init panthor_client_init(void)
{
	struct platform_device *pdev;
	int ret;

	pdev = platform_device_register_simple(PLATFORM_NAME, -1, NULL, 0);
	if (IS_ERR(pdev))
		return PTR_ERR(pdev);

	if (!devres_open_group(&pdev->dev, NULL, GFP_KERNEL)) {
		ret = -ENOMEM;
		goto err_unregister_platform;
	}

	ret = dma_coerce_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (ret)
		goto err_release_devres;

	panthor_client = devm_drm_dev_alloc(&pdev->dev,
					    &panthor_client_driver,
					    struct panthor_client_device,
					    drm);
	if (IS_ERR(panthor_client)) {
		ret = PTR_ERR(panthor_client);
		panthor_client = NULL;
		goto err_release_devres;
	}

	panthor_client->platform = pdev;
	atomic_set(&panthor_client->open_cnt, 0);

	ret = drm_dev_register(&panthor_client->drm, 0);
	if (ret)
		goto err_release_devres;

	ret = panthor_client_chrdev_register(panthor_client);
	if (ret)
		goto err_unregister_drm;

	ret = panthor_client_dev_query_selftest_run();
	if (ret)
		pr_warn("panthor-client: DEV_QUERY selftest failed (%d)\n", ret);

	ret = panthor_client_dev_query_perf_rtt();
	if (ret)
		pr_warn("panthor-client: DEV_QUERY perf selftest failed (%d)\n", ret);

	pr_info("panthor-client: registered DRM frontend and /dev/%s\n",
		DRIVER_NAME);
	return 0;

err_unregister_drm:
	drm_dev_unregister(&panthor_client->drm);
err_release_devres:
	devres_release_group(&pdev->dev, NULL);
err_unregister_platform:
	platform_device_unregister(pdev);
	return ret;
}

static void __exit panthor_client_exit(void)
{
	struct platform_device *pdev;

	if (!panthor_client)
		return;

	pdev = panthor_client->platform;
	panthor_client_chrdev_unregister(panthor_client);
	drm_dev_unregister(&panthor_client->drm);
	devres_release_group(&pdev->dev, NULL);
	platform_device_unregister(pdev);
	panthor_client = NULL;
}

module_init(panthor_client_init);
module_exit(panthor_client_exit);

MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL and additional rights");
