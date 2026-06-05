// SPDX-License-Identifier: GPL-2.0 or MIT
/*
 * Panthor client VM DRM frontend.
 *
 * This frontend exposes the Panthor DEV_QUERY ioctl in the client VM and
 * forwards the actual query to the proxy VM over client_vmshm_comm.
 */

#include <linux/bits.h>
#include <linux/dma-mapping.h>
#include <linux/client_vmshm.h>
#include <linux/fs.h>
#include <linux/ktime.h>
#include <linux/limits.h>
#include <linux/math64.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/proxy_vmshm.h>
#include <linux/refcount.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/vmshm_comm.h>
#include <linux/panthor_vmshm.h>
#include <linux/xarray.h>

#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_print.h>
#include <uapi/drm/drm.h>
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
#define PANTHOR_CLIENT_BO_MMAP_OFFSET_BASE	(1ULL << 32)
#define PANTHOR_CLIENT_SYNC_OP_FLAGS_MASK \
	(DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_MASK | DRM_PANTHOR_SYNC_OP_SIGNAL)

struct panthor_client_device {
	struct drm_device drm;
	struct platform_device *platform;
	struct page *flush_id_page;
};

struct panthor_client_bo {
	u64 session_id;
	u32 client_handle;
	u32 proxy_handle;
	u32 flags;
	u64 size;
	u64 payload_handle;
	u64 payload_offset;
	u64 payload_size;
	u64 mmap_offset;
	struct client_vmshm_object *payload_obj;
	struct vmshm_manager_desc payload_desc;
	refcount_t refcnt;
	bool destroy_pending;
};

struct panthor_client_syncobj {
	u64 session_id;
	u32 client_handle;
	u32 proxy_handle;
	refcount_t refcnt;
	bool destroy_pending;
};

struct panthor_client_group {
	u64 session_id;
	u32 client_handle;
	u32 proxy_handle;
	refcount_t refcnt;
	bool destroy_pending;
};

struct panthor_client_heap {
	u64 session_id;
	u32 client_handle;
	u32 proxy_handle;
	u32 client_vm_id;
	u32 proxy_vm_id;
	u64 tiler_heap_ctx_gpu_va;
	u64 first_heap_chunk_gpu_va;
};

struct panthor_client_file {
	u64 session_id;
	struct mutex lock;
	struct xarray bos;
	struct xarray syncobjs;
	struct xarray groups;
	struct xarray heaps;
	u64 next_mmap_offset;
};

struct panthor_client_syncobj_wait_legacy {
	__u64 handles;
	__s64 timeout_nsec;
	__u32 count_handles;
	__u32 flags;
	__u32 first_signaled;
	__u32 pad;
};

#define PANTHOR_CLIENT_IOCTL_SYNCOBJ_WAIT_LEGACY \
	DRM_IOWR(0xC3, struct panthor_client_syncobj_wait_legacy)

struct panthor_client_syncobj_timeline_wait_legacy {
	__u64 handles;
	__u64 points;
	__s64 timeout_nsec;
	__u32 count_handles;
	__u32 flags;
	__u32 first_signaled;
	__u32 pad;
};

#define PANTHOR_CLIENT_IOCTL_SYNCOBJ_TIMELINE_WAIT_LEGACY \
	DRM_IOWR(0xCA, struct panthor_client_syncobj_timeline_wait_legacy)

static struct panthor_client_device *panthor_client;
static bool panthor_client_bo_mmap_cached;
module_param_named(bo_mmap_cached, panthor_client_bo_mmap_cached, bool, 0644);
MODULE_PARM_DESC(bo_mmap_cached,
		 "Map shared-virtualization BO payloads as cached WB instead of write-combine");

static int panthor_client_bo_destroy_rpc_session(u64 session_id,
						 u32 client_bo_handle,
						 u32 *proxy_bo_handle);
static int panthor_client_syncobj_destroy_rpc_session(u64 session_id,
						      u32 client_syncobj_handle,
						      u32 *proxy_syncobj_handle);
static int panthor_client_group_destroy_rpc_session(u64 session_id,
						    u32 client_group_handle,
						    u32 *proxy_group_handle);
static int panthor_client_tiler_heap_destroy_rpc_session(u64 session_id,
							 u32 client_heap_handle,
							 u32 *proxy_heap_handle);

static void panthor_client_group_put(struct panthor_client_group *group)
{
	u32 proxy_group_handle = 0;
	int ret;

	if (!group)
		return;

	if (!refcount_dec_and_test(&group->refcnt))
		return;

	if (group->destroy_pending) {
		ret = panthor_client_group_destroy_rpc_session(
			group->session_id, group->client_handle,
			&proxy_group_handle);
		if (ret)
			pr_warn("panthor-client: delayed GROUP_DESTROY failed session=%llu client_group=%u ret=%d\n",
				group->session_id, group->client_handle, ret);
		else
			pr_info("panthor-client: GROUP_DESTROY session=%llu client_group=%u proxy_group=%u\n",
				group->session_id, group->client_handle,
				proxy_group_handle);
	}

	kfree(group);
}

static bool panthor_client_group_get(struct panthor_client_group *group)
{
	return group && refcount_inc_not_zero(&group->refcnt);
}

static void panthor_client_bo_put(struct panthor_client_bo *bo)
{
	u32 proxy_bo_handle = 0;
	int ret;

	if (!bo)
		return;

	if (!refcount_dec_and_test(&bo->refcnt))
		return;

	if (bo->destroy_pending) {
		ret = panthor_client_bo_destroy_rpc_session(
			bo->session_id, bo->client_handle, &proxy_bo_handle);
		if (ret)
			pr_warn("panthor-client: delayed BO_DESTROY failed session=%llu client_bo=%u ret=%d\n",
				bo->session_id, bo->client_handle, ret);
		else
			pr_info("panthor-client: BO_DESTROY session=%llu client_bo=%u proxy_bo=%u\n",
				bo->session_id, bo->client_handle,
				proxy_bo_handle);
	}

	client_vmshm_manager_put(bo->payload_obj);
	kfree(bo);
}

static bool panthor_client_bo_get(struct panthor_client_bo *bo)
{
	return bo && refcount_inc_not_zero(&bo->refcnt);
}

static void panthor_client_bo_vma_open(struct vm_area_struct *vma)
{
	panthor_client_bo_get(vma->vm_private_data);
}

static void panthor_client_bo_vma_close(struct vm_area_struct *vma)
{
	panthor_client_bo_put(vma->vm_private_data);
}

static const struct vm_operations_struct panthor_client_bo_vm_ops = {
	.open = panthor_client_bo_vma_open,
	.close = panthor_client_bo_vma_close,
};

static u64 panthor_client_alloc_mmap_offset(struct panthor_client_file *pcfile,
					    u64 size)
{
	u64 offset = pcfile->next_mmap_offset;
	u64 aligned_size;

	if (!size || size > U64_MAX - (PAGE_SIZE - 1))
		return 0;

	aligned_size = PAGE_ALIGN(size);
	if (!aligned_size ||
	    offset > U64_MAX - aligned_size ||
	    offset >= DRM_PANTHOR_USER_MMIO_OFFSET)
		return 0;

	pcfile->next_mmap_offset += aligned_size;
	return offset;
}

static s64 panthor_client_abs_timeout_to_rel_ns(s64 abs_timeout_ns)
{
	s64 now_ns, rel_ns;

	if (abs_timeout_ns <= 0)
		return 0;

	now_ns = ktime_get_ns();
	if (abs_timeout_ns <= now_ns)
		return 0;

	rel_ns = abs_timeout_ns - now_ns;
	return rel_ns > 0 ? rel_ns : S64_MAX;
}

static u64 panthor_client_abs_deadline_to_rel_ns(u64 abs_deadline_ns)
{
	u64 now_ns;

	if (!abs_deadline_ns)
		return 0;

	now_ns = ktime_get_ns();
	if (abs_deadline_ns <= now_ns)
		return 0;

	return abs_deadline_ns - now_ns;
}

static struct panthor_client_bo *
panthor_client_bo_lookup_locked(struct panthor_client_file *pcfile,
				u32 client_bo_handle)
{
	return xa_load(&pcfile->bos, client_bo_handle);
}

static struct panthor_client_bo *
panthor_client_bo_lookup_mmap_get(struct panthor_client_file *pcfile,
				  u64 mmap_offset, u64 *bo_offset)
{
	struct panthor_client_bo *bo;
	unsigned long index;

	mutex_lock(&pcfile->lock);
	xa_for_each(&pcfile->bos, index, bo) {
		u64 map_size = PAGE_ALIGN(bo->size);

		if (bo->mmap_offset &&
		    mmap_offset >= bo->mmap_offset &&
		    mmap_offset - bo->mmap_offset < map_size &&
		    panthor_client_bo_get(bo)) {
			if (bo_offset)
				*bo_offset = mmap_offset - bo->mmap_offset;
			mutex_unlock(&pcfile->lock);
			return bo;
		}
	}
	mutex_unlock(&pcfile->lock);

	return NULL;
}

static void panthor_client_bos_release_local(struct panthor_client_file *pcfile)
{
	struct panthor_client_bo *bo;
	unsigned long index;

	mutex_lock(&pcfile->lock);
	xa_for_each(&pcfile->bos, index, bo) {
		panthor_client_bo_put(bo);
	}
	mutex_unlock(&pcfile->lock);
	xa_destroy(&pcfile->bos);
}

static void
panthor_client_syncobj_put(struct panthor_client_syncobj *syncobj)
{
	u32 proxy_handle = 0;
	int ret;

	if (!syncobj)
		return;

	if (!refcount_dec_and_test(&syncobj->refcnt))
		return;

	if (syncobj->destroy_pending) {
		ret = panthor_client_syncobj_destroy_rpc_session(
			syncobj->session_id, syncobj->client_handle,
			&proxy_handle);
		if (ret)
			pr_warn("panthor-client: delayed SYNCOBJ_DESTROY failed session=%llu client_sync=%u ret=%d\n",
				syncobj->session_id, syncobj->client_handle,
				ret);
		else
			pr_info("panthor-client: SYNCOBJ_DESTROY session=%llu client_sync=%u proxy_sync=%u\n",
				syncobj->session_id, syncobj->client_handle,
				proxy_handle);
	}

	kfree(syncobj);
}

static bool
panthor_client_syncobj_get(struct panthor_client_syncobj *syncobj)
{
	return syncobj && refcount_inc_not_zero(&syncobj->refcnt);
}

static void
panthor_client_syncobjs_release_local(struct panthor_client_file *pcfile)
{
	struct panthor_client_syncobj *syncobj;
	unsigned long index = 1;

	for (;;) {
		mutex_lock(&pcfile->lock);
		syncobj = xa_find(&pcfile->syncobjs, &index, ULONG_MAX,
				  XA_PRESENT);
		if (syncobj)
			xa_erase(&pcfile->syncobjs, index);
		mutex_unlock(&pcfile->lock);
		if (!syncobj)
			break;

		syncobj->destroy_pending = true;
		panthor_client_syncobj_put(syncobj);
		index++;
	}

	xa_destroy(&pcfile->syncobjs);
}

static void
panthor_client_groups_release_local(struct panthor_client_file *pcfile)
{
	struct panthor_client_group *group;
	unsigned long index = 1;

	if (!pcfile)
		return;

	for (;;) {
		mutex_lock(&pcfile->lock);
		group = xa_find(&pcfile->groups, &index, ULONG_MAX,
				XA_PRESENT);
		if (group)
			xa_erase(&pcfile->groups, index);
		mutex_unlock(&pcfile->lock);
		if (!group)
			break;

		group->destroy_pending = true;
		panthor_client_group_put(group);
		index++;
	}

	xa_destroy(&pcfile->groups);
}

static void
panthor_client_heaps_release_local(struct panthor_client_file *pcfile)
{
	struct panthor_client_heap *heap;
	unsigned long index = 1;
	u32 proxy_heap_handle = 0;
	int ret;

	if (!pcfile)
		return;

	for (;;) {
		mutex_lock(&pcfile->lock);
		heap = xa_find(&pcfile->heaps, &index, ULONG_MAX, XA_PRESENT);
		if (heap)
			xa_erase(&pcfile->heaps, index);
		mutex_unlock(&pcfile->lock);
		if (!heap)
			break;

		ret = panthor_client_tiler_heap_destroy_rpc_session(
			heap->session_id, heap->client_handle,
			&proxy_heap_handle);
		if (ret)
			pr_warn("panthor-client: TILER_HEAP_DESTROY release failed session=%llu client_heap=%u ret=%d\n",
				heap->session_id, heap->client_handle, ret);
		else
			pr_info("panthor-client: TILER_HEAP_DESTROY session=%llu client_heap=%u proxy_heap=%u\n",
				heap->session_id, heap->client_handle,
				proxy_heap_handle);
		kfree(heap);
		index++;
	}

	xa_destroy(&pcfile->heaps);
}

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

static int
panthor_client_rpc_vm_bind(const struct panthor_vmshm_vm_bind_req *req,
			   struct panthor_vmshm_vm_bind_rsp *rsp)
{
	return panthor_client_rpc_call(PANTHOR_VMSHM_MSG_VM_BIND_REQ,
				      req, sizeof(*req),
				      PANTHOR_VMSHM_MSG_VM_BIND_RSP,
				      rsp, sizeof(*rsp), sizeof(*rsp), NULL);
}

static int
panthor_client_rpc_vm_get_state(const struct panthor_vmshm_vm_get_state_req *req,
				struct panthor_vmshm_vm_get_state_rsp *rsp)
{
	return panthor_client_rpc_call(PANTHOR_VMSHM_MSG_VM_GET_STATE_REQ,
				      req, sizeof(*req),
				      PANTHOR_VMSHM_MSG_VM_GET_STATE_RSP,
				      rsp, sizeof(*rsp), sizeof(*rsp), NULL);
}

static int
panthor_client_rpc_bo_create(const struct panthor_vmshm_bo_create_req *req,
			     struct panthor_vmshm_bo_create_rsp *rsp)
{
	return panthor_client_rpc_call(PANTHOR_VMSHM_MSG_BO_CREATE_REQ,
				      req, sizeof(*req),
				      PANTHOR_VMSHM_MSG_BO_CREATE_RSP,
				      rsp, sizeof(*rsp), sizeof(*rsp), NULL);
}

static int
panthor_client_rpc_bo_destroy(const struct panthor_vmshm_bo_destroy_req *req,
			      struct panthor_vmshm_bo_destroy_rsp *rsp)
{
	return panthor_client_rpc_call(PANTHOR_VMSHM_MSG_BO_DESTROY_REQ,
				      req, sizeof(*req),
				      PANTHOR_VMSHM_MSG_BO_DESTROY_RSP,
				      rsp, sizeof(*rsp), sizeof(*rsp), NULL);
}

static int
panthor_client_rpc_syncobj_create(
	const struct panthor_vmshm_syncobj_create_req *req,
	struct panthor_vmshm_syncobj_create_rsp *rsp)
{
	return panthor_client_rpc_call(PANTHOR_VMSHM_MSG_SYNCOBJ_CREATE_REQ,
				      req, sizeof(*req),
				      PANTHOR_VMSHM_MSG_SYNCOBJ_CREATE_RSP,
				      rsp, sizeof(*rsp), sizeof(*rsp), NULL);
}

static int
panthor_client_rpc_syncobj_destroy(
	const struct panthor_vmshm_syncobj_destroy_req *req,
	struct panthor_vmshm_syncobj_destroy_rsp *rsp)
{
	return panthor_client_rpc_call(PANTHOR_VMSHM_MSG_SYNCOBJ_DESTROY_REQ,
				      req, sizeof(*req),
				      PANTHOR_VMSHM_MSG_SYNCOBJ_DESTROY_RSP,
				      rsp, sizeof(*rsp), sizeof(*rsp), NULL);
}

static int
panthor_client_rpc_syncobj_wait(
	const struct panthor_vmshm_syncobj_wait_req *req,
	struct panthor_vmshm_syncobj_wait_rsp *rsp)
{
	return panthor_client_rpc_call(PANTHOR_VMSHM_MSG_SYNCOBJ_WAIT_REQ,
				      req, sizeof(*req),
				      PANTHOR_VMSHM_MSG_SYNCOBJ_WAIT_RSP,
				      rsp, sizeof(*rsp), sizeof(*rsp), NULL);
}

static int
panthor_client_rpc_syncobj_transfer(
	const struct panthor_vmshm_syncobj_transfer_req *req,
	struct panthor_vmshm_syncobj_transfer_rsp *rsp)
{
	return panthor_client_rpc_call(PANTHOR_VMSHM_MSG_SYNCOBJ_TRANSFER_REQ,
				      req, sizeof(*req),
				      PANTHOR_VMSHM_MSG_SYNCOBJ_TRANSFER_RSP,
				      rsp, sizeof(*rsp), sizeof(*rsp), NULL);
}

static int
panthor_client_rpc_syncobj_timeline_wait(
	const struct panthor_vmshm_syncobj_timeline_wait_req *req,
	struct panthor_vmshm_syncobj_timeline_wait_rsp *rsp)
{
	return panthor_client_rpc_call(
		PANTHOR_VMSHM_MSG_SYNCOBJ_TIMELINE_WAIT_REQ,
		req, sizeof(*req),
		PANTHOR_VMSHM_MSG_SYNCOBJ_TIMELINE_WAIT_RSP,
		rsp, sizeof(*rsp), sizeof(*rsp), NULL);
}

static int
panthor_client_rpc_syncobj_array(
	u32 req_type, u32 rsp_type,
	const struct panthor_vmshm_syncobj_array_req *req,
	struct panthor_vmshm_syncobj_array_rsp *rsp)
{
	return panthor_client_rpc_call(req_type, req, sizeof(*req),
				      rsp_type, rsp, sizeof(*rsp),
				      sizeof(*rsp), NULL);
}

static int
panthor_client_rpc_syncobj_timeline_array(
	u32 req_type, u32 rsp_type,
	const struct panthor_vmshm_syncobj_timeline_array_req *req,
	struct panthor_vmshm_syncobj_timeline_array_rsp *rsp)
{
	return panthor_client_rpc_call(req_type, req, sizeof(*req),
					      rsp_type, rsp, sizeof(*rsp),
					      sizeof(*rsp), NULL);
}

static int
panthor_client_rpc_group_create(
	const struct panthor_vmshm_group_create_req *req,
	struct panthor_vmshm_group_create_rsp *rsp)
{
	return panthor_client_rpc_call(PANTHOR_VMSHM_MSG_GROUP_CREATE_REQ,
				      req, sizeof(*req),
				      PANTHOR_VMSHM_MSG_GROUP_CREATE_RSP,
				      rsp, sizeof(*rsp), sizeof(*rsp), NULL);
}

static int
panthor_client_rpc_group_destroy(
	const struct panthor_vmshm_group_destroy_req *req,
	struct panthor_vmshm_group_destroy_rsp *rsp)
{
	return panthor_client_rpc_call(PANTHOR_VMSHM_MSG_GROUP_DESTROY_REQ,
				      req, sizeof(*req),
				      PANTHOR_VMSHM_MSG_GROUP_DESTROY_RSP,
				      rsp, sizeof(*rsp), sizeof(*rsp), NULL);
}

static int
panthor_client_rpc_group_get_state(
	const struct panthor_vmshm_group_get_state_req *req,
	struct panthor_vmshm_group_get_state_rsp *rsp)
{
	return panthor_client_rpc_call(PANTHOR_VMSHM_MSG_GROUP_GET_STATE_REQ,
				      req, sizeof(*req),
					      PANTHOR_VMSHM_MSG_GROUP_GET_STATE_RSP,
					      rsp, sizeof(*rsp), sizeof(*rsp), NULL);
}

static int
panthor_client_rpc_group_submit(
	const struct panthor_vmshm_group_submit_req *req,
	struct panthor_vmshm_group_submit_rsp *rsp)
{
	return panthor_client_rpc_call(PANTHOR_VMSHM_MSG_GROUP_SUBMIT_REQ,
				      req, sizeof(*req),
				      PANTHOR_VMSHM_MSG_GROUP_SUBMIT_RSP,
				      rsp, sizeof(*rsp), sizeof(*rsp), NULL);
}

static int
panthor_client_rpc_tiler_heap_create(
	const struct panthor_vmshm_tiler_heap_create_req *req,
	struct panthor_vmshm_tiler_heap_create_rsp *rsp)
{
	return panthor_client_rpc_call(PANTHOR_VMSHM_MSG_TILER_HEAP_CREATE_REQ,
				      req, sizeof(*req),
				      PANTHOR_VMSHM_MSG_TILER_HEAP_CREATE_RSP,
				      rsp, sizeof(*rsp), sizeof(*rsp), NULL);
}

static int
panthor_client_rpc_tiler_heap_destroy(
	const struct panthor_vmshm_tiler_heap_destroy_req *req,
	struct panthor_vmshm_tiler_heap_destroy_rsp *rsp)
{
	return panthor_client_rpc_call(PANTHOR_VMSHM_MSG_TILER_HEAP_DESTROY_REQ,
				      req, sizeof(*req),
				      PANTHOR_VMSHM_MSG_TILER_HEAP_DESTROY_RSP,
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

static int panthor_client_bo_destroy_rpc_session(u64 session_id,
						 u32 client_bo_handle,
						 u32 *proxy_bo_handle)
{
	struct panthor_vmshm_bo_destroy_req req = {
		.session_id = session_id,
		.client_bo_handle = client_bo_handle,
	};
	struct panthor_vmshm_bo_destroy_rsp rsp;
	int ret;

	ret = panthor_client_rpc_bo_destroy(&req, &rsp);
	if (ret)
		return ret;
	if (rsp.ret)
		return rsp.ret;

	if (proxy_bo_handle)
		*proxy_bo_handle = rsp.proxy_bo_handle;
	return 0;
}

static int panthor_client_bo_destroy_rpc(struct panthor_client_file *pcfile,
					 u32 client_bo_handle,
					 u32 *proxy_bo_handle)
{
	return panthor_client_bo_destroy_rpc_session(
		pcfile ? pcfile->session_id : 0, client_bo_handle,
		proxy_bo_handle);
}

static int panthor_client_bo_destroy(struct panthor_client_file *pcfile,
				     u32 client_bo_handle)
{
	struct panthor_client_bo *bo;

	if (!pcfile || !client_bo_handle)
		return -EINVAL;

	mutex_lock(&pcfile->lock);
	bo = xa_erase(&pcfile->bos, client_bo_handle);
	mutex_unlock(&pcfile->lock);
	if (!bo)
		return -EINVAL;

	bo->destroy_pending = true;
	panthor_client_bo_put(bo);
	return 0;
}

static int panthor_client_group_destroy_rpc_session(u64 session_id,
						    u32 client_group_handle,
						    u32 *proxy_group_handle)
{
	struct panthor_vmshm_group_destroy_req req = {
		.session_id = session_id,
		.client_group_handle = client_group_handle,
	};
	struct panthor_vmshm_group_destroy_rsp rsp;
	int ret;

	ret = panthor_client_rpc_group_destroy(&req, &rsp);
	if (ret)
		return ret;
	if (rsp.ret)
		return rsp.ret;

	if (proxy_group_handle)
		*proxy_group_handle = rsp.proxy_group_handle;
	return 0;
}

static int panthor_client_group_destroy(struct panthor_client_file *pcfile,
					u32 client_group_handle)
{
	struct panthor_client_group *group;

	if (!pcfile || !client_group_handle)
		return -EINVAL;

	mutex_lock(&pcfile->lock);
	group = xa_erase(&pcfile->groups, client_group_handle);
	mutex_unlock(&pcfile->lock);
	if (!group)
		return -EINVAL;

	group->destroy_pending = true;
	panthor_client_group_put(group);
	return 0;
}

static int panthor_client_tiler_heap_destroy_rpc_session(u64 session_id,
							 u32 client_heap_handle,
							 u32 *proxy_heap_handle)
{
	struct panthor_vmshm_tiler_heap_destroy_req req = {
		.session_id = session_id,
		.client_heap_handle = client_heap_handle,
	};
	struct panthor_vmshm_tiler_heap_destroy_rsp rsp;
	int ret;

	ret = panthor_client_rpc_tiler_heap_destroy(&req, &rsp);
	if (ret)
		return ret;
	if (rsp.ret)
		return rsp.ret;

	if (proxy_heap_handle)
		*proxy_heap_handle = rsp.proxy_heap_handle;
	return 0;
}

static int panthor_client_tiler_heap_destroy(struct panthor_client_file *pcfile,
					     u32 client_heap_handle)
{
	struct panthor_client_heap *heap;
	u32 proxy_heap_handle = 0;
	int ret;

	if (!pcfile || !client_heap_handle)
		return -EINVAL;

	mutex_lock(&pcfile->lock);
	heap = xa_erase(&pcfile->heaps, client_heap_handle);
	mutex_unlock(&pcfile->lock);
	if (!heap)
		return -EINVAL;

	ret = panthor_client_tiler_heap_destroy_rpc_session(
		heap->session_id, heap->client_handle, &proxy_heap_handle);
	if (ret) {
		pr_warn("panthor-client: TILER_HEAP_DESTROY failed session=%llu client_heap=%u ret=%d; keeping local handle retired\n",
			heap->session_id, heap->client_handle, ret);
		kfree(heap);
		return ret;
	}

	pr_info("panthor-client: TILER_HEAP_DESTROY session=%llu client_heap=%u proxy_heap=%u\n",
		heap->session_id, heap->client_handle, proxy_heap_handle);
	kfree(heap);
	return 0;
}

static int
panthor_client_syncobj_destroy_rpc_session(u64 session_id,
					   u32 client_syncobj_handle,
					   u32 *proxy_syncobj_handle)
{
	struct panthor_vmshm_syncobj_destroy_req req = {
		.session_id = session_id,
		.client_syncobj_handle = client_syncobj_handle,
	};
	struct panthor_vmshm_syncobj_destroy_rsp rsp;
	int ret;

	ret = panthor_client_rpc_syncobj_destroy(&req, &rsp);
	if (ret)
		return ret;
	if (rsp.ret)
		return rsp.ret;

	if (proxy_syncobj_handle)
		*proxy_syncobj_handle = rsp.proxy_syncobj_handle;
	return 0;
}

static int panthor_client_syncobj_create(struct panthor_client_file *pcfile,
					 struct drm_syncobj_create *args)
{
	struct panthor_vmshm_syncobj_create_req req = { 0 };
	struct panthor_vmshm_syncobj_create_rsp rsp;
	struct panthor_client_syncobj *syncobj;
	int ret;

	if (!pcfile || !args)
		return -EINVAL;
	if (args->flags & ~DRM_SYNCOBJ_CREATE_SIGNALED)
		return -EINVAL;

	req.session_id = pcfile->session_id;
	req.flags = args->flags;

	ret = panthor_client_rpc_syncobj_create(&req, &rsp);
	if (ret)
		return ret;
	if (rsp.ret)
		return rsp.ret;
	if (!rsp.client_syncobj_handle || !rsp.proxy_syncobj_handle)
		return -EPROTO;

	syncobj = kzalloc(sizeof(*syncobj), GFP_KERNEL);
	if (!syncobj) {
		panthor_client_syncobj_destroy_rpc_session(
			pcfile->session_id, rsp.client_syncobj_handle, NULL);
		return -ENOMEM;
	}

	syncobj->session_id = pcfile->session_id;
	syncobj->client_handle = rsp.client_syncobj_handle;
	syncobj->proxy_handle = rsp.proxy_syncobj_handle;
	refcount_set(&syncobj->refcnt, 1);

	mutex_lock(&pcfile->lock);
	ret = xa_insert(&pcfile->syncobjs, rsp.client_syncobj_handle,
			syncobj, GFP_KERNEL);
	mutex_unlock(&pcfile->lock);
	if (ret) {
		kfree(syncobj);
		panthor_client_syncobj_destroy_rpc_session(
			pcfile->session_id, rsp.client_syncobj_handle, NULL);
		return ret;
	}

	args->handle = rsp.client_syncobj_handle;
	pr_info("panthor-client: SYNCOBJ_CREATE session=%llu client_sync=%u proxy_sync=%u flags=0x%x\n",
		pcfile->session_id, rsp.client_syncobj_handle,
		rsp.proxy_syncobj_handle, args->flags);
	return 0;
}

static int panthor_client_syncobj_destroy(struct panthor_client_file *pcfile,
					  struct drm_syncobj_destroy *args)
{
	struct panthor_client_syncobj *syncobj;

	if (!pcfile || !args || args->pad || !args->handle)
		return -EINVAL;

	mutex_lock(&pcfile->lock);
	syncobj = xa_erase(&pcfile->syncobjs, args->handle);
	mutex_unlock(&pcfile->lock);
	if (!syncobj)
		return -EINVAL;

	syncobj->destroy_pending = true;
	panthor_client_syncobj_put(syncobj);
	return 0;
}

static int panthor_client_syncobj_wait(struct panthor_client_file *pcfile,
				       struct drm_syncobj_wait *args)
{
	struct panthor_client_syncobj *syncobjs[PANTHOR_VMSHM_MAX_SYNCOBJ_WAIT_HANDLES] = { 0 };
	struct panthor_vmshm_syncobj_wait_req req = { 0 };
	struct panthor_vmshm_syncobj_wait_rsp rsp;
	u32 handles[PANTHOR_VMSHM_MAX_SYNCOBJ_WAIT_HANDLES];
	void __user *user_handles;
	u32 possible_flags;
	int ret = 0;
	u32 i;

	if (!pcfile || !args)
		return -EINVAL;

	possible_flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL |
			 DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT |
			 DRM_SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE;
	if (args->flags & ~possible_flags)
		return -EINVAL;
	if (!args->count_handles)
		return 0;
	if (args->count_handles > PANTHOR_VMSHM_MAX_SYNCOBJ_WAIT_HANDLES)
		return -E2BIG;
	if (!args->handles)
		return -EFAULT;

	user_handles = u64_to_user_ptr(args->handles);
	if (copy_from_user(handles, user_handles,
			   sizeof(handles[0]) * args->count_handles))
		return -EFAULT;

	req.session_id = pcfile->session_id;
	req.count_handles = args->count_handles;
	req.flags = args->flags;
	req.timeout_rel_nsec =
		panthor_client_abs_timeout_to_rel_ns(args->timeout_nsec);
	if (args->flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE)
		req.deadline_rel_nsec =
			panthor_client_abs_deadline_to_rel_ns(args->deadline_nsec);

	mutex_lock(&pcfile->lock);
	for (i = 0; i < args->count_handles; i++) {
		struct panthor_client_syncobj *syncobj =
			xa_load(&pcfile->syncobjs, handles[i]);

		if (!syncobj || !panthor_client_syncobj_get(syncobj)) {
			ret = -ENOENT;
			goto out_unlock;
		}

		syncobjs[i] = syncobj;
		req.handles[i] = syncobj->client_handle;
	}
	mutex_unlock(&pcfile->lock);

	ret = panthor_client_rpc_syncobj_wait(&req, &rsp);
	if (ret)
		goto out_put_syncobjs;
	if (rsp.ret) {
		ret = rsp.ret;
		goto out_put_syncobjs;
	}

	args->first_signaled = rsp.first_signaled;
	pr_info_ratelimited("panthor-client: SYNCOBJ_WAIT session=%llu count=%u flags=0x%x first=%u\n",
			    pcfile->session_id, args->count_handles,
			    args->flags, rsp.first_signaled);
	ret = 0;
	goto out_put_syncobjs;

out_unlock:
	mutex_unlock(&pcfile->lock);

out_put_syncobjs:
	for (i = 0; i < args->count_handles; i++)
		panthor_client_syncobj_put(syncobjs[i]);

	return ret;
}

static int
panthor_client_syncobj_transfer(struct panthor_client_file *pcfile,
				struct drm_syncobj_transfer *args)
{
	struct panthor_client_syncobj *src_syncobj = NULL;
	struct panthor_client_syncobj *dst_syncobj = NULL;
	struct panthor_vmshm_syncobj_transfer_req req = { 0 };
	struct panthor_vmshm_syncobj_transfer_rsp rsp;
	int ret = 0;

	if (!pcfile || !args || args->pad || !args->src_handle ||
	    !args->dst_handle)
		return -EINVAL;

	mutex_lock(&pcfile->lock);
	src_syncobj = xa_load(&pcfile->syncobjs, args->src_handle);
	if (!src_syncobj || !panthor_client_syncobj_get(src_syncobj)) {
		ret = -ENOENT;
		goto out_unlock;
	}

	dst_syncobj = xa_load(&pcfile->syncobjs, args->dst_handle);
	if (!dst_syncobj || !panthor_client_syncobj_get(dst_syncobj)) {
		ret = -ENOENT;
		goto out_unlock;
	}
	mutex_unlock(&pcfile->lock);

	req.session_id = pcfile->session_id;
	req.src_handle = src_syncobj->client_handle;
	req.dst_handle = dst_syncobj->client_handle;
	req.src_point = args->src_point;
	req.dst_point = args->dst_point;
	req.flags = args->flags;

	ret = panthor_client_rpc_syncobj_transfer(&req, &rsp);
	if (ret)
		goto out_put_syncobjs;
	if (rsp.ret) {
		ret = rsp.ret;
		goto out_put_syncobjs;
	}

	pr_info("panthor-client: SYNCOBJ_TRANSFER session=%llu src=%u dst=%u src_point=%llu dst_point=%llu flags=0x%x\n",
		pcfile->session_id, args->src_handle, args->dst_handle,
		args->src_point, args->dst_point, args->flags);
	ret = 0;
	goto out_put_syncobjs;

out_unlock:
	mutex_unlock(&pcfile->lock);
	goto out_put_syncobjs;

out_put_syncobjs:
	panthor_client_syncobj_put(dst_syncobj);
	panthor_client_syncobj_put(src_syncobj);
	return ret;
}

static int
panthor_client_syncobj_timeline_wait(
	struct panthor_client_file *pcfile,
	struct drm_syncobj_timeline_wait *args)
{
	struct panthor_client_syncobj *syncobjs[PANTHOR_VMSHM_MAX_SYNCOBJ_WAIT_HANDLES] = { 0 };
	struct panthor_vmshm_syncobj_timeline_wait_req req = { 0 };
	struct panthor_vmshm_syncobj_timeline_wait_rsp rsp;
	u32 handles[PANTHOR_VMSHM_MAX_SYNCOBJ_WAIT_HANDLES];
	u64 points[PANTHOR_VMSHM_MAX_SYNCOBJ_WAIT_HANDLES];
	void __user *user_handles;
	void __user *user_points;
	u32 possible_flags;
	int ret = 0;
	u32 i;

	if (!pcfile || !args)
		return -EINVAL;

	possible_flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL |
			 DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT |
			 DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE |
			 DRM_SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE;
	if (args->flags & ~possible_flags)
		return -EINVAL;
	if (!args->count_handles)
		return 0;
	if (args->count_handles > PANTHOR_VMSHM_MAX_SYNCOBJ_WAIT_HANDLES)
		return -E2BIG;
	if (!args->handles || !args->points)
		return -EFAULT;

	user_handles = u64_to_user_ptr(args->handles);
	user_points = u64_to_user_ptr(args->points);
	if (copy_from_user(handles, user_handles,
			   sizeof(handles[0]) * args->count_handles))
		return -EFAULT;
	if (copy_from_user(points, user_points,
			   sizeof(points[0]) * args->count_handles))
		return -EFAULT;

	req.session_id = pcfile->session_id;
	req.count_handles = args->count_handles;
	req.flags = args->flags;
	req.timeout_rel_nsec =
		panthor_client_abs_timeout_to_rel_ns(args->timeout_nsec);
	if (args->flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE)
		req.deadline_rel_nsec =
			panthor_client_abs_deadline_to_rel_ns(args->deadline_nsec);

	mutex_lock(&pcfile->lock);
	for (i = 0; i < args->count_handles; i++) {
		struct panthor_client_syncobj *syncobj =
			xa_load(&pcfile->syncobjs, handles[i]);

		if (!syncobj || !panthor_client_syncobj_get(syncobj)) {
			ret = -ENOENT;
			goto out_unlock;
		}

		syncobjs[i] = syncobj;
		req.handles[i] = syncobj->client_handle;
		req.points[i] = points[i];
	}
	mutex_unlock(&pcfile->lock);

	ret = panthor_client_rpc_syncobj_timeline_wait(&req, &rsp);
	if (ret)
		goto out_put_syncobjs;
	if (rsp.ret) {
		ret = rsp.ret;
		goto out_put_syncobjs;
	}

	args->first_signaled = rsp.first_signaled;
	pr_info_ratelimited("panthor-client: SYNCOBJ_TIMELINE_WAIT session=%llu count=%u flags=0x%x first=%u\n",
			    pcfile->session_id, args->count_handles,
			    args->flags, rsp.first_signaled);
	ret = 0;
	goto out_put_syncobjs;

out_unlock:
	mutex_unlock(&pcfile->lock);

out_put_syncobjs:
	for (i = 0; i < args->count_handles; i++)
		panthor_client_syncobj_put(syncobjs[i]);

	return ret;
}

static int
panthor_client_fill_syncobj_handles(
	struct panthor_client_file *pcfile, const u32 *handles, u32 count_handles,
	struct panthor_client_syncobj **syncobjs, u32 *out_handles)
{
	int ret = 0;
	u32 i;

	if (!pcfile || (count_handles && (!handles || !syncobjs || !out_handles)))
		return -EINVAL;
	if (count_handles > PANTHOR_VMSHM_MAX_SYNCOBJ_WAIT_HANDLES)
		return -E2BIG;

	mutex_lock(&pcfile->lock);
	for (i = 0; i < count_handles; i++) {
		struct panthor_client_syncobj *syncobj =
			xa_load(&pcfile->syncobjs, handles[i]);

		if (!syncobj || !panthor_client_syncobj_get(syncobj)) {
			ret = -ENOENT;
			goto out_put_syncobjs;
		}

		syncobjs[i] = syncobj;
		out_handles[i] = syncobj->client_handle;
	}

	mutex_unlock(&pcfile->lock);
	return ret;

out_put_syncobjs:
	mutex_unlock(&pcfile->lock);
	while (i--)
		panthor_client_syncobj_put(syncobjs[i]);
	return ret;
}

static void
panthor_client_put_syncobj_array(struct panthor_client_syncobj **syncobjs,
				 u32 count_handles)
{
	u32 i;

	for (i = 0; i < count_handles; i++)
		panthor_client_syncobj_put(syncobjs[i]);
}

static int
panthor_client_syncobj_array_op(struct panthor_client_file *pcfile,
				struct drm_syncobj_array *args, u32 req_type,
				u32 rsp_type, const char *name)
{
	struct panthor_client_syncobj *syncobjs[PANTHOR_VMSHM_MAX_SYNCOBJ_WAIT_HANDLES] = { 0 };
	struct panthor_vmshm_syncobj_array_req req = { 0 };
	struct panthor_vmshm_syncobj_array_rsp rsp;
	u32 handles[PANTHOR_VMSHM_MAX_SYNCOBJ_WAIT_HANDLES];
	void __user *user_handles;
	int ret;

	if (!pcfile || !args || args->pad)
		return -EINVAL;
	if (!args->count_handles)
		return -EINVAL;
	if (args->count_handles > PANTHOR_VMSHM_MAX_SYNCOBJ_WAIT_HANDLES)
		return -E2BIG;
	if (!args->handles)
		return -EFAULT;

	user_handles = u64_to_user_ptr(args->handles);
	if (copy_from_user(handles, user_handles,
			   sizeof(handles[0]) * args->count_handles))
		return -EFAULT;

	req.session_id = pcfile->session_id;
	req.count_handles = args->count_handles;

	ret = panthor_client_fill_syncobj_handles(
		pcfile, handles, args->count_handles, syncobjs, req.handles);
	if (ret)
		goto out_put_syncobjs;

	ret = panthor_client_rpc_syncobj_array(req_type, rsp_type, &req, &rsp);
	if (ret)
		goto out_put_syncobjs;
	if (rsp.ret) {
		ret = rsp.ret;
		goto out_put_syncobjs;
	}

	pr_info("panthor-client: %s session=%llu count=%u first=%u\n",
		name, pcfile->session_id, args->count_handles, handles[0]);
	ret = 0;

out_put_syncobjs:
	panthor_client_put_syncobj_array(syncobjs, args->count_handles);
	return ret;
}

static int
panthor_client_syncobj_timeline_array_op(
	struct panthor_client_file *pcfile, struct drm_syncobj_timeline_array *args,
	u32 req_type, u32 rsp_type, const char *name, bool copy_points_to_user)
{
	struct panthor_client_syncobj *syncobjs[PANTHOR_VMSHM_MAX_SYNCOBJ_WAIT_HANDLES] = { 0 };
	struct panthor_vmshm_syncobj_timeline_array_req req = { 0 };
	struct panthor_vmshm_syncobj_timeline_array_rsp rsp;
	u32 handles[PANTHOR_VMSHM_MAX_SYNCOBJ_WAIT_HANDLES];
	u64 points[PANTHOR_VMSHM_MAX_SYNCOBJ_WAIT_HANDLES] = { 0 };
	void __user *user_handles;
	void __user *user_points;
	int ret;

	if (!pcfile || !args)
		return -EINVAL;
	if (!args->count_handles)
		return -EINVAL;
	if (args->count_handles > PANTHOR_VMSHM_MAX_SYNCOBJ_WAIT_HANDLES)
		return -E2BIG;
	if (!args->handles)
		return -EFAULT;
	if (copy_points_to_user && !args->points)
		return -EFAULT;
	if (!copy_points_to_user && req_type == PANTHOR_VMSHM_MSG_SYNCOBJ_QUERY_REQ)
		return -EINVAL;
	if (args->flags & ~DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED)
		return -EINVAL;
	if (!copy_points_to_user && args->flags)
		return -EINVAL;

	user_handles = u64_to_user_ptr(args->handles);
	if (copy_from_user(handles, user_handles,
			   sizeof(handles[0]) * args->count_handles))
		return -EFAULT;

	if (!copy_points_to_user && args->points) {
		user_points = u64_to_user_ptr(args->points);
		if (copy_from_user(points, user_points,
				   sizeof(points[0]) * args->count_handles))
			return -EFAULT;
	}

	req.session_id = pcfile->session_id;
	req.count_handles = args->count_handles;
	req.flags = args->flags;
	memcpy(req.points, points, sizeof(points[0]) * args->count_handles);

	ret = panthor_client_fill_syncobj_handles(
		pcfile, handles, args->count_handles, syncobjs, req.handles);
	if (ret)
		goto out_put_syncobjs;

	ret = panthor_client_rpc_syncobj_timeline_array(req_type, rsp_type,
						       &req, &rsp);
	if (ret)
		goto out_put_syncobjs;
	if (rsp.ret) {
		ret = rsp.ret;
		goto out_put_syncobjs;
	}

	if (copy_points_to_user) {
		user_points = u64_to_user_ptr(args->points);
		if (copy_to_user(user_points, rsp.points,
				 sizeof(rsp.points[0]) * args->count_handles)) {
			ret = -EFAULT;
			goto out_put_syncobjs;
		}
	}

	pr_info("panthor-client: %s session=%llu count=%u flags=0x%x first=%u\n",
		name, pcfile->session_id, args->count_handles, args->flags,
		handles[0]);
	ret = 0;

out_put_syncobjs:
	panthor_client_put_syncobj_array(syncobjs, args->count_handles);
	return ret;
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

static int panthor_client_vm_get_state(struct panthor_client_file *pcfile,
				       struct drm_panthor_vm_get_state *args)
{
	struct panthor_vmshm_vm_get_state_req req = { 0 };
	struct panthor_vmshm_vm_get_state_rsp rsp;
	int ret;

	if (!pcfile || !args || !args->vm_id)
		return -EINVAL;

	req.session_id = pcfile->session_id;
	req.client_vm_id = args->vm_id;

	ret = panthor_client_rpc_vm_get_state(&req, &rsp);
	if (ret)
		return ret;
	if (rsp.ret)
		return rsp.ret;
	if (rsp.state != DRM_PANTHOR_VM_STATE_USABLE &&
	    rsp.state != DRM_PANTHOR_VM_STATE_UNUSABLE)
		return -EPROTO;

	args->state = rsp.state;
	pr_info("panthor-client: VM_GET_STATE session=%llu client_vm=%u proxy_vm=%u state=%u\n",
		pcfile->session_id, args->vm_id, rsp.proxy_vm_id,
		rsp.state);
	return 0;
}

static int
panthor_client_copy_group_queues(struct drm_panthor_group_create *args,
				 struct drm_panthor_queue_create *queues)
{
	void __user *user_ptr = u64_to_user_ptr(args->queues.array);
	u32 i;

	if (!args->queues.count)
		return -EINVAL;
	if (args->queues.count > PANTHOR_VMSHM_MAX_GROUP_QUEUES)
		return -E2BIG;
	if (!args->queues.array)
		return -EINVAL;
	if (args->queues.stride <
	    offsetofend(struct drm_panthor_queue_create, ringbuf_size))
		return -EINVAL;

	for (i = 0; i < args->queues.count; i++) {
		int ret = copy_struct_from_user(&queues[i], sizeof(queues[i]),
						user_ptr, args->queues.stride);

		if (ret)
			return ret;

		user_ptr += args->queues.stride;
	}

	return 0;
}

static int panthor_client_group_create(struct panthor_client_file *pcfile,
				       struct drm_panthor_group_create *args)
{
	struct drm_panthor_queue_create queues[PANTHOR_VMSHM_MAX_GROUP_QUEUES] = { 0 };
	struct panthor_vmshm_group_create_req req = { 0 };
	struct panthor_vmshm_group_create_rsp rsp;
	struct panthor_client_group *group;
	int ret;

	if (!pcfile || !args)
		return -EINVAL;
	if (args->pad)
		return -EINVAL;
	if (args->priority > PANTHOR_GROUP_PRIORITY_MEDIUM)
		return -EACCES;

	ret = panthor_client_copy_group_queues(args, queues);
	if (ret)
		return ret;

	req.session_id = pcfile->session_id;
	req.client_vm_id = args->vm_id;
	req.queue_count = args->queues.count;
	req.max_compute_cores = args->max_compute_cores;
	req.max_fragment_cores = args->max_fragment_cores;
	req.max_tiler_cores = args->max_tiler_cores;
	req.priority = args->priority;
	req.compute_core_mask = args->compute_core_mask;
	req.fragment_core_mask = args->fragment_core_mask;
	req.tiler_core_mask = args->tiler_core_mask;
	memcpy(req.queues, queues, sizeof(queues[0]) * args->queues.count);

	ret = panthor_client_rpc_group_create(&req, &rsp);
	if (ret)
		return ret;
	if (rsp.ret)
		return rsp.ret;
	if (!rsp.client_group_handle || !rsp.proxy_group_handle ||
	    !rsp.proxy_vm_id)
		return -EPROTO;

	group = kzalloc(sizeof(*group), GFP_KERNEL);
	if (!group) {
		panthor_client_group_destroy_rpc_session(
			pcfile->session_id, rsp.client_group_handle, NULL);
		return -ENOMEM;
	}

	group->session_id = pcfile->session_id;
	group->client_handle = rsp.client_group_handle;
	group->proxy_handle = rsp.proxy_group_handle;
	refcount_set(&group->refcnt, 1);

	mutex_lock(&pcfile->lock);
	ret = xa_insert(&pcfile->groups, rsp.client_group_handle, group,
			GFP_KERNEL);
	mutex_unlock(&pcfile->lock);
	if (ret) {
		kfree(group);
		panthor_client_group_destroy_rpc_session(
			pcfile->session_id, rsp.client_group_handle, NULL);
		return ret;
	}

	args->group_handle = rsp.client_group_handle;
	pr_info("panthor-client: GROUP_CREATE session=%llu client_group=%u proxy_group=%u client_vm=%u proxy_vm=%u queues=%u\n",
		pcfile->session_id, rsp.client_group_handle,
		rsp.proxy_group_handle, args->vm_id, rsp.proxy_vm_id,
		args->queues.count);
	return 0;
}

static int
panthor_client_group_get_state(struct panthor_client_file *pcfile,
			       struct drm_panthor_group_get_state *args)
{
	struct panthor_vmshm_group_get_state_req req = { 0 };
	struct panthor_vmshm_group_get_state_rsp rsp;
	struct panthor_client_group *group = NULL;
	int ret = 0;

	if (!pcfile || !args || args->pad || !args->group_handle)
		return -EINVAL;

	mutex_lock(&pcfile->lock);
	group = xa_load(&pcfile->groups, args->group_handle);
	if (!group || !panthor_client_group_get(group))
		ret = -ENOENT;
	else
		req.client_group_handle = group->client_handle;
	mutex_unlock(&pcfile->lock);
	if (ret)
		return ret;

	req.session_id = pcfile->session_id;
	ret = panthor_client_rpc_group_get_state(&req, &rsp);
	if (ret)
		goto out_put_group;
	if (rsp.ret) {
		ret = rsp.ret;
		goto out_put_group;
	}

	args->state = rsp.state;
	args->fatal_queues = rsp.fatal_queues;
	pr_info("panthor-client: GROUP_GET_STATE session=%llu client_group=%u proxy_group=%u state=0x%x fatal_queues=0x%x\n",
		pcfile->session_id, args->group_handle, rsp.proxy_group_handle,
		rsp.state, rsp.fatal_queues);
	ret = 0;

out_put_group:
	panthor_client_group_put(group);
	return ret;
}

static int
panthor_client_check_group_submit_sync_op(
	const struct drm_panthor_sync_op *sync_op)
{
	u8 handle_type;

	if (sync_op->flags & ~PANTHOR_CLIENT_SYNC_OP_FLAGS_MASK)
		return -EINVAL;

	handle_type = sync_op->flags & DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_MASK;
	if (handle_type != DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_SYNCOBJ &&
	    handle_type != DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_TIMELINE_SYNCOBJ)
		return -EINVAL;

	if (handle_type == DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_SYNCOBJ &&
	    sync_op->timeline_value)
		return -EINVAL;

	return 0;
}

static int
panthor_client_copy_group_submit(
	struct drm_panthor_group_submit *args,
	struct drm_panthor_queue_submit *jobs,
	struct drm_panthor_sync_op *syncs,
	u32 *sync_starts,
	u32 *sync_counts,
	u32 *sync_count)
{
	void __user *job_ptr = u64_to_user_ptr(args->queue_submits.array);
	u32 total_syncs = 0;
	u32 i;

	if (!args->queue_submits.count)
		return -EINVAL;
	if (args->queue_submits.count > PANTHOR_VMSHM_MAX_GROUP_SUBMITS)
		return -E2BIG;
	if (!args->queue_submits.array)
		return -EINVAL;
	if (args->queue_submits.stride <
	    offsetofend(struct drm_panthor_queue_submit, syncs))
		return -EINVAL;

	for (i = 0; i < args->queue_submits.count; i++) {
		u32 count;
		void __user *sync_ptr;
		int ret;
		u32 j;

		ret = copy_struct_from_user(&jobs[i], sizeof(jobs[i]), job_ptr,
					    args->queue_submits.stride);
			if (ret)
				return ret;

			if (jobs[i].pad)
				return -EINVAL;
			if (jobs[i].latest_flush & GENMASK(30, 24))
				return -EINVAL;

		count = jobs[i].syncs.count;
		if (count > PANTHOR_VMSHM_MAX_GROUP_SUBMIT_SYNCS ||
		    total_syncs > PANTHOR_VMSHM_MAX_GROUP_SUBMIT_SYNCS - count)
			return -E2BIG;
		if (count && !jobs[i].syncs.array)
			return -EINVAL;
		if (count && jobs[i].syncs.stride <
		    offsetofend(struct drm_panthor_sync_op, timeline_value))
			return -EINVAL;

		sync_starts[i] = total_syncs;
		sync_counts[i] = count;
		sync_ptr = u64_to_user_ptr(jobs[i].syncs.array);

		for (j = 0; j < count; j++) {
			ret = copy_struct_from_user(&syncs[total_syncs + j],
						    sizeof(syncs[0]), sync_ptr,
						    jobs[i].syncs.stride);
			if (ret)
				return ret;

			ret = panthor_client_check_group_submit_sync_op(
				&syncs[total_syncs + j]);
			if (ret)
				return ret;

			sync_ptr += jobs[i].syncs.stride;
		}

		total_syncs += count;
		job_ptr += args->queue_submits.stride;
	}

	*sync_count = total_syncs;
	return 0;
}

static int
panthor_client_group_submit(struct panthor_client_file *pcfile,
			    struct drm_panthor_group_submit *args)
{
	struct drm_panthor_queue_submit jobs[PANTHOR_VMSHM_MAX_GROUP_SUBMITS] = { 0 };
	struct drm_panthor_sync_op syncs[PANTHOR_VMSHM_MAX_GROUP_SUBMIT_SYNCS] = { 0 };
	struct panthor_client_syncobj *syncobjs[PANTHOR_VMSHM_MAX_GROUP_SUBMIT_SYNCS] = { 0 };
	u32 sync_starts[PANTHOR_VMSHM_MAX_GROUP_SUBMITS] = { 0 };
	u32 sync_counts[PANTHOR_VMSHM_MAX_GROUP_SUBMITS] = { 0 };
	struct panthor_vmshm_group_submit_req req = { 0 };
	struct panthor_vmshm_group_submit_rsp rsp;
	struct panthor_client_group *group = NULL;
	u32 sync_count = 0;
	u32 i;
	int ret;

	if (!pcfile || !args || args->pad || !args->group_handle)
		return -EINVAL;

	ret = panthor_client_copy_group_submit(args, jobs, syncs, sync_starts,
					       sync_counts, &sync_count);
	if (ret)
		return ret;

	req.session_id = pcfile->session_id;
	req.job_count = args->queue_submits.count;
	req.sync_count = sync_count;

	for (i = 0; i < req.job_count; i++) {
		req.jobs[i].queue_index = jobs[i].queue_index;
		req.jobs[i].stream_size = jobs[i].stream_size;
		req.jobs[i].stream_addr = jobs[i].stream_addr;
		req.jobs[i].latest_flush = jobs[i].latest_flush;
		req.jobs[i].sync_start = sync_starts[i];
		req.jobs[i].sync_count = sync_counts[i];
	}

	mutex_lock(&pcfile->lock);
	group = xa_load(&pcfile->groups, args->group_handle);
	if (!group) {
		ret = -ENOENT;
		goto out_unlock;
	}
	if (!panthor_client_group_get(group)) {
		group = NULL;
		ret = -ENOENT;
		goto out_unlock;
	}
	req.client_group_handle = group->client_handle;

	for (i = 0; i < sync_count; i++) {
		struct panthor_client_syncobj *syncobj =
			xa_load(&pcfile->syncobjs, syncs[i].handle);

		if (!syncobj || !panthor_client_syncobj_get(syncobj)) {
			ret = -ENOENT;
			goto out_unlock;
		}

		syncobjs[i] = syncobj;
		req.syncs[i].flags = syncs[i].flags;
		req.syncs[i].client_syncobj_handle = syncobj->client_handle;
		req.syncs[i].timeline_value = syncs[i].timeline_value;
	}
	mutex_unlock(&pcfile->lock);

	ret = panthor_client_rpc_group_submit(&req, &rsp);
	if (ret)
		goto out_put_refs;
	if (rsp.ret) {
		ret = rsp.ret;
		goto out_put_refs;
	}
	if (rsp.job_count != req.job_count || rsp.sync_count != req.sync_count ||
	    !rsp.proxy_group_handle) {
		ret = -EPROTO;
		goto out_put_refs;
	}

	pr_info_ratelimited("panthor-client: GROUP_SUBMIT session=%llu client_group=%u proxy_group=%u jobs=%u syncs=%u first_stream=0x%llx first_size=%u first_latest_flush=0x%x\n",
			    pcfile->session_id, args->group_handle,
			    rsp.proxy_group_handle, req.job_count, req.sync_count,
			    req.jobs[0].stream_addr, req.jobs[0].stream_size,
			    req.jobs[0].latest_flush);
	ret = 0;
	goto out_put_refs;

out_unlock:
	mutex_unlock(&pcfile->lock);

out_put_refs:
	for (i = 0; i < sync_count; i++)
		panthor_client_syncobj_put(syncobjs[i]);
	panthor_client_group_put(group);
	return ret;
}

static int
panthor_client_tiler_heap_create(struct panthor_client_file *pcfile,
				 struct drm_panthor_tiler_heap_create *args)
{
	struct panthor_vmshm_tiler_heap_create_req req = { 0 };
	struct panthor_vmshm_tiler_heap_create_rsp rsp;
	struct panthor_client_heap *heap;
	int ret;

	if (!pcfile || !args || !args->vm_id)
		return -EINVAL;

	req.session_id = pcfile->session_id;
	req.client_vm_id = args->vm_id;
	req.initial_chunk_count = args->initial_chunk_count;
	req.chunk_size = args->chunk_size;
	req.max_chunks = args->max_chunks;
	req.target_in_flight = args->target_in_flight;

	ret = panthor_client_rpc_tiler_heap_create(&req, &rsp);
	if (ret)
		return ret;
	if (rsp.ret)
		return rsp.ret;
	if (!rsp.client_heap_handle || !rsp.proxy_heap_handle ||
	    !rsp.proxy_vm_id || !rsp.tiler_heap_ctx_gpu_va ||
	    !rsp.first_heap_chunk_gpu_va)
		return -EPROTO;

	heap = kzalloc(sizeof(*heap), GFP_KERNEL);
	if (!heap) {
		panthor_client_tiler_heap_destroy_rpc_session(
			pcfile->session_id, rsp.client_heap_handle, NULL);
		return -ENOMEM;
	}

	heap->session_id = pcfile->session_id;
	heap->client_handle = rsp.client_heap_handle;
	heap->proxy_handle = rsp.proxy_heap_handle;
	heap->client_vm_id = args->vm_id;
	heap->proxy_vm_id = rsp.proxy_vm_id;
	heap->tiler_heap_ctx_gpu_va = rsp.tiler_heap_ctx_gpu_va;
	heap->first_heap_chunk_gpu_va = rsp.first_heap_chunk_gpu_va;

	mutex_lock(&pcfile->lock);
	ret = xa_insert(&pcfile->heaps, rsp.client_heap_handle, heap,
			GFP_KERNEL);
	mutex_unlock(&pcfile->lock);
	if (ret) {
		kfree(heap);
		panthor_client_tiler_heap_destroy_rpc_session(
			pcfile->session_id, rsp.client_heap_handle, NULL);
		return ret;
	}

	args->handle = rsp.client_heap_handle;
	args->tiler_heap_ctx_gpu_va = rsp.tiler_heap_ctx_gpu_va;
	args->first_heap_chunk_gpu_va = rsp.first_heap_chunk_gpu_va;

	pr_info("panthor-client: TILER_HEAP_CREATE session=%llu client_heap=%u proxy_heap=%u client_vm=%u proxy_vm=%u ctx_va=0x%llx first_chunk_va=0x%llx\n",
		pcfile->session_id, rsp.client_heap_handle,
		rsp.proxy_heap_handle, args->vm_id, rsp.proxy_vm_id,
		rsp.tiler_heap_ctx_gpu_va, rsp.first_heap_chunk_gpu_va);
	return 0;
}

static int
panthor_client_copy_vm_bind_ops(struct drm_panthor_vm_bind *args,
				struct drm_panthor_vm_bind_op *ops,
				struct drm_panthor_sync_op *syncs,
				u32 *sync_starts, u32 *sync_counts,
				u32 *sync_count)
{
	void __user *user_ptr = u64_to_user_ptr(args->ops.array);
	u32 total_syncs = 0;
	u32 i;

	if (!args->ops.count)
		return 0;
	if (!args->ops.array)
		return -EINVAL;
	if (args->ops.stride <
	    offsetofend(struct drm_panthor_vm_bind_op, syncs))
		return -EINVAL;

	for (i = 0; i < args->ops.count; i++) {
		int ret = copy_struct_from_user(&ops[i], sizeof(ops[i]),
						user_ptr, args->ops.stride);

		if (ret)
			return ret;

		if (args->flags & DRM_PANTHOR_VM_BIND_ASYNC) {
			u32 count = ops[i].syncs.count;
			void __user *sync_ptr;
			u32 j;

			if (count > PANTHOR_VMSHM_MAX_VM_BIND_SYNCS ||
			    total_syncs > PANTHOR_VMSHM_MAX_VM_BIND_SYNCS - count)
				return -E2BIG;
			if (count && !ops[i].syncs.array)
				return -EINVAL;
			if (count && ops[i].syncs.stride <
			    offsetofend(struct drm_panthor_sync_op, timeline_value))
				return -EINVAL;

			sync_starts[i] = total_syncs;
			sync_counts[i] = count;
			sync_ptr = u64_to_user_ptr(ops[i].syncs.array);

			for (j = 0; j < count; j++) {
				ret = copy_struct_from_user(
					&syncs[total_syncs + j],
					sizeof(syncs[0]), sync_ptr,
					ops[i].syncs.stride);
				if (ret)
					return ret;

				ret = panthor_client_check_group_submit_sync_op(
					&syncs[total_syncs + j]);
				if (ret)
					return ret;

				sync_ptr += ops[i].syncs.stride;
			}

			total_syncs += count;
		} else if (ops[i].syncs.count || ops[i].syncs.array) {
			return -EINVAL;
		}

		user_ptr += args->ops.stride;
	}

	*sync_count = total_syncs;
	return 0;
}

static int panthor_client_vm_bind(struct panthor_client_file *pcfile,
				  struct drm_panthor_vm_bind *args)
{
	struct drm_panthor_vm_bind_op ops[PANTHOR_VMSHM_MAX_VM_BIND_OPS] = { 0 };
	struct drm_panthor_sync_op syncs[PANTHOR_VMSHM_MAX_VM_BIND_SYNCS] = { 0 };
	struct panthor_client_bo *map_bos[PANTHOR_VMSHM_MAX_VM_BIND_OPS] = { 0 };
	struct panthor_client_syncobj *syncobjs[PANTHOR_VMSHM_MAX_VM_BIND_SYNCS] = { 0 };
	u32 sync_starts[PANTHOR_VMSHM_MAX_VM_BIND_OPS] = { 0 };
	u32 sync_counts[PANTHOR_VMSHM_MAX_VM_BIND_OPS] = { 0 };
	struct panthor_vmshm_vm_bind_req req = { 0 };
	struct panthor_vmshm_vm_bind_rsp rsp;
	u32 sync_count = 0;
	int ret;
	u32 op_count;
	u32 i;

	if (!pcfile || !args)
		return -EINVAL;
	if (args->flags & ~DRM_PANTHOR_VM_BIND_ASYNC)
		return -EINVAL;
	if (args->ops.count > PANTHOR_VMSHM_MAX_VM_BIND_OPS)
		return -E2BIG;

	op_count = args->ops.count;
	ret = panthor_client_copy_vm_bind_ops(args, ops, syncs, sync_starts,
					     sync_counts, &sync_count);
	if (ret)
		return ret;

	req.session_id = pcfile->session_id;
	req.client_vm_id = args->vm_id;
	req.flags = args->flags;
	req.op_count = op_count;
	req.sync_count = sync_count;

	mutex_lock(&pcfile->lock);
	for (i = 0; i < op_count; i++) {
		struct drm_panthor_vm_bind_op *op = &ops[i];
		struct panthor_vmshm_vm_bind_op *out = &req.ops[i];
		u32 type = op->flags & DRM_PANTHOR_VM_BIND_OP_TYPE_MASK;

		out->flags = op->flags;
		out->bo_offset = op->bo_offset;
		out->va = op->va;
		out->size = op->size;
		out->sync_start = sync_starts[i];
		out->sync_count = sync_counts[i];

		switch (type) {
		case DRM_PANTHOR_VM_BIND_OP_TYPE_MAP: {
			struct panthor_client_bo *bo =
				panthor_client_bo_lookup_locked(pcfile,
								op->bo_handle);

			if (!bo) {
				ret = -EINVAL;
				goto out_unlock;
			}
			if (!panthor_client_bo_get(bo)) {
				ret = -ENOENT;
				goto out_unlock;
			}

			out->client_bo_handle = bo->client_handle;
			map_bos[i] = bo;
			break;
		}

		case DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP:
			if (op->bo_handle || op->bo_offset) {
				ret = -EINVAL;
				goto out_unlock;
			}
			break;

		case DRM_PANTHOR_VM_BIND_OP_TYPE_SYNC_ONLY:
			if (!(args->flags & DRM_PANTHOR_VM_BIND_ASYNC) ||
			    op->bo_handle || op->bo_offset || op->va || op->size ||
			    !sync_counts[i]) {
				ret = -EINVAL;
				goto out_unlock;
			}
			break;

		default:
			ret = -EINVAL;
			goto out_unlock;
		}
	}

	for (i = 0; i < sync_count; i++) {
		struct panthor_client_syncobj *syncobj =
			xa_load(&pcfile->syncobjs, syncs[i].handle);

		if (!syncobj || !panthor_client_syncobj_get(syncobj)) {
			ret = -ENOENT;
			goto out_unlock;
		}

		syncobjs[i] = syncobj;
		req.syncs[i].flags = syncs[i].flags;
		req.syncs[i].client_syncobj_handle = syncobj->client_handle;
		req.syncs[i].timeline_value = syncs[i].timeline_value;
	}
	mutex_unlock(&pcfile->lock);

	ret = panthor_client_rpc_vm_bind(&req, &rsp);
	if (ret)
		goto out_put_refs;
	if (rsp.ret) {
		if (rsp.failed_op != PANTHOR_VMSHM_VM_BIND_FAILED_OP_NONE)
			args->ops.count = rsp.failed_op;
		ret = rsp.ret;
		goto out_put_refs;
	}
	if (rsp.op_count != op_count) {
		ret = -EPROTO;
		goto out_put_refs;
	}

	pr_info_ratelimited("panthor-client: VM_BIND session=%llu client_vm=%u proxy_vm=%u ops=%u syncs=%u flags=0x%x\n",
			    pcfile->session_id, args->vm_id, rsp.proxy_vm_id,
			    args->ops.count, sync_count, args->flags);
	ret = 0;
	goto out_put_refs;

out_unlock:
	mutex_unlock(&pcfile->lock);

out_put_refs:
	for (i = 0; i < sync_count; i++)
		panthor_client_syncobj_put(syncobjs[i]);
	for (i = 0; i < op_count; i++)
		panthor_client_bo_put(map_bos[i]);

	return ret;
}

static int panthor_client_bo_create(struct panthor_client_file *pcfile,
				    struct drm_panthor_bo_create *args)
{
	struct panthor_vmshm_bo_create_req req = { 0 };
	struct panthor_vmshm_bo_create_rsp rsp;
	struct client_vmshm_lookup_params lookup = {
		.lookup = VMSHM_MANAGER_LOOKUP_HANDLE,
		.requester_vmid = PANTHOR_VMSHM_POC_CLIENT_VMID,
		.required_perms = PROXY_VMSHM_PERM_MMAP |
				  PROXY_VMSHM_PERM_CPU_READ |
				  PROXY_VMSHM_PERM_CPU_WRITE,
	};
	struct panthor_client_bo *bo;
	int ret;

	if (!pcfile)
		return -EINVAL;
	if (!args->size || args->pad)
		return -EINVAL;
	if (args->flags & ~DRM_PANTHOR_BO_NO_MMAP)
		return -EINVAL;

	req.session_id = pcfile->session_id;
	req.size = args->size;
	req.flags = args->flags;
	req.client_exclusive_vm_id = args->exclusive_vm_id;

	ret = panthor_client_rpc_bo_create(&req, &rsp);
	if (ret)
		return ret;
	if (rsp.ret)
		return rsp.ret;
	if (!rsp.client_bo_handle || !rsp.proxy_bo_handle || !rsp.size ||
	    !rsp.payload_handle || !rsp.payload_size)
		return -EPROTO;

	bo = kzalloc(sizeof(*bo), GFP_KERNEL);
	if (!bo) {
		panthor_client_bo_destroy_rpc(pcfile, rsp.client_bo_handle, NULL);
		return -ENOMEM;
	}

	lookup.handle = rsp.payload_handle;
	ret = client_vmshm_manager_get(&lookup, &bo->payload_obj);
	if (ret) {
		kfree(bo);
		panthor_client_bo_destroy_rpc(pcfile, rsp.client_bo_handle, NULL);
		return ret;
	}

	bo->session_id = pcfile->session_id;
	bo->client_handle = rsp.client_bo_handle;
	bo->proxy_handle = rsp.proxy_bo_handle;
	bo->flags = args->flags;
	bo->size = rsp.size;
	bo->payload_handle = rsp.payload_handle;
	bo->payload_offset = rsp.payload_offset;
	bo->payload_size = rsp.payload_size;
	bo->payload_desc = *client_vmshm_object_desc(bo->payload_obj);
	refcount_set(&bo->refcnt, 1);

	if (bo->payload_desc.handle != rsp.payload_handle ||
	    bo->payload_desc.alloc_size < rsp.size ||
	    bo->payload_desc.offset != rsp.payload_offset) {
		client_vmshm_manager_put(bo->payload_obj);
		kfree(bo);
		panthor_client_bo_destroy_rpc(pcfile, rsp.client_bo_handle, NULL);
		return -EPROTO;
	}

	mutex_lock(&pcfile->lock);
	ret = xa_insert(&pcfile->bos, rsp.client_bo_handle, bo, GFP_KERNEL);
	mutex_unlock(&pcfile->lock);
	if (ret) {
		client_vmshm_manager_put(bo->payload_obj);
		kfree(bo);
		panthor_client_bo_destroy_rpc(pcfile, rsp.client_bo_handle, NULL);
		return ret;
	}

	args->size = rsp.size;
	args->handle = rsp.client_bo_handle;

	pr_info_ratelimited("panthor-client: BO_CREATE session=%llu client_bo=%u proxy_bo=%u size=0x%llx payload=0x%llx payload_size=0x%llx\n",
			    pcfile->session_id, rsp.client_bo_handle,
			    rsp.proxy_bo_handle, rsp.size, rsp.payload_handle,
			    rsp.payload_size);
	return 0;
}

static int panthor_client_bo_mmap_offset(struct panthor_client_file *pcfile,
					 struct drm_panthor_bo_mmap_offset *args)
{
	struct panthor_client_bo *bo;

	if (!pcfile)
		return -EINVAL;
	if (args->pad)
		return -EINVAL;

	mutex_lock(&pcfile->lock);
	bo = panthor_client_bo_lookup_locked(pcfile, args->handle);
	if (!bo) {
		mutex_unlock(&pcfile->lock);
		return -ENOENT;
	}
	if (bo->flags & DRM_PANTHOR_BO_NO_MMAP) {
		mutex_unlock(&pcfile->lock);
		return -EINVAL;
	}
	if (!bo->mmap_offset) {
		bo->mmap_offset =
			panthor_client_alloc_mmap_offset(pcfile, bo->size);
		if (!bo->mmap_offset) {
			mutex_unlock(&pcfile->lock);
			return -EOVERFLOW;
		}
	}

	args->offset = bo->mmap_offset;
	mutex_unlock(&pcfile->lock);

	pr_info("panthor-client: BO_MMAP_OFFSET session=%llu client_bo=%u offset=0x%llx size=0x%llx payload=0x%llx\n",
		pcfile->session_id, args->handle, args->offset, bo->size,
		bo->payload_handle);
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

static int panthor_client_ioctl_vm_bind(struct drm_device *ddev, void *data,
					struct drm_file *file)
{
	return panthor_client_vm_bind(file->driver_priv, data);
}

static int panthor_client_ioctl_vm_get_state(struct drm_device *ddev,
					     void *data,
					     struct drm_file *file)
{
	return panthor_client_vm_get_state(file->driver_priv, data);
}

static int panthor_client_ioctl_bo_create(struct drm_device *ddev, void *data,
					  struct drm_file *file)
{
	return panthor_client_bo_create(file->driver_priv, data);
}

static int panthor_client_ioctl_bo_mmap_offset(struct drm_device *ddev,
					       void *data,
					       struct drm_file *file)
{
	return panthor_client_bo_mmap_offset(file->driver_priv, data);
}

static int panthor_client_ioctl_group_create(struct drm_device *ddev, void *data,
					    struct drm_file *file)
{
	return panthor_client_group_create(file->driver_priv, data);
}

static int panthor_client_ioctl_group_destroy(struct drm_device *ddev,
					     void *data,
					     struct drm_file *file)
{
	struct drm_panthor_group_destroy *args = data;

	if (!args || args->pad)
		return -EINVAL;

	return panthor_client_group_destroy(file->driver_priv,
					    args->group_handle);
}

static int panthor_client_ioctl_group_get_state(struct drm_device *ddev,
					       void *data,
					       struct drm_file *file)
{
	return panthor_client_group_get_state(file->driver_priv, data);
}

static int panthor_client_ioctl_group_submit(struct drm_device *ddev,
					     void *data,
					     struct drm_file *file)
{
	return panthor_client_group_submit(file->driver_priv, data);
}

static int
panthor_client_ioctl_tiler_heap_create(struct drm_device *ddev, void *data,
				       struct drm_file *file)
{
	return panthor_client_tiler_heap_create(file->driver_priv, data);
}

static int
panthor_client_ioctl_tiler_heap_destroy(struct drm_device *ddev, void *data,
					struct drm_file *file)
{
	struct drm_panthor_tiler_heap_destroy *args = data;

	if (!args || args->pad)
		return -EINVAL;

	return panthor_client_tiler_heap_destroy(file->driver_priv, args->handle);
}

static const struct drm_ioctl_desc panthor_client_ioctls[] = {
	DRM_IOCTL_DEF_DRV(PANTHOR_DEV_QUERY, panthor_client_ioctl_dev_query,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(PANTHOR_VM_CREATE, panthor_client_ioctl_vm_create,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(PANTHOR_VM_DESTROY, panthor_client_ioctl_vm_destroy,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(PANTHOR_VM_BIND, panthor_client_ioctl_vm_bind,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(PANTHOR_VM_GET_STATE,
			  panthor_client_ioctl_vm_get_state,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(PANTHOR_BO_CREATE, panthor_client_ioctl_bo_create,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(PANTHOR_BO_MMAP_OFFSET,
			  panthor_client_ioctl_bo_mmap_offset,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(PANTHOR_GROUP_CREATE,
			  panthor_client_ioctl_group_create,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(PANTHOR_GROUP_DESTROY,
			  panthor_client_ioctl_group_destroy,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(PANTHOR_GROUP_GET_STATE,
			  panthor_client_ioctl_group_get_state,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(PANTHOR_GROUP_SUBMIT,
			  panthor_client_ioctl_group_submit,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(PANTHOR_TILER_HEAP_CREATE,
			  panthor_client_ioctl_tiler_heap_create,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(PANTHOR_TILER_HEAP_DESTROY,
			  panthor_client_ioctl_tiler_heap_destroy,
			  DRM_RENDER_ALLOW),
};

static int panthor_client_mmap_flush_id(struct vm_area_struct *vma)
{
	unsigned long map_size = vma->vm_end - vma->vm_start;
	int ret;

	if (!panthor_client || !panthor_client->flush_id_page)
		return -ENODEV;
	if ((vma->vm_flags & VM_SHARED) == 0)
		return -EINVAL;
	if (map_size != PAGE_SIZE || (vma->vm_flags & (VM_WRITE | VM_EXEC)))
		return -EINVAL;

	vm_flags_clear(vma, VM_MAYWRITE);
	vm_flags_set(vma, VM_DONTCOPY | VM_DONTEXPAND | VM_DONTDUMP);

	ret = vm_insert_page(vma, vma->vm_start,
			     panthor_client->flush_id_page);
	if (ret)
		return ret;

	pr_info("panthor-client: MMAP_FLUSH_ID offset=0x%llx size=0x%lx value=0x%08x\n",
		(unsigned long long)DRM_PANTHOR_USER_FLUSH_ID_MMIO_OFFSET,
		map_size, *(u32 *)page_address(panthor_client->flush_id_page));
	return 0;
}

static long panthor_client_ioctl(struct file *filp, unsigned int cmd,
				 unsigned long arg)
{
	struct drm_file *file = filp->private_data;
	struct panthor_client_file *pcfile = file ? file->driver_priv : NULL;

	if (cmd == DRM_IOCTL_GET_CAP) {
		struct drm_get_cap cap;

		if (copy_from_user(&cap, (void __user *)arg, sizeof(cap)))
			return -EFAULT;
		if (cap.capability == DRM_CAP_PRIME) {
			cap.value = 0;
			if (copy_to_user((void __user *)arg, &cap, sizeof(cap)))
				return -EFAULT;
			return 0;
		}
		return drm_ioctl(filp, cmd, arg);
	}
	if (cmd == DRM_IOCTL_PRIME_HANDLE_TO_FD ||
	    cmd == DRM_IOCTL_PRIME_FD_TO_HANDLE ||
	    cmd == DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD ||
	    cmd == DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE ||
	    cmd == DRM_IOCTL_SYNCOBJ_EVENTFD)
		return -EOPNOTSUPP;
	if (cmd == DRM_IOCTL_GEM_CLOSE) {
		struct drm_gem_close close;

		if (copy_from_user(&close, (void __user *)arg, sizeof(close)))
			return -EFAULT;
		return panthor_client_bo_destroy(pcfile, close.handle);
	}
	if (cmd == DRM_IOCTL_SYNCOBJ_CREATE) {
		struct drm_syncobj_create create;
		int ret;

		if (copy_from_user(&create, (void __user *)arg, sizeof(create)))
			return -EFAULT;
		ret = panthor_client_syncobj_create(pcfile, &create);
		if (ret)
			return ret;
		if (copy_to_user((void __user *)arg, &create, sizeof(create)))
			return -EFAULT;
		return 0;
	}
	if (cmd == DRM_IOCTL_SYNCOBJ_DESTROY) {
		struct drm_syncobj_destroy destroy;

		if (copy_from_user(&destroy, (void __user *)arg,
				   sizeof(destroy)))
			return -EFAULT;
		return panthor_client_syncobj_destroy(pcfile, &destroy);
	}
	if (cmd == DRM_IOCTL_SYNCOBJ_WAIT ||
	    cmd == PANTHOR_CLIENT_IOCTL_SYNCOBJ_WAIT_LEGACY) {
		struct drm_syncobj_wait wait;
		int ret;

		if (cmd == PANTHOR_CLIENT_IOCTL_SYNCOBJ_WAIT_LEGACY) {
			struct panthor_client_syncobj_wait_legacy legacy;

			if (copy_from_user(&legacy, (void __user *)arg,
					   sizeof(legacy)))
				return -EFAULT;

			memset(&wait, 0, sizeof(wait));
			wait.handles = legacy.handles;
			wait.timeout_nsec = legacy.timeout_nsec;
			wait.count_handles = legacy.count_handles;
			wait.flags = legacy.flags;
			wait.first_signaled = legacy.first_signaled;
			wait.pad = legacy.pad;
		} else if (copy_from_user(&wait, (void __user *)arg, sizeof(wait))) {
			return -EFAULT;
		}

		ret = panthor_client_syncobj_wait(pcfile, &wait);
		if (ret)
			return ret;
		if (cmd == PANTHOR_CLIENT_IOCTL_SYNCOBJ_WAIT_LEGACY) {
			struct panthor_client_syncobj_wait_legacy legacy = {
				.handles = wait.handles,
				.timeout_nsec = wait.timeout_nsec,
				.count_handles = wait.count_handles,
				.flags = wait.flags,
				.first_signaled = wait.first_signaled,
				.pad = wait.pad,
			};

			if (copy_to_user((void __user *)arg, &legacy,
					 sizeof(legacy)))
				return -EFAULT;
		} else if (copy_to_user((void __user *)arg, &wait, sizeof(wait))) {
			return -EFAULT;
		}

		return 0;
	}
	if (cmd == DRM_IOCTL_SYNCOBJ_TRANSFER) {
		struct drm_syncobj_transfer transfer;

		if (copy_from_user(&transfer, (void __user *)arg,
				   sizeof(transfer)))
			return -EFAULT;
		return panthor_client_syncobj_transfer(pcfile, &transfer);
	}
	if (cmd == DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT ||
	    cmd == PANTHOR_CLIENT_IOCTL_SYNCOBJ_TIMELINE_WAIT_LEGACY) {
		struct drm_syncobj_timeline_wait wait;
		int ret;

		if (cmd == PANTHOR_CLIENT_IOCTL_SYNCOBJ_TIMELINE_WAIT_LEGACY) {
			struct panthor_client_syncobj_timeline_wait_legacy legacy;

			if (copy_from_user(&legacy, (void __user *)arg,
					   sizeof(legacy)))
				return -EFAULT;

			memset(&wait, 0, sizeof(wait));
			wait.handles = legacy.handles;
			wait.points = legacy.points;
			wait.timeout_nsec = legacy.timeout_nsec;
			wait.count_handles = legacy.count_handles;
			wait.flags = legacy.flags;
			wait.first_signaled = legacy.first_signaled;
			wait.pad = legacy.pad;
		} else if (copy_from_user(&wait, (void __user *)arg,
					  sizeof(wait))) {
			return -EFAULT;
		}

		ret = panthor_client_syncobj_timeline_wait(pcfile, &wait);
		if (ret)
			return ret;
		if (cmd == PANTHOR_CLIENT_IOCTL_SYNCOBJ_TIMELINE_WAIT_LEGACY) {
			struct panthor_client_syncobj_timeline_wait_legacy legacy = {
				.handles = wait.handles,
				.points = wait.points,
				.timeout_nsec = wait.timeout_nsec,
				.count_handles = wait.count_handles,
				.flags = wait.flags,
				.first_signaled = wait.first_signaled,
				.pad = wait.pad,
			};

			if (copy_to_user((void __user *)arg, &legacy,
					 sizeof(legacy)))
				return -EFAULT;
		} else if (copy_to_user((void __user *)arg, &wait,
					sizeof(wait))) {
			return -EFAULT;
		}

		return 0;
	}
	if (cmd == DRM_IOCTL_SYNCOBJ_RESET) {
		struct drm_syncobj_array array;

		if (copy_from_user(&array, (void __user *)arg, sizeof(array)))
			return -EFAULT;
		return panthor_client_syncobj_array_op(
			pcfile, &array, PANTHOR_VMSHM_MSG_SYNCOBJ_RESET_REQ,
			PANTHOR_VMSHM_MSG_SYNCOBJ_RESET_RSP,
			"SYNCOBJ_RESET");
	}
	if (cmd == DRM_IOCTL_SYNCOBJ_SIGNAL) {
		struct drm_syncobj_array array;

		if (copy_from_user(&array, (void __user *)arg, sizeof(array)))
			return -EFAULT;
		return panthor_client_syncobj_array_op(
			pcfile, &array, PANTHOR_VMSHM_MSG_SYNCOBJ_SIGNAL_REQ,
			PANTHOR_VMSHM_MSG_SYNCOBJ_SIGNAL_RSP,
			"SYNCOBJ_SIGNAL");
	}
	if (cmd == DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL) {
		struct drm_syncobj_timeline_array array;

		if (copy_from_user(&array, (void __user *)arg, sizeof(array)))
			return -EFAULT;
		return panthor_client_syncobj_timeline_array_op(
			pcfile, &array,
			PANTHOR_VMSHM_MSG_SYNCOBJ_TIMELINE_SIGNAL_REQ,
			PANTHOR_VMSHM_MSG_SYNCOBJ_TIMELINE_SIGNAL_RSP,
			"SYNCOBJ_TIMELINE_SIGNAL", false);
	}
	if (cmd == DRM_IOCTL_SYNCOBJ_QUERY) {
		struct drm_syncobj_timeline_array array;
		int ret;

		if (copy_from_user(&array, (void __user *)arg, sizeof(array)))
			return -EFAULT;
		ret = panthor_client_syncobj_timeline_array_op(
			pcfile, &array, PANTHOR_VMSHM_MSG_SYNCOBJ_QUERY_REQ,
			PANTHOR_VMSHM_MSG_SYNCOBJ_QUERY_RSP,
			"SYNCOBJ_QUERY", true);
		if (ret)
			return ret;
		if (copy_to_user((void __user *)arg, &array, sizeof(array)))
			return -EFAULT;
		return 0;
	}

	return drm_ioctl(filp, cmd, arg);
}

#ifdef CONFIG_COMPAT
static bool panthor_client_handles_core_ioctl(unsigned int cmd)
{
	switch (cmd) {
	case DRM_IOCTL_GET_CAP:
	case DRM_IOCTL_GEM_CLOSE:
	case DRM_IOCTL_PRIME_HANDLE_TO_FD:
	case DRM_IOCTL_PRIME_FD_TO_HANDLE:
	case DRM_IOCTL_SYNCOBJ_CREATE:
	case DRM_IOCTL_SYNCOBJ_DESTROY:
	case DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD:
	case DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE:
	case DRM_IOCTL_SYNCOBJ_WAIT:
	case PANTHOR_CLIENT_IOCTL_SYNCOBJ_WAIT_LEGACY:
	case DRM_IOCTL_SYNCOBJ_TRANSFER:
	case DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT:
	case PANTHOR_CLIENT_IOCTL_SYNCOBJ_TIMELINE_WAIT_LEGACY:
	case DRM_IOCTL_SYNCOBJ_EVENTFD:
	case DRM_IOCTL_SYNCOBJ_RESET:
	case DRM_IOCTL_SYNCOBJ_SIGNAL:
	case DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL:
	case DRM_IOCTL_SYNCOBJ_QUERY:
		return true;
	default:
		return false;
	}
}

static long panthor_client_compat_ioctl(struct file *filp, unsigned int cmd,
					unsigned long arg)
{
	if (panthor_client_handles_core_ioctl(cmd))
		return panthor_client_ioctl(filp, cmd, arg);

	return drm_compat_ioctl(filp, cmd, arg);
}
#else
#define panthor_client_compat_ioctl NULL
#endif

static int panthor_client_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct drm_file *file = filp->private_data;
	struct panthor_client_file *pcfile = file ? file->driver_priv : NULL;
	struct panthor_client_bo *bo;
	u64 offset = (u64)vma->vm_pgoff << PAGE_SHIFT;
	u64 bo_offset = 0;
	u64 map_size = vma->vm_end - vma->vm_start;
	phys_addr_t gpa;
	int ret;

	if (!pcfile)
		return -EINVAL;

#ifdef CONFIG_ARM64
	if (test_tsk_thread_flag(current, TIF_32BIT) &&
	    offset >= DRM_PANTHOR_USER_MMIO_OFFSET_32BIT) {
		offset += DRM_PANTHOR_USER_MMIO_OFFSET_64BIT -
			  DRM_PANTHOR_USER_MMIO_OFFSET_32BIT;
		vma->vm_pgoff = offset >> PAGE_SHIFT;
	}
#endif

	if (offset >= DRM_PANTHOR_USER_MMIO_OFFSET) {
		if (offset == DRM_PANTHOR_USER_FLUSH_ID_MMIO_OFFSET)
			return panthor_client_mmap_flush_id(vma);
		return -EOPNOTSUPP;
	}

	bo = panthor_client_bo_lookup_mmap_get(pcfile, offset, &bo_offset);
	if (!bo)
		return -EINVAL;

	if (bo->flags & DRM_PANTHOR_BO_NO_MMAP) {
		ret = -EINVAL;
		goto out_put_bo;
	}

	if (!map_size || bo_offset >= bo->size ||
	    map_size > PAGE_ALIGN(bo->size) - bo_offset ||
	    bo_offset > bo->payload_desc.alloc_size ||
	    map_size > bo->payload_desc.alloc_size - bo_offset) {
		ret = -EINVAL;
		goto out_put_bo;
	}

	gpa = client_vmshm_object_gpa(bo->payload_obj) + bo_offset;
	vm_flags_set(vma, VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);
	if (!panthor_client_bo_mmap_cached)
		vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
	vma->vm_private_data = bo;
	vma->vm_ops = &panthor_client_bo_vm_ops;

	ret = remap_pfn_range(vma, vma->vm_start, gpa >> PAGE_SHIFT,
			      map_size, vma->vm_page_prot);
	if (ret) {
		vma->vm_private_data = NULL;
		goto out_put_bo;
	}

	pr_info("panthor-client: MMAP session=%llu client_bo=%u offset=0x%llx bo_offset=0x%llx size=0x%llx payload_gpa=%pa attr=%s\n",
		pcfile->session_id, bo->client_handle, offset, bo_offset,
		map_size, &gpa,
		panthor_client_bo_mmap_cached ? "WB" : "WC");
	return 0;

out_put_bo:
	panthor_client_bo_put(bo);
	return ret;
}

static const struct file_operations panthor_client_fops = {
	.owner = THIS_MODULE,
	.open = drm_open,
	.release = drm_release,
	.unlocked_ioctl = panthor_client_ioctl,
	.compat_ioctl = panthor_client_compat_ioctl,
	.poll = drm_poll,
	.read = drm_read,
	.llseek = noop_llseek,
	.mmap = panthor_client_mmap,
	.fop_flags = FOP_UNSIGNED_OFFSET,
};

static int panthor_client_open(struct drm_device *ddev, struct drm_file *file)
{
	struct panthor_client_file *pcfile;
	int ret;

	pcfile = kzalloc(sizeof(*pcfile), GFP_KERNEL);
	if (!pcfile)
		return -ENOMEM;

	mutex_init(&pcfile->lock);
	xa_init(&pcfile->bos);
	xa_init(&pcfile->syncobjs);
	xa_init(&pcfile->groups);
	xa_init(&pcfile->heaps);
	pcfile->next_mmap_offset = PANTHOR_CLIENT_BO_MMAP_OFFSET_BASE;

	ret = panthor_client_rpc_open_session(&pcfile->session_id);
	if (ret) {
		xa_destroy(&pcfile->heaps);
		xa_destroy(&pcfile->groups);
		xa_destroy(&pcfile->syncobjs);
		xa_destroy(&pcfile->bos);
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

	panthor_client_groups_release_local(pcfile);
	panthor_client_heaps_release_local(pcfile);
	panthor_client_bos_release_local(pcfile);
	panthor_client_syncobjs_release_local(pcfile);

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

	pr_info("panthor-client: BO mmap cached=%d\n",
		panthor_client_bo_mmap_cached ? 1 : 0);

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
	panthor_client->flush_id_page = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!panthor_client->flush_id_page) {
		ret = -ENOMEM;
		goto err_release_devres;
	}

	ret = drm_dev_register(&panthor_client->drm, 0);
	if (ret)
		goto err_free_flush_id_page;

	ret = panthor_client_dev_query_selftest_run();
	if (ret)
		pr_warn("panthor-client: DEV_QUERY selftest failed (%d)\n", ret);

	ret = panthor_client_dev_query_perf_rtt();
	if (ret)
		pr_warn("panthor-client: DEV_QUERY perf selftest failed (%d)\n", ret);

	pr_info("panthor-client: registered DRM frontend\n");
	return 0;

err_free_flush_id_page:
	__free_page(panthor_client->flush_id_page);
	panthor_client->flush_id_page = NULL;
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
	__free_page(panthor_client->flush_id_page);
	panthor_client->flush_id_page = NULL;
	devres_release_group(&pdev->dev, NULL);
	platform_device_unregister(pdev);
	panthor_client = NULL;
}

module_init(panthor_client_init);
module_exit(panthor_client_exit);

MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL and additional rights");
