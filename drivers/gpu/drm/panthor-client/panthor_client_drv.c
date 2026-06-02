// SPDX-License-Identifier: GPL-2.0 or MIT
/*
 * Panthor client VM DRM frontend.
 *
 * This frontend exposes the Panthor DEV_QUERY ioctl in the client VM and
 * forwards the actual query to the proxy VM over client_vmshm_comm.
 */

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
#define PANTHOR_CLIENT_DRIVER_FEATURES \
	(DRIVER_RENDER | DRIVER_GEM | DRIVER_SYNCOBJ | \
	 DRIVER_SYNCOBJ_TIMELINE | DRIVER_GEM_GPUVA)

struct panthor_client_device {
	struct drm_device drm;
	struct platform_device *platform;
};

struct panthor_client_file {
	u64 session_id;
};

static struct panthor_client_device *panthor_client;

static int
panthor_client_rpc_call(u32 req_type, const void *req, u32 req_len,
			u32 rsp_type, void *rsp, u32 rsp_capacity,
			u32 rsp_min_len, struct vmshm_comm_rx *rx_out)
{
	struct vmshm_comm_rx rx;
	int ret;

	ret = client_comm_vmshm_call(req_type, 0, req, req_len, rsp_type,
				     rsp, rsp_capacity, &rx);
	if (ret)
		return ret;

	if (rx.len < rsp_min_len || rx.len > rsp_capacity)
		return -EPROTO;

	if (rx_out)
		*rx_out = rx;

	return 0;
}

static int panthor_client_rpc_open_session(u64 *session_id)
{
	struct panthor_vmshm_open_session_req req = {
		.version = PANTHOR_VMSHM_ABI_VERSION,
	};
	struct panthor_vmshm_open_session_rsp rsp;
	int ret;

	if (!session_id)
		return -EINVAL;

	ret = panthor_client_rpc_call(PANTHOR_VMSHM_MSG_OPEN_SESSION_REQ,
				      &req, sizeof(req),
				      PANTHOR_VMSHM_MSG_OPEN_SESSION_RSP,
				      &rsp, sizeof(rsp), sizeof(rsp), NULL);
	if (ret)
		return ret;
	if (rsp.ret)
		return rsp.ret;
	if (!rsp.session_id)
		return -EPROTO;

	*session_id = rsp.session_id;
	pr_info("panthor-client: OPEN_SESSION session=%llu\n", *session_id);
	return 0;
}

static int panthor_client_rpc_close_session(u64 session_id)
{
	struct panthor_vmshm_close_session_req req = {
		.session_id = session_id,
	};
	struct panthor_vmshm_close_session_rsp rsp;
	int ret;

	if (!session_id)
		return 0;

	ret = panthor_client_rpc_call(PANTHOR_VMSHM_MSG_CLOSE_SESSION_REQ,
				      &req, sizeof(req),
				      PANTHOR_VMSHM_MSG_CLOSE_SESSION_RSP,
				      &rsp, sizeof(rsp), sizeof(rsp), NULL);
	if (ret)
		return ret;

	return rsp.ret;
}

static int
panthor_client_rpc_dev_query(const struct panthor_vmshm_dev_query_req *req,
			     struct panthor_vmshm_dev_query_rsp *rsp)
{
	struct vmshm_comm_rx rx;
	int ret;

	ret = panthor_client_rpc_call(PANTHOR_VMSHM_MSG_DEV_QUERY_REQ,
				      req, sizeof(*req),
				      PANTHOR_VMSHM_MSG_DEV_QUERY_RSP,
				      rsp, sizeof(*rsp),
				      offsetof(struct panthor_vmshm_dev_query_rsp,
					       data),
				      &rx);
	if (ret)
		return ret;

	if (rsp->data_len > sizeof(rsp->data) ||
	    rsp->data_len >
		    rx.len - offsetof(struct panthor_vmshm_dev_query_rsp, data))
		return -EPROTO;

	return 0;
}

static int
panthor_client_rpc_vm_create(const struct panthor_vmshm_vm_create_req *req,
			     struct panthor_vmshm_vm_create_rsp *rsp)
{
	return panthor_client_rpc_call(PANTHOR_VMSHM_MSG_VM_CREATE_REQ,
				      req, sizeof(*req),
				      PANTHOR_VMSHM_MSG_VM_CREATE_RSP,
				      rsp, sizeof(*rsp), sizeof(*rsp), NULL);
}

static int
panthor_client_rpc_vm_destroy(const struct panthor_vmshm_vm_destroy_req *req,
			      struct panthor_vmshm_vm_destroy_rsp *rsp)
{
	return panthor_client_rpc_call(PANTHOR_VMSHM_MSG_VM_DESTROY_REQ,
				      req, sizeof(*req),
				      PANTHOR_VMSHM_MSG_VM_DESTROY_RSP,
				      rsp, sizeof(*rsp), sizeof(*rsp), NULL);
}

static int panthor_client_dev_query(struct panthor_client_file *pcfile,
				    struct drm_panthor_dev_query *args)
{
	struct panthor_vmshm_dev_query_req req = {
		.session_id = pcfile ? pcfile->session_id : 0,
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
		if (pcfile)
			pr_info("panthor-client: DEV_QUERY session=%llu type=%u size=%u data=0\n",
				pcfile->session_id, args->type, rsp.size);
		return 0;
	}

	if (rsp.data_len &&
	    copy_to_user(user_ptr, rsp.data, rsp.data_len))
		return -EFAULT;

	if (user_size > rsp.data_len &&
	    clear_user(u64_to_user_ptr(args->pointer + rsp.data_len),
		       user_size - rsp.data_len))
		return -EFAULT;

	if (pcfile)
		pr_info("panthor-client: DEV_QUERY session=%llu type=%u size=%u data=%u\n",
			pcfile->session_id, args->type, rsp.size, rsp.data_len);
	return 0;
}

static int panthor_client_vm_create(struct panthor_client_file *pcfile,
				    struct drm_panthor_vm_create *args)
{
	struct panthor_vmshm_vm_create_req req;
	struct panthor_vmshm_vm_create_rsp rsp;
	int ret;

	if (!pcfile)
		return -EINVAL;
	if (args->flags)
		return -EINVAL;

	memset(&req, 0, sizeof(req));
	req.session_id = pcfile->session_id;
	req.flags = args->flags;
	req.user_va_range = args->user_va_range;

	ret = panthor_client_rpc_vm_create(&req, &rsp);
	if (ret)
		return ret;
	if (rsp.ret)
		return rsp.ret;
	if (!rsp.client_vm_id || !rsp.proxy_vm_id || !rsp.user_va_range)
		return -EPROTO;

	args->id = rsp.client_vm_id;
	args->user_va_range = rsp.user_va_range;

	pr_info("panthor-client: VM_CREATE session=%llu client_vm=%u proxy_vm=%u user_va_range=0x%llx\n",
		pcfile->session_id, rsp.client_vm_id, rsp.proxy_vm_id,
		rsp.user_va_range);
	return 0;
}

static int panthor_client_vm_destroy(struct panthor_client_file *pcfile,
				     struct drm_panthor_vm_destroy *args)
{
	struct panthor_vmshm_vm_destroy_req req = { 0 };
	struct panthor_vmshm_vm_destroy_rsp rsp;
	int ret;

	if (!pcfile)
		return -EINVAL;
	if (args->pad)
		return -EINVAL;

	req.session_id = pcfile->session_id;
	req.client_vm_id = args->id;

	ret = panthor_client_rpc_vm_destroy(&req, &rsp);
	if (ret)
		return ret;
	if (rsp.ret)
		return rsp.ret;

	pr_info("panthor-client: VM_DESTROY session=%llu client_vm=%u proxy_vm=%u\n",
		pcfile->session_id, args->id, rsp.proxy_vm_id);
	return 0;
}

static int panthor_client_ioctl_dev_query(struct drm_device *ddev, void *data,
					  struct drm_file *file)
{
	return panthor_client_dev_query(file->driver_priv, data);
}

static int panthor_client_ioctl_vm_create(struct drm_device *ddev, void *data,
					  struct drm_file *file)
{
	return panthor_client_vm_create(file->driver_priv, data);
}

static int panthor_client_ioctl_vm_destroy(struct drm_device *ddev, void *data,
					   struct drm_file *file)
{
	return panthor_client_vm_destroy(file->driver_priv, data);
}

static const struct drm_ioctl_desc panthor_client_ioctls[] = {
	DRM_IOCTL_DEF_DRV(PANTHOR_DEV_QUERY, panthor_client_ioctl_dev_query,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(PANTHOR_VM_CREATE, panthor_client_ioctl_vm_create,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(PANTHOR_VM_DESTROY, panthor_client_ioctl_vm_destroy,
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
	.fop_flags = FOP_UNSIGNED_OFFSET,
};

static int panthor_client_open(struct drm_device *ddev, struct drm_file *file)
{
	struct panthor_client_file *pcfile;
	int ret;

	pcfile = kzalloc(sizeof(*pcfile), GFP_KERNEL);
	if (!pcfile)
		return -ENOMEM;

	ret = panthor_client_rpc_open_session(&pcfile->session_id);
	if (ret) {
		kfree(pcfile);
		return ret;
	}

	file->driver_priv = pcfile;
	return 0;
}

static void panthor_client_postclose(struct drm_device *ddev,
				     struct drm_file *file)
{
	struct panthor_client_file *pcfile = file->driver_priv;
	int ret;

	if (!pcfile)
		return;

	ret = panthor_client_rpc_close_session(pcfile->session_id);
	if (ret)
		pr_warn_ratelimited("panthor-client: CLOSE_SESSION failed session=%llu ret=%d\n",
				    pcfile->session_id, ret);
	else
		pr_info("panthor-client: CLOSE_SESSION session=%llu\n",
			pcfile->session_id);

	kfree(pcfile);
	file->driver_priv = NULL;
}

static const struct drm_driver panthor_client_driver = {
	.driver_features = PANTHOR_CLIENT_DRIVER_FEATURES,
	.open = panthor_client_open,
	.postclose = panthor_client_postclose,
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

	ret = drm_dev_register(&panthor_client->drm, 0);
	if (ret)
		goto err_release_devres;

	ret = panthor_client_dev_query_selftest_run();
	if (ret)
		pr_warn("panthor-client: DEV_QUERY selftest failed (%d)\n", ret);

	ret = panthor_client_dev_query_perf_rtt();
	if (ret)
		pr_warn("panthor-client: DEV_QUERY perf selftest failed (%d)\n", ret);

	pr_info("panthor-client: registered DRM frontend\n");
	return 0;

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
	drm_dev_unregister(&panthor_client->drm);
	devres_release_group(&pdev->dev, NULL);
	platform_device_unregister(pdev);
	panthor_client = NULL;
}

module_init(panthor_client_init);
module_exit(panthor_client_exit);

MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL and additional rights");
