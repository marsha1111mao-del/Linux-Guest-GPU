// SPDX-License-Identifier: GPL-2.0 or MIT
/*
 * Panthor client VM DRM frontend.
 *
 * This frontend exposes the Panthor DEV_QUERY ioctl in the client VM and
 * forwards the actual query to the proxy VM over client_vmshm_comm.
 */

#include <linux/bits.h>
#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/dma-fence.h>
#include <linux/client_vmshm.h>
#include <linux/fdtable.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/ktime.h>
#include <linux/kstrtox.h>
#include <linux/limits.h>
#include <linux/math64.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/proxy_vmshm.h>
#include <linux/refcount.h>
#include <linux/slab.h>
#include <linux/sync_file.h>
#include <linux/uaccess.h>
#include <linux/vmshm_comm.h>
#include <linux/workqueue.h>
#include <linux/panthor_vmshm.h>
#include <linux/realm_xshm.h>
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
#define PANTHOR_CLIENT_SYNC_FILE_WAIT_NSEC	(60ULL * NSEC_PER_SEC)

struct panthor_client_device {
	struct drm_device drm;
	struct platform_device *platform;
	struct page *flush_id_page;
	struct workqueue_struct *sync_file_wq;
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

struct panthor_client_completion_page {
	refcount_t refs;
	struct client_vmshm_object *object;
	const struct panthor_vmshm_completion_page *kva;
	/* Private local state, never stored in the shared completion page. */
	spinlock_t retire_lock;
	u64 session_id;
	u64 retire_queued, retire_sent, retire_fallback;
	u32 retired[PANTHOR_VMSHM_RETIRE_MAX];
	u32 retire_count;
	bool retire_enabled;
	bool retire_closing;
	bool timing_enabled;
	spinlock_t observation_lock;
	u64 observed_cookies[PANTHOR_VMSHM_COMPLETION_SLOTS];
};

struct panthor_client_syncobj {
	/* Set only for immutable private snapshot wrappers. */
	struct panthor_client_completion_page *completion_page;
	u64 completion_cookie;
	u32 completion_slot;
	/* Public handles are local. A wire handle is published only once. */
	struct mutex state_lock;
	u32 create_flags;
	/* Only an immutable private snapshot may back an unmaterialized object.
	 * It has no file reference, so a same-file import cannot form a cycle.
	 */
	struct panthor_client_syncobj *snapshot;
	struct panthor_client_syncobj *export_snapshot;
	u64 capture_epoch;
	/* Complete native timeline chain proof, protected by file->lock.
	 * With no intervening destination write, a complete chain at P also
	 * completes every positive earlier point. Never a last-job watermark.
	 */
	u64 proof_point;
	u64 proof_cookie;
	u64 proof_epoch;
	u32 proof_slot;
	/* RPC success cache remains exact-point only under sync_epoch. */
	u64 completed_point;
	u64 completed_epoch;
	bool completed_valid;
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

struct panthor_client_plan {
	u64 id;
	struct panthor_vmshm_plan_register_req shape;
};

struct panthor_client_file {
	struct panthor_client_completion_page *completion_page;
	u64 session_id;
	u64 feature_bits;
	struct mutex plan_lock;
	struct panthor_client_plan plans[PANTHOR_VMSHM_PLAN_SLOTS];
	u32 next_plan_slot;
	u64 plan_hits;
	u64 plan_misses;
	struct mutex lock;
	struct xarray bos;
	struct xarray syncobjs;
	struct xarray groups;
	struct xarray heaps;
	u64 next_mmap_offset;
	/* All possible fence mutations invalidate the file-wide generation. */
	u64 sync_epoch;
	/* Unknown writes invalidate all public proofs; known writes clear targets. */
	u64 proof_epoch;
	u32 sync_mutations;
	bool sync_epoch_wrapped;
};

struct panthor_client_sync_fence {
	struct dma_fence base;
	struct work_struct work;
	struct work_struct cleanup_work;
	bool lazy_wait;
	spinlock_t lock;
	u64 session_id;
	struct panthor_client_syncobj *snapshot;
	struct file *session_file;
	struct mutex snapshot_lock;
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
static unsigned int panthor_client_sync_file_poll_us = 1000;
static bool panthor_client_fused_snapshot;
module_param_named(fused_snapshot, panthor_client_fused_snapshot, bool, 0444);
MODULE_PARM_DESC(fused_snapshot,
		 "Use negotiated single-RPC sync-file snapshot creation (experimental)");
static bool panthor_client_transfer_capture;
static bool panthor_client_completion_trace;
module_param_named(completion_trace, panthor_client_completion_trace, bool, 0444);
MODULE_PARM_DESC(completion_trace, "Record the first physical-counter observation of each completion cookie");
static atomic64_t completion_first_seen = ATOMIC64_INIT(0);
static atomic64_t completion_unsampled = ATOMIC64_INIT(0);
static atomic64_t completion_samples = ATOMIC64_INIT(0);
static atomic64_t completion_metadata_errors = ATOMIC64_INIT(0);
static atomic64_t completion_rpc_successes = ATOMIC64_INIT(0);
static atomic64_t completion_rpc_without_proof = ATOMIC64_INIT(0);
module_param_named(transfer_capture, panthor_client_transfer_capture, bool, 0444);
MODULE_PARM_DESC(transfer_capture, "Capture reusable sync-file snapshots during binary transfers");
static atomic64_t panthor_client_capture_calls = ATOMIC64_INIT(0);
static atomic64_t panthor_client_capture_records = ATOMIC64_INIT(0);
static atomic64_t panthor_client_capture_hits = ATOMIC64_INIT(0);
static atomic64_t panthor_client_capture_discarded = ATOMIC64_INIT(0);
static atomic64_t panthor_client_transfer_binary = ATOMIC64_INIT(0);
static atomic64_t panthor_client_transfer_timeline = ATOMIC64_INIT(0);
static bool panthor_client_completed_snapshot;
module_param_named(completed_snapshot, panthor_client_completed_snapshot, bool, 0444);
MODULE_PARM_DESC(completed_snapshot,
		 "Elide proxy snapshots of verified successful fences (experimental)");
static bool panthor_client_lazy_snapshot_wait;
module_param_named(lazy_snapshot_wait, panthor_client_lazy_snapshot_wait, bool, 0444);
MODULE_PARM_DESC(lazy_snapshot_wait,
                 "Start snapshot polling only when local signaling is requested (experimental)");
static atomic64_t panthor_client_lazy_discards = ATOMIC64_INIT(0);
static atomic64_t panthor_client_completed_exports = ATOMIC64_INIT(0);
static atomic64_t panthor_client_sync_file_waits = ATOMIC64_INIT(0);
static atomic64_t panthor_client_sync_file_polls = ATOMIC64_INIT(0);
static atomic64_t panthor_client_sync_file_wait_ns = ATOMIC64_INIT(0);
static atomic64_t panthor_client_sync_file_errors = ATOMIC64_INIT(0);
/* Classify actual timeline RPCs before choosing a cache/notification design. */
static atomic64_t panthor_client_timeline_wait_flags[16];
static atomic64_t panthor_client_timeline_wait_single_nonzero[16];
static atomic64_t panthor_client_timeline_wait_nonblock = ATOMIC64_INIT(0);
static atomic64_t panthor_client_timeline_wait_success = ATOMIC64_INIT(0);
static atomic64_t panthor_client_timeline_wait_pending = ATOMIC64_INIT(0);
static bool panthor_client_completed_wait_cache;
module_param_named(completed_wait_cache, panthor_client_completed_wait_cache, bool, 0444);
MODULE_PARM_DESC(completed_wait_cache,
                 "Reuse confirmed exact-point nonblocking waits between mutations (experimental)");
static atomic64_t panthor_client_wait_cache_hits = ATOMIC64_INIT(0);
static atomic64_t panthor_client_wait_cache_candidates = ATOMIC64_INIT(0);
static atomic64_t panthor_client_wait_cache_records = ATOMIC64_INIT(0);
static atomic64_t panthor_client_wait_cache_mutations = ATOMIC64_INIT(0);
static atomic64_t panthor_client_wait_cache_miss_empty = ATOMIC64_INIT(0);
static atomic64_t panthor_client_wait_cache_miss_point = ATOMIC64_INIT(0);
static atomic64_t panthor_client_wait_cache_miss_epoch = ATOMIC64_INIT(0);
static atomic64_t panthor_client_wait_cache_miss_blocked = ATOMIC64_INIT(0);
static atomic64_t panthor_client_lazy_creates = ATOMIC64_INIT(0);
static atomic64_t panthor_client_materialized_syncobjs = ATOMIC64_INIT(0);
static atomic64_t panthor_client_snapshot_imports = ATOMIC64_INIT(0);
static atomic64_t panthor_client_snapshot_waits = ATOMIC64_INIT(0);
static atomic64_t panthor_client_point_records = ATOMIC64_INIT(0);
static atomic64_t panthor_client_point_checks = ATOMIC64_INIT(0);
static atomic64_t panthor_client_point_hits = ATOMIC64_INIT(0);
static atomic64_t panthor_client_point_prefix_hits = ATOMIC64_INIT(0);
static atomic64_t panthor_client_completion_pages = ATOMIC64_INIT(0);
static atomic64_t panthor_client_completion_checks = ATOMIC64_INIT(0);
static atomic64_t panthor_client_completion_hits = ATOMIC64_INIT(0);
static atomic64_t panthor_client_completion_fallbacks = ATOMIC64_INIT(0);


static int panthor_client_sync_file_poll_set(const char *value,
					  const struct kernel_param *kp)
{
	unsigned int poll_us;
	int ret = kstrtouint(value, 0, &poll_us);

	if (ret)
		return ret;
	if (poll_us < 50 || poll_us > 10000)
		return -ERANGE;
	*(unsigned int *)kp->arg = poll_us;
	return 0;
}

static const struct kernel_param_ops panthor_client_sync_file_poll_ops = {
	.set = panthor_client_sync_file_poll_set,
	.get = param_get_uint,
};
module_param_cb(sync_file_poll_us, &panthor_client_sync_file_poll_ops,
		&panthor_client_sync_file_poll_us, 0444);
MODULE_PARM_DESC(sync_file_poll_us,
		 "Minimum sleep between pending sync-file snapshot checks, 50..10000 us");

static int panthor_client_sync_file_stats_get(char *buffer,
					    const struct kernel_param *kp)
{
	int len, i;

	len = scnprintf(buffer, PAGE_SIZE,
			 "capture_calls=%lld capture_records=%lld capture_hits=%lld capture_discarded=%lld transfer_binary=%lld transfer_timeline=%lld ",
			 atomic64_read(&panthor_client_capture_calls),
			 atomic64_read(&panthor_client_capture_records),
			 atomic64_read(&panthor_client_capture_hits),
			 atomic64_read(&panthor_client_capture_discarded),
			 atomic64_read(&panthor_client_transfer_binary),
			 atomic64_read(&panthor_client_transfer_timeline));
	len += scnprintf(buffer + len, PAGE_SIZE - len,
			 "poll_us=%u waits=%lld polls=%lld total_wait_ns=%lld errors=%lld completed_exports=%lld lazy_discards=%lld",
			 panthor_client_sync_file_poll_us,
			 atomic64_read(&panthor_client_sync_file_waits),
			 atomic64_read(&panthor_client_sync_file_polls),
			 atomic64_read(&panthor_client_sync_file_wait_ns),
			 atomic64_read(&panthor_client_sync_file_errors),
			 atomic64_read(&panthor_client_completed_exports),
			 atomic64_read(&panthor_client_lazy_discards));
	for (i = 0; i < ARRAY_SIZE(panthor_client_timeline_wait_flags); i++)
		len += scnprintf(buffer + len, PAGE_SIZE - len,
				 " timeline_flags_%x=%lld/%lld", i,
				 atomic64_read(&panthor_client_timeline_wait_flags[i]),
				 atomic64_read(&panthor_client_timeline_wait_single_nonzero[i]));
	len += scnprintf(buffer + len, PAGE_SIZE - len,
			 " timeline_nonblock=%lld timeline_success=%lld timeline_pending=%lld cache_hits=%lld cache_candidates=%lld cache_records=%lld cache_mutations=%lld cache_miss_empty=%lld cache_miss_point=%lld cache_miss_epoch=%lld cache_miss_blocked=%lld ",
			 atomic64_read(&panthor_client_timeline_wait_nonblock),
			 atomic64_read(&panthor_client_timeline_wait_success),
			 atomic64_read(&panthor_client_timeline_wait_pending),
			 atomic64_read(&panthor_client_wait_cache_hits),
			 atomic64_read(&panthor_client_wait_cache_candidates),
			 atomic64_read(&panthor_client_wait_cache_records),
			 atomic64_read(&panthor_client_wait_cache_mutations),
			 atomic64_read(&panthor_client_wait_cache_miss_empty),
			 atomic64_read(&panthor_client_wait_cache_miss_point),
			 atomic64_read(&panthor_client_wait_cache_miss_epoch),
			 atomic64_read(&panthor_client_wait_cache_miss_blocked));
	len += scnprintf(buffer + len, PAGE_SIZE - len,
			 "lazy_creates=%lld materialized=%lld snapshot_imports=%lld snapshot_waits=%lld completion_pages=%lld completion_checks=%lld completion_hits=%lld completion_fallbacks=%lld\n",
			 atomic64_read(&panthor_client_lazy_creates),
			 atomic64_read(&panthor_client_materialized_syncobjs),
			 atomic64_read(&panthor_client_snapshot_imports),
			 atomic64_read(&panthor_client_snapshot_waits),
			 atomic64_read(&panthor_client_completion_pages),
			 atomic64_read(&panthor_client_completion_checks),
			 atomic64_read(&panthor_client_completion_hits),
			 atomic64_read(&panthor_client_completion_fallbacks));
	len += scnprintf(buffer + len, PAGE_SIZE - len,
			 "point_records=%lld point_checks=%lld point_hits=%lld point_prefix_hits=%lld\n",
			 atomic64_read(&panthor_client_point_records),
			 atomic64_read(&panthor_client_point_checks),
			 atomic64_read(&panthor_client_point_hits),
			 atomic64_read(&panthor_client_point_prefix_hits));
	len += scnprintf(buffer + len, PAGE_SIZE - len,
			 "first_seen=%lld unsampled=%lld completion_samples=%lld metadata_errors=%lld rpc_successes=%lld rpc_without_proof=%lld\n",
			 atomic64_read(&completion_first_seen), atomic64_read(&completion_unsampled),
			 atomic64_read(&completion_samples), atomic64_read(&completion_metadata_errors),
			 atomic64_read(&completion_rpc_successes), atomic64_read(&completion_rpc_without_proof));
	return len;
}

static const struct kernel_param_ops panthor_client_sync_file_stats_ops = {
	.get = panthor_client_sync_file_stats_get,
};
module_param_cb(sync_file_wait_stats, &panthor_client_sync_file_stats_ops, NULL, 0444);
MODULE_PARM_DESC(sync_file_wait_stats,
		 "Cumulative snapshot worker wait counters; excludes time queued before worker starts");

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

/* Called only after the last Client reference to a private snapshot is gone.
 * Its already refcounted completion-page wrapper pins this bounded queue.
 */
static bool panthor_client_snapshot_retire(struct panthor_client_syncobj *syncobj)
{
	struct panthor_client_completion_page *page = syncobj->completion_page;
	unsigned long flags;
	bool queued = false;

	if (!page || !page->retire_enabled || page->session_id != syncobj->session_id)
		return false;
	spin_lock_irqsave(&page->retire_lock, flags);
	if (!page->retire_closing && page->retire_count < PANTHOR_VMSHM_RETIRE_MAX) {
		page->retired[page->retire_count++] = syncobj->client_handle;
		page->retire_queued++;
		queued = true;
	} else {
		page->retire_fallback++;
	}
	spin_unlock_irqrestore(&page->retire_lock, flags);
	return queued;
}

/* A batch is consumed once, including on an ambiguous RPC failure. Session
 * teardown owns any unreceived retirements; never retry a capture blindly.
 */
static void panthor_client_snapshot_retire_take(struct panthor_client_completion_page *page,
					      struct panthor_vmshm_snapshot_retire_req *req,
					      bool closing)
{
	unsigned long flags;

	spin_lock_irqsave(&page->retire_lock, flags);
	page->retire_closing |= closing;
	req->count = page->retire_count;
	memcpy(req->handles, page->retired, req->count * sizeof(req->handles[0]));
	page->retire_count = 0;
	if (!closing)
		page->retire_sent += req->count;
	spin_unlock_irqrestore(&page->retire_lock, flags);
}

static void panthor_client_snapshot_retire_close(struct panthor_client_completion_page *page)
{
	struct panthor_vmshm_snapshot_retire_req req = { 0 };
	unsigned long flags;
	u64 queued, sent, fallback;
	u32 i;
	int ret;

	if (!page || !page->retire_enabled)
		return;
	panthor_client_snapshot_retire_take(page, &req, true);
	for (i = 0; i < req.count; i++) {
		ret = panthor_client_syncobj_destroy_rpc_session(page->session_id,
							 req.handles[i], NULL);
		if (ret)
			pr_warn("panthor-client: retired snapshot cleanup failed session=%llu handle=%u ret=%d\n",
				page->session_id, req.handles[i], ret);
	}
	spin_lock_irqsave(&page->retire_lock, flags);
	queued = page->retire_queued;
	sent = page->retire_sent;
	fallback = page->retire_fallback;
	spin_unlock_irqrestore(&page->retire_lock, flags);
	pr_info("panthor-client: SNAPSHOT_RETIRE_STATS session=%llu queued=%llu sent=%llu fallback=%llu close=%u\n",
		page->session_id, queued, sent, fallback, req.count);
}

static void panthor_client_completion_page_put(struct panthor_client_completion_page *page)
{
	if (page && refcount_dec_and_test(&page->refs)) {
		client_vmshm_manager_put(page->object);
		kfree(page);
	}
}

/* Only an acquire-read matching the private expected cookie proves success.
 * The peer publishes that cookie after the exact native snapshot completes.
 */
static bool panthor_client_cookie_completed(struct panthor_client_completion_page *page,
					    u32 slot, u64 cookie, u32 path)
{
	const struct panthor_vmshm_completion_slot *record;
	u64 ticks[5];
	unsigned long flags;
	u32 kind;

	if (!page || !cookie || slot >= PANTHOR_VMSHM_COMPLETION_SLOTS)
		return false;
	record = &page->kva->slots[slot];
	if (!page->timing_enabled)
		return smp_load_acquire(&record->cookie) == cookie;
	spin_lock_irqsave(&page->observation_lock, flags);
	if (smp_load_acquire(&record->cookie) != cookie)
		goto not_completed;
	if (page->observed_cookies[slot] >= cookie) {
		spin_unlock_irqrestore(&page->observation_lock, flags);
		return true;
	}
	ticks[2] = realm_xshm_counter();
	ticks[0] = READ_ONCE(record->completed_counter);
	ticks[1] = READ_ONCE(record->published_counter);
	kind = READ_ONCE(record->observation_kind);
	ticks[4] = READ_ONCE(record->counter_frequency);
	smp_rmb();
	ticks[3] = realm_xshm_counter();
	if (smp_load_acquire(&record->cookie) != cookie)
		goto not_completed;
	page->observed_cookies[slot] = cookie;
	spin_unlock_irqrestore(&page->observation_lock, flags);
	atomic64_inc(&completion_first_seen);
	if (!kind) {
		atomic64_inc(&completion_unsampled);
		return true;
	}
	if ((kind != 1 && kind != 2) || !ticks[0] || ticks[1] < ticks[0] ||
	    ticks[2] < ticks[1] || !ticks[4]) {
		atomic64_inc(&completion_metadata_errors);
		return true;
	}
	atomic64_inc(&completion_samples);
	realm_xshm_trace_completion(page->session_id, cookie, path, kind, ticks);
	return true;
not_completed:
	spin_unlock_irqrestore(&page->observation_lock, flags);
	return false;
}

static bool panthor_client_snapshot_completed(struct panthor_client_syncobj *snapshot,
					       u32 path)
{
	if (!snapshot || !snapshot->completion_page || !snapshot->completion_cookie ||
	    snapshot->completion_slot >= PANTHOR_VMSHM_COMPLETION_SLOTS)
		return false;
	return panthor_client_cookie_completed(snapshot->completion_page,
		snapshot->completion_slot, snapshot->completion_cookie, path);
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

	if (syncobj->destroy_pending && syncobj->client_handle &&
	    !panthor_client_snapshot_retire(syncobj)) {
		ret = panthor_client_syncobj_destroy_rpc_session(
			syncobj->session_id, syncobj->client_handle,
			&proxy_handle);
		if (ret)
			pr_warn("panthor-client: delayed SYNCOBJ_DESTROY failed session=%llu client_sync=%u ret=%d\n",
				syncobj->session_id, syncobj->client_handle,
				ret);
		else
			pr_info_once("panthor-client: SYNCOBJ_DESTROY session=%llu client_sync=%u proxy_sync=%u\n",
				syncobj->session_id, syncobj->client_handle,
				proxy_handle);
	}

	panthor_client_syncobj_put(syncobj->snapshot);
	panthor_client_syncobj_put(syncobj->export_snapshot);
	panthor_client_completion_page_put(syncobj->completion_page);
	mutex_destroy(&syncobj->state_lock);
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

static int panthor_client_rpc_open_session(u64 *session_id, u64 *feature_bits)
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
	*feature_bits = rsp.feature_bits;
	pr_info("panthor-client: OPEN_SESSION session=%llu features=0x%llx fused_snapshot=%u\n",
		*session_id, *feature_bits,
		panthor_client_fused_snapshot &&
		!!(*feature_bits & PANTHOR_VMSHM_FEATURE_SYNCOBJ_SNAPSHOT));
	return 0;
}

static void panthor_client_completion_page_init(struct panthor_client_file *pcfile)
{
	struct panthor_vmshm_completion_page_req req = { .session_id = pcfile->session_id };
	struct panthor_vmshm_completion_page_rsp rsp = { 0 };
	struct client_vmshm_lookup_params lookup = {
		.lookup = VMSHM_MANAGER_LOOKUP_HANDLE,
		.requester_vmid = client_comm_vmshm_client_vmid(),
		.required_perms = PROXY_VMSHM_PERM_CPU_READ,
	};
	struct panthor_client_completion_page *page;
	const struct vmshm_manager_desc *desc;
	int ret;

	if (!(pcfile->feature_bits & PANTHOR_VMSHM_FEATURE_SNAPSHOT_COMPLETION))
		return;
	ret = panthor_client_rpc_call(PANTHOR_VMSHM_MSG_COMPLETION_PAGE_REQ,
		&req, sizeof(req), PANTHOR_VMSHM_MSG_COMPLETION_PAGE_RSP,
		&rsp, sizeof(rsp), sizeof(rsp), NULL);
	if (ret || rsp.ret || rsp.pad || !rsp.payload_handle ||
	    rsp.payload_size != sizeof(struct panthor_vmshm_completion_page))
		return;
	page = kzalloc(sizeof(*page), GFP_KERNEL);
	if (!page)
		return;
	lookup.handle = rsp.payload_handle;
	ret = client_vmshm_manager_get(&lookup, &page->object);
	if (ret) {
		kfree(page);
		return;
	}
	desc = client_vmshm_object_desc(page->object);
	page->kva = client_vmshm_object_kva(page->object);
	if (!desc || !page->kva || desc->handle != rsp.payload_handle ||
	    desc->offset != rsp.payload_offset || desc->size != rsp.payload_size ||
	    desc->type != PROXY_VMSHM_OBJ_FENCE_PAGE ||
	    desc->perms != PROXY_VMSHM_PERM_CPU_READ ||
	    !(desc->flags & VMSHM_MANAGER_DESC_F_CONTIG) ||
	    !IS_ALIGNED((unsigned long)page->kva, sizeof(u64))) {
		client_vmshm_manager_put(page->object);
		kfree(page);
		return;
	}
	refcount_set(&page->refs, 1);
	spin_lock_init(&page->retire_lock);
	spin_lock_init(&page->observation_lock);
	page->session_id = pcfile->session_id;
	page->timing_enabled = panthor_client_completion_trace && realm_xshm_trace_enabled() &&
		(pcfile->feature_bits & PANTHOR_VMSHM_FEATURE_COMPLETION_TIMING);
	page->retire_enabled = panthor_client_fused_snapshot &&
		(pcfile->feature_bits & PANTHOR_VMSHM_FEATURE_SNAPSHOT_RETIRE);
	pcfile->completion_page = page;
	atomic64_inc(&panthor_client_completion_pages);
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

static struct panthor_client_syncobj *panthor_client_syncobj_alloc(u64 session_id)
{
	struct panthor_client_syncobj *syncobj = kzalloc(sizeof(*syncobj), GFP_KERNEL);

	if (syncobj) {
		syncobj->session_id = session_id;
		mutex_init(&syncobj->state_lock);
		refcount_set(&syncobj->refcnt, 1);
	}
	return syncobj;
}

/* Materialization is irreversible. Waiters may retain an older immutable
 * snapshot while a later reset/import starts using this new mutable object.
 * Callers own a reference and never hold pcfile->lock across these RPCs.
 */
static int panthor_client_syncobj_materialize(struct panthor_client_syncobj *syncobj)
{
	struct panthor_vmshm_syncobj_create_req req = {
		.session_id = syncobj->session_id,
		.flags = syncobj->create_flags,
	};
	struct panthor_vmshm_syncobj_create_rsp rsp;
	struct panthor_vmshm_syncobj_transfer_req transfer = {
		.session_id = syncobj->session_id,
	};
	struct panthor_vmshm_syncobj_transfer_rsp transferred;
	struct panthor_client_syncobj *snapshot = NULL;
	int ret = 0;

	mutex_lock(&syncobj->state_lock);
	if (syncobj->client_handle)
		goto out_unlock;
	ret = panthor_client_rpc_syncobj_create(&req, &rsp);
	if (ret)
		goto out_unlock;
	if (rsp.ret) {
		ret = rsp.ret;
		goto out_unlock;
	}
	if (!rsp.client_syncobj_handle || !rsp.proxy_syncobj_handle) {
		ret = -EPROTO;
		goto out_unlock;
	}
	if (syncobj->snapshot) {
		transfer.src_handle = syncobj->snapshot->client_handle;
		transfer.dst_handle = rsp.client_syncobj_handle;
		ret = panthor_client_rpc_syncobj_transfer(&transfer, &transferred);
		if (!ret)
			ret = transferred.ret;
		if (ret) {
			panthor_client_syncobj_destroy_rpc_session(syncobj->session_id,
							 rsp.client_syncobj_handle, NULL);
			goto out_unlock;
		}
	}
	/* Publish only a complete object. Failure leaves the old snapshot live. */
	syncobj->proxy_handle = rsp.proxy_syncobj_handle;
	syncobj->client_handle = rsp.client_syncobj_handle;
	atomic64_inc(&panthor_client_materialized_syncobjs);
	snapshot = syncobj->snapshot;
	syncobj->snapshot = NULL;
out_unlock:
	mutex_unlock(&syncobj->state_lock);
	panthor_client_syncobj_put(snapshot);
	return ret;
}

static int panthor_client_syncobj_wait_handle(struct panthor_client_syncobj *syncobj,
					    u32 *handle,
					    struct panthor_client_syncobj **snapshot)
{
	int ret;

	mutex_lock(&syncobj->state_lock);
	if (syncobj->snapshot && panthor_client_syncobj_get(syncobj->snapshot)) {
		*snapshot = syncobj->snapshot;
		*handle = (*snapshot)->client_handle;
		atomic64_inc(&panthor_client_snapshot_waits);
		mutex_unlock(&syncobj->state_lock);
		return 0;
	}
	mutex_unlock(&syncobj->state_lock);
	/* An empty object must be materialized even for WAIT_FOR_SUBMIT, so a
	 * concurrent signal/import can satisfy the same backend wait.
	 */
	ret = panthor_client_syncobj_materialize(syncobj);
	if (!ret)
		*handle = syncobj->client_handle;
	return ret;
}

/* The caller holds the exported fence's snapshot_lock. This ref has no
 * session_file, unlike the exported fence, so local imports cannot pin their
 * own file forever. A materialized destination keeps using ordinary TRANSFER.
 */
static int panthor_client_syncobj_import_snapshot(struct panthor_client_syncobj *syncobj,
						 struct panthor_client_syncobj *snapshot)
{
	struct panthor_vmshm_syncobj_transfer_req req = {
		.session_id = syncobj->session_id,
	};
	struct panthor_vmshm_syncobj_transfer_rsp rsp;
	struct panthor_client_syncobj *old = NULL;
	int ret;

	if (snapshot == syncobj || snapshot->session_id != syncobj->session_id ||
	    !snapshot->client_handle || snapshot->snapshot)
		return -EINVAL;
	mutex_lock(&syncobj->state_lock);
	if (!syncobj->client_handle) {
		if (!panthor_client_syncobj_get(snapshot)) {
			ret = -ESTALE;
		} else {
			old = syncobj->snapshot;
			syncobj->snapshot = snapshot;
			atomic64_inc(&panthor_client_snapshot_imports);
			ret = 0;
		}
	} else {
		req.src_handle = snapshot->client_handle;
		req.dst_handle = syncobj->client_handle;
		ret = panthor_client_rpc_syncobj_transfer(&req, &rsp);
		if (!ret)
			ret = rsp.ret;
	}
	mutex_unlock(&syncobj->state_lock);
	panthor_client_syncobj_put(old);
	return ret;
}

static int panthor_client_syncobj_create(struct panthor_client_file *pcfile,
					 struct drm_syncobj_create *args)
{
	struct panthor_client_syncobj *syncobj;
	u32 handle;
	int ret;

	if (!pcfile || !args || args->flags & ~DRM_SYNCOBJ_CREATE_SIGNALED)
		return -EINVAL;
	syncobj = panthor_client_syncobj_alloc(pcfile->session_id);
	if (!syncobj)
		return -ENOMEM;
	syncobj->create_flags = args->flags;
	mutex_lock(&pcfile->lock);
	ret = xa_alloc(&pcfile->syncobjs, &handle, syncobj,
		       XA_LIMIT(1, U32_MAX), GFP_KERNEL);
	mutex_unlock(&pcfile->lock);
	if (ret) {
		panthor_client_syncobj_put(syncobj);
		return ret;
	}
	args->handle = handle;
	atomic64_inc(&panthor_client_lazy_creates);
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

static bool panthor_client_completion_wait(struct panthor_client_syncobj **snapshots,
					 struct drm_syncobj_wait *args)
{
	bool all = args->flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL;
	u64 deadline = ktime_get_ns() + 1000 * NSEC_PER_USEC;
	bool any_hint = false;
	u32 i;

	for (i = 0; i < args->count_handles; i++) {
		bool hint = snapshots[i] && snapshots[i]->completion_page &&
			    snapshots[i]->completion_cookie;

		if (all && !hint)
			return false;
		any_hint |= hint;
	}
	if (!any_hint)
		return false;
	atomic64_inc(&panthor_client_completion_checks);
	if (args->timeout_nsec <= 0)
		deadline = 0;
	else
		deadline = min_t(u64, deadline, args->timeout_nsec);
	for (;;) {
		u32 ready = 0, first = 0;

		for (i = 0; i < args->count_handles; i++) {
			if (panthor_client_snapshot_completed(snapshots[i], 3)) {
				if (!ready)
					first = i;
				ready++;
			}
		}
		if ((all && ready == args->count_handles) || (!all && ready)) {
			args->first_signaled = all ? 0 : first;
			atomic64_inc(&panthor_client_completion_hits);
			return true;
		}
		if (need_resched() || signal_pending(current) || ktime_get_ns() >= deadline)
			break;
		cpu_relax();
	}
	atomic64_inc(&panthor_client_completion_fallbacks);
	return false;
}

static int panthor_client_syncobj_wait(struct panthor_client_file *pcfile,
				       struct drm_syncobj_wait *args)
{
	struct panthor_client_syncobj *syncobjs[PANTHOR_VMSHM_MAX_SYNCOBJ_WAIT_HANDLES] = { 0 };
	struct panthor_client_syncobj *snapshots[PANTHOR_VMSHM_MAX_SYNCOBJ_WAIT_HANDLES] = { 0 };
	struct panthor_vmshm_syncobj_wait_req req = { 0 };
	struct panthor_vmshm_syncobj_wait_rsp rsp = { 0 };
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
	}
	mutex_unlock(&pcfile->lock);

	for (i = 0; i < args->count_handles; i++) {
		ret = panthor_client_syncobj_wait_handle(syncobjs[i], &req.handles[i],
						 &snapshots[i]);
		if (ret)
			goto out_put_syncobjs;
	}
	if (panthor_client_completion_wait(snapshots, args))
		goto out_put_syncobjs;
	/* Polling/materialization consume the user's original absolute deadline. */
	req.timeout_rel_nsec = panthor_client_abs_timeout_to_rel_ns(args->timeout_nsec);
	if (args->flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE)
		req.deadline_rel_nsec =
			panthor_client_abs_deadline_to_rel_ns(args->deadline_nsec);
	ret = panthor_client_rpc_syncobj_wait(&req, &rsp);
	if (ret)
		goto out_put_syncobjs;
	if (rsp.ret) {
		ret = rsp.ret;
		goto out_put_syncobjs;
	}

	args->first_signaled = rsp.first_signaled;
	if (panthor_client_completion_trace) {
		bool all = args->flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL;

		for (i = 0; i < args->count_handles; i++) {
			if (!all && i != rsp.first_signaled)
				continue;
			atomic64_inc(&completion_rpc_successes);
			if (!panthor_client_snapshot_completed(snapshots[i], 6))
				atomic64_inc(&completion_rpc_without_proof);
		}
	}
	pr_debug("panthor-client: SYNCOBJ_WAIT session=%llu count=%u flags=0x%x first=%u\n",
			    pcfile->session_id, args->count_handles,
			    args->flags, rsp.first_signaled);
	ret = 0;
	goto out_put_syncobjs;

out_unlock:
	mutex_unlock(&pcfile->lock);

out_put_syncobjs:
	for (i = 0; i < args->count_handles; i++) {
		panthor_client_syncobj_put(snapshots[i]);
		panthor_client_syncobj_put(syncobjs[i]);
	}

	return ret;
}

static const char *
panthor_client_sync_fence_driver_name(struct dma_fence *fence)
{
	return "panthor-client";
}

static const char *
panthor_client_sync_fence_timeline_name(struct dma_fence *fence)
{
	return "proxy-syncobj";
}

/* release() may run in atomic context. An unused exported snapshot still
 * owns a proxy handle and a DRM file reference, so destroy it asynchronously.
 * A running wait worker owns a fence reference and clears the handle before
 * dropping that reference; consequently it can never race this cleanup.
 */
static void panthor_client_sync_fence_cleanup(struct work_struct *work)
{
	struct panthor_client_sync_fence *sync_fence =
		container_of(work, struct panthor_client_sync_fence, cleanup_work);

	panthor_client_syncobj_put(sync_fence->snapshot);
	atomic64_inc(&panthor_client_lazy_discards);
	fput(sync_fence->session_file);
	mutex_destroy(&sync_fence->snapshot_lock);
	dma_fence_free(&sync_fence->base);
}

static bool panthor_client_sync_fence_enable_signaling(struct dma_fence *fence)
{
	struct panthor_client_sync_fence *sync_fence =
		container_of(fence, struct panthor_client_sync_fence, base);

	if (sync_fence->lazy_wait) {
		/* Called once, with fence->lock held. Never perform an RPC here. */
		dma_fence_get(fence);
		queue_work(panthor_client->sync_file_wq, &sync_fence->work);
	}
	return true;
}

static void
panthor_client_sync_fence_release(struct dma_fence *fence)
{
	struct panthor_client_sync_fence *sync_fence =
		container_of(fence, struct panthor_client_sync_fence, base);

	if (sync_fence->snapshot) {
		queue_work(panthor_client->sync_file_wq, &sync_fence->cleanup_work);
		return;
	}
	mutex_destroy(&sync_fence->snapshot_lock);
	dma_fence_free(&sync_fence->base);
}

static const struct dma_fence_ops panthor_client_sync_fence_ops = {
	.get_driver_name = panthor_client_sync_fence_driver_name,
	.get_timeline_name = panthor_client_sync_fence_timeline_name,
	.enable_signaling = panthor_client_sync_fence_enable_signaling,
	.release = panthor_client_sync_fence_release,
};

static void panthor_client_sync_fence_work(struct work_struct *work)
{
	struct panthor_client_sync_fence *sync_fence =
		container_of(work, struct panthor_client_sync_fence, work);
	struct panthor_vmshm_syncobj_wait_req req = {
		.session_id = sync_fence->session_id,
		.count_handles = 1,
		.flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL |
			 PANTHOR_VMSHM_SYNCOBJ_WAIT_FENCE_STATUS,
		.timeout_rel_nsec = PANTHOR_CLIENT_SYNC_FILE_WAIT_NSEC,
	};
	struct panthor_vmshm_syncobj_wait_rsp rsp = { 0 };
	u64 start_ns = ktime_get_ns();
	u64 elapsed_ns;
	unsigned int segments = 0;
	int wait_ret = 0;
	int ret;

	req.handles[0] = sync_fence->snapshot->client_handle;

	for (;;) {
		/*
		 * All client RPCs share one transport mutex, and the proxy
		 * dispatches them serially. A blocking wait here prevents the
		 * importing/submitting thread from publishing work which may
		 * be needed to complete this fence. Poll the immutable snapshot
		 * and sleep outside the RPC instead.
		 */
		req.timeout_rel_nsec = 0;
		segments++;
		if (panthor_client_snapshot_completed(sync_fence->snapshot, 1)) {
			ret = 0;
			break;
		}
		ret = panthor_client_rpc_syncobj_wait(&req, &rsp);
		if (!ret && !rsp.ret) {
			if (panthor_client_completion_trace) {
				atomic64_inc(&completion_rpc_successes);
				if (!panthor_client_snapshot_completed(sync_fence->snapshot, 4))
					atomic64_inc(&completion_rpc_without_proof);
			}
			break;
		}

		/*
		 * An unsignaled snapshot returns -ETIME. Transport timeouts
		 * retain the existing bounded retry policy.
		 */
		if (ret == -ETIMEDOUT || ret == -ETIME ||
		    (!ret && (rsp.ret == -ETIMEDOUT || rsp.ret == -ETIME))) {
			elapsed_ns = ktime_get_ns() - start_ns;
			if (elapsed_ns < PANTHOR_CLIENT_SYNC_FILE_WAIT_NSEC) {
				usleep_range(panthor_client_sync_file_poll_us,
					     panthor_client_sync_file_poll_us * 2);
				continue;
			}
		}

		wait_ret = ret ? ret : rsp.ret;
		dma_fence_set_error(&sync_fence->base, wait_ret);
		break;
	}

	atomic64_inc(&panthor_client_sync_file_waits);
	atomic64_add(segments, &panthor_client_sync_file_polls);
	atomic64_add(ktime_get_ns() - start_ns, &panthor_client_sync_file_wait_ns);
	if (wait_ret)
		atomic64_inc(&panthor_client_sync_file_errors);

	if (wait_ret)
		pr_debug("panthor-client: sync-file proxy wait failed session=%llu client_sync=%u ret=%d rsp_ret=%d segments=%u\n",
				    sync_fence->session_id,
				    sync_fence->snapshot->client_handle, ret,
				    rsp.ret, segments);
	else
		pr_debug("panthor-client: SYNC_FILE_WAIT_DONE session=%llu client_sync=%u segments=%u elapsed_ms=%llu\n",
				    sync_fence->session_id,
				    sync_fence->snapshot->client_handle,
				    segments,
				    (ktime_get_ns() - start_ns) / NSEC_PER_MSEC);
	mutex_lock(&sync_fence->snapshot_lock);
	dma_fence_signal(&sync_fence->base);
	/*
	 * This handle is a private snapshot, not the user's mutable syncobj.
	 * Keep the DRM file/session alive until the wait and destroy complete.
	 */
	panthor_client_syncobj_put(sync_fence->snapshot);
	sync_fence->snapshot = NULL;
	mutex_unlock(&sync_fence->snapshot_lock);
	fput(sync_fence->session_file);
	dma_fence_put(&sync_fence->base);
}

static int
panthor_client_syncobj_snapshot(u64 session_id, u64 feature_bits,
				struct panthor_client_completion_page *page,
				u32 source_handle,
				u32 *snapshot_handle,
				struct panthor_vmshm_snapshot_watch_rsp *watch)
{
	struct panthor_vmshm_syncobj_create_req create = {
		.session_id = session_id,
	};
	struct panthor_vmshm_syncobj_create_rsp created;
	struct panthor_vmshm_syncobj_transfer_req transfer = {
		.session_id = session_id,
		.src_handle = source_handle,
	};
	struct panthor_vmshm_syncobj_transfer_rsp transferred;
	int ret;

	if (panthor_client_fused_snapshot &&
	    (feature_bits & PANTHOR_VMSHM_FEATURE_SYNCOBJ_SNAPSHOT)) {
		struct panthor_vmshm_syncobj_snapshot_req snapshot = {
			.session_id = session_id,
			.source_handle = source_handle,
		};

		if (panthor_client_completed_snapshot &&
		    (feature_bits & PANTHOR_VMSHM_FEATURE_COMPLETED_SNAPSHOT))
			snapshot.pad = PANTHOR_VMSHM_SNAPSHOT_ALLOW_COMPLETED;
		if (watch) {
			if (page && page->retire_enabled) {
				struct panthor_vmshm_snapshot_retire_req req = { .snapshot = snapshot };

				panthor_client_snapshot_retire_take(page, &req, false);
				ret = panthor_client_rpc_call(PANTHOR_VMSHM_MSG_SNAPSHOT_RETIRE_REQ,
					&req, sizeof(req), PANTHOR_VMSHM_MSG_SNAPSHOT_RETIRE_RSP,
					watch, sizeof(*watch), sizeof(*watch), NULL);
			} else {
				ret = panthor_client_rpc_call(PANTHOR_VMSHM_MSG_SNAPSHOT_WATCH_REQ,
					&snapshot, sizeof(snapshot), PANTHOR_VMSHM_MSG_SNAPSHOT_WATCH_RSP,
					watch, sizeof(*watch), sizeof(*watch), NULL);
			}
			created = watch->snapshot;
			if (!ret && (watch->pad ||
			    (watch->cookie && watch->slot >= PANTHOR_VMSHM_COMPLETION_SLOTS)))
				return -EPROTO;
		} else {
		ret = panthor_client_rpc_call(
			PANTHOR_VMSHM_MSG_SYNCOBJ_SNAPSHOT_REQ,
			&snapshot, sizeof(snapshot),
			PANTHOR_VMSHM_MSG_SYNCOBJ_SNAPSHOT_RSP,
			&created, sizeof(created), sizeof(created), NULL);
		}
		if (ret)
			return ret;
		if (created.ret)
			return created.ret;
		if (created.pad) {
			if (created.pad != PANTHOR_VMSHM_SNAPSHOT_COMPLETED ||
			    !snapshot.pad || created.client_syncobj_handle ||
			    created.proxy_syncobj_handle)
				return -EPROTO;
			/* Zero denotes a successfully completed, handle-free snapshot. */
			*snapshot_handle = 0;
			return 0;
		}
		if (!created.client_syncobj_handle || !created.proxy_syncobj_handle)
			return -EPROTO;
		*snapshot_handle = created.client_syncobj_handle;
		return 0;
	}

	ret = panthor_client_rpc_syncobj_create(&create, &created);
	if (ret)
		return ret;
	if (created.ret)
		return created.ret;
	if (!created.client_syncobj_handle || !created.proxy_syncobj_handle)
		return -EPROTO;

	/*
	 * Binary transfer takes a reference to the fence present at transfer
	 * time. The private destination cannot be reset/replaced by userspace,
	 * so the worker waits for this fence even after the source is reused.
	 */
	transfer.dst_handle = created.client_syncobj_handle;
	ret = panthor_client_rpc_syncobj_transfer(&transfer, &transferred);
	if (!ret)
		ret = transferred.ret;
	if (ret) {
		panthor_client_syncobj_destroy_rpc_session(
			session_id, created.client_syncobj_handle, NULL);
		return ret;
	}

	*snapshot_handle = created.client_syncobj_handle;
	return 0;
}

static int
panthor_client_syncobj_export_sync_file(struct panthor_client_file *pcfile,
					struct drm_syncobj_handle *args,
					struct file *filp)
{
	struct panthor_vmshm_snapshot_watch_rsp watched = { 0 };
	struct panthor_client_sync_fence *sync_fence;
	struct panthor_client_syncobj *syncobj;
	struct sync_file *sync_file;
	u32 snapshot_handle;
	int fd;
	int ret;

	if (!pcfile || !args || args->pad ||
	    args->flags != DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE)
		return -EINVAL;
	if (!panthor_client || !panthor_client->sync_file_wq)
		return -ENODEV;

	mutex_lock(&pcfile->lock);
	syncobj = xa_load(&pcfile->syncobjs, args->handle);
	if (!syncobj || !panthor_client_syncobj_get(syncobj)) {
		mutex_unlock(&pcfile->lock);
		return -ENOENT;
	}
	mutex_unlock(&pcfile->lock);

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		ret = fd;
		goto out_put_syncobj;
	}

	sync_fence = kzalloc(sizeof(*sync_fence), GFP_KERNEL);
	if (!sync_fence) {
		ret = -ENOMEM;
		goto out_put_fd;
	}

	mutex_lock(&pcfile->lock);
	if (panthor_client_transfer_capture && !pcfile->sync_mutations &&
	    !pcfile->sync_epoch_wrapped && syncobj->capture_epoch == pcfile->sync_epoch &&
	    panthor_client_syncobj_get(syncobj->export_snapshot)) {
		sync_fence->snapshot = syncobj->export_snapshot;
		atomic64_inc(&panthor_client_capture_hits);
	}
	mutex_unlock(&pcfile->lock);
	if (sync_fence->snapshot) {
		snapshot_handle = sync_fence->snapshot->client_handle;
		goto init_export;
	}
	ret = panthor_client_syncobj_materialize(syncobj);
	if (ret) {
		kfree(sync_fence);
		goto out_put_fd;
	}
	ret = panthor_client_syncobj_snapshot(pcfile->session_id,
					     pcfile->feature_bits, pcfile->completion_page,
					     syncobj->client_handle,
					     &snapshot_handle, pcfile->completion_page ? &watched : NULL);
	if (ret) {
		kfree(sync_fence);
		goto out_put_fd;
	}

	if (snapshot_handle) {
		sync_fence->snapshot = panthor_client_syncobj_alloc(pcfile->session_id);
		if (!sync_fence->snapshot) {
			panthor_client_syncobj_destroy_rpc_session(pcfile->session_id,
							 snapshot_handle, NULL);
			kfree(sync_fence);
			ret = -ENOMEM;
			goto out_put_fd;
		}
		sync_fence->snapshot->client_handle = snapshot_handle;
		sync_fence->snapshot->destroy_pending = true;
		if (pcfile->completion_page &&
		    (watched.cookie || pcfile->completion_page->retire_enabled)) {
			refcount_inc(&pcfile->completion_page->refs);
			sync_fence->snapshot->completion_page = pcfile->completion_page;
			sync_fence->snapshot->completion_cookie = watched.cookie;
			sync_fence->snapshot->completion_slot = watched.slot;
		}
	}
init_export:
	sync_fence->session_id = pcfile->session_id;
	mutex_init(&sync_fence->snapshot_lock);
	spin_lock_init(&sync_fence->lock);
	INIT_WORK(&sync_fence->work, panthor_client_sync_fence_work);
	INIT_WORK(&sync_fence->cleanup_work, panthor_client_sync_fence_cleanup);
	sync_fence->lazy_wait = panthor_client_lazy_snapshot_wait;
	dma_fence_init(&sync_fence->base, &panthor_client_sync_fence_ops,
		       &sync_fence->lock, dma_fence_context_alloc(1), 1);

	if (snapshot_handle) {
		sync_fence->session_file = get_file(filp);
		if (sync_fence->lazy_wait)
			goto create_sync_file;
		/* The work item and the sync_file each own a fence reference. */
		dma_fence_get(&sync_fence->base);
		if (!queue_work(panthor_client->sync_file_wq, &sync_fence->work)) {
			ret = -EWOULDBLOCK;
			dma_fence_put(&sync_fence->base);
			panthor_client_syncobj_put(sync_fence->snapshot);
			fput(sync_fence->session_file);
			sync_fence->snapshot = NULL;
			goto out_put_export_ref;
		}
	} else {
		/* Keep our fence type/session identity for same-session import.
		 * No proxy handle, worker or DRM file reference is needed after
		 * a successful completion has been captured by the proxy.
		 */
		dma_fence_signal(&sync_fence->base);
	}

create_sync_file:
	sync_file = sync_file_create(&sync_fence->base);
	if (!sync_file) {
		ret = -ENOMEM;
		goto out_put_export_ref;
	}

	args->fd = fd;
	fd_install(fd, sync_file->file);
	if (!snapshot_handle)
		atomic64_inc(&panthor_client_completed_exports);
	pr_debug("panthor-client: SYNCOBJ_EXPORT_SYNC_FILE session=%llu client_sync=%u proxy_sync=%u snapshot_sync=%u fd=%d\n",
			    pcfile->session_id, syncobj->client_handle,
			    syncobj->proxy_handle, snapshot_handle, fd);
	ret = 0;
	goto out_put_export_ref;

out_put_fd:
	put_unused_fd(fd);
	goto out_put_syncobj;
out_put_export_ref:
	dma_fence_put(&sync_fence->base);
	if (ret)
		put_unused_fd(fd);
out_put_syncobj:
	panthor_client_syncobj_put(syncobj);
	return ret;
}

static int
panthor_client_syncobj_signal_proxy(u64 session_id,
				    struct panthor_client_syncobj *syncobj)
{
	struct panthor_vmshm_syncobj_array_req req = {
		.session_id = session_id,
		.count_handles = 1,
	};
	struct panthor_vmshm_syncobj_array_rsp rsp;
	int ret;

	ret = panthor_client_syncobj_materialize(syncobj);
	if (ret)
		return ret;
	req.handles[0] = syncobj->client_handle;
	ret = panthor_client_rpc_syncobj_array(
		PANTHOR_VMSHM_MSG_SYNCOBJ_SIGNAL_REQ,
		PANTHOR_VMSHM_MSG_SYNCOBJ_SIGNAL_RSP, &req, &rsp);
	if (!ret && rsp.ret)
		ret = rsp.ret;

	return ret;
}

/* Import publishes the captured immutable fence before returning to userspace. */
static int
panthor_client_syncobj_import_sync_file(struct panthor_client_file *pcfile,
					struct drm_syncobj_handle *args)
{
	struct panthor_client_sync_fence *sync_fence;
	struct panthor_client_syncobj *syncobj;
	struct dma_fence *fence;
	int status;
	int ret;

	if (!pcfile || !args || args->pad ||
	    args->flags != DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE)
		return -EINVAL;

	mutex_lock(&pcfile->lock);
	syncobj = xa_load(&pcfile->syncobjs, args->handle);
	if (!syncobj || !panthor_client_syncobj_get(syncobj)) {
		mutex_unlock(&pcfile->lock);
		return -ENOENT;
	}
	syncobj->proof_cookie = 0;
	mutex_unlock(&pcfile->lock);

	fence = sync_file_get_fence(args->fd);
	if (!fence) {
		ret = -EINVAL;
		goto out_put_syncobj;
	}
	if (fence->ops != &panthor_client_sync_fence_ops) {
		ret = -EOPNOTSUPP;
		goto out_put_fence;
	}

	sync_fence = container_of(fence, struct panthor_client_sync_fence,
				  base);
	if (sync_fence->session_id != pcfile->session_id) {
		ret = -EOPNOTSUPP;
		goto out_put_fence;
	}

	/*
	 * Serialize against snapshot destruction. A pending fence must replace
	 * the destination now, not signal it from a worker later: that would
	 * leave an old signaled fence visible to subsequent GPU submissions,
	 * or overwrite a newer fence after userspace resets/reuses the target.
	 */
	mutex_lock(&sync_fence->snapshot_lock);
	status = dma_fence_get_status(fence);
	if (status < 0) {
		ret = status;
	} else if (status == 1) {
		ret = panthor_client_syncobj_signal_proxy(pcfile->session_id,
							 syncobj);
	} else if (!sync_fence->snapshot) {
		ret = -EPROTO;
	} else {
		ret = panthor_client_syncobj_import_snapshot(syncobj,
							      sync_fence->snapshot);
	}
	mutex_unlock(&sync_fence->snapshot_lock);

	if (ret)
		pr_warn("panthor-client: SYNCOBJ_IMPORT_SYNC_FILE failed session=%llu client_sync=%u status=%d ret=%d\n",
			pcfile->session_id, syncobj->client_handle, status, ret);
	else
		pr_debug("panthor-client: SYNCOBJ_IMPORT_SYNC_FILE session=%llu client_sync=%u status=%d\n",
				    pcfile->session_id, syncobj->client_handle,
				    status);

out_put_fence:
	dma_fence_put(fence);
out_put_syncobj:
	panthor_client_syncobj_put(syncobj);
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
	struct panthor_client_syncobj *captured = NULL, *old_capture = NULL;
	u64 capture_epoch = 0;
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
	dst_syncobj->proof_cookie = 0;
	old_capture = dst_syncobj->export_snapshot;
	dst_syncobj->export_snapshot = NULL;
	capture_epoch = pcfile->sync_epoch;
	mutex_unlock(&pcfile->lock);
	panthor_client_syncobj_put(old_capture);

	ret = panthor_client_syncobj_materialize(src_syncobj);
	if (ret)
		goto out_put_syncobjs;
	ret = panthor_client_syncobj_materialize(dst_syncobj);
	if (ret)
		goto out_put_syncobjs;
	req.session_id = pcfile->session_id;
	req.src_handle = src_syncobj->client_handle;
	req.dst_handle = dst_syncobj->client_handle;
	req.src_point = args->src_point;
	req.dst_point = args->dst_point;
	req.flags = args->flags;

	if (args->dst_point)
		atomic64_inc(&panthor_client_transfer_timeline);
	else
		atomic64_inc(&panthor_client_transfer_binary);
	if (panthor_client_transfer_capture && !args->dst_point &&
	    pcfile->completion_page && pcfile->completion_page->retire_enabled &&
	    (pcfile->feature_bits & PANTHOR_VMSHM_FEATURE_TRANSFER_CAPTURE))
		captured = panthor_client_syncobj_alloc(pcfile->session_id);
	if (captured) {
		struct panthor_vmshm_transfer_capture_req capture = { .transfer = req };
		struct panthor_vmshm_transfer_capture_rsp response = { 0 };

		capture.retire.snapshot.session_id = req.session_id;
		capture.retire.snapshot.source_handle = req.src_handle;
		panthor_client_snapshot_retire_take(pcfile->completion_page,
						    &capture.retire, false);
		atomic64_inc(&panthor_client_capture_calls);
		ret = panthor_client_rpc_call(PANTHOR_VMSHM_MSG_TRANSFER_CAPTURE_REQ,
			&capture, sizeof(capture), PANTHOR_VMSHM_MSG_TRANSFER_CAPTURE_RSP,
			&response, sizeof(response), sizeof(response), NULL);
		rsp = response.transfer;
		if (!ret && !rsp.ret && !response.watch.snapshot.ret &&
		    response.watch.snapshot.client_syncobj_handle) {
			captured->client_handle = response.watch.snapshot.client_syncobj_handle;
			captured->proxy_handle = response.watch.snapshot.proxy_syncobj_handle;
			captured->destroy_pending = true;
			refcount_inc(&pcfile->completion_page->refs);
			captured->completion_page = pcfile->completion_page;
			if (response.watch.cookie &&
			    response.watch.slot < PANTHOR_VMSHM_COMPLETION_SLOTS) {
				captured->completion_cookie = response.watch.cookie;
				captured->completion_slot = response.watch.slot;
			}
			mutex_lock(&pcfile->lock);
			if (pcfile->sync_mutations == 1 && !pcfile->sync_epoch_wrapped &&
			    pcfile->sync_epoch == capture_epoch &&
			    !dst_syncobj->export_snapshot && !dst_syncobj->destroy_pending) {
				dst_syncobj->capture_epoch = capture_epoch;
				dst_syncobj->export_snapshot = captured;
				captured = NULL;
				atomic64_inc(&panthor_client_capture_records);
			} else {
				atomic64_inc(&panthor_client_capture_discarded);
			}
			mutex_unlock(&pcfile->lock);
		}
	} else {
		ret = panthor_client_rpc_syncobj_transfer(&req, &rsp);
	}
	if (ret)
		goto out_put_syncobjs;
	if (rsp.ret) {
		ret = rsp.ret;
		goto out_put_syncobjs;
	}

	pr_info_once("panthor-client: SYNCOBJ_TRANSFER session=%llu src=%u dst=%u src_point=%llu dst_point=%llu flags=0x%x\n",
		pcfile->session_id, args->src_handle, args->dst_handle,
		args->src_point, args->dst_point, args->flags);
	ret = 0;
	goto out_put_syncobjs;

out_unlock:
	mutex_unlock(&pcfile->lock);
	goto out_put_syncobjs;

out_put_syncobjs:
	panthor_client_syncobj_put(captured);
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
	u64 cache_epoch = 0;
	bool cache_candidate, cache_record = false;
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

	cache_candidate = panthor_client_completed_wait_cache &&
		args->count_handles == 1 && points[0] &&
		args->flags == DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL &&
		!req.timeout_rel_nsec;
	if (cache_candidate)
		atomic64_inc(&panthor_client_wait_cache_candidates);

	mutex_lock(&pcfile->lock);
	for (i = 0; i < args->count_handles; i++) {
		struct panthor_client_syncobj *syncobj =
			xa_load(&pcfile->syncobjs, handles[i]);

		if (!syncobj || !panthor_client_syncobj_get(syncobj)) {
			ret = -ENOENT;
			goto out_unlock;
		}

		syncobjs[i] = syncobj;
		req.points[i] = points[i];
	}
	if (cache_candidate && !pcfile->sync_mutations &&
	    !pcfile->sync_epoch_wrapped) {
		struct panthor_client_syncobj *syncobj = syncobjs[0];

		if (pcfile->completion_page && syncobj->proof_cookie &&
		    syncobj->proof_epoch == pcfile->proof_epoch &&
		    syncobj->proof_point >= points[0]) {
			atomic64_inc(&panthor_client_point_checks);
			if (panthor_client_cookie_completed(pcfile->completion_page,
					syncobj->proof_slot, syncobj->proof_cookie, 2)) {
				args->first_signaled = 0;
				atomic64_inc(&panthor_client_point_hits);
				if (points[0] < syncobj->proof_point)
					atomic64_inc(&panthor_client_point_prefix_hits);
				goto out_unlock;
			}
		}
		cache_epoch = pcfile->sync_epoch;
		cache_record = true;
		if (syncobjs[0]->completed_valid &&
		    syncobjs[0]->completed_epoch == cache_epoch &&
		    syncobjs[0]->completed_point == points[0]) {
			args->first_signaled = 0;
			atomic64_inc(&panthor_client_wait_cache_hits);
			goto out_unlock;
		}
		/* Classify misses without relaxing the invalidation rules. Point
		 * mismatch takes priority when both point and epoch changed.
		 */
		if (!syncobjs[0]->completed_valid)
			atomic64_inc(&panthor_client_wait_cache_miss_empty);
		else if (syncobjs[0]->completed_point != points[0])
			atomic64_inc(&panthor_client_wait_cache_miss_point);
		else
			atomic64_inc(&panthor_client_wait_cache_miss_epoch);
	} else if (cache_candidate) {
		atomic64_inc(&panthor_client_wait_cache_miss_blocked);
	}
	mutex_unlock(&pcfile->lock);

	for (i = 0; i < args->count_handles; i++) {
		ret = panthor_client_syncobj_materialize(syncobjs[i]);
		if (ret)
			goto out_put_syncobjs;
		req.handles[i] = syncobjs[i]->client_handle;
	}
	atomic64_inc(&panthor_client_timeline_wait_flags[args->flags]);
	if (args->count_handles == 1 && points[0])
		atomic64_inc(&panthor_client_timeline_wait_single_nonzero[args->flags]);
	if (!req.timeout_rel_nsec)
		atomic64_inc(&panthor_client_timeline_wait_nonblock);
	ret = panthor_client_rpc_syncobj_timeline_wait(&req, &rsp);
	if (!ret) {
		if (!rsp.ret)
			atomic64_inc(&panthor_client_timeline_wait_success);
		else if (rsp.ret == -ETIME || rsp.ret == -ETIMEDOUT)
			atomic64_inc(&panthor_client_timeline_wait_pending);
	}
	if (ret)
		goto out_put_syncobjs;
	if (rsp.ret) {
		ret = rsp.ret;
		goto out_put_syncobjs;
	}

	if (panthor_client_completion_trace) {
		bool proved = false;

		atomic64_inc(&completion_rpc_successes);
		if (args->count_handles == 1 && !rsp.first_signaled && points[0]) {
			struct panthor_client_syncobj *syncobj = syncobjs[0];

			mutex_lock(&pcfile->lock);
			if (!pcfile->sync_mutations && !pcfile->sync_epoch_wrapped &&
			    syncobj->proof_epoch == pcfile->proof_epoch &&
			    syncobj->proof_point >= points[0])
				proved = panthor_client_cookie_completed(pcfile->completion_page,
					syncobj->proof_slot, syncobj->proof_cookie, 5);
			mutex_unlock(&pcfile->lock);
		}
		if (!proved)
			atomic64_inc(&completion_rpc_without_proof);
	}

	/* A concurrent mutation, even one that failed, invalidates this proof. */
	if (cache_record && !rsp.first_signaled) {
		mutex_lock(&pcfile->lock);
		if (!pcfile->sync_mutations && !pcfile->sync_epoch_wrapped &&
		    pcfile->sync_epoch == cache_epoch) {
			syncobjs[0]->completed_point = points[0];
			syncobjs[0]->completed_epoch = cache_epoch;
			syncobjs[0]->completed_valid = true;
			atomic64_inc(&panthor_client_wait_cache_records);
		}
		mutex_unlock(&pcfile->lock);
	}

	args->first_signaled = rsp.first_signaled;
	pr_debug("panthor-client: SYNCOBJ_TIMELINE_WAIT session=%llu count=%u flags=0x%x first=%u\n",
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
	}

	mutex_unlock(&pcfile->lock);
	for (i = 0; i < count_handles; i++) {
		ret = panthor_client_syncobj_materialize(syncobjs[i]);
		if (ret)
			return ret; /* Caller drops every acquired reference. */
		out_handles[i] = syncobjs[i]->client_handle;
	}
	return 0;

out_put_syncobjs:
	mutex_unlock(&pcfile->lock);
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

static void panthor_client_point_invalidate(struct panthor_client_file *pcfile,
	struct panthor_client_syncobj **syncobjs, u32 count)
{
	u32 i;

	mutex_lock(&pcfile->lock);
	for (i = 0; i < count; i++)
		syncobjs[i]->proof_cookie = 0;
	mutex_unlock(&pcfile->lock);
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

	panthor_client_point_invalidate(pcfile, syncobjs, args->count_handles);
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

	if (!copy_points_to_user)
		panthor_client_point_invalidate(pcfile, syncobjs, args->count_handles);
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
	if (args->flags & ~DRM_PANTHOR_VM_CREATE_ISOLATED)
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

/* Serialize plan replacement and execution, independently of object lookup.
 * Transport errors are never retried: the server may already have submitted.
 */
static int panthor_client_execute_plan(struct panthor_client_file *pcfile,
	const struct panthor_vmshm_plan_register_req *submit,
	const struct panthor_vmshm_plan_exec_req *patch,
	struct panthor_vmshm_plan_exec_watch_rsp *watch)
{
	struct panthor_vmshm_plan_exec_rsp *rsp = &watch->result;
	u32 rsp_size = sizeof(*rsp);
	struct panthor_vmshm_plan_register_req shape = *submit;
	struct panthor_vmshm_plan_exec_req exec = *patch;
	struct panthor_vmshm_plan_register_rsp registered;
	struct panthor_client_plan *plan;
	u32 slot;
	int attempt, ret;

	mutex_lock(&pcfile->plan_lock);
	for (slot = 0; slot < PANTHOR_VMSHM_PLAN_SLOTS; slot++)
		if (pcfile->plans[slot].id &&
		    !memcmp(&pcfile->plans[slot].shape, &shape, sizeof(shape)))
			break;
	if (slot == PANTHOR_VMSHM_PLAN_SLOTS) {
		slot = pcfile->next_plan_slot++ % PANTHOR_VMSHM_PLAN_SLOTS;
		pcfile->plans[slot].id = 0;
		pcfile->plan_misses++;
	} else {
		pcfile->plan_hits++;
	}
	plan = &pcfile->plans[slot];
	exec.slot = slot;
	if (panthor_client_completed_wait_cache && pcfile->completion_page &&
	    (pcfile->feature_bits & PANTHOR_VMSHM_FEATURE_PLAN_COMPLETION)) {
		exec.pad = PANTHOR_VMSHM_PLAN_EXEC_WATCH;
		rsp_size = sizeof(*watch);
	}
	for (attempt = 0; attempt < 2; attempt++) {
		if (!plan->id) {
			shape.slot = slot;
			ret = panthor_client_rpc_call(PANTHOR_VMSHM_MSG_PLAN_REGISTER_REQ,
				&shape, sizeof(shape), PANTHOR_VMSHM_MSG_PLAN_REGISTER_RSP,
				&registered, sizeof(registered), sizeof(registered), NULL);
			shape.slot = 0;
			if (ret)
				goto out;
			if (registered.ret) {
				ret = registered.ret;
				goto out;
			}
			if (registered.slot != slot || !registered.plan_id) {
				ret = -EPROTO;
				goto out;
			}
			plan->shape = shape;
			plan->id = registered.plan_id;
		}
		exec.plan_id = plan->id;
		ret = panthor_client_rpc_call(PANTHOR_VMSHM_MSG_PLAN_EXEC_REQ,
			&exec, sizeof(exec), PANTHOR_VMSHM_MSG_PLAN_EXEC_RSP,
			watch, rsp_size, rsp_size, NULL);
		if (ret)
			goto out;
		/* Only a proven pre-dispatch stale response may be retried. */
		if (rsp->ret != -ESTALE || rsp->proxy_group_handle)
			goto out;
		plan->id = 0;
	}
	ret = -ESTALE;
out:
	mutex_unlock(&pcfile->plan_lock);
	return ret;
}

static void panthor_client_plan_completion_record(struct panthor_client_file *pcfile,
	const struct panthor_vmshm_plan_exec_watch_rsp *watch,
	const struct panthor_vmshm_plan_register_req *req,
	const struct panthor_vmshm_plan_exec_req *exec,
	struct panthor_client_syncobj **syncobjs, u64 mutation_epoch,
	u64 proof_epoch, bool proof_record)
{
	u32 i;

	/* The submit must be the sole writer throughout capture and publication.
	 * Malformed optional metadata is ignored, never retry submitted work.
	 */
	i = watch->sync_index;
	if (proof_record && panthor_client_completed_wait_cache && pcfile->completion_page &&
	    (pcfile->feature_bits & PANTHOR_VMSHM_FEATURE_PLAN_COMPLETION) &&
	    watch->cookie && watch->slot < PANTHOR_VMSHM_COMPLETION_SLOTS &&
	    i < req->sync_count && exec->timeline_values[i] &&
	    req->syncs[i].flags == (DRM_PANTHOR_SYNC_OP_SIGNAL |
		DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_TIMELINE_SYNCOBJ)) {
		mutex_lock(&pcfile->lock);
		if (pcfile->sync_mutations == 1 && !pcfile->sync_epoch_wrapped &&
		    pcfile->sync_epoch == mutation_epoch &&
		    pcfile->proof_epoch == proof_epoch) {
			syncobjs[i]->proof_point = exec->timeline_values[i];
			syncobjs[i]->proof_epoch = proof_epoch;
			syncobjs[i]->proof_slot = watch->slot;
			syncobjs[i]->proof_cookie = watch->cookie;
			atomic64_inc(&panthor_client_point_records);
		}
		mutex_unlock(&pcfile->lock);
	}

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
	struct panthor_vmshm_plan_register_req req = { 0 };
	struct panthor_vmshm_plan_exec_req exec = { 0 };
	struct panthor_vmshm_plan_exec_watch_rsp watch = { 0 };
	struct panthor_vmshm_plan_exec_rsp *rsp = &watch.result;
	u64 mutation_epoch = 0, proof_epoch = 0;
	bool proof_record;
	struct panthor_client_group *group = NULL;
	u32 sync_count = 0;
	u32 i;
	int ret;

	if (!pcfile || !args || args->pad || !args->group_handle) {
		ret = -EINVAL;
		pr_info_ratelimited(
			"panthor-client: GROUP_SUBMIT_DIAG stage=args ret=%d session=%llu group=%u pad=%u jobs=%u job_stride=%u job_array=0x%llx\n",
			ret, pcfile ? pcfile->session_id : 0,
			args ? args->group_handle : 0,
			args ? args->pad : 0,
			args ? args->queue_submits.count : 0,
			args ? args->queue_submits.stride : 0,
			args ? (unsigned long long)args->queue_submits.array : 0);
		return ret;
	}

	pr_debug(
		"panthor-client: GROUP_SUBMIT_DIAG stage=enter session=%llu group=%u jobs=%u job_stride=%u job_array=0x%llx\n",
		pcfile->session_id, args->group_handle,
		args->queue_submits.count, args->queue_submits.stride,
		(unsigned long long)args->queue_submits.array);

	ret = panthor_client_copy_group_submit(args, jobs, syncs, sync_starts,
					       sync_counts, &sync_count);
	if (ret) {
		pr_info_ratelimited(
			"panthor-client: GROUP_SUBMIT_DIAG stage=copy ret=%d session=%llu group=%u jobs=%u syncs_copied=%u first_queue=%u first_size=%u first_addr=0x%llx first_sync_flags=0x%x first_sync_handle=%u first_sync_value=%llu\n",
			ret, pcfile->session_id, args->group_handle,
			args->queue_submits.count, sync_count,
			jobs[0].queue_index, jobs[0].stream_size,
			(unsigned long long)jobs[0].stream_addr,
			syncs[0].flags, syncs[0].handle,
			(unsigned long long)syncs[0].timeline_value);
		return ret;
	}

	req.session_id = pcfile->session_id;
	exec.session_id = pcfile->session_id;
	req.job_count = args->queue_submits.count;
	req.sync_count = sync_count;

	for (i = 0; i < req.job_count; i++) {
		req.jobs[i].queue_index = jobs[i].queue_index;
		exec.jobs[i].stream_size = jobs[i].stream_size;
		exec.jobs[i].stream_addr = jobs[i].stream_addr;
		exec.jobs[i].latest_flush = jobs[i].latest_flush;
		req.jobs[i].sync_start = sync_starts[i];
		req.jobs[i].sync_count = sync_counts[i];
	}

	mutex_lock(&pcfile->lock);
	group = xa_load(&pcfile->groups, args->group_handle);
	if (!group) {
		ret = -ENOENT;
		pr_info_ratelimited(
			"panthor-client: GROUP_SUBMIT_DIAG stage=group_lookup ret=%d session=%llu group=%u jobs=%u syncs=%u\n",
			ret, pcfile->session_id, args->group_handle,
			req.job_count, req.sync_count);
		goto out_unlock;
	}
	if (!panthor_client_group_get(group)) {
		group = NULL;
		ret = -ENOENT;
		pr_info_ratelimited(
			"panthor-client: GROUP_SUBMIT_DIAG stage=group_ref ret=%d session=%llu group=%u jobs=%u syncs=%u\n",
			ret, pcfile->session_id, args->group_handle,
			req.job_count, req.sync_count);
		goto out_unlock;
	}
	req.client_group_handle = group->client_handle;

	for (i = 0; i < sync_count; i++) {
		struct panthor_client_syncobj *syncobj =
			xa_load(&pcfile->syncobjs, syncs[i].handle);

		if (!syncobj || !panthor_client_syncobj_get(syncobj)) {
			ret = -ENOENT;
			pr_info_ratelimited(
				"panthor-client: GROUP_SUBMIT_DIAG stage=sync_lookup ret=%d session=%llu group=%u index=%u flags=0x%x handle=%u timeline_value=%llu client_sync=%u jobs=%u\n",
				ret, pcfile->session_id, args->group_handle,
				i, syncs[i].flags, syncs[i].handle,
				(unsigned long long)syncs[i].timeline_value,
				syncobj ? syncobj->client_handle : 0,
				req.job_count);
			goto out_unlock;
		}

		syncobjs[i] = syncobj;
		req.syncs[i].flags = syncs[i].flags;
		exec.timeline_values[i] = syncs[i].timeline_value;
	}
	proof_record = pcfile->sync_mutations == 1;
	mutation_epoch = pcfile->sync_epoch;
	proof_epoch = pcfile->proof_epoch;
	mutex_unlock(&pcfile->lock);

	for (i = 0; i < sync_count; i++) {
		ret = panthor_client_syncobj_materialize(syncobjs[i]);
		if (ret)
			goto out_put_refs;
		req.syncs[i].client_syncobj_handle = syncobjs[i]->client_handle;
	}
	ret = panthor_client_execute_plan(pcfile, &req, &exec, &watch);
	if (ret) {
		pr_info_ratelimited(
			"panthor-client: GROUP_SUBMIT_DIAG stage=rpc ret=%d session=%llu group=%u jobs=%u syncs=%u first_queue=%u first_size=%u first_addr=0x%llx first_sync_flags=0x%x first_client_sync=%u first_sync_value=%llu\n",
			ret, pcfile->session_id, args->group_handle,
			req.job_count, req.sync_count,
			req.jobs[0].queue_index, exec.jobs[0].stream_size,
			(unsigned long long)exec.jobs[0].stream_addr,
			req.sync_count ? req.syncs[0].flags : 0,
			req.sync_count ? req.syncs[0].client_syncobj_handle : 0,
			req.sync_count ?
				(unsigned long long)exec.timeline_values[0] : 0);
		goto out_put_refs;
	}
	if (rsp->ret) {
		ret = rsp->ret;
		pr_info_ratelimited(
			"panthor-client: GROUP_SUBMIT_DIAG stage=proxy_rsp ret=%d session=%llu group=%u jobs=%u syncs=%u rsp_group=%u rsp_jobs=%u rsp_syncs=%u first_sync_flags=0x%x first_client_sync=%u first_sync_value=%llu\n",
			ret, pcfile->session_id, args->group_handle,
			req.job_count, req.sync_count,
			rsp->proxy_group_handle, rsp->job_count, rsp->sync_count,
			req.sync_count ? req.syncs[0].flags : 0,
			req.sync_count ? req.syncs[0].client_syncobj_handle : 0,
			req.sync_count ?
				(unsigned long long)exec.timeline_values[0] : 0);
		goto out_put_refs;
	}
	if (rsp->job_count != req.job_count || rsp->sync_count != req.sync_count ||
	    !rsp->proxy_group_handle) {
		ret = -EPROTO;
		pr_info_ratelimited(
			"panthor-client: GROUP_SUBMIT_DIAG stage=rsp_mismatch ret=%d session=%llu group=%u req_jobs=%u rsp_jobs=%u req_syncs=%u rsp_syncs=%u rsp_group=%u\n",
			ret, pcfile->session_id, args->group_handle,
			req.job_count, rsp->job_count, req.sync_count,
			rsp->sync_count, rsp->proxy_group_handle);
		goto out_put_refs;
	}

	panthor_client_plan_completion_record(pcfile, &watch, &req, &exec,
		syncobjs, mutation_epoch, proof_epoch, proof_record);

	pr_debug("panthor-client: GROUP_SUBMIT session=%llu client_group=%u proxy_group=%u jobs=%u syncs=%u first_stream=0x%llx first_size=%u first_latest_flush=0x%x\n",
			    pcfile->session_id, args->group_handle,
			    rsp->proxy_group_handle, req.job_count, req.sync_count,
			    exec.jobs[0].stream_addr, exec.jobs[0].stream_size,
			    exec.jobs[0].latest_flush);
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
		req.syncs[i].timeline_value = syncs[i].timeline_value;
	}
	mutex_unlock(&pcfile->lock);

	for (i = 0; i < sync_count; i++) {
		ret = panthor_client_syncobj_materialize(syncobjs[i]);
		if (ret)
			goto out_put_refs;
		req.syncs[i].client_syncobj_handle = syncobjs[i]->client_handle;
	}
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

	pr_debug("panthor-client: VM_BIND session=%llu client_vm=%u proxy_vm=%u ops=%u syncs=%u flags=0x%x\n",
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
	struct panthor_vmshm_bo_create_rsp rsp = { 0 };
	struct client_vmshm_lookup_params lookup = {
		.lookup = VMSHM_MANAGER_LOOKUP_HANDLE,
		.required_perms = PROXY_VMSHM_PERM_MMAP |
				  PROXY_VMSHM_PERM_CPU_READ |
				  PROXY_VMSHM_PERM_CPU_WRITE,
	};
	struct panthor_client_bo *bo;
	const char *stage = "args_validation";
	int ret;

	if (!pcfile)
		return -EINVAL;
	if (!args->size || args->pad ||
	    (args->flags & ~DRM_PANTHOR_BO_NO_MMAP)) {
		ret = -EINVAL;
		goto out_error;
	}

	req.session_id = pcfile->session_id;
	req.size = args->size;
	req.flags = args->flags;
	req.client_exclusive_vm_id = args->exclusive_vm_id;

	ret = panthor_client_rpc_bo_create(&req, &rsp);
	stage = "rpc_call";
	if (ret)
		goto out_error;
	stage = "rsp_ret";
	if (rsp.ret) {
		ret = rsp.ret;
		goto out_error;
	}
	if (!rsp.client_bo_handle || !rsp.proxy_bo_handle || !rsp.size ||
	    !rsp.payload_handle || !rsp.payload_size) {
		stage = "rsp_validation";
		ret = -EPROTO;
		goto out_error;
	}

	bo = kzalloc(sizeof(*bo), GFP_KERNEL);
	if (!bo) {
		panthor_client_bo_destroy_rpc(pcfile, rsp.client_bo_handle, NULL);
		stage = "kzalloc";
		ret = -ENOMEM;
		goto out_error;
	}

	lookup.handle = rsp.payload_handle;
	lookup.requester_vmid = client_comm_vmshm_client_vmid();
	if (!lookup.requester_vmid) {
		kfree(bo);
		panthor_client_bo_destroy_rpc(pcfile, rsp.client_bo_handle, NULL);
		stage = "requester_vmid";
		ret = -EPROTO;
		goto out_error;
	}
	ret = client_vmshm_manager_get(&lookup, &bo->payload_obj);
	if (ret) {
		kfree(bo);
		panthor_client_bo_destroy_rpc(pcfile, rsp.client_bo_handle, NULL);
		stage = "manager_get";
		goto out_error;
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
		stage = "descriptor_validation";
		ret = -EPROTO;
		goto out_error;
	}

	mutex_lock(&pcfile->lock);
	ret = xa_insert(&pcfile->bos, rsp.client_bo_handle, bo, GFP_KERNEL);
	mutex_unlock(&pcfile->lock);
	if (ret) {
		client_vmshm_manager_put(bo->payload_obj);
		kfree(bo);
		panthor_client_bo_destroy_rpc(pcfile, rsp.client_bo_handle, NULL);
		stage = "xa_insert";
		goto out_error;
	}

	args->size = rsp.size;
	args->handle = rsp.client_bo_handle;

	pr_debug("panthor-client: BO_CREATE session=%llu client_bo=%u proxy_bo=%u size=0x%llx payload=0x%llx payload_size=0x%llx\n",
			    pcfile->session_id, rsp.client_bo_handle,
			    rsp.proxy_bo_handle, rsp.size, rsp.payload_handle,
			    rsp.payload_size);
	return 0;

out_error:
	pr_warn("panthor-client: BO_CREATE failed stage=%s session=%llu req_size=0x%llx flags=0x%x ret=%d rsp_ret=%d client_bo=%u proxy_bo=%u payload=0x%llx payload_size=0x%llx\n",
		stage, pcfile->session_id, args->size, args->flags, ret,
		rsp.ret, rsp.client_bo_handle, rsp.proxy_bo_handle,
		rsp.payload_handle, rsp.payload_size);
	return ret;
}

static int panthor_client_bo_mmap_offset(struct panthor_client_file *pcfile,
					 struct drm_panthor_bo_mmap_offset *args)
{
	struct panthor_client_bo *bo;
	u64 size, payload_handle;

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
	size = bo->size;
	payload_handle = bo->payload_handle;
	mutex_unlock(&pcfile->lock);

	pr_info("panthor-client: BO_MMAP_OFFSET session=%llu client_bo=%u offset=0x%llx size=0x%llx payload=0x%llx\n",
		pcfile->session_id, args->handle, args->offset, size,
		payload_handle);
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

static long panthor_client_ioctl_dispatch(struct file *filp, unsigned int cmd,
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
	    cmd == DRM_IOCTL_SYNCOBJ_EVENTFD)
	{
		pr_info_ratelimited("panthor-client: reject unsupported DMA-BUF/syncobj eventfd ioctl cmd=0x%x\n",
				    cmd);
		return -EOPNOTSUPP;
	}
	if (cmd == DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD) {
		struct drm_syncobj_handle handle;
		int ret;

		if (copy_from_user(&handle, (void __user *)arg, sizeof(handle)))
			return -EFAULT;
		ret = panthor_client_syncobj_export_sync_file(pcfile, &handle,
							     filp);
		if (ret)
			return ret;
		if (copy_to_user((void __user *)arg, &handle, sizeof(handle))) {
			close_fd(handle.fd);
			return -EFAULT;
		}
		return 0;
	}
	if (cmd == DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE) {
		struct drm_syncobj_handle handle;
		int ret;

		if (copy_from_user(&handle, (void __user *)arg, sizeof(handle)))
			return -EFAULT;
		ret = panthor_client_syncobj_import_sync_file(pcfile, &handle);
		if (ret)
			return ret;
		if (copy_to_user((void __user *)arg, &handle, sizeof(handle)))
			return -EFAULT;
		return 0;
	}
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

/* Default-deny caching across IOCTLs: anything not explicitly fence-preserving
 * may replace a public syncobj fence, including driver VM_BIND/GROUP_SUBMIT.
 * Do not hold the file lock across an RPC: waits and submissions may overlap.
 */
static bool panthor_client_wait_cache_mutation_begin(struct file *filp,
						   unsigned int cmd)
{
	struct drm_file *file = filp->private_data;
	struct panthor_client_file *pcfile = file ? file->driver_priv : NULL;

	if ((!panthor_client_completed_wait_cache && !panthor_client_transfer_capture) || !pcfile)
		return false;
	switch (cmd) {
	case DRM_IOCTL_GET_CAP:
	case DRM_IOCTL_VERSION:
	case DRM_IOCTL_PANTHOR_DEV_QUERY:
	case DRM_IOCTL_PANTHOR_VM_GET_STATE:
	case DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD:
	case DRM_IOCTL_SYNCOBJ_WAIT:
	case PANTHOR_CLIENT_IOCTL_SYNCOBJ_WAIT_LEGACY:
	case DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT:
	case PANTHOR_CLIENT_IOCTL_SYNCOBJ_TIMELINE_WAIT_LEGACY:
	case DRM_IOCTL_SYNCOBJ_QUERY:
	/* These paths only manage BO handles/payloads/mmap offsets. They never
	 * replace a public syncobj fence, including deferred BO destruction.
	 */
	case DRM_IOCTL_PANTHOR_BO_CREATE:
	case DRM_IOCTL_PANTHOR_BO_MMAP_OFFSET:
	case DRM_IOCTL_GEM_CLOSE:
		return false;
	default:
		break;
	}

	mutex_lock(&pcfile->lock);
	if (!++pcfile->sync_epoch)
		pcfile->sync_epoch_wrapped = true;
	pcfile->sync_mutations++;
	switch (cmd) {
	/* These handlers clear only their acquired destination object(s).
	 * CREATE has a fresh zeroed object; DESTROY removes its identity.
	 */
	case DRM_IOCTL_SYNCOBJ_CREATE:
	case DRM_IOCTL_SYNCOBJ_DESTROY:
	case DRM_IOCTL_SYNCOBJ_TRANSFER:
	case DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE:
	case DRM_IOCTL_SYNCOBJ_RESET:
	case DRM_IOCTL_SYNCOBJ_SIGNAL:
	case DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL:
		break;
	default:
		/* Includes GROUP_SUBMIT, VM_BIND and all unknown/compat writes. */
		if (!++pcfile->proof_epoch)
			pcfile->sync_epoch_wrapped = true;
		break;
	}
	mutex_unlock(&pcfile->lock);
	atomic64_inc(&panthor_client_wait_cache_mutations);
	return true;
}

static void panthor_client_wait_cache_mutation_end(struct file *filp)
{
	struct drm_file *file = filp->private_data;
	struct panthor_client_file *pcfile = file->driver_priv;

	mutex_lock(&pcfile->lock);
	pcfile->sync_mutations--;
	mutex_unlock(&pcfile->lock);
}

static long panthor_client_ioctl(struct file *filp, unsigned int cmd,
				 unsigned long arg)
{
	bool mutation = panthor_client_wait_cache_mutation_begin(filp, cmd);
	long ret = panthor_client_ioctl_dispatch(filp, cmd, arg);

	if (mutation)
		panthor_client_wait_cache_mutation_end(filp);
	return ret;
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

	/* Non-core compat commands bypass the ordinary IOCTL wrapper. */
	{
		bool mutation = panthor_client_wait_cache_mutation_begin(filp, cmd);
		long ret = drm_compat_ioctl(filp, cmd, arg);

		if (mutation)
			panthor_client_wait_cache_mutation_end(filp);
		return ret;
	}
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

	vm_flags_set(vma, VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);
	if (!panthor_client_bo_mmap_cached)
		vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
	vma->vm_private_data = bo;
	vma->vm_ops = &panthor_client_bo_vm_ops;

	ret = client_vmshm_object_mmap(bo->payload_obj, vma, bo_offset);
	if (ret) {
		vma->vm_private_data = NULL;
		goto out_put_bo;
	}

	pr_info("panthor-client: MMAP session=%llu client_bo=%u offset=0x%llx bo_offset=0x%llx size=0x%llx segments=%u attr=%s\n",
		pcfile->session_id, bo->client_handle, offset, bo_offset,
		map_size, bo->payload_desc.nr_segments,
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

	if (!client_comm_vmshm_get())
		return -ENODEV;
	pcfile = kzalloc(sizeof(*pcfile), GFP_KERNEL);
	if (!pcfile) {
		client_comm_vmshm_put();
		return -ENOMEM;
	}

	mutex_init(&pcfile->lock);
	mutex_init(&pcfile->plan_lock);
	xa_init(&pcfile->bos);
	xa_init_flags(&pcfile->syncobjs, XA_FLAGS_ALLOC);
	xa_init(&pcfile->groups);
	xa_init(&pcfile->heaps);
	pcfile->next_mmap_offset = PANTHOR_CLIENT_BO_MMAP_OFFSET_BASE;

	ret = panthor_client_rpc_open_session(&pcfile->session_id,
					      &pcfile->feature_bits);
	if (ret) {
		xa_destroy(&pcfile->heaps);
		xa_destroy(&pcfile->groups);
		xa_destroy(&pcfile->syncobjs);
		xa_destroy(&pcfile->bos);
		kfree(pcfile);
		client_comm_vmshm_put();
		return ret;
	}

	panthor_client_completion_page_init(pcfile);
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

	pr_info("panthor-client: TASK_PLAN_STATS session=%llu hits=%llu misses=%llu\n",
		pcfile->session_id, pcfile->plan_hits, pcfile->plan_misses);
	panthor_client_snapshot_retire_close(pcfile->completion_page);
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

	panthor_client_completion_page_put(pcfile->completion_page);
	kfree(pcfile);
	file->driver_priv = NULL;
	client_comm_vmshm_put();
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
	panthor_client->sync_file_wq =
		alloc_workqueue("panthor-client-sync-file", WQ_UNBOUND, 0);
	if (!panthor_client->sync_file_wq) {
		ret = -ENOMEM;
		goto err_release_devres;
	}

	panthor_client->flush_id_page = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!panthor_client->flush_id_page) {
		ret = -ENOMEM;
		goto err_destroy_workqueues;
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
err_destroy_workqueues:
	destroy_workqueue(panthor_client->sync_file_wq);
	panthor_client->sync_file_wq = NULL;
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

	if (panthor_client->sync_file_wq) {
		destroy_workqueue(panthor_client->sync_file_wq);
		panthor_client->sync_file_wq = NULL;
	}

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
