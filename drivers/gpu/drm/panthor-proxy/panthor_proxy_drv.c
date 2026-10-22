// SPDX-License-Identifier: GPL-2.0 or MIT
/*
 * Panthor proxy VM bridge.
 *
 * The proxy VM owns the real Panthor device. This worker consumes Panthor
 * DEV_QUERY requests from proxy_vmshm_comm and sends query results back to the
 * client VM.
 */

#include <linux/atomic.h>
#include <linux/bitmap.h>
#include <linux/dma-fence.h>
#include <linux/completion.h>
#include <linux/bitops.h>
#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/kstrtox.h>
#include <linux/limits.h>
#include <linux/list.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/refcount.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/proxy_vmshm.h>
#include <linux/vmshm_comm.h>
#include <linux/workqueue.h>
#include <linux/xarray.h>
#include <linux/panthor_vmshm.h>

#define PANTHOR_PROXY_SEND_RETRIES	1000
#define PANTHOR_PROXY_SEND_WAIT_US	1000
#define PANTHOR_PROXY_MAX_VMS_PER_SESSION	32
#define PANTHOR_PROXY_MAX_BOS_PER_SESSION	4096
#define PANTHOR_PROXY_MAX_SYNCOBJS_PER_SESSION	4096
#define PANTHOR_PROXY_MAX_GROUPS_PER_SESSION	128
#define PANTHOR_PROXY_MAX_HEAPS_PER_SESSION	512
#define PANTHOR_PROXY_RPC_STATS_MAX	40
#define PANTHOR_PROXY_SUBMIT_SCHED_MAX_DEPTH	256
#define PANTHOR_PROXY_BO_CREATE_SLOW_NS		100000000ULL
#define PANTHOR_PROXY_SYNC_OP_FLAGS_MASK \
	(DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_MASK | DRM_PANTHOR_SYNC_OP_SIGNAL)

struct panthor_proxy_vm {
	u32 client_vm_id;
	u32 proxy_vm_id;
	u64 user_va_range;
	bool group_created;
	bool isolated;
};

struct panthor_proxy_bo {
	u32 client_bo_handle;
	u32 proxy_bo_handle;
	u64 payload_handle;
	u64 payload_offset;
	u64 payload_size;
	struct proxy_vmshm_object *payload;
};

struct panthor_proxy_snapshot_watch {
	struct dma_fence *fence;
	struct dma_fence_cb cb;
	struct completion done;
	struct panthor_vmshm_completion_slot *record;
	u64 cookie;
	u32 slot;
	bool armed;
};

struct panthor_proxy_syncobj {
	bool private_snapshot;
	struct panthor_proxy_snapshot_watch *watch;
	u32 client_syncobj_handle;
	u32 proxy_syncobj_handle;
	refcount_t refcnt;
	bool destroy_pending;
};

struct panthor_proxy_group {
	u32 client_group_handle;
	u32 proxy_group_handle;
	u32 client_vm_id;
	u32 proxy_vm_id;
};

struct panthor_proxy_heap {
	u32 client_heap_handle;
	u32 proxy_heap_handle;
	u32 client_vm_id;
	u32 proxy_vm_id;
	u64 tiler_heap_ctx_gpu_va;
	u64 first_heap_chunk_gpu_va;
};

struct panthor_proxy_plan {
	u64 id;
	u32 proxy_group_handle;
	struct panthor_vmshm_plan_register_req shape;
	struct panthor_proxy_syncobj *syncobjs[PANTHOR_VMSHM_MAX_GROUP_SUBMIT_SYNCS];
};

struct panthor_proxy_session {
	struct list_head node;
	struct list_head sched_node;
	struct list_head submit_queue;
	u64 session_id;
	u32 owner_vmid;
	struct panthor_vmshm_session *real_session;
	struct proxy_comm_vmshm_channel *channel;
	struct mutex lock;
	struct proxy_vmshm_object *completion_page;
	DECLARE_BITMAP(completion_slots, PANTHOR_VMSHM_COMPLETION_SLOTS);
	u64 next_completion_cookie;
	struct xarray vms;
	struct xarray bos;
	struct xarray syncobjs;
	struct xarray groups;
	struct xarray heaps;
	refcount_t refcnt;
	bool closing;
	bool sched_queued;
	u32 submit_queue_depth;
	u64 next_plan_id;
	u64 plan_runs;
	u64 plan_registers;
	u64 snapshot_retired;
	struct panthor_proxy_plan *plans[PANTHOR_VMSHM_PLAN_SLOTS];
};

typedef void (*panthor_proxy_msg_handler_t)(
	struct proxy_comm_vmshm_channel *channel,
	const struct vmshm_comm_rx *rx,
	const void *req);

struct panthor_proxy_msg_handler {
	u32 req_type;
	u32 rsp_type;
	u32 req_size;
	const char *name;
	panthor_proxy_msg_handler_t handler;
};

struct panthor_proxy_rpc_stat {
	u64 calls;
	u64 total_ns;
	u64 max_ns;
};

struct panthor_proxy_rpc_stats {
	u64 bad_rx;
	u64 unknown_type;
	u64 bad_len;
	u64 response_calls;
	u64 response_errors;
	u64 response_ns;
	u64 response_max_ns;
	struct panthor_proxy_rpc_stat op[PANTHOR_PROXY_RPC_STATS_MAX];
};

struct panthor_proxy_submit_req {
	struct list_head node;
	struct panthor_proxy_session *session;
	struct panthor_vmshm_plan_exec_req plan_exec;
	struct panthor_vmshm_plan_exec_watch_rsp rsp;
	struct completion done;
	int ret;
};

struct panthor_proxy_submit_scheduler {
	struct mutex lock;
	struct list_head runnable_sessions;
	struct work_struct work;
	atomic_t queued;
	bool busy;
};

static DEFINE_MUTEX(panthor_proxy_sessions_lock);
static LIST_HEAD(panthor_proxy_sessions);
static atomic64_t panthor_proxy_next_session_id = ATOMIC64_INIT(0);
static atomic_t panthor_proxy_active_sessions = ATOMIC_INIT(0);
static DEFINE_SPINLOCK(panthor_proxy_rpc_stats_lock);
static struct panthor_proxy_submit_scheduler panthor_proxy_submit_sched;
static bool panthor_proxy_rpc_stats_enabled;
static struct panthor_proxy_rpc_stats panthor_proxy_rpc_stats_data;
static unsigned int panthor_proxy_group_core_partitions;
module_param_named(group_core_partitions,
		   panthor_proxy_group_core_partitions, uint, 0644);
MODULE_PARM_DESC(group_core_partitions,
		 "Experimental proxy group core partition count (0/1 disables; 2 splits shader cores between two clients)");

static void panthor_proxy_syncobj_put(struct panthor_proxy_session *session,
				      struct panthor_proxy_syncobj *syncobj);
static void panthor_proxy_plans_clear(struct panthor_proxy_session *session);
static void panthor_proxy_plans_invalidate(struct panthor_proxy_session *session,
					 u32 group_handle, u32 sync_handle);
static int panthor_proxy_plan_execute(struct panthor_proxy_session *session,
	const struct panthor_vmshm_plan_exec_req *req,
	struct panthor_vmshm_plan_exec_watch_rsp *rsp);
static void
panthor_proxy_submit_sched_fail_session(struct panthor_proxy_session *session,
					int ret);
static void
panthor_proxy_rpc_stats_dump_snapshot(const struct panthor_proxy_rpc_stats *stats);
static void panthor_proxy_rpc_stats_dump_current_if_enabled(const char *reason);

static bool panthor_proxy_rpc_stats_is_enabled(void)
{
	return READ_ONCE(panthor_proxy_rpc_stats_enabled);
}

static u64 panthor_proxy_partition_core_mask(u64 mask, u64 session_id,
					     unsigned int partitions)
{
	u32 slot, start, end, idx = 0;
	u32 count = hweight64(mask);
	u64 remaining = mask;
	u64 out = 0;

	if (partitions < 2 || count < partitions || !session_id)
		return mask;

	slot = (u32)((session_id - 1) % partitions);
	start = div_u64((u64)count * slot, partitions);
	end = div_u64((u64)count * (slot + 1), partitions);
	if (start == end)
		return mask;

	while (remaining) {
		unsigned int bit = __ffs64(remaining);

		if (idx >= start && idx < end)
			out |= BIT_ULL(bit);
		idx++;
		remaining &= remaining - 1;
	}

	return out ?: mask;
}

static u8 panthor_proxy_clamp_max_cores(u8 max_cores, u64 mask)
{
	u32 available = hweight64(mask);

	if (available < max_cores)
		return available;

	return max_cores;
}

static void
panthor_proxy_apply_group_core_partition(struct panthor_proxy_session *session,
					 struct drm_panthor_group_create *args)
{
	unsigned int partitions =
		READ_ONCE(panthor_proxy_group_core_partitions);
	u64 old_compute_mask, old_fragment_mask;
	u8 old_max_compute, old_max_fragment;

	if (!session || !args || partitions < 2)
		return;

	old_compute_mask = args->compute_core_mask;
	old_fragment_mask = args->fragment_core_mask;
	old_max_compute = args->max_compute_cores;
	old_max_fragment = args->max_fragment_cores;

	args->compute_core_mask = panthor_proxy_partition_core_mask(
		args->compute_core_mask, session->session_id, partitions);
	args->fragment_core_mask = panthor_proxy_partition_core_mask(
		args->fragment_core_mask, session->session_id, partitions);
	args->max_compute_cores =
		panthor_proxy_clamp_max_cores(args->max_compute_cores,
					      args->compute_core_mask);
	args->max_fragment_cores =
		panthor_proxy_clamp_max_cores(args->max_fragment_cores,
					      args->fragment_core_mask);

	if (old_compute_mask != args->compute_core_mask ||
	    old_fragment_mask != args->fragment_core_mask ||
	    old_max_compute != args->max_compute_cores ||
	    old_max_fragment != args->max_fragment_cores) {
		pr_info("panthor-proxy: GROUP_CORE_PARTITION session=%llu partitions=%u compute_mask=0x%llx->0x%llx max_compute=%u->%u fragment_mask=0x%llx->0x%llx max_fragment=%u->%u\n",
			session->session_id, partitions, old_compute_mask,
			args->compute_core_mask, old_max_compute,
			args->max_compute_cores, old_fragment_mask,
			args->fragment_core_mask, old_max_fragment,
			args->max_fragment_cores);
	}
}

static void panthor_proxy_rpc_stats_record_bad_rx(void)
{
	unsigned long flags;

	if (!panthor_proxy_rpc_stats_is_enabled())
		return;

	spin_lock_irqsave(&panthor_proxy_rpc_stats_lock, flags);
	panthor_proxy_rpc_stats_data.bad_rx++;
	spin_unlock_irqrestore(&panthor_proxy_rpc_stats_lock, flags);
}

static void panthor_proxy_rpc_stats_record_unknown_type(void)
{
	unsigned long flags;

	if (!panthor_proxy_rpc_stats_is_enabled())
		return;

	spin_lock_irqsave(&panthor_proxy_rpc_stats_lock, flags);
	panthor_proxy_rpc_stats_data.unknown_type++;
	spin_unlock_irqrestore(&panthor_proxy_rpc_stats_lock, flags);
}

static void panthor_proxy_rpc_stats_record_bad_len(void)
{
	unsigned long flags;

	if (!panthor_proxy_rpc_stats_is_enabled())
		return;

	spin_lock_irqsave(&panthor_proxy_rpc_stats_lock, flags);
	panthor_proxy_rpc_stats_data.bad_len++;
	spin_unlock_irqrestore(&panthor_proxy_rpc_stats_lock, flags);
}

static void panthor_proxy_rpc_stats_record_op(u32 index, u64 elapsed_ns)
{
	struct panthor_proxy_rpc_stat *stat;
	unsigned long flags;

	if (!panthor_proxy_rpc_stats_is_enabled() ||
	    index >= PANTHOR_PROXY_RPC_STATS_MAX)
		return;

	spin_lock_irqsave(&panthor_proxy_rpc_stats_lock, flags);
	stat = &panthor_proxy_rpc_stats_data.op[index];
	stat->calls++;
	stat->total_ns += elapsed_ns;
	if (elapsed_ns > stat->max_ns)
		stat->max_ns = elapsed_ns;
	spin_unlock_irqrestore(&panthor_proxy_rpc_stats_lock, flags);
}

static void panthor_proxy_rpc_stats_dump_current_if_enabled(const char *reason)
{
	struct panthor_proxy_rpc_stats snapshot;
	unsigned long flags;
	bool enabled;

	spin_lock_irqsave(&panthor_proxy_rpc_stats_lock, flags);
	enabled = panthor_proxy_rpc_stats_enabled;
	if (enabled)
		snapshot = panthor_proxy_rpc_stats_data;
	spin_unlock_irqrestore(&panthor_proxy_rpc_stats_lock, flags);

	if (!enabled)
		return;

	pr_info("panthor-proxy: [MZH][PANTHOR_PROXY_RPC_STATS_SNAPSHOT] reason=%s\n",
		reason ? reason : "unspecified");
	panthor_proxy_rpc_stats_dump_snapshot(&snapshot);
}

static struct panthor_proxy_session *
panthor_proxy_session_find_locked(u64 session_id)
{
	struct panthor_proxy_session *session;

	list_for_each_entry(session, &panthor_proxy_sessions, node) {
		if (session->session_id == session_id)
			return session;
	}

	return NULL;
}

static void panthor_proxy_log_session_access_denied(const char *op,
						    u64 session_id,
						    u32 owner_vmid,
						    u32 requester_vmid)
{
	pr_warn_ratelimited("panthor-proxy: SESSION_ACCESS_DENIED op=%s session=%llu owner_vmid=%u requester_vmid=%u\n",
			    op, session_id, owner_vmid, requester_vmid);
}

static int panthor_proxy_session_create(struct proxy_comm_vmshm_channel *channel,
					u32 owner_vmid, u64 *session_id)
{
	struct panthor_proxy_session *session;
	int ret;

	if (!owner_vmid || !session_id)
		return -EINVAL;

	if (!proxy_comm_vmshm_channel_get(channel))
		return -ENODEV;
	session = kzalloc(sizeof(*session), GFP_KERNEL);
	if (!session) {
		proxy_comm_vmshm_channel_put(channel);
		return -ENOMEM;
	}
	session->channel = channel;

	INIT_LIST_HEAD(&session->sched_node);
	INIT_LIST_HEAD(&session->submit_queue);
	session->owner_vmid = owner_vmid;
	mutex_init(&session->lock);
	xa_init_flags(&session->vms, XA_FLAGS_ALLOC1);
	xa_init_flags(&session->bos, XA_FLAGS_ALLOC1);
	xa_init_flags(&session->syncobjs, XA_FLAGS_ALLOC1);
	xa_init_flags(&session->groups, XA_FLAGS_ALLOC1);
	xa_init_flags(&session->heaps, XA_FLAGS_ALLOC1);
	refcount_set(&session->refcnt, 1);

	ret = panthor_vmshm_session_open(&session->real_session);
	if (ret) {
		xa_destroy(&session->heaps);
		xa_destroy(&session->groups);
		xa_destroy(&session->syncobjs);
		xa_destroy(&session->bos);
		xa_destroy(&session->vms);
		kfree(session);
		proxy_comm_vmshm_channel_put(channel);
		return ret;
	}

	session->session_id = (u64)atomic64_inc_return(&panthor_proxy_next_session_id);
	if (!session->session_id) {
		panthor_vmshm_session_close(session->real_session);
		xa_destroy(&session->heaps);
		xa_destroy(&session->groups);
		xa_destroy(&session->syncobjs);
		xa_destroy(&session->bos);
		xa_destroy(&session->vms);
		kfree(session);
		proxy_comm_vmshm_channel_put(channel);
		return -EOVERFLOW;
	}

	mutex_lock(&panthor_proxy_sessions_lock);
	list_add_tail(&session->node, &panthor_proxy_sessions);
	mutex_unlock(&panthor_proxy_sessions_lock);
	atomic_inc(&panthor_proxy_active_sessions);

	*session_id = session->session_id;
	return 0;
}

/* Native callbacks may run under a fence spinlock. Publish only immutable
 * success and make completion the final context access; never send RPC here.
 */
static void panthor_proxy_snapshot_complete(struct dma_fence *fence,
					   struct dma_fence_cb *cb)
{
	struct panthor_proxy_snapshot_watch *watch =
		container_of(cb, struct panthor_proxy_snapshot_watch, cb);

	/* The callback (or add_callback -ENOENT) proves it is signaled.
	 * Do not reacquire fence->lock through dma_fence_get_status().
	 */
	if (!READ_ONCE(fence->error))
		smp_store_release(&watch->record->cookie, watch->cookie);
	complete(&watch->done);
}

static void panthor_proxy_snapshot_unwatch(struct panthor_proxy_session *session,
					 struct panthor_proxy_snapshot_watch *watch)
{
	if (!watch)
		return;
	if (watch->armed && !dma_fence_remove_callback(watch->fence, &watch->cb))
		wait_for_completion(&watch->done);
	/* An old callback can no longer publish into a reused slot. */
	dma_fence_put(watch->fence);
	clear_bit(watch->slot, session->completion_slots);
	kfree(watch);
}

/* Called with session->lock; references the exact native fence chain. */
static void panthor_proxy_point_watch(struct panthor_proxy_session *session,
					struct panthor_proxy_syncobj *syncobj,
					u64 point, struct panthor_vmshm_snapshot_watch_rsp *rsp)
{
	struct panthor_vmshm_completion_page *page;
	struct panthor_proxy_snapshot_watch *watch;
	u32 slot;
	int ret;

	if (!session->completion_page ||
	    session->next_completion_cookie == U64_MAX)
		return;
	/* Drain before reusing this public object's slot; stale readers require
	 * the exact non-reused cookie and therefore cannot accept a new fence. */
	panthor_proxy_snapshot_unwatch(session, syncobj->watch);
	syncobj->watch = NULL;
	slot = find_first_zero_bit(session->completion_slots,
				   PANTHOR_VMSHM_COMPLETION_SLOTS);
	if (slot == PANTHOR_VMSHM_COMPLETION_SLOTS)
		return;
	watch = kzalloc(sizeof(*watch), GFP_KERNEL);
	if (!watch)
		return;
	ret = panthor_vmshm_syncobj_fence_get_at(session->real_session,
					     syncobj->proxy_syncobj_handle, point,
					     &watch->fence);
	if (ret) {
		kfree(watch);
		return;
	}
	set_bit(slot, session->completion_slots);
	page = proxy_vmshm_obj_kva(session->completion_page);
	watch->record = &page->slots[slot];
	watch->slot = slot;
	watch->cookie = ++session->next_completion_cookie;
	init_completion(&watch->done);
	WRITE_ONCE(watch->record->cookie, 0);
	ret = dma_fence_add_callback(watch->fence, &watch->cb,
				     panthor_proxy_snapshot_complete);
	if (!ret) {
		watch->armed = true;
	} else if (ret == -ENOENT) {
		panthor_proxy_snapshot_complete(watch->fence, &watch->cb);
	} else {
		panthor_proxy_snapshot_unwatch(session, watch);
		return;
	}
	syncobj->watch = watch;
	rsp->cookie = watch->cookie;
	rsp->slot = slot;
}

static void panthor_proxy_snapshot_watch(struct panthor_proxy_session *session,
	struct panthor_proxy_syncobj *syncobj,
	struct panthor_vmshm_snapshot_watch_rsp *rsp)
{
	if (syncobj->private_snapshot)
		panthor_proxy_point_watch(session, syncobj, 0, rsp);
}

static void panthor_proxy_session_release(struct panthor_proxy_session *session)
{
	struct panthor_proxy_bo *bo;
	struct panthor_proxy_group *group;
	struct panthor_proxy_heap *heap;
	struct panthor_proxy_syncobj *syncobj;
	struct panthor_proxy_vm *vm;
	unsigned long i;
	u32 leftover_bos = 0, leftover_syncobjs = 0, leftover_vms = 0;
	u32 leftover_groups = 0, leftover_heaps = 0;
	u64 session_id;
	bool last_session;

	if (!session)
		return;

	session_id = session->session_id;
	panthor_proxy_plans_clear(session);
	pr_info("panthor-proxy: TASK_PLAN_STATS session=%llu registered=%llu runs=%llu retired_snapshots=%llu\n",
		session_id, session->plan_registers, session->plan_runs,
		session->snapshot_retired);
	xa_for_each(&session->groups, i, group) {
		leftover_groups++;
		if (group->proxy_group_handle)
			panthor_vmshm_group_destroy(session->real_session,
						    group->proxy_group_handle);
		kfree(group);
	}
	xa_destroy(&session->groups);
	xa_for_each(&session->heaps, i, heap) {
		leftover_heaps++;
		if (heap->proxy_heap_handle)
			panthor_vmshm_tiler_heap_destroy(
				session->real_session, heap->proxy_heap_handle);
		kfree(heap);
	}
	xa_destroy(&session->heaps);
	xa_for_each(&session->bos, i, bo) {
		leftover_bos++;
		if (bo->proxy_bo_handle)
			panthor_vmshm_bo_destroy(session->real_session,
						 bo->proxy_bo_handle);
		proxy_vmshm_free(bo->payload);
		kfree(bo);
	}
	xa_destroy(&session->bos);
	i = 1;
	for (;;) {
		syncobj = xa_find(&session->syncobjs, &i, ULONG_MAX,
				  XA_PRESENT);
		if (!syncobj)
			break;
		leftover_syncobjs++;
		xa_erase(&session->syncobjs, i);
		syncobj->destroy_pending = true;
		panthor_proxy_syncobj_put(session, syncobj);
		i++;
	}
	xa_destroy(&session->syncobjs);
	xa_for_each(&session->vms, i, vm) {
		leftover_vms++;
		kfree(vm);
	}
	xa_destroy(&session->vms);
	if (leftover_bos || leftover_syncobjs || leftover_vms ||
	    leftover_groups || leftover_heaps) {
		pr_info("panthor-proxy: SESSION_RELEASE session=%llu leftover_bos=%u leftover_syncobjs=%u leftover_vms=%u leftover_groups=%u leftover_heaps=%u\n",
			session_id, leftover_bos, leftover_syncobjs,
			leftover_vms, leftover_groups, leftover_heaps);
	}

	if (session->completion_page)
		proxy_vmshm_free(session->completion_page);
	panthor_vmshm_session_close(session->real_session);
	last_session = atomic_dec_and_test(&panthor_proxy_active_sessions);
	if (last_session)
		panthor_proxy_rpc_stats_dump_current_if_enabled("proxy-last-session-close");
	proxy_comm_vmshm_channel_put(session->channel);
	kfree(session);
}

static void panthor_proxy_session_put(struct panthor_proxy_session *session)
{
	if (session && refcount_dec_and_test(&session->refcnt))
		panthor_proxy_session_release(session);
}

static void panthor_proxy_syncobj_put(struct panthor_proxy_session *session,
				      struct panthor_proxy_syncobj *syncobj)
{
	int ret;

	if (!syncobj)
		return;

	if (!refcount_dec_and_test(&syncobj->refcnt))
		return;

	panthor_proxy_snapshot_unwatch(session, syncobj->watch);
	if (syncobj->destroy_pending) {
		ret = panthor_vmshm_syncobj_destroy(
			session->real_session, syncobj->proxy_syncobj_handle);
		if (ret)
			pr_warn("panthor-proxy: delayed SYNCOBJ_DESTROY failed session=%llu client_sync=%u proxy_sync=%u ret=%d\n",
				session->session_id,
				syncobj->client_syncobj_handle,
				syncobj->proxy_syncobj_handle, ret);
		else
			pr_info_once("panthor-proxy: SYNCOBJ_DESTROY session=%llu client_sync=%u proxy_sync=%u\n",
				session->session_id,
				syncobj->client_syncobj_handle,
				syncobj->proxy_syncobj_handle);
	}

	kfree(syncobj);
}

static bool panthor_proxy_syncobj_get(struct panthor_proxy_syncobj *syncobj)
{
	return syncobj && refcount_inc_not_zero(&syncobj->refcnt);
}

static int panthor_proxy_session_destroy(u64 session_id, u32 requester_vmid)
{
	struct panthor_proxy_session *session;

	if (!session_id)
		return -EINVAL;

	mutex_lock(&panthor_proxy_sessions_lock);
	session = panthor_proxy_session_find_locked(session_id);
	if (session && session->owner_vmid != requester_vmid) {
		panthor_proxy_log_session_access_denied("destroy", session_id,
							session->owner_vmid,
							requester_vmid);
		session = NULL;
	}
	if (session) {
		session->closing = true;
		list_del(&session->node);
	}
	mutex_unlock(&panthor_proxy_sessions_lock);

	if (!session)
		return -ENOENT;

	panthor_proxy_submit_sched_fail_session(session, -ESHUTDOWN);
	panthor_proxy_session_put(session);
	return 0;
}

static void panthor_proxy_sessions_destroy_all(void)
{
	struct panthor_proxy_session *session, *tmp;
	LIST_HEAD(sessions);

	mutex_lock(&panthor_proxy_sessions_lock);
	list_for_each_entry_safe(session, tmp, &panthor_proxy_sessions, node) {
		session->closing = true;
		list_move_tail(&session->node, &sessions);
	}
	mutex_unlock(&panthor_proxy_sessions_lock);

	list_for_each_entry_safe(session, tmp, &sessions, node) {
		list_del(&session->node);
		panthor_proxy_submit_sched_fail_session(session, -ESHUTDOWN);
		panthor_proxy_session_put(session);
	}
}

static struct panthor_proxy_session *
panthor_proxy_session_lookup(u64 session_id, u32 requester_vmid)
{
	struct panthor_proxy_session *session;

	if (!session_id || !requester_vmid)
		return NULL;

	mutex_lock(&panthor_proxy_sessions_lock);
	session = panthor_proxy_session_find_locked(session_id);
	if (session && session->owner_vmid != requester_vmid) {
		panthor_proxy_log_session_access_denied("lookup", session_id,
							session->owner_vmid,
							requester_vmid);
		session = NULL;
	}
	if (session && session->closing)
		session = NULL;
	if (session)
		refcount_inc(&session->refcnt);
	mutex_unlock(&panthor_proxy_sessions_lock);

	return session;
}

static bool panthor_proxy_session_exists(u64 session_id, u32 requester_vmid)
{
	struct panthor_proxy_session *session;
	bool exists;

	if (!session_id)
		return true;

	session = panthor_proxy_session_lookup(session_id, requester_vmid);
	exists = session;
	panthor_proxy_session_put(session);
	return exists;
}

static void
panthor_proxy_submit_sched_fail_session(struct panthor_proxy_session *session,
					int ret)
{
	struct panthor_proxy_submit_req *submit, *tmp;
	LIST_HEAD(failed);

	if (!session)
		return;

	mutex_lock(&panthor_proxy_submit_sched.lock);
	if (session->sched_queued) {
		list_del_init(&session->sched_node);
		session->sched_queued = false;
	}
	list_splice_init(&session->submit_queue, &failed);
	session->submit_queue_depth = 0;
	list_for_each_entry(submit, &failed, node)
		atomic_dec(&panthor_proxy_submit_sched.queued);
	mutex_unlock(&panthor_proxy_submit_sched.lock);

	list_for_each_entry_safe(submit, tmp, &failed, node) {
		list_del_init(&submit->node);
		submit->ret = ret;
		submit->rsp.result.ret = ret;
		panthor_proxy_session_put(session);
		complete(&submit->done);
	}
}

static void
panthor_proxy_submit_sched_queue_locked(struct panthor_proxy_session *session)
{
	lockdep_assert_held(&panthor_proxy_submit_sched.lock);

	if (session->sched_queued)
		return;

	session->sched_queued = true;
	list_add_tail(&session->sched_node,
		      &panthor_proxy_submit_sched.runnable_sessions);
}

static struct panthor_proxy_submit_req *
panthor_proxy_submit_sched_pick_locked(void)
{
	struct panthor_proxy_session *session;
	struct panthor_proxy_submit_req *submit;

	lockdep_assert_held(&panthor_proxy_submit_sched.lock);

	session = list_first_entry_or_null(
		&panthor_proxy_submit_sched.runnable_sessions,
		struct panthor_proxy_session, sched_node);
	if (!session)
		return NULL;

	list_del_init(&session->sched_node);
	session->sched_queued = false;

	submit = list_first_entry_or_null(&session->submit_queue,
					  struct panthor_proxy_submit_req,
					  node);
	if (!submit)
		return NULL;

	list_del_init(&submit->node);
	session->submit_queue_depth--;

	if (!list_empty(&session->submit_queue))
		panthor_proxy_submit_sched_queue_locked(session);

	atomic_dec(&panthor_proxy_submit_sched.queued);
	return submit;
}

static void panthor_proxy_submit_execute(struct panthor_proxy_submit_req *submit)
{
	submit->ret = panthor_proxy_plan_execute(submit->session,
		&submit->plan_exec, &submit->rsp);
	panthor_proxy_session_put(submit->session);
	complete(&submit->done);
}

static void panthor_proxy_submit_sched_work(struct work_struct *work)
{
	mutex_lock(&panthor_proxy_submit_sched.lock);
	if (panthor_proxy_submit_sched.busy) {
		mutex_unlock(&panthor_proxy_submit_sched.lock);
		return;
	}
	panthor_proxy_submit_sched.busy = true;
	mutex_unlock(&panthor_proxy_submit_sched.lock);

	for (;;) {
		struct panthor_proxy_submit_req *submit;

		mutex_lock(&panthor_proxy_submit_sched.lock);
		submit = panthor_proxy_submit_sched_pick_locked();
		if (!submit)
			panthor_proxy_submit_sched.busy = false;
		mutex_unlock(&panthor_proxy_submit_sched.lock);
		if (!submit)
			return;

		panthor_proxy_submit_execute(submit);
	}
}

static int
panthor_proxy_submit_sched_run(struct panthor_proxy_session *session,
			       const struct panthor_vmshm_plan_exec_req *plan_exec,
			       struct panthor_vmshm_plan_exec_watch_rsp *rsp)
{
	struct panthor_proxy_submit_req submit = {
		.session = session,
	};
	bool inline_submit = false;
	int queued;

	if (!session || !plan_exec || !rsp)
		return -EINVAL;
	submit.plan_exec = *plan_exec;

	init_completion(&submit.done);
	INIT_LIST_HEAD(&submit.node);
	refcount_inc(&session->refcnt);

	mutex_lock(&panthor_proxy_sessions_lock);
	if (session->closing) {
		mutex_unlock(&panthor_proxy_sessions_lock);
		panthor_proxy_session_put(session);
		return -ESHUTDOWN;
	}

	mutex_lock(&panthor_proxy_submit_sched.lock);
	queued = atomic_read(&panthor_proxy_submit_sched.queued);
	if (queued >= PANTHOR_PROXY_SUBMIT_SCHED_MAX_DEPTH) {
		mutex_unlock(&panthor_proxy_submit_sched.lock);
		mutex_unlock(&panthor_proxy_sessions_lock);
		panthor_proxy_session_put(session);
		return -EBUSY;
	}

	/* An idle scheduler has no fairness decision to defer. Keep admission
	 * serialized with the worker, but execute on the receiving thread to
	 * avoid a workqueue handoff followed immediately by a blocking wait.
	 * Contended submissions retain the same per-session round robin.
	 */
	if (!queued && !panthor_proxy_submit_sched.busy) {
		panthor_proxy_submit_sched.busy = true;
		inline_submit = true;
	} else {
		list_add_tail(&submit.node, &session->submit_queue);
		session->submit_queue_depth++;
		atomic_inc(&panthor_proxy_submit_sched.queued);
		panthor_proxy_submit_sched_queue_locked(session);
	}
	mutex_unlock(&panthor_proxy_submit_sched.lock);
	mutex_unlock(&panthor_proxy_sessions_lock);

	if (inline_submit) {
		panthor_proxy_submit_execute(&submit);
		mutex_lock(&panthor_proxy_submit_sched.lock);
		panthor_proxy_submit_sched.busy = false;
		queued = atomic_read(&panthor_proxy_submit_sched.queued);
		mutex_unlock(&panthor_proxy_submit_sched.lock);
		if (queued)
			schedule_work(&panthor_proxy_submit_sched.work);
	} else {
		schedule_work(&panthor_proxy_submit_sched.work);
		wait_for_completion(&submit.done);
	}

	*rsp = submit.rsp;
	return submit.ret;
}

static int
panthor_proxy_session_vm_create(struct panthor_proxy_session *session,
				const struct panthor_vmshm_vm_create_req *req,
				struct panthor_vmshm_vm_create_rsp *rsp)
{
	struct panthor_proxy_vm *vm;
	struct drm_panthor_vm_create args = {
		.user_va_range = req->user_va_range,
	};
	u32 client_vm_id;
	int ret;

	if (!session || !rsp)
		return -EINVAL;
	if (req->flags & ~DRM_PANTHOR_VM_CREATE_ISOLATED)
		return -EINVAL;

	vm = kzalloc(sizeof(*vm), GFP_KERNEL);
	if (!vm)
		return -ENOMEM;

	mutex_lock(&session->lock);
	ret = panthor_vmshm_vm_create(session->real_session, &args);
	if (ret)
		goto err_unlock;

	ret = xa_alloc(&session->vms, &client_vm_id, vm,
		       XA_LIMIT(1, PANTHOR_PROXY_MAX_VMS_PER_SESSION),
		       GFP_KERNEL);
	if (ret)
		goto err_destroy_proxy_vm;

	vm->client_vm_id = client_vm_id;
	vm->proxy_vm_id = args.id;
	vm->user_va_range = args.user_va_range;
	vm->isolated = req->flags & DRM_PANTHOR_VM_CREATE_ISOLATED;

	rsp->client_vm_id = client_vm_id;
	rsp->proxy_vm_id = args.id;
	rsp->user_va_range = args.user_va_range;
	mutex_unlock(&session->lock);
	return 0;

err_destroy_proxy_vm:
	panthor_vmshm_vm_destroy(session->real_session, args.id);

err_unlock:
	mutex_unlock(&session->lock);
	kfree(vm);
	return ret;
}

static int
panthor_proxy_session_vm_destroy(struct panthor_proxy_session *session,
				 u32 client_vm_id, u32 *proxy_vm_id)
{
	struct panthor_proxy_vm *vm;
	struct panthor_proxy_group *group;
	struct panthor_proxy_heap *heap;
	unsigned long index;
	int ret;

	if (!session || !client_vm_id)
		return -EINVAL;

	mutex_lock(&session->lock);
	vm = xa_load(&session->vms, client_vm_id);
	if (!vm) {
		mutex_unlock(&session->lock);
		return -EINVAL;
	}
	xa_for_each(&session->groups, index, group) {
		if (group->client_vm_id == client_vm_id) {
			mutex_unlock(&session->lock);
			return -EBUSY;
		}
	}
	xa_for_each(&session->heaps, index, heap) {
		if (heap->client_vm_id == client_vm_id) {
			mutex_unlock(&session->lock);
			return -EBUSY;
		}
	}

	ret = panthor_vmshm_vm_destroy(session->real_session, vm->proxy_vm_id);
	if (ret) {
		mutex_unlock(&session->lock);
		return ret;
	}
	xa_erase(&session->vms, client_vm_id);
	if (proxy_vm_id)
		*proxy_vm_id = vm->proxy_vm_id;
	mutex_unlock(&session->lock);

	kfree(vm);
	return ret;
}

static int
panthor_proxy_session_translate_vm_locked(struct panthor_proxy_session *session,
					  u32 client_vm_id,
					  u32 *proxy_vm_id)
{
	struct panthor_proxy_vm *vm;

	if (!client_vm_id) {
		if (proxy_vm_id)
			*proxy_vm_id = 0;
		return 0;
	}

	vm = xa_load(&session->vms, client_vm_id);
	if (!vm)
		return -EINVAL;

	if (proxy_vm_id)
		*proxy_vm_id = vm->proxy_vm_id;
	return 0;
}

static int
panthor_proxy_session_translate_bo_locked(struct panthor_proxy_session *session,
					  u32 client_bo_handle,
					  u32 *proxy_bo_handle)
{
	struct panthor_proxy_bo *bo;

	if (!client_bo_handle) {
		if (proxy_bo_handle)
			*proxy_bo_handle = 0;
		return 0;
	}

	bo = xa_load(&session->bos, client_bo_handle);
	if (!bo)
		return -EINVAL;

	if (proxy_bo_handle)
		*proxy_bo_handle = bo->proxy_bo_handle;
	return 0;
}

static int
panthor_proxy_session_translate_group_locked(struct panthor_proxy_session *session,
					     u32 client_group_handle,
					     u32 *proxy_group_handle)
{
	struct panthor_proxy_group *group;

	if (!client_group_handle) {
		if (proxy_group_handle)
			*proxy_group_handle = 0;
		return 0;
	}

	group = xa_load(&session->groups, client_group_handle);
	if (!group)
		return -EINVAL;

	if (proxy_group_handle)
		*proxy_group_handle = group->proxy_group_handle;
	return 0;
}

static int
panthor_proxy_session_bo_create(struct panthor_proxy_session *session,
				const struct panthor_vmshm_bo_create_req *req,
				struct panthor_vmshm_bo_create_rsp *rsp)
{
	u64 payload_size;
	struct proxy_vmshm_alloc_params payload_params = {
		.type = PROXY_VMSHM_OBJ_GPU_BO,
		.flags = PROXY_VMSHM_F_ALLOW_SG,
		.perms = PROXY_VMSHM_PERM_CPU_READ |
			 PROXY_VMSHM_PERM_CPU_WRITE |
			 PROXY_VMSHM_PERM_MMAP |
			 PROXY_VMSHM_PERM_GPU_READ |
			 PROXY_VMSHM_PERM_GPU_WRITE,
		.align = PAGE_SIZE,
	};
	struct drm_panthor_bo_create args = {
		.size = req->size,
		.flags = req->flags,
	};
	struct panthor_proxy_bo *bo;
	struct proxy_vmshm_span first;
	u32 client_bo_handle;
	int ret;

	if (!session || !req || !rsp)
		return -EINVAL;
	if (!session->owner_vmid)
		return -EINVAL;
	if (!req->size)
		return -EINVAL;
	if (req->size > U64_MAX - (PAGE_SIZE - 1))
		return -EOVERFLOW;

	payload_size = ALIGN(req->size, PAGE_SIZE);
	payload_params.size = payload_size;
	payload_params.owner_vmid = session->owner_vmid;

	bo = kzalloc(sizeof(*bo), GFP_KERNEL);
	if (!bo)
		return -ENOMEM;

	mutex_lock(&session->lock);
	ret = panthor_proxy_session_translate_vm_locked(
		session, req->client_exclusive_vm_id, &args.exclusive_vm_id);
	if (ret)
		goto err_unlock;

	bo->payload = proxy_vmshm_alloc_ext(&payload_params, GFP_KERNEL);
	if (IS_ERR(bo->payload)) {
		ret = PTR_ERR(bo->payload);
		bo->payload = NULL;
		goto err_unlock;
	}

	ret = proxy_vmshm_obj_get_segment(bo->payload, 0, &first);
	if (ret)
		goto err_free_payload;

	ret = panthor_vmshm_bo_create_from_payload(session->real_session, &args,
						   bo->payload);
	if (ret)
		goto err_free_payload;

	ret = xa_alloc(&session->bos, &client_bo_handle, bo,
		       XA_LIMIT(1, PANTHOR_PROXY_MAX_BOS_PER_SESSION),
		       GFP_KERNEL);
	if (ret)
		goto err_destroy_proxy_bo;

	bo->client_bo_handle = client_bo_handle;
	bo->proxy_bo_handle = args.handle;
	bo->payload_handle = proxy_vmshm_obj_handle(bo->payload);
	bo->payload_offset = first.offset;
	bo->payload_size = proxy_vmshm_obj_alloc_size(bo->payload);

	rsp->client_bo_handle = client_bo_handle;
	rsp->proxy_bo_handle = args.handle;
	rsp->size = args.size;
	rsp->payload_handle = bo->payload_handle;
	rsp->payload_offset = bo->payload_offset;
	rsp->payload_size = bo->payload_size;
	mutex_unlock(&session->lock);
	pr_debug("panthor-proxy: BO_CREATE vmshm-backed session=%llu client_bo=%u proxy_bo=%u size=0x%llx payload=0x%llx payload_size=0x%llx\n",
			    session->session_id, client_bo_handle, args.handle,
			    args.size, bo->payload_handle, bo->payload_size);
	return 0;

err_destroy_proxy_bo:
	panthor_vmshm_bo_destroy(session->real_session, args.handle);

err_free_payload:
	proxy_vmshm_free(bo->payload);

err_unlock:
	mutex_unlock(&session->lock);
	kfree(bo);
	return ret;
}

static int
panthor_proxy_session_bo_destroy(struct panthor_proxy_session *session,
				 u32 client_bo_handle, u32 *proxy_bo_handle)
{
	struct panthor_proxy_bo *bo;
	int ret;

	if (!session || !client_bo_handle)
		return -EINVAL;

	mutex_lock(&session->lock);
	bo = xa_erase(&session->bos, client_bo_handle);
	if (!bo) {
		mutex_unlock(&session->lock);
		return -EINVAL;
	}

	ret = panthor_vmshm_bo_destroy(session->real_session,
				       bo->proxy_bo_handle);
	if (proxy_bo_handle)
		*proxy_bo_handle = bo->proxy_bo_handle;
	proxy_vmshm_free(bo->payload);
	mutex_unlock(&session->lock);

	kfree(bo);
	return ret;
}

static int
panthor_proxy_session_group_create(
	struct panthor_proxy_session *session,
	const struct panthor_vmshm_group_create_req *req,
	struct panthor_vmshm_group_create_rsp *rsp)
{
	struct drm_panthor_group_create args = { 0 };
	struct panthor_proxy_group *group;
	struct panthor_proxy_vm *vm;
	u32 client_group_handle;
	int ret;

	if (!session || !req || !rsp || !req->client_vm_id)
		return -EINVAL;
	if (!req->queue_count ||
	    req->queue_count > PANTHOR_VMSHM_MAX_GROUP_QUEUES)
		return -EINVAL;
	if (req->pad)
		return -EINVAL;
	if (req->priority > PANTHOR_GROUP_PRIORITY_MEDIUM)
		return -EACCES;

	group = kzalloc(sizeof(*group), GFP_KERNEL);
	if (!group)
		return -ENOMEM;

	args.queues.count = req->queue_count;
	args.max_compute_cores = req->max_compute_cores;
	args.max_fragment_cores = req->max_fragment_cores;
	args.max_tiler_cores = req->max_tiler_cores;
	args.priority = req->priority;
	args.compute_core_mask = req->compute_core_mask;
	args.fragment_core_mask = req->fragment_core_mask;
	args.tiler_core_mask = req->tiler_core_mask;
	panthor_proxy_apply_group_core_partition(session, &args);

	mutex_lock(&session->lock);
	vm = xa_load(&session->vms, req->client_vm_id);
	if (!vm) {
		ret = -EINVAL;
		goto err_unlock;
	}
	if (vm->isolated && vm->group_created) {
		ret = -EACCES;
		goto err_unlock;
	}
	args.vm_id = vm->proxy_vm_id;

	ret = panthor_vmshm_group_create(session->real_session, &args,
					 req->queues);
	if (ret)
		goto err_unlock;

	ret = xa_alloc(&session->groups, &client_group_handle, group,
		       XA_LIMIT(1, PANTHOR_PROXY_MAX_GROUPS_PER_SESSION),
		       GFP_KERNEL);
	if (ret)
		goto err_destroy_proxy_group;

	group->client_group_handle = client_group_handle;
	group->proxy_group_handle = args.group_handle;
	group->client_vm_id = req->client_vm_id;
	group->proxy_vm_id = args.vm_id;
	vm->group_created = true;

	rsp->client_group_handle = client_group_handle;
	rsp->proxy_group_handle = args.group_handle;
	rsp->proxy_vm_id = args.vm_id;
	mutex_unlock(&session->lock);
	return 0;

err_destroy_proxy_group:
	panthor_vmshm_group_destroy(session->real_session, args.group_handle);

err_unlock:
	mutex_unlock(&session->lock);
	kfree(group);
	return ret;
}

static int
panthor_proxy_session_group_destroy(struct panthor_proxy_session *session,
				    u32 client_group_handle,
				    u32 *proxy_group_handle)
{
	struct panthor_proxy_group *group;
	int ret;

	if (!session || !client_group_handle)
		return -EINVAL;

	mutex_lock(&session->lock);
	/* A recycled public handle must never bind an old executable plan. */
	panthor_proxy_plans_invalidate(session, client_group_handle, 0);
	group = xa_erase(&session->groups, client_group_handle);
	if (!group) {
		mutex_unlock(&session->lock);
		return -EINVAL;
	}

	ret = panthor_vmshm_group_destroy(session->real_session,
					  group->proxy_group_handle);
	if (proxy_group_handle)
		*proxy_group_handle = group->proxy_group_handle;
	mutex_unlock(&session->lock);

	kfree(group);
	return ret;
}

static int
panthor_proxy_session_group_get_state(
	struct panthor_proxy_session *session,
	const struct panthor_vmshm_group_get_state_req *req,
	struct panthor_vmshm_group_get_state_rsp *rsp)
{
	struct drm_panthor_group_get_state args = { 0 };
	u32 proxy_group_handle = 0;
	int ret;

	if (!session || !req || !rsp || !req->client_group_handle)
		return -EINVAL;
	if (req->pad)
		return -EINVAL;

	mutex_lock(&session->lock);
	ret = panthor_proxy_session_translate_group_locked(
		session, req->client_group_handle, &proxy_group_handle);
	if (!ret) {
		args.group_handle = proxy_group_handle;
		ret = panthor_vmshm_group_get_state(session->real_session,
						    &args);
	}
	mutex_unlock(&session->lock);

	rsp->proxy_group_handle = proxy_group_handle;
	rsp->state = args.state;
	rsp->fatal_queues = args.fatal_queues;
	return ret;
}

static int
panthor_proxy_session_tiler_heap_create(
	struct panthor_proxy_session *session,
	const struct panthor_vmshm_tiler_heap_create_req *req,
	struct panthor_vmshm_tiler_heap_create_rsp *rsp)
{
	struct drm_panthor_tiler_heap_create args = { 0 };
	struct panthor_proxy_heap *heap;
	u32 client_heap_handle;
	int ret;

	if (!session || !req || !rsp || !req->client_vm_id)
		return -EINVAL;
	if (req->pad)
		return -EINVAL;

	heap = kzalloc(sizeof(*heap), GFP_KERNEL);
	if (!heap)
		return -ENOMEM;

	args.initial_chunk_count = req->initial_chunk_count;
	args.chunk_size = req->chunk_size;
	args.max_chunks = req->max_chunks;
	args.target_in_flight = req->target_in_flight;

	mutex_lock(&session->lock);
	ret = panthor_proxy_session_translate_vm_locked(
		session, req->client_vm_id, &args.vm_id);
	if (ret)
		goto err_unlock;

	ret = panthor_vmshm_tiler_heap_create(session->real_session, &args);
	if (ret)
		goto err_unlock;

	ret = xa_alloc(&session->heaps, &client_heap_handle, heap,
		       XA_LIMIT(1, PANTHOR_PROXY_MAX_HEAPS_PER_SESSION),
		       GFP_KERNEL);
	if (ret)
		goto err_destroy_proxy_heap;

	heap->client_heap_handle = client_heap_handle;
	heap->proxy_heap_handle = args.handle;
	heap->client_vm_id = req->client_vm_id;
	heap->proxy_vm_id = args.vm_id;
	heap->tiler_heap_ctx_gpu_va = args.tiler_heap_ctx_gpu_va;
	heap->first_heap_chunk_gpu_va = args.first_heap_chunk_gpu_va;

	rsp->client_heap_handle = client_heap_handle;
	rsp->proxy_heap_handle = args.handle;
	rsp->proxy_vm_id = args.vm_id;
	rsp->tiler_heap_ctx_gpu_va = args.tiler_heap_ctx_gpu_va;
	rsp->first_heap_chunk_gpu_va = args.first_heap_chunk_gpu_va;
	mutex_unlock(&session->lock);
	return 0;

err_destroy_proxy_heap:
	panthor_vmshm_tiler_heap_destroy(session->real_session, args.handle);

err_unlock:
	mutex_unlock(&session->lock);
	kfree(heap);
	return ret;
}

static int
panthor_proxy_session_tiler_heap_destroy(struct panthor_proxy_session *session,
					 u32 client_heap_handle,
					 u32 *proxy_heap_handle)
{
	struct panthor_proxy_heap *heap;
	int ret;

	if (!session || !client_heap_handle)
		return -EINVAL;

	mutex_lock(&session->lock);
	heap = xa_erase(&session->heaps, client_heap_handle);
	if (!heap) {
		mutex_unlock(&session->lock);
		return -EINVAL;
	}

	ret = panthor_vmshm_tiler_heap_destroy(session->real_session,
					       heap->proxy_heap_handle);
	if (proxy_heap_handle)
		*proxy_heap_handle = heap->proxy_heap_handle;
	mutex_unlock(&session->lock);

	kfree(heap);
	return ret;
}

static int
panthor_proxy_session_syncobj_create(
	struct panthor_proxy_session *session,
	const struct panthor_vmshm_syncobj_create_req *req,
	struct panthor_vmshm_syncobj_create_rsp *rsp, bool private_snapshot)
{
	struct panthor_proxy_syncobj *syncobj;
	u32 client_syncobj_handle;
	u32 proxy_syncobj_handle = 0;
	int ret;

	if (!session || !req || !rsp)
		return -EINVAL;
	if (req->flags & ~DRM_SYNCOBJ_CREATE_SIGNALED)
		return -EINVAL;

	syncobj = kzalloc(sizeof(*syncobj), GFP_KERNEL);
	if (!syncobj)
		return -ENOMEM;

	mutex_lock(&session->lock);
	ret = panthor_vmshm_syncobj_create(session->real_session, req->flags,
					   &proxy_syncobj_handle);
	if (ret)
		goto err_unlock;

	syncobj->private_snapshot = private_snapshot;
	ret = xa_alloc(&session->syncobjs, &client_syncobj_handle, syncobj,
		       XA_LIMIT(1, PANTHOR_PROXY_MAX_SYNCOBJS_PER_SESSION),
		       GFP_KERNEL);
	if (ret)
		goto err_destroy_proxy_syncobj;

	syncobj->client_syncobj_handle = client_syncobj_handle;
	syncobj->proxy_syncobj_handle = proxy_syncobj_handle;
	refcount_set(&syncobj->refcnt, 1);
	rsp->client_syncobj_handle = client_syncobj_handle;
	rsp->proxy_syncobj_handle = proxy_syncobj_handle;
	mutex_unlock(&session->lock);
	return 0;

err_destroy_proxy_syncobj:
	panthor_vmshm_syncobj_destroy(session->real_session,
				      proxy_syncobj_handle);

err_unlock:
	mutex_unlock(&session->lock);
	kfree(syncobj);
	return ret;
}

static int
panthor_proxy_session_syncobj_destroy(struct panthor_proxy_session *session,
				      u32 client_syncobj_handle,
				      u32 *proxy_syncobj_handle)
{
	struct panthor_proxy_syncobj *syncobj;

	if (!session || !client_syncobj_handle)
		return -EINVAL;

	mutex_lock(&session->lock);
	panthor_proxy_plans_invalidate(session, 0, client_syncobj_handle);
	syncobj = xa_erase(&session->syncobjs, client_syncobj_handle);
	if (!syncobj) {
		mutex_unlock(&session->lock);
		return -EINVAL;
	}

	if (proxy_syncobj_handle)
		*proxy_syncobj_handle = syncobj->proxy_syncobj_handle;
	syncobj->destroy_pending = true;
	mutex_unlock(&session->lock);

	panthor_proxy_syncobj_put(session, syncobj);
	return 0;
}

static int
panthor_proxy_session_syncobj_wait(
	struct panthor_proxy_session *session,
	const struct panthor_vmshm_syncobj_wait_req *req,
	struct panthor_vmshm_syncobj_wait_rsp *rsp)
{
	struct panthor_proxy_syncobj *syncobjs[PANTHOR_VMSHM_MAX_SYNCOBJ_WAIT_HANDLES] = { 0 };
	u32 proxy_handles[PANTHOR_VMSHM_MAX_SYNCOBJ_WAIT_HANDLES] = { 0 };
	u32 possible_flags;
	int ret = 0;
	u32 i;

	if (!session || !req || !rsp)
		return -EINVAL;

	possible_flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL |
			 DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT |
			 DRM_SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE |
			 PANTHOR_VMSHM_SYNCOBJ_WAIT_FENCE_STATUS;
	if (req->flags & ~possible_flags)
		return -EINVAL;
	if (req->count_handles > PANTHOR_VMSHM_MAX_SYNCOBJ_WAIT_HANDLES)
		return -E2BIG;
	if ((req->flags & PANTHOR_VMSHM_SYNCOBJ_WAIT_FENCE_STATUS) &&
	    req->count_handles != 1)
		return -EINVAL;

	mutex_lock(&session->lock);
	for (i = 0; i < req->count_handles; i++) {
		struct panthor_proxy_syncobj *syncobj;

		syncobj = xa_load(&session->syncobjs, req->handles[i]);
		if (!syncobj || !panthor_proxy_syncobj_get(syncobj)) {
			ret = -ENOENT;
			goto out_unlock;
		}

		syncobjs[i] = syncobj;
		proxy_handles[i] = syncobj->proxy_syncobj_handle;
	}
	mutex_unlock(&session->lock);

	ret = panthor_vmshm_syncobj_wait(session->real_session, proxy_handles,
					 req->count_handles,
					 req->timeout_rel_nsec,
					 req->flags & ~PANTHOR_VMSHM_SYNCOBJ_WAIT_FENCE_STATUS,
					 req->deadline_rel_nsec,
					 &rsp->first_signaled);
	if (!ret && (req->flags & PANTHOR_VMSHM_SYNCOBJ_WAIT_FENCE_STATUS)) {
		int status;

		ret = panthor_vmshm_syncobj_status(session->real_session,
						 proxy_handles[0], &status);
		if (!ret)
			ret = status < 0 ? status : status ? 0 : -ETIME;
	}
	goto out_put_syncobjs;

out_unlock:
	mutex_unlock(&session->lock);

out_put_syncobjs:
	for (i = 0; i < req->count_handles; i++)
		panthor_proxy_syncobj_put(session, syncobjs[i]);

	return ret;
}

static int
panthor_proxy_session_syncobj_transfer(
	struct panthor_proxy_session *session,
	const struct panthor_vmshm_syncobj_transfer_req *req)
{
	struct panthor_proxy_syncobj *src_syncobj = NULL;
	struct panthor_proxy_syncobj *dst_syncobj = NULL;
	struct drm_syncobj_transfer args = { 0 };
	int ret = 0;

	if (!session || !req || req->pad || !req->src_handle ||
	    !req->dst_handle)
		return -EINVAL;

	mutex_lock(&session->lock);
	src_syncobj = xa_load(&session->syncobjs, req->src_handle);
	if (!src_syncobj || !panthor_proxy_syncobj_get(src_syncobj)) {
		ret = -ENOENT;
		goto out_unlock;
	}

	dst_syncobj = xa_load(&session->syncobjs, req->dst_handle);
	if (!dst_syncobj || !panthor_proxy_syncobj_get(dst_syncobj)) {
		ret = -ENOENT;
		goto out_unlock;
	}
	if (dst_syncobj->private_snapshot) {
		ret = -EPERM;
		goto out_unlock;
	}
	mutex_unlock(&session->lock);

	args.src_handle = src_syncobj->proxy_syncobj_handle;
	args.dst_handle = dst_syncobj->proxy_syncobj_handle;
	args.src_point = req->src_point;
	args.dst_point = req->dst_point;
	args.flags = req->flags;

	ret = panthor_vmshm_syncobj_transfer(session->real_session, &args);
	goto out_put_syncobjs;

out_unlock:
	mutex_unlock(&session->lock);
	goto out_put_syncobjs;

out_put_syncobjs:
	panthor_proxy_syncobj_put(session, dst_syncobj);
	panthor_proxy_syncobj_put(session, src_syncobj);
	return ret;
}

static int
panthor_proxy_session_syncobj_timeline_wait(
	struct panthor_proxy_session *session,
	const struct panthor_vmshm_syncobj_timeline_wait_req *req,
	struct panthor_vmshm_syncobj_timeline_wait_rsp *rsp)
{
	struct panthor_proxy_syncobj *syncobjs[PANTHOR_VMSHM_MAX_SYNCOBJ_WAIT_HANDLES] = { 0 };
	u32 proxy_handles[PANTHOR_VMSHM_MAX_SYNCOBJ_WAIT_HANDLES] = { 0 };
	u32 possible_flags;
	int ret = 0;
	u32 i;

	if (!session || !req || !rsp)
		return -EINVAL;

	possible_flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL |
			 DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT |
			 DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE |
			 DRM_SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE;
	if (req->flags & ~possible_flags)
		return -EINVAL;
	if (req->count_handles > PANTHOR_VMSHM_MAX_SYNCOBJ_WAIT_HANDLES)
		return -E2BIG;

	mutex_lock(&session->lock);
	for (i = 0; i < req->count_handles; i++) {
		struct panthor_proxy_syncobj *syncobj;

		syncobj = xa_load(&session->syncobjs, req->handles[i]);
		if (!syncobj || !panthor_proxy_syncobj_get(syncobj)) {
			ret = -ENOENT;
			goto out_unlock;
		}

		syncobjs[i] = syncobj;
		proxy_handles[i] = syncobj->proxy_syncobj_handle;
	}
	mutex_unlock(&session->lock);

	ret = panthor_vmshm_syncobj_timeline_wait(
		session->real_session, proxy_handles, req->points,
		req->count_handles, req->timeout_rel_nsec, req->flags,
		req->deadline_rel_nsec, &rsp->first_signaled);
	goto out_put_syncobjs;

out_unlock:
	mutex_unlock(&session->lock);

out_put_syncobjs:
	for (i = 0; i < req->count_handles; i++)
		panthor_proxy_syncobj_put(session, syncobjs[i]);

	return ret;
}

static int
panthor_proxy_session_translate_syncobj_array_locked(
	struct panthor_proxy_session *session, const u32 *client_handles,
	u32 count_handles, struct panthor_proxy_syncobj **syncobjs,
	u32 *proxy_handles)
{
	u32 i;

	if ((count_handles && (!client_handles || !syncobjs || !proxy_handles)) ||
	    count_handles > PANTHOR_VMSHM_MAX_SYNCOBJ_WAIT_HANDLES)
		return -EINVAL;

	for (i = 0; i < count_handles; i++) {
		struct panthor_proxy_syncobj *syncobj;

		syncobj = xa_load(&session->syncobjs, client_handles[i]);
		if (!syncobj || !panthor_proxy_syncobj_get(syncobj)) {
			while (i--) {
				panthor_proxy_syncobj_put(session, syncobjs[i]);
				syncobjs[i] = NULL;
			}
			return -ENOENT;
		}

		syncobjs[i] = syncobj;
		proxy_handles[i] = syncobj->proxy_syncobj_handle;
	}

	return 0;
}

static void
panthor_proxy_syncobj_array_put(struct panthor_proxy_session *session,
				struct panthor_proxy_syncobj **syncobjs,
				u32 count_handles)
{
	u32 i;

	for (i = 0; i < count_handles; i++)
		panthor_proxy_syncobj_put(session, syncobjs[i]);
}

static int
panthor_proxy_session_syncobj_array_op(
	struct panthor_proxy_session *session,
	const struct panthor_vmshm_syncobj_array_req *req, bool signal)
{
	struct panthor_proxy_syncobj *syncobjs[PANTHOR_VMSHM_MAX_SYNCOBJ_WAIT_HANDLES] = { 0 };
	u32 proxy_handles[PANTHOR_VMSHM_MAX_SYNCOBJ_WAIT_HANDLES] = { 0 };
	int ret;

	if (!session || !req || req->pad)
		return -EINVAL;
	if (!req->count_handles ||
	    req->count_handles > PANTHOR_VMSHM_MAX_SYNCOBJ_WAIT_HANDLES)
		return req->count_handles ? -E2BIG : -EINVAL;

	mutex_lock(&session->lock);
	ret = panthor_proxy_session_translate_syncobj_array_locked(
		session, req->handles, req->count_handles, syncobjs,
		proxy_handles);
	mutex_unlock(&session->lock);
	if (ret)
		goto out_put_syncobjs;

	for (u32 i = 0; i < req->count_handles; i++) {
		if (syncobjs[i]->private_snapshot) {
			ret = -EPERM;
			goto out_put_syncobjs;
		}
	}
	if (signal)
		ret = panthor_vmshm_syncobj_signal(session->real_session,
						   proxy_handles,
						   req->count_handles);
	else
		ret = panthor_vmshm_syncobj_reset(session->real_session,
						  proxy_handles,
						  req->count_handles);

out_put_syncobjs:
	panthor_proxy_syncobj_array_put(session, syncobjs, req->count_handles);
	return ret;
}

static int
panthor_proxy_session_syncobj_timeline_array_op(
	struct panthor_proxy_session *session,
	const struct panthor_vmshm_syncobj_timeline_array_req *req,
	struct panthor_vmshm_syncobj_timeline_array_rsp *rsp, bool query)
{
	struct panthor_proxy_syncobj *syncobjs[PANTHOR_VMSHM_MAX_SYNCOBJ_WAIT_HANDLES] = { 0 };
	u32 proxy_handles[PANTHOR_VMSHM_MAX_SYNCOBJ_WAIT_HANDLES] = { 0 };
	u64 points[PANTHOR_VMSHM_MAX_SYNCOBJ_WAIT_HANDLES] = { 0 };
	int ret;

	if (!session || !req || !rsp)
		return -EINVAL;
	if (!req->count_handles ||
	    req->count_handles > PANTHOR_VMSHM_MAX_SYNCOBJ_WAIT_HANDLES)
		return req->count_handles ? -E2BIG : -EINVAL;
	if (!query && req->flags)
		return -EINVAL;
	if (query && (req->flags & ~DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED))
		return -EINVAL;

	mutex_lock(&session->lock);
	ret = panthor_proxy_session_translate_syncobj_array_locked(
		session, req->handles, req->count_handles, syncobjs,
		proxy_handles);
	mutex_unlock(&session->lock);
	if (ret)
		goto out_put_syncobjs;

	if (!query) {
		for (u32 i = 0; i < req->count_handles; i++) {
			if (syncobjs[i]->private_snapshot) {
				ret = -EPERM;
				goto out_put_syncobjs;
			}
		}
	}
	if (query) {
		ret = panthor_vmshm_syncobj_query(session->real_session,
						  proxy_handles, points,
						  req->count_handles,
						  req->flags);
		if (!ret)
			memcpy(rsp->points, points,
			       sizeof(points[0]) * req->count_handles);
	} else {
		ret = panthor_vmshm_syncobj_timeline_signal(
			session->real_session, proxy_handles, req->points,
			req->count_handles, req->flags);
	}

out_put_syncobjs:
	panthor_proxy_syncobj_array_put(session, syncobjs, req->count_handles);
	return ret;
}

static int
panthor_proxy_check_sync_op_fields(__u32 flags, __u64 timeline_value)
{
	u8 handle_type;

	if (flags & ~PANTHOR_PROXY_SYNC_OP_FLAGS_MASK)
		return -EINVAL;

	handle_type = flags & DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_MASK;
	if (handle_type != DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_SYNCOBJ &&
	    handle_type != DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_TIMELINE_SYNCOBJ)
		return -EINVAL;

	if (handle_type == DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_SYNCOBJ &&
	    timeline_value)
		return -EINVAL;

	return 0;
}


static int
panthor_proxy_check_vm_bind_sync_op(
	const struct panthor_vmshm_vm_bind_sync *sync_op)
{
	if (!sync_op)
		return -EINVAL;

	return panthor_proxy_check_sync_op_fields(sync_op->flags,
						 sync_op->timeline_value);
}

static int
panthor_proxy_session_vm_bind(struct panthor_proxy_session *session,
			      const struct panthor_vmshm_vm_bind_req *req,
			      struct panthor_vmshm_vm_bind_rsp *rsp)
{
	struct drm_panthor_vm_bind_op ops[PANTHOR_VMSHM_MAX_VM_BIND_OPS] = { 0 };
	struct drm_panthor_sync_op syncs[PANTHOR_VMSHM_MAX_VM_BIND_SYNCS] = { 0 };
	struct panthor_proxy_syncobj *syncobjs[PANTHOR_VMSHM_MAX_VM_BIND_SYNCS] = { 0 };
	u32 sync_starts[PANTHOR_VMSHM_MAX_VM_BIND_OPS] = { 0 };
	u32 sync_counts[PANTHOR_VMSHM_MAX_VM_BIND_OPS] = { 0 };
	u32 failed_op = PANTHOR_VMSHM_VM_BIND_FAILED_OP_NONE;
	u32 proxy_vm_id = 0;
	int ret = 0;
	struct panthor_proxy_vm *vm;
	u32 i;

	if (!session || !req || !rsp)
		return -EINVAL;
	if (req->flags & ~DRM_PANTHOR_VM_BIND_ASYNC)
		return -EINVAL;
	if (!req->client_vm_id || req->op_count > PANTHOR_VMSHM_MAX_VM_BIND_OPS)
		return -EINVAL;
	if (req->sync_count > PANTHOR_VMSHM_MAX_VM_BIND_SYNCS)
		return -E2BIG;

	mutex_lock(&session->lock);
	vm = xa_load(&session->vms, req->client_vm_id);
	if (!vm) {
		ret = -EINVAL;
		goto out_unlock;
	}
	proxy_vm_id = vm->proxy_vm_id;

	for (i = 0; i < req->op_count; i++) {
		const struct panthor_vmshm_vm_bind_op *in = &req->ops[i];
		struct drm_panthor_vm_bind_op *out = &ops[i];
		u32 type = in->flags & DRM_PANTHOR_VM_BIND_OP_TYPE_MASK;

		if (type != DRM_PANTHOR_VM_BIND_OP_TYPE_SYNC_ONLY && in->size &&
		    (in->va >= vm->user_va_range ||
		     in->size > vm->user_va_range - in->va)) {
			ret = -EINVAL;
			goto out_unlock;
		}

		out->flags = in->flags;
		out->bo_offset = in->bo_offset;
		out->va = in->va;
		out->size = in->size;
		sync_starts[i] = in->sync_start;
		sync_counts[i] = in->sync_count;

		if (in->sync_count &&
		    (!(req->flags & DRM_PANTHOR_VM_BIND_ASYNC) ||
		     in->sync_start >= req->sync_count ||
		     in->sync_count > req->sync_count ||
		     in->sync_start > req->sync_count - in->sync_count)) {
			ret = -EINVAL;
			goto out_unlock;
		}

		switch (type) {
		case DRM_PANTHOR_VM_BIND_OP_TYPE_MAP:
		{
			struct panthor_proxy_bo *bo =
				xa_load(&session->bos, in->client_bo_handle);

			if (!bo ||
			    proxy_vmshm_obj_owner_vmid(bo->payload) != session->owner_vmid ||
			    proxy_vmshm_obj_handle(bo->payload) != bo->payload_handle) {
				ret = -EACCES;
				goto out_unlock;
			}
			ret = panthor_proxy_session_translate_bo_locked(
				session, in->client_bo_handle,
				&out->bo_handle);
			if (ret)
				goto out_unlock;
			if (!out->bo_handle) {
				ret = -EINVAL;
				goto out_unlock;
			}
			break;
		}

		case DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP:
			if (in->client_bo_handle || in->bo_offset) {
				ret = -EINVAL;
				goto out_unlock;
			}
			break;

		case DRM_PANTHOR_VM_BIND_OP_TYPE_SYNC_ONLY:
			if (!(req->flags & DRM_PANTHOR_VM_BIND_ASYNC) ||
			    in->client_bo_handle || in->bo_offset ||
			    in->va || in->size || !in->sync_count) {
				ret = -EINVAL;
				goto out_unlock;
			}
			break;

		default:
			ret = -EINVAL;
			goto out_unlock;
		}
	}

	for (i = 0; i < req->sync_count; i++) {
		struct panthor_proxy_syncobj *syncobj;

		ret = panthor_proxy_check_vm_bind_sync_op(&req->syncs[i]);
		if (ret)
			goto out_unlock;

		syncobj = xa_load(&session->syncobjs,
				  req->syncs[i].client_syncobj_handle);
		if (!syncobj || !panthor_proxy_syncobj_get(syncobj)) {
			ret = -ENOENT;
			goto out_unlock;
		}

		syncobjs[i] = syncobj;
		if (syncobj->private_snapshot &&
		    (req->syncs[i].flags & DRM_PANTHOR_SYNC_OP_SIGNAL)) {
			ret = -EPERM;
			goto out_unlock;
		}
		syncs[i].flags = req->syncs[i].flags;
		syncs[i].handle = syncobj->proxy_syncobj_handle;
		syncs[i].timeline_value = req->syncs[i].timeline_value;
	}

	ret = panthor_vmshm_vm_bind(session->real_session, proxy_vm_id, ops,
				   req->op_count,
				   req->sync_count ? syncs : NULL,
				   sync_starts, sync_counts, req->sync_count,
				   req->flags, &failed_op);

out_unlock:
	mutex_unlock(&session->lock);

	for (i = 0; i < req->sync_count; i++)
		panthor_proxy_syncobj_put(session, syncobjs[i]);

	rsp->proxy_vm_id = proxy_vm_id;
	rsp->op_count = req->op_count;
	rsp->failed_op = failed_op;
	return ret;
}


static int
panthor_proxy_session_vm_get_state(struct panthor_proxy_session *session,
				   const struct panthor_vmshm_vm_get_state_req *req,
				   struct panthor_vmshm_vm_get_state_rsp *rsp)
{
	u32 proxy_vm_id = 0;
	int ret;

	if (!session || !req || !rsp || !req->client_vm_id)
		return -EINVAL;

	mutex_lock(&session->lock);
	ret = panthor_proxy_session_translate_vm_locked(session,
							req->client_vm_id,
							&proxy_vm_id);
	if (!ret)
		ret = panthor_vmshm_vm_get_state(session->real_session,
						 proxy_vm_id, &rsp->state);
	mutex_unlock(&session->lock);

	rsp->proxy_vm_id = proxy_vm_id;
	return ret;
}

static void panthor_proxy_plan_free(struct panthor_proxy_session *session,
                                    struct panthor_proxy_plan *plan)
{
	u32 i;

	if (!plan)
		return;
	for (i = 0; i < plan->shape.sync_count; i++)
		panthor_proxy_syncobj_put(session, plan->syncobjs[i]);
	kfree(plan);
}

/* Called with session->lock, or after the final session reference is gone. */
static void panthor_proxy_plans_clear(struct panthor_proxy_session *session)
{
	u32 i;

	for (i = 0; i < PANTHOR_VMSHM_PLAN_SLOTS; i++) {
		panthor_proxy_plan_free(session, session->plans[i]);
		session->plans[i] = NULL;
	}
}

static void panthor_proxy_plans_invalidate(struct panthor_proxy_session *session,
					 u32 group_handle, u32 sync_handle)
{
	u32 i, j;

	lockdep_assert_held(&session->lock);
	for (i = 0; i < PANTHOR_VMSHM_PLAN_SLOTS; i++) {
		struct panthor_proxy_plan *plan = session->plans[i];
		bool match;

		if (!plan)
			continue;
		match = group_handle && plan->shape.client_group_handle == group_handle;
		for (j = 0; sync_handle && j < plan->shape.sync_count; j++)
			match |= plan->shape.syncs[j].client_syncobj_handle == sync_handle;
		if (match) {
			panthor_proxy_plan_free(session, plan);
			session->plans[i] = NULL;
		}
	}
}

static int panthor_proxy_plan_register(struct panthor_proxy_session *session,
	const struct panthor_vmshm_plan_register_req *req,
	struct panthor_vmshm_plan_register_rsp *rsp)
{
	struct panthor_proxy_plan *plan;
	u32 i, slot;
	int ret = -EINVAL;

	if (req->slot >= PANTHOR_VMSHM_PLAN_SLOTS ||
	    !req->job_count || req->job_count > PANTHOR_VMSHM_MAX_GROUP_SUBMITS ||
	    req->sync_count > PANTHOR_VMSHM_MAX_GROUP_SUBMIT_SYNCS)
		return -EINVAL;
	slot = req->slot;
	plan = kzalloc(sizeof(*plan), GFP_KERNEL);
	if (!plan)
		return -ENOMEM;
	plan->shape = *req;
	plan->shape.slot = 0;
	mutex_lock(&session->lock);
	ret = panthor_proxy_session_translate_group_locked(session,
		req->client_group_handle, &plan->proxy_group_handle);
	if (ret)
		goto out;
	ret = -EINVAL;
	for (i = 0; i < req->job_count; i++) {
		const struct panthor_vmshm_plan_job *job = &req->jobs[i];

		if (job->pad ||
		    job->queue_index >= PANTHOR_VMSHM_MAX_GROUP_QUEUES ||
		    job->sync_start > req->sync_count ||
		    job->sync_count > req->sync_count - job->sync_start)
			goto out;
	}
	for (i = 0; i < req->sync_count; i++) {
		struct panthor_proxy_syncobj *syncobj;

		if (panthor_proxy_check_sync_op_fields(req->syncs[i].flags, 0))
			goto out;
		syncobj = xa_load(&session->syncobjs,
				  req->syncs[i].client_syncobj_handle);
		if (!syncobj || !panthor_proxy_syncobj_get(syncobj)) {
			ret = -ENOENT;
			goto out;
		}
		plan->syncobjs[i] = syncobj;
		if (syncobj->private_snapshot &&
		    (req->syncs[i].flags & DRM_PANTHOR_SYNC_OP_SIGNAL)) {
			ret = -EPERM;
			goto out;
		}
	}
	if (session->next_plan_id == U64_MAX) {
		ret = -EOVERFLOW;
		goto out;
	}
	plan->id = ++session->next_plan_id;
	panthor_proxy_plan_free(session, session->plans[slot]);
	session->plans[slot] = plan;
	session->plan_registers++;
	rsp->slot = slot;
	rsp->plan_id = plan->id;
	mutex_unlock(&session->lock);
	return 0;
out:
	mutex_unlock(&session->lock);
	panthor_proxy_plan_free(session, plan);
	return ret;
}

static int panthor_proxy_plan_execute(struct panthor_proxy_session *session,
	const struct panthor_vmshm_plan_exec_req *req,
	struct panthor_vmshm_plan_exec_watch_rsp *rsp)
{
	struct drm_panthor_queue_submit jobs[PANTHOR_VMSHM_MAX_GROUP_SUBMITS] = { 0 };
	struct drm_panthor_sync_op syncs[PANTHOR_VMSHM_MAX_GROUP_SUBMIT_SYNCS] = { 0 };
	u32 starts[PANTHOR_VMSHM_MAX_GROUP_SUBMITS] = { 0 };
	u32 counts[PANTHOR_VMSHM_MAX_GROUP_SUBMITS] = { 0 };
	struct panthor_proxy_plan *plan;
	u32 i;
	int ret;

	if (req->pad & ~PANTHOR_VMSHM_PLAN_EXEC_WATCH ||
	    req->slot >= PANTHOR_VMSHM_PLAN_SLOTS || !req->plan_id)
		return -EINVAL;
	mutex_lock(&session->lock);
	plan = session->plans[req->slot];
	if (!plan || plan->id != req->plan_id) {
		/* No dispatch has occurred; the client may register a fresh plan. */
		ret = -ESTALE;
		goto out;
	}
	for (i = 0; i < plan->shape.job_count; i++) {
		if (req->jobs[i].latest_flush & GENMASK(30, 24)) {
			ret = -EINVAL;
			goto out;
		}
		jobs[i].queue_index = plan->shape.jobs[i].queue_index;
		jobs[i].stream_addr = req->jobs[i].stream_addr;
		jobs[i].stream_size = req->jobs[i].stream_size;
		jobs[i].latest_flush = req->jobs[i].latest_flush;
		starts[i] = plan->shape.jobs[i].sync_start;
		counts[i] = plan->shape.jobs[i].sync_count;
	}
	for (i = 0; i < plan->shape.sync_count; i++) {
		ret = panthor_proxy_check_sync_op_fields(plan->shape.syncs[i].flags,
						       req->timeline_values[i]);
		if (ret)
			goto out;
		syncs[i].flags = plan->shape.syncs[i].flags;
		syncs[i].handle = plan->syncobjs[i]->proxy_syncobj_handle;
		syncs[i].timeline_value = req->timeline_values[i];
	}
	rsp->result.proxy_group_handle = plan->proxy_group_handle;
	rsp->result.job_count = plan->shape.job_count;
	rsp->result.sync_count = plan->shape.sync_count;
	ret = panthor_vmshm_group_submit(session->real_session,
		plan->proxy_group_handle, jobs, syncs, starts, counts,
		plan->shape.job_count);
	if (!ret) {
		u32 signal = plan->shape.sync_count, signals = 0;

		session->plan_runs++;
		/* Native submit has installed its output chain before returning.
		 * One optional exact-point proof; never allocate a shadow syncobj.
		 */
		if (req->pad & PANTHOR_VMSHM_PLAN_EXEC_WATCH) {
			for (i = 0; i < plan->shape.sync_count; i++) {
				if (!(syncs[i].flags & DRM_PANTHOR_SYNC_OP_SIGNAL))
					continue;
				signals++;
				signal = i;
			}
			if (signals == 1 &&
			    syncs[signal].flags == (DRM_PANTHOR_SYNC_OP_SIGNAL |
				DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_TIMELINE_SYNCOBJ) &&
			    syncs[signal].timeline_value) {
				struct panthor_vmshm_snapshot_watch_rsp proof = { 0 };

				panthor_proxy_point_watch(session, plan->syncobjs[signal],
					syncs[signal].timeline_value, &proof);
				rsp->cookie = proof.cookie;
				rsp->slot = proof.slot;
				rsp->sync_index = signal;
			}
		}
	}
	pr_debug("panthor-proxy: GROUP_SUBMIT session=%llu client_group=%u proxy_group=%u jobs=%u syncs=%u first_stream=0x%llx first_size=%u ret=%d plan=%llu\n",
		session->session_id, plan->shape.client_group_handle,
		plan->proxy_group_handle, plan->shape.job_count,
		plan->shape.sync_count, req->jobs[0].stream_addr,
		req->jobs[0].stream_size, ret, plan->id);
out:
	mutex_unlock(&session->lock);
	return ret;
}

static int
panthor_proxy_send_rsp(struct proxy_comm_vmshm_channel *channel,
		       u64 reply_to, u32 type, s32 status,
		       const void *payload, u32 len)
{
	struct vmshm_comm_tx tx = {
		.type = type,
		.reply_to = reply_to,
		.status = status,
		.payload = payload,
		.len = len,
	};
	bool stats_enabled = panthor_proxy_rpc_stats_is_enabled();
	u64 start_ns = stats_enabled ? ktime_get_ns() : 0;
	int ret, i;

	for (i = 0; i < PANTHOR_PROXY_SEND_RETRIES; i++) {
		ret = proxy_comm_vmshm_send_to_channel(channel, &tx);
		if (ret != -EAGAIN)
			break;

		usleep_range(PANTHOR_PROXY_SEND_WAIT_US,
			     PANTHOR_PROXY_SEND_WAIT_US * 2);
	}

	if (i == PANTHOR_PROXY_SEND_RETRIES)
		ret = -ETIMEDOUT;
	if (stats_enabled) {
		u64 elapsed_ns = ktime_get_ns() - start_ns;
		unsigned long flags;

		spin_lock_irqsave(&panthor_proxy_rpc_stats_lock, flags);
		panthor_proxy_rpc_stats_data.response_calls++;
		panthor_proxy_rpc_stats_data.response_errors += !!ret;
		panthor_proxy_rpc_stats_data.response_ns += elapsed_ns;
		panthor_proxy_rpc_stats_data.response_max_ns = max(
			panthor_proxy_rpc_stats_data.response_max_ns, elapsed_ns);
		spin_unlock_irqrestore(&panthor_proxy_rpc_stats_lock, flags);
	}
	return ret;
}

static void
panthor_proxy_handle_open_session(struct proxy_comm_vmshm_channel *channel,
				  const struct vmshm_comm_rx *rx,
				  const void *payload)
{
	const struct panthor_vmshm_open_session_req *req = payload;
	struct panthor_vmshm_open_session_rsp rsp = { 0 };
	u32 owner_vmid = proxy_comm_vmshm_channel_client_vmid(channel);
	int ret;

	if (req->version != PANTHOR_VMSHM_ABI_VERSION || req->flags ||
	    !owner_vmid) {
		rsp.ret = -EINVAL;
		goto out_send;
	}

	rsp.feature_bits = PANTHOR_VMSHM_FEATURE_SYNCOBJ_SNAPSHOT |
			   PANTHOR_VMSHM_FEATURE_COMPLETED_SNAPSHOT |
			   PANTHOR_VMSHM_FEATURE_TASK_PLAN |
			   PANTHOR_VMSHM_FEATURE_SNAPSHOT_COMPLETION |
			   PANTHOR_VMSHM_FEATURE_PLAN_COMPLETION |
			   PANTHOR_VMSHM_FEATURE_SNAPSHOT_RETIRE;
	ret = panthor_proxy_session_create(channel, owner_vmid, &rsp.session_id);
	if (ret)
		rsp.ret = ret;
	else
		pr_info("panthor-proxy: OPEN_SESSION session=%llu owner_vmid=%u\n",
			rsp.session_id, owner_vmid);

out_send:
	ret = panthor_proxy_send_rsp(channel, rx->seq,
				     PANTHOR_VMSHM_MSG_OPEN_SESSION_RSP,
				     rsp.ret, &rsp, sizeof(rsp));
	if (ret)
		pr_warn_ratelimited("panthor-proxy: OPEN_SESSION response send failed (%d)\n",
				    ret);
}

static void
panthor_proxy_handle_close_session(struct proxy_comm_vmshm_channel *channel,
				   const struct vmshm_comm_rx *rx,
				   const void *payload)
{
	const struct panthor_vmshm_close_session_req *req = payload;
	struct panthor_vmshm_close_session_rsp rsp = { 0 };
	u32 owner_vmid = proxy_comm_vmshm_channel_client_vmid(channel);
	int ret;

	if (req->flags || req->pad || !owner_vmid)
		rsp.ret = -EINVAL;
	else
		rsp.ret = panthor_proxy_session_destroy(req->session_id,
							owner_vmid);
	if (!rsp.ret)
		pr_info("panthor-proxy: CLOSE_SESSION session=%llu\n",
			req->session_id);

	ret = panthor_proxy_send_rsp(channel, rx->seq,
				     PANTHOR_VMSHM_MSG_CLOSE_SESSION_RSP,
				     rsp.ret, &rsp, sizeof(rsp));
	if (ret)
		pr_warn_ratelimited("panthor-proxy: CLOSE_SESSION response send failed (%d)\n",
				    ret);
}

static void
panthor_proxy_handle_dev_query(struct proxy_comm_vmshm_channel *channel,
			       const struct vmshm_comm_rx *rx,
			       const void *payload)
{
	const struct panthor_vmshm_dev_query_req *req = payload;
	struct panthor_vmshm_dev_query_rsp rsp;
	int ret;

	memset(&rsp, 0, sizeof(rsp));
	if (!panthor_proxy_session_exists(req->session_id, proxy_comm_vmshm_channel_client_vmid(channel))) {
		rsp.type = req->type;
		rsp.ret = -ENOENT;
		goto out_send;
	}

	ret = panthor_vmshm_dev_query(req, &rsp);
	if (ret) {
		memset(&rsp, 0, sizeof(rsp));
		rsp.type = req->type;
		rsp.ret = ret;
	}

out_send:
	if (!rsp.ret)
		pr_info("panthor-proxy: DEV_QUERY session=%llu type=%u size=%u data=%u\n",
			req->session_id, req->type, rsp.size, rsp.data_len);

	ret = panthor_proxy_send_rsp(channel, rx->seq,
				     PANTHOR_VMSHM_MSG_DEV_QUERY_RSP,
				     rsp.ret, &rsp, sizeof(rsp));
	if (ret)
		pr_warn_ratelimited("panthor-proxy: DEV_QUERY response send failed (%d)\n",
				    ret);
}

static void
panthor_proxy_handle_vm_create(struct proxy_comm_vmshm_channel *channel,
			       const struct vmshm_comm_rx *rx,
			       const void *payload)
{
	const struct panthor_vmshm_vm_create_req *req = payload;
	struct panthor_vmshm_vm_create_rsp rsp = { 0 };
	struct panthor_proxy_session *session;
	int ret;

	session = panthor_proxy_session_lookup(req->session_id, proxy_comm_vmshm_channel_client_vmid(channel));
	if (!session) {
		rsp.ret = -ENOENT;
		goto out_send;
	}

	ret = panthor_proxy_session_vm_create(session, req, &rsp);
	panthor_proxy_session_put(session);
	if (ret)
		rsp.ret = ret;
	else
		pr_info("panthor-proxy: VM_CREATE session=%llu client_vm=%u proxy_vm=%u user_va_range=0x%llx\n",
			req->session_id, rsp.client_vm_id, rsp.proxy_vm_id,
			rsp.user_va_range);

out_send:
	ret = panthor_proxy_send_rsp(channel, rx->seq,
				     PANTHOR_VMSHM_MSG_VM_CREATE_RSP,
				     rsp.ret, &rsp, sizeof(rsp));
	if (ret)
		pr_warn_ratelimited("panthor-proxy: VM_CREATE response send failed (%d)\n",
				    ret);
}

static void
panthor_proxy_handle_vm_destroy(struct proxy_comm_vmshm_channel *channel,
				const struct vmshm_comm_rx *rx,
				const void *payload)
{
	const struct panthor_vmshm_vm_destroy_req *req = payload;
	struct panthor_vmshm_vm_destroy_rsp rsp = { 0 };
	struct panthor_proxy_session *session;
	int ret;

	session = panthor_proxy_session_lookup(req->session_id, proxy_comm_vmshm_channel_client_vmid(channel));
	if (!session) {
		rsp.ret = -ENOENT;
		goto out_send;
	}

	rsp.ret = panthor_proxy_session_vm_destroy(session, req->client_vm_id,
						  &rsp.proxy_vm_id);
	panthor_proxy_session_put(session);
	if (!rsp.ret)
		pr_info("panthor-proxy: VM_DESTROY session=%llu client_vm=%u proxy_vm=%u\n",
			req->session_id, req->client_vm_id, rsp.proxy_vm_id);

out_send:
	ret = panthor_proxy_send_rsp(channel, rx->seq,
				     PANTHOR_VMSHM_MSG_VM_DESTROY_RSP,
				     rsp.ret, &rsp, sizeof(rsp));
	if (ret)
			pr_warn_ratelimited("panthor-proxy: VM_DESTROY response send failed (%d)\n",
					    ret);
}

static void
panthor_proxy_handle_vm_bind(struct proxy_comm_vmshm_channel *channel,
			     const struct vmshm_comm_rx *rx,
			     const void *payload)
{
	const struct panthor_vmshm_vm_bind_req *req = payload;
	struct panthor_vmshm_vm_bind_rsp rsp = {
		.failed_op = PANTHOR_VMSHM_VM_BIND_FAILED_OP_NONE,
	};
	struct panthor_proxy_session *session;
	int ret;

	session = panthor_proxy_session_lookup(req->session_id, proxy_comm_vmshm_channel_client_vmid(channel));
	if (!session) {
		rsp.ret = -ENOENT;
		goto out_send;
	}

	ret = panthor_proxy_session_vm_bind(session, req, &rsp);
	panthor_proxy_session_put(session);
	if (ret)
		rsp.ret = ret;

	pr_debug("panthor-proxy: VM_BIND session=%llu client_vm=%u proxy_vm=%u async=%u flags=0x%x ops=%u ret=%d failed_op=%u\n",
			    req->session_id, req->client_vm_id, rsp.proxy_vm_id,
			    !!(req->flags & DRM_PANTHOR_VM_BIND_ASYNC),
			    req->flags, req->op_count, rsp.ret, rsp.failed_op);

out_send:
	ret = panthor_proxy_send_rsp(channel, rx->seq,
				     PANTHOR_VMSHM_MSG_VM_BIND_RSP,
				     rsp.ret, &rsp, sizeof(rsp));
	if (ret)
		pr_warn_ratelimited("panthor-proxy: VM_BIND response send failed (%d)\n",
				    ret);
}

static void
panthor_proxy_handle_vm_get_state(struct proxy_comm_vmshm_channel *channel,
				  const struct vmshm_comm_rx *rx,
				  const void *payload)
{
	const struct panthor_vmshm_vm_get_state_req *req = payload;
	struct panthor_vmshm_vm_get_state_rsp rsp = { 0 };
	struct panthor_proxy_session *session;
	int ret;

	if (req->pad) {
		rsp.ret = -EINVAL;
		goto out_send;
	}

	session = panthor_proxy_session_lookup(req->session_id, proxy_comm_vmshm_channel_client_vmid(channel));
	if (!session) {
		rsp.ret = -ENOENT;
		goto out_send;
	}

	ret = panthor_proxy_session_vm_get_state(session, req, &rsp);
	panthor_proxy_session_put(session);
	if (ret)
		rsp.ret = ret;

	pr_info("panthor-proxy: VM_GET_STATE session=%llu client_vm=%u proxy_vm=%u state=%u ret=%d\n",
		req->session_id, req->client_vm_id, rsp.proxy_vm_id,
		rsp.state, rsp.ret);

out_send:
	ret = panthor_proxy_send_rsp(channel, rx->seq,
				     PANTHOR_VMSHM_MSG_VM_GET_STATE_RSP,
				     rsp.ret, &rsp, sizeof(rsp));
	if (ret)
		pr_warn_ratelimited("panthor-proxy: VM_GET_STATE response send failed (%d)\n",
				    ret);
}

static void
panthor_proxy_handle_bo_create(struct proxy_comm_vmshm_channel *channel,
			       const struct vmshm_comm_rx *rx,
			       const void *payload)
{
	const struct panthor_vmshm_bo_create_req *req = payload;
	struct panthor_vmshm_bo_create_rsp rsp = { 0 };
	struct panthor_proxy_session *session;
	u64 start_ns;
	int ret;

	start_ns = ktime_get_ns();
	session = panthor_proxy_session_lookup(req->session_id, proxy_comm_vmshm_channel_client_vmid(channel));
	if (!session) {
		rsp.ret = -ENOENT;
		goto out_send;
	}

	ret = panthor_proxy_session_bo_create(session, req, &rsp);
	panthor_proxy_session_put(session);
	if (ret)
		rsp.ret = ret;
	else if (ktime_get_ns() - start_ns > PANTHOR_PROXY_BO_CREATE_SLOW_NS)
		pr_warn_ratelimited("panthor-proxy: BO_CREATE slow seq=%llu session=%llu client_bo=%u proxy_bo=%u size=0x%llx payload=0x%llx payload_size=0x%llx duration_ns=%llu\n",
				    rx->seq, req->session_id,
				    rsp.client_bo_handle, rsp.proxy_bo_handle,
				    rsp.size, rsp.payload_handle,
				    rsp.payload_size,
				    ktime_get_ns() - start_ns);
	else
		pr_debug("panthor-proxy: BO_CREATE seq=%llu session=%llu client_bo=%u proxy_bo=%u size=0x%llx payload=0x%llx payload_size=0x%llx\n",
				    rx->seq, req->session_id,
				    rsp.client_bo_handle, rsp.proxy_bo_handle,
				    rsp.size, rsp.payload_handle,
				    rsp.payload_size);

out_send:
	if (rsp.ret)
		pr_warn("panthor-proxy: BO_CREATE failed seq=%llu session=%llu size=0x%llx ret=%d\n",
				    rx->seq, req->session_id, req->size,
				    rsp.ret);

	ret = panthor_proxy_send_rsp(channel, rx->seq,
				     PANTHOR_VMSHM_MSG_BO_CREATE_RSP,
				     rsp.ret, &rsp, sizeof(rsp));
	if (ret)
		pr_warn("panthor-proxy: BO_CREATE response send failed seq=%llu ret=%d\n",
				    rx->seq, ret);
}

static void
panthor_proxy_handle_bo_destroy(struct proxy_comm_vmshm_channel *channel,
				const struct vmshm_comm_rx *rx,
				const void *payload)
{
	const struct panthor_vmshm_bo_destroy_req *req = payload;
	struct panthor_vmshm_bo_destroy_rsp rsp = { 0 };
	struct panthor_proxy_session *session;
	int ret;

	if (req->pad) {
		rsp.ret = -EINVAL;
		goto out_send;
	}

	session = panthor_proxy_session_lookup(req->session_id, proxy_comm_vmshm_channel_client_vmid(channel));
	if (!session) {
		rsp.ret = -ENOENT;
		goto out_send;
	}

	rsp.ret = panthor_proxy_session_bo_destroy(session,
						  req->client_bo_handle,
						  &rsp.proxy_bo_handle);
	panthor_proxy_session_put(session);
	if (!rsp.ret)
		pr_info("panthor-proxy: BO_DESTROY session=%llu client_bo=%u proxy_bo=%u\n",
			req->session_id, req->client_bo_handle,
			rsp.proxy_bo_handle);
	else
		pr_info("panthor-proxy: BO_DESTROY session=%llu client_bo=%u ret=%d\n",
			req->session_id, req->client_bo_handle, rsp.ret);

out_send:
	ret = panthor_proxy_send_rsp(channel, rx->seq,
				     PANTHOR_VMSHM_MSG_BO_DESTROY_RSP,
				     rsp.ret, &rsp, sizeof(rsp));
	if (ret)
		pr_warn_ratelimited("panthor-proxy: BO_DESTROY response send failed (%d)\n",
				    ret);
}

/* Keep the destination private to this export, retaining the exact fence
 * captured by binary transfer even if the source is reset or destroyed later.
 */
static void panthor_proxy_handle_completion_page(
	struct proxy_comm_vmshm_channel *channel, const struct vmshm_comm_rx *rx,
	const void *payload)
{
	const struct panthor_vmshm_completion_page_req *req = payload;
	struct panthor_vmshm_completion_page_rsp rsp = { 0 };
	struct proxy_vmshm_alloc_params params = {
		.type = PROXY_VMSHM_OBJ_FENCE_PAGE,
		.flags = PROXY_VMSHM_F_CONTIG,
		.perms = PROXY_VMSHM_PERM_CPU_READ,
		.size = sizeof(struct panthor_vmshm_completion_page),
		.align = PAGE_SIZE,
	};
	struct panthor_proxy_session *session;

	session = panthor_proxy_session_lookup(req->session_id,
		proxy_comm_vmshm_channel_client_vmid(channel));
	if (!session) {
		rsp.ret = -ENOENT;
		goto out;
	}
	mutex_lock(&session->lock);
	if (!session->completion_page) {
		struct proxy_vmshm_object *obj;

		params.owner_vmid = session->owner_vmid;
		obj = proxy_vmshm_alloc_ext(&params, GFP_KERNEL);
		if (IS_ERR(obj)) {
			rsp.ret = PTR_ERR(obj);
			goto out_unlock;
		}
		memset(proxy_vmshm_obj_kva(obj), 0, params.size);
		session->completion_page = obj;
	}
	rsp.payload_handle = proxy_vmshm_obj_handle(session->completion_page);
	rsp.payload_offset = proxy_vmshm_obj_offset(session->completion_page);
	rsp.payload_size = proxy_vmshm_obj_size(session->completion_page);
out_unlock:
	mutex_unlock(&session->lock);
	panthor_proxy_session_put(session);
out:
	if (panthor_proxy_send_rsp(channel, rx->seq,
		PANTHOR_VMSHM_MSG_COMPLETION_PAGE_RSP, rsp.ret, &rsp, sizeof(rsp)))
		pr_warn_ratelimited("panthor-proxy: completion page response lost\n");
}

/* All-or-nothing validation under the session lock. Remove only private
 * snapshot names; outstanding native/plan references keep their fences alive.
 */
static int panthor_proxy_snapshot_retire(struct panthor_proxy_session *session,
					const struct panthor_vmshm_snapshot_retire_req *req)
{
	struct panthor_proxy_syncobj *objects[PANTHOR_VMSHM_RETIRE_MAX];
	u32 i, j;
	int ret = 0;

	if (req->pad || req->count > PANTHOR_VMSHM_RETIRE_MAX)
		return -EINVAL;
	mutex_lock(&session->lock);
	for (i = 0; i < req->count; i++) {
		if (!req->handles[i] || req->handles[i] == req->snapshot.source_handle) {
			ret = -EINVAL;
			goto out_unlock;
		}
		for (j = 0; j < i; j++) {
			if (req->handles[i] == req->handles[j]) {
				ret = -EINVAL;
				goto out_unlock;
			}
		}
		objects[i] = xa_load(&session->syncobjs, req->handles[i]);
		if (!objects[i] || !objects[i]->private_snapshot) {
			ret = -ENOENT;
			goto out_unlock;
		}
	}
	for (i = 0; i < req->count; i++) {
		panthor_proxy_plans_invalidate(session, 0, req->handles[i]);
		xa_erase(&session->syncobjs, req->handles[i]);
		objects[i]->destroy_pending = true;
	}
	session->snapshot_retired += req->count;
	mutex_unlock(&session->lock);
	for (i = 0; i < req->count; i++)
		panthor_proxy_syncobj_put(session, objects[i]);
	return 0;
out_unlock:
	mutex_unlock(&session->lock);
	return ret;
}

static void
panthor_proxy_handle_syncobj_snapshot(struct proxy_comm_vmshm_channel *channel,
				      const struct vmshm_comm_rx *rx,
				      const void *payload)
{
	const struct panthor_vmshm_syncobj_snapshot_req *req = payload;
	struct panthor_vmshm_syncobj_create_req create = {
		.session_id = req->session_id,
	};
	struct panthor_vmshm_snapshot_watch_rsp watched = { 0 };
	struct panthor_vmshm_syncobj_create_rsp rsp = { 0 };
	struct drm_syncobj_transfer transfer = { 0 };
	struct panthor_proxy_syncobj *source;
	struct panthor_proxy_session *session;
	int ret;

	if ((req->pad & ~PANTHOR_VMSHM_SNAPSHOT_ALLOW_COMPLETED) ||
	    !req->source_handle) {
		rsp.ret = -EINVAL;
		goto out_send;
	}
	session = panthor_proxy_session_lookup(req->session_id,
		proxy_comm_vmshm_channel_client_vmid(channel));
	if (!session) {
		rsp.ret = -ENOENT;
		goto out_send;
	}
	/* Validate and retain the source before allocating the destination: an
	 * invalid source handle must not become valid by aliasing that new ID.
	 */
	mutex_lock(&session->lock);
	source = xa_load(&session->syncobjs, req->source_handle);
	if (!source || !panthor_proxy_syncobj_get(source)) {
		mutex_unlock(&session->lock);
		rsp.ret = -ENOENT;
		goto out_session;
	}
	mutex_unlock(&session->lock);
	if (rx->type == PANTHOR_VMSHM_MSG_SNAPSHOT_RETIRE_REQ) {
		rsp.ret = panthor_proxy_snapshot_retire(session, payload);
		if (rsp.ret)
			goto out_source;
	}
	if (req->pad & PANTHOR_VMSHM_SNAPSHOT_ALLOW_COMPLETED) {
		int status;

		/* A successful signaled fence is immutable. Capturing its status
		 * here is the export's snapshot point; later source reuse cannot
		 * revoke that completion. Error/pending fences keep the real copy.
		 */
		rsp.ret = panthor_vmshm_syncobj_status(session->real_session,
						 source->proxy_syncobj_handle, &status);
		if (rsp.ret)
			goto out_source;
		if (status > 0) {
			rsp.pad = PANTHOR_VMSHM_SNAPSHOT_COMPLETED;
			goto out_source;
		}
	}
	rsp.ret = panthor_proxy_session_syncobj_create(session, &create, &rsp, true);
	if (!rsp.ret) {
		transfer.src_handle = source->proxy_syncobj_handle;
		transfer.dst_handle = rsp.proxy_syncobj_handle;
		rsp.ret = panthor_vmshm_syncobj_transfer(session->real_session,
						     &transfer);
		if (rsp.ret) {
			panthor_proxy_session_syncobj_destroy(session,
				rsp.client_syncobj_handle, NULL);
			rsp.client_syncobj_handle = 0;
			rsp.proxy_syncobj_handle = 0;
		}
	}
out_source:
	if (!rsp.ret && rsp.client_syncobj_handle &&
	    (rx->type == PANTHOR_VMSHM_MSG_SNAPSHOT_WATCH_REQ ||
	     rx->type == PANTHOR_VMSHM_MSG_SNAPSHOT_RETIRE_REQ)) {
		struct panthor_proxy_syncobj *snapshot;

		mutex_lock(&session->lock);
		snapshot = xa_load(&session->syncobjs, rsp.client_syncobj_handle);
		if (snapshot)
			panthor_proxy_snapshot_watch(session, snapshot, &watched);
		mutex_unlock(&session->lock);
	}
	panthor_proxy_syncobj_put(session, source);
out_session:
	panthor_proxy_session_put(session);

out_send:
	if (rx->type == PANTHOR_VMSHM_MSG_SNAPSHOT_WATCH_REQ ||
	    rx->type == PANTHOR_VMSHM_MSG_SNAPSHOT_RETIRE_REQ) {
		watched.snapshot = rsp;
		ret = panthor_proxy_send_rsp(channel, rx->seq,
			rx->type == PANTHOR_VMSHM_MSG_SNAPSHOT_RETIRE_REQ ?
			PANTHOR_VMSHM_MSG_SNAPSHOT_RETIRE_RSP :
			PANTHOR_VMSHM_MSG_SNAPSHOT_WATCH_RSP, rsp.ret,
			&watched, sizeof(watched));
	} else {
		ret = panthor_proxy_send_rsp(channel, rx->seq,
		PANTHOR_VMSHM_MSG_SYNCOBJ_SNAPSHOT_RSP,
		rsp.ret, &rsp, sizeof(rsp));
	}
	if (ret)
		pr_warn_ratelimited("panthor-proxy: SYNCOBJ_SNAPSHOT response send failed (%d)\n",
				    ret);
}

static void
panthor_proxy_handle_syncobj_create(struct proxy_comm_vmshm_channel *channel,
				    const struct vmshm_comm_rx *rx,
				    const void *payload)
{
	const struct panthor_vmshm_syncobj_create_req *req = payload;
	struct panthor_vmshm_syncobj_create_rsp rsp = { 0 };
	struct panthor_proxy_session *session;
	int ret;

	if (req->pad) {
		rsp.ret = -EINVAL;
		goto out_send;
	}

	session = panthor_proxy_session_lookup(req->session_id, proxy_comm_vmshm_channel_client_vmid(channel));
	if (!session) {
		rsp.ret = -ENOENT;
		goto out_send;
	}

	ret = panthor_proxy_session_syncobj_create(session, req, &rsp, false);
	panthor_proxy_session_put(session);
	if (ret)
		rsp.ret = ret;
	else
		pr_info_once("panthor-proxy: SYNCOBJ_CREATE session=%llu client_sync=%u proxy_sync=%u flags=0x%x\n",
			req->session_id, rsp.client_syncobj_handle,
			rsp.proxy_syncobj_handle, req->flags);

out_send:
	ret = panthor_proxy_send_rsp(channel, rx->seq,
				     PANTHOR_VMSHM_MSG_SYNCOBJ_CREATE_RSP,
				     rsp.ret, &rsp, sizeof(rsp));
	if (ret)
		pr_warn_ratelimited("panthor-proxy: SYNCOBJ_CREATE response send failed (%d)\n",
				    ret);
}

static void
panthor_proxy_handle_syncobj_destroy(struct proxy_comm_vmshm_channel *channel,
				     const struct vmshm_comm_rx *rx,
				     const void *payload)
{
	const struct panthor_vmshm_syncobj_destroy_req *req = payload;
	struct panthor_vmshm_syncobj_destroy_rsp rsp = { 0 };
	struct panthor_proxy_session *session;
	int ret;

	if (req->pad) {
		rsp.ret = -EINVAL;
		goto out_send;
	}

	session = panthor_proxy_session_lookup(req->session_id, proxy_comm_vmshm_channel_client_vmid(channel));
	if (!session) {
		rsp.ret = -ENOENT;
		goto out_send;
	}

	rsp.ret = panthor_proxy_session_syncobj_destroy(
		session, req->client_syncobj_handle, &rsp.proxy_syncobj_handle);
	panthor_proxy_session_put(session);
	if (rsp.ret)
		pr_info("panthor-proxy: SYNCOBJ_DESTROY session=%llu client_sync=%u ret=%d\n",
			req->session_id, req->client_syncobj_handle, rsp.ret);

out_send:
	ret = panthor_proxy_send_rsp(channel, rx->seq,
				     PANTHOR_VMSHM_MSG_SYNCOBJ_DESTROY_RSP,
				     rsp.ret, &rsp, sizeof(rsp));
	if (ret)
		pr_warn_ratelimited("panthor-proxy: SYNCOBJ_DESTROY response send failed (%d)\n",
				    ret);
}

static void
panthor_proxy_handle_syncobj_wait(struct proxy_comm_vmshm_channel *channel,
				  const struct vmshm_comm_rx *rx,
				  const void *payload)
{
	const struct panthor_vmshm_syncobj_wait_req *req = payload;
	struct panthor_vmshm_syncobj_wait_rsp rsp = {
		.first_signaled = ~0U,
	};
	struct panthor_proxy_session *session;
	int ret;

	session = panthor_proxy_session_lookup(req->session_id, proxy_comm_vmshm_channel_client_vmid(channel));
	if (!session) {
		rsp.ret = -ENOENT;
		goto out_send;
	}

	rsp.ret = panthor_proxy_session_syncobj_wait(session, req, &rsp);
	panthor_proxy_session_put(session);
	pr_debug("panthor-proxy: SYNCOBJ_WAIT session=%llu count=%u flags=0x%x first=%u ret=%d\n",
			    req->session_id, req->count_handles, req->flags,
			    rsp.first_signaled, rsp.ret);

out_send:
	ret = panthor_proxy_send_rsp(channel, rx->seq,
				     PANTHOR_VMSHM_MSG_SYNCOBJ_WAIT_RSP,
				     rsp.ret, &rsp, sizeof(rsp));
	if (ret)
		pr_warn_ratelimited("panthor-proxy: SYNCOBJ_WAIT response send failed (%d)\n",
				    ret);
}

static void
panthor_proxy_handle_syncobj_transfer(struct proxy_comm_vmshm_channel *channel,
				      const struct vmshm_comm_rx *rx,
				      const void *payload)
{
	const struct panthor_vmshm_syncobj_transfer_req *req = payload;
	struct panthor_vmshm_syncobj_transfer_rsp rsp = { 0 };
	struct panthor_proxy_session *session;
	int ret;

	session = panthor_proxy_session_lookup(req->session_id, proxy_comm_vmshm_channel_client_vmid(channel));
	if (!session) {
		rsp.ret = -ENOENT;
		goto out_send;
	}

	rsp.ret = panthor_proxy_session_syncobj_transfer(session, req);
	panthor_proxy_session_put(session);
	if (rsp.ret)
		pr_warn_ratelimited("panthor-proxy: SYNCOBJ_TRANSFER failed session=%llu src=%u dst=%u ret=%d\n",
			req->session_id, req->src_handle, req->dst_handle, rsp.ret);
	else
		pr_info_once("panthor-proxy: SYNCOBJ_TRANSFER session=%llu src=%u dst=%u src_point=%llu dst_point=%llu flags=0x%x ret=%d\n",
			req->session_id, req->src_handle, req->dst_handle,
			req->src_point, req->dst_point, req->flags, rsp.ret);

out_send:
	ret = panthor_proxy_send_rsp(channel, rx->seq,
				     PANTHOR_VMSHM_MSG_SYNCOBJ_TRANSFER_RSP,
				     rsp.ret, &rsp, sizeof(rsp));
	if (ret)
		pr_warn_ratelimited("panthor-proxy: SYNCOBJ_TRANSFER response send failed (%d)\n",
				    ret);
}

static void
panthor_proxy_handle_syncobj_timeline_wait(
	struct proxy_comm_vmshm_channel *channel,
	const struct vmshm_comm_rx *rx,
	const void *payload)
{
	const struct panthor_vmshm_syncobj_timeline_wait_req *req = payload;
	struct panthor_vmshm_syncobj_timeline_wait_rsp rsp = {
		.first_signaled = ~0U,
	};
	struct panthor_proxy_session *session;
	int ret;

	session = panthor_proxy_session_lookup(req->session_id, proxy_comm_vmshm_channel_client_vmid(channel));
	if (!session) {
		rsp.ret = -ENOENT;
		goto out_send;
	}

	rsp.ret = panthor_proxy_session_syncobj_timeline_wait(session, req,
							      &rsp);
	panthor_proxy_session_put(session);
	pr_debug("panthor-proxy: SYNCOBJ_TIMELINE_WAIT session=%llu count=%u flags=0x%x first=%u ret=%d\n",
			    req->session_id, req->count_handles, req->flags,
			    rsp.first_signaled, rsp.ret);

out_send:
	ret = panthor_proxy_send_rsp(
		channel, rx->seq,
		PANTHOR_VMSHM_MSG_SYNCOBJ_TIMELINE_WAIT_RSP,
		rsp.ret, &rsp, sizeof(rsp));
	if (ret)
		pr_warn_ratelimited("panthor-proxy: SYNCOBJ_TIMELINE_WAIT response send failed (%d)\n",
				    ret);
}

static void
panthor_proxy_handle_syncobj_array_op(
	struct proxy_comm_vmshm_channel *channel,
	const struct vmshm_comm_rx *rx,
	const void *payload, u32 rsp_type, const char *name, bool signal)
{
	const struct panthor_vmshm_syncobj_array_req *req = payload;
	struct panthor_vmshm_syncobj_array_rsp rsp = { 0 };
	struct panthor_proxy_session *session;
	int ret;

	session = panthor_proxy_session_lookup(req->session_id, proxy_comm_vmshm_channel_client_vmid(channel));
	if (!session) {
		rsp.ret = -ENOENT;
		goto out_send;
	}

	rsp.ret = panthor_proxy_session_syncobj_array_op(session, req, signal);
	panthor_proxy_session_put(session);
	pr_info("panthor-proxy: %s session=%llu count=%u first=%u ret=%d\n",
		name, req->session_id, req->count_handles,
		req->count_handles ? req->handles[0] : 0, rsp.ret);

out_send:
	ret = panthor_proxy_send_rsp(channel, rx->seq, rsp_type, rsp.ret, &rsp,
				     sizeof(rsp));
	if (ret)
		pr_warn_ratelimited("panthor-proxy: %s response send failed (%d)\n",
				    name, ret);
}

static void
panthor_proxy_handle_syncobj_reset(struct proxy_comm_vmshm_channel *channel,
				   const struct vmshm_comm_rx *rx,
				   const void *payload)
{
	panthor_proxy_handle_syncobj_array_op(
		channel, rx, payload, PANTHOR_VMSHM_MSG_SYNCOBJ_RESET_RSP,
		"SYNCOBJ_RESET", false);
}

static void
panthor_proxy_handle_syncobj_signal(struct proxy_comm_vmshm_channel *channel,
				    const struct vmshm_comm_rx *rx,
				    const void *payload)
{
	panthor_proxy_handle_syncobj_array_op(
		channel, rx, payload, PANTHOR_VMSHM_MSG_SYNCOBJ_SIGNAL_RSP,
		"SYNCOBJ_SIGNAL", true);
}

static void
panthor_proxy_handle_syncobj_timeline_array_op(
	struct proxy_comm_vmshm_channel *channel,
	const struct vmshm_comm_rx *rx,
	const void *payload, u32 rsp_type, const char *name, bool query)
{
	const struct panthor_vmshm_syncobj_timeline_array_req *req = payload;
	struct panthor_vmshm_syncobj_timeline_array_rsp rsp = { 0 };
	struct panthor_proxy_session *session;
	int ret;

	session = panthor_proxy_session_lookup(req->session_id, proxy_comm_vmshm_channel_client_vmid(channel));
	if (!session) {
		rsp.ret = -ENOENT;
		goto out_send;
	}

	rsp.ret = panthor_proxy_session_syncobj_timeline_array_op(session, req,
								  &rsp, query);
	panthor_proxy_session_put(session);
	pr_info("panthor-proxy: %s session=%llu count=%u flags=0x%x first=%u ret=%d\n",
		name, req->session_id, req->count_handles, req->flags,
		req->count_handles ? req->handles[0] : 0, rsp.ret);

out_send:
	ret = panthor_proxy_send_rsp(channel, rx->seq, rsp_type, rsp.ret, &rsp,
				     sizeof(rsp));
	if (ret)
		pr_warn_ratelimited("panthor-proxy: %s response send failed (%d)\n",
				    name, ret);
}

static void
panthor_proxy_handle_syncobj_timeline_signal(
	struct proxy_comm_vmshm_channel *channel,
	const struct vmshm_comm_rx *rx,
	const void *payload)
{
	panthor_proxy_handle_syncobj_timeline_array_op(
		channel, rx, payload,
		PANTHOR_VMSHM_MSG_SYNCOBJ_TIMELINE_SIGNAL_RSP,
		"SYNCOBJ_TIMELINE_SIGNAL", false);
}

static void
panthor_proxy_handle_syncobj_query(struct proxy_comm_vmshm_channel *channel,
				   const struct vmshm_comm_rx *rx,
				   const void *payload)
{
	panthor_proxy_handle_syncobj_timeline_array_op(
		channel, rx, payload, PANTHOR_VMSHM_MSG_SYNCOBJ_QUERY_RSP,
		"SYNCOBJ_QUERY", true);
}

static void
panthor_proxy_handle_group_create(struct proxy_comm_vmshm_channel *channel,
				  const struct vmshm_comm_rx *rx,
				  const void *payload)
{
	const struct panthor_vmshm_group_create_req *req = payload;
	struct panthor_vmshm_group_create_rsp rsp = { 0 };
	struct panthor_proxy_session *session;
	int ret;

	session = panthor_proxy_session_lookup(req->session_id, proxy_comm_vmshm_channel_client_vmid(channel));
	if (!session) {
		rsp.ret = -ENOENT;
		goto out_send;
	}

	ret = panthor_proxy_session_group_create(session, req, &rsp);
	panthor_proxy_session_put(session);
	if (ret)
		rsp.ret = ret;

	pr_info("panthor-proxy: GROUP_CREATE session=%llu client_group=%u proxy_group=%u client_vm=%u proxy_vm=%u queues=%u ret=%d\n",
		req->session_id, rsp.client_group_handle,
		rsp.proxy_group_handle, req->client_vm_id, rsp.proxy_vm_id,
		req->queue_count, rsp.ret);

out_send:
	ret = panthor_proxy_send_rsp(channel, rx->seq,
				     PANTHOR_VMSHM_MSG_GROUP_CREATE_RSP,
				     rsp.ret, &rsp, sizeof(rsp));
	if (ret)
		pr_warn_ratelimited("panthor-proxy: GROUP_CREATE response send failed (%d)\n",
				    ret);
}

static void
panthor_proxy_handle_group_destroy(struct proxy_comm_vmshm_channel *channel,
				   const struct vmshm_comm_rx *rx,
				   const void *payload)
{
	const struct panthor_vmshm_group_destroy_req *req = payload;
	struct panthor_vmshm_group_destroy_rsp rsp = { 0 };
	struct panthor_proxy_session *session;
	int ret;

	if (req->pad) {
		rsp.ret = -EINVAL;
		goto out_send;
	}

	session = panthor_proxy_session_lookup(req->session_id, proxy_comm_vmshm_channel_client_vmid(channel));
	if (!session) {
		rsp.ret = -ENOENT;
		goto out_send;
	}

	rsp.ret = panthor_proxy_session_group_destroy(
		session, req->client_group_handle, &rsp.proxy_group_handle);
	panthor_proxy_session_put(session);
	pr_info("panthor-proxy: GROUP_DESTROY session=%llu client_group=%u proxy_group=%u ret=%d\n",
		req->session_id, req->client_group_handle,
		rsp.proxy_group_handle, rsp.ret);

out_send:
	ret = panthor_proxy_send_rsp(channel, rx->seq,
				     PANTHOR_VMSHM_MSG_GROUP_DESTROY_RSP,
				     rsp.ret, &rsp, sizeof(rsp));
	if (ret)
		pr_warn_ratelimited("panthor-proxy: GROUP_DESTROY response send failed (%d)\n",
				    ret);
}

static void
panthor_proxy_handle_group_get_state(struct proxy_comm_vmshm_channel *channel,
				     const struct vmshm_comm_rx *rx,
				     const void *payload)
{
	const struct panthor_vmshm_group_get_state_req *req = payload;
	struct panthor_vmshm_group_get_state_rsp rsp = { 0 };
	struct panthor_proxy_session *session;
	int ret;

	if (req->pad) {
		rsp.ret = -EINVAL;
		goto out_send;
	}

	session = panthor_proxy_session_lookup(req->session_id, proxy_comm_vmshm_channel_client_vmid(channel));
	if (!session) {
		rsp.ret = -ENOENT;
		goto out_send;
	}

	rsp.ret = panthor_proxy_session_group_get_state(session, req, &rsp);
	panthor_proxy_session_put(session);
	pr_info("panthor-proxy: GROUP_GET_STATE session=%llu client_group=%u proxy_group=%u state=0x%x fatal_queues=0x%x ret=%d\n",
		req->session_id, req->client_group_handle,
		rsp.proxy_group_handle, rsp.state, rsp.fatal_queues, rsp.ret);

out_send:
	ret = panthor_proxy_send_rsp(channel, rx->seq,
				     PANTHOR_VMSHM_MSG_GROUP_GET_STATE_RSP,
				     rsp.ret, &rsp, sizeof(rsp));
	if (ret)
			pr_warn_ratelimited("panthor-proxy: GROUP_GET_STATE response send failed (%d)\n",
					    ret);
}


static void panthor_proxy_handle_plan_register(
	struct proxy_comm_vmshm_channel *channel, const struct vmshm_comm_rx *rx,
	const void *payload)
{
	const struct panthor_vmshm_plan_register_req *req = payload;
	struct panthor_vmshm_plan_register_rsp rsp = { 0 };
	struct panthor_proxy_session *session;

	session = panthor_proxy_session_lookup(req->session_id,
		proxy_comm_vmshm_channel_client_vmid(channel));
	rsp.ret = session ? panthor_proxy_plan_register(session, req, &rsp) : -ENOENT;
	panthor_proxy_session_put(session);
	if (panthor_proxy_send_rsp(channel, rx->seq,
		PANTHOR_VMSHM_MSG_PLAN_REGISTER_RSP, rsp.ret, &rsp, sizeof(rsp)))
		pr_warn_ratelimited("panthor-proxy: PLAN_REGISTER response lost\n");
}

static void panthor_proxy_handle_plan_exec(
	struct proxy_comm_vmshm_channel *channel, const struct vmshm_comm_rx *rx,
	const void *payload)
{
	const struct panthor_vmshm_plan_exec_req *req = payload;
	struct panthor_vmshm_plan_exec_watch_rsp rsp = { 0 };
	struct panthor_proxy_session *session;

	session = panthor_proxy_session_lookup(req->session_id,
		proxy_comm_vmshm_channel_client_vmid(channel));
	rsp.result.ret = session ? panthor_proxy_submit_sched_run(session, req,
		&rsp) : -ENOENT;
	panthor_proxy_session_put(session);
	if (panthor_proxy_send_rsp(channel, rx->seq,
		PANTHOR_VMSHM_MSG_PLAN_EXEC_RSP, rsp.result.ret, &rsp, req->pad & PANTHOR_VMSHM_PLAN_EXEC_WATCH ?
			sizeof(rsp) : sizeof(rsp.result)))
		pr_warn_ratelimited("panthor-proxy: PLAN_EXEC response lost (no retry)\n");
}

static void
panthor_proxy_handle_tiler_heap_create(struct proxy_comm_vmshm_channel *channel,
				       const struct vmshm_comm_rx *rx,
				       const void *payload)
{
	const struct panthor_vmshm_tiler_heap_create_req *req = payload;
	struct panthor_vmshm_tiler_heap_create_rsp rsp = { 0 };
	struct panthor_proxy_session *session;
	int ret;

	session = panthor_proxy_session_lookup(req->session_id, proxy_comm_vmshm_channel_client_vmid(channel));
	if (!session) {
		rsp.ret = -ENOENT;
		goto out_send;
	}

	ret = panthor_proxy_session_tiler_heap_create(session, req, &rsp);
	panthor_proxy_session_put(session);
	if (ret)
		rsp.ret = ret;

	pr_info("panthor-proxy: TILER_HEAP_CREATE session=%llu client_heap=%u proxy_heap=%u client_vm=%u proxy_vm=%u ctx_va=0x%llx first_chunk_va=0x%llx ret=%d\n",
		req->session_id, rsp.client_heap_handle, rsp.proxy_heap_handle,
		req->client_vm_id, rsp.proxy_vm_id, rsp.tiler_heap_ctx_gpu_va,
		rsp.first_heap_chunk_gpu_va, rsp.ret);

out_send:
	ret = panthor_proxy_send_rsp(channel, rx->seq,
				     PANTHOR_VMSHM_MSG_TILER_HEAP_CREATE_RSP,
				     rsp.ret, &rsp, sizeof(rsp));
	if (ret)
		pr_warn_ratelimited("panthor-proxy: TILER_HEAP_CREATE response send failed (%d)\n",
				    ret);
}

static void
panthor_proxy_handle_tiler_heap_destroy(struct proxy_comm_vmshm_channel *channel,
					const struct vmshm_comm_rx *rx,
					const void *payload)
{
	const struct panthor_vmshm_tiler_heap_destroy_req *req = payload;
	struct panthor_vmshm_tiler_heap_destroy_rsp rsp = { 0 };
	struct panthor_proxy_session *session;
	int ret;

	if (req->pad) {
		rsp.ret = -EINVAL;
		goto out_send;
	}

	session = panthor_proxy_session_lookup(req->session_id, proxy_comm_vmshm_channel_client_vmid(channel));
	if (!session) {
		rsp.ret = -ENOENT;
		goto out_send;
	}

	rsp.ret = panthor_proxy_session_tiler_heap_destroy(
		session, req->client_heap_handle, &rsp.proxy_heap_handle);
	panthor_proxy_session_put(session);
	pr_info("panthor-proxy: TILER_HEAP_DESTROY session=%llu client_heap=%u proxy_heap=%u ret=%d\n",
		req->session_id, req->client_heap_handle,
		rsp.proxy_heap_handle, rsp.ret);

out_send:
	ret = panthor_proxy_send_rsp(channel, rx->seq,
				     PANTHOR_VMSHM_MSG_TILER_HEAP_DESTROY_RSP,
				     rsp.ret, &rsp, sizeof(rsp));
	if (ret)
		pr_warn_ratelimited("panthor-proxy: TILER_HEAP_DESTROY response send failed (%d)\n",
				    ret);
}

static const struct panthor_proxy_msg_handler panthor_proxy_handlers[] = {
	{
		.req_type = PANTHOR_VMSHM_MSG_SNAPSHOT_RETIRE_REQ,
		.rsp_type = PANTHOR_VMSHM_MSG_SNAPSHOT_RETIRE_RSP,
		.req_size = sizeof(struct panthor_vmshm_snapshot_retire_req),
		.name = "SNAPSHOT_RETIRE",
		.handler = panthor_proxy_handle_syncobj_snapshot,
	},
	{
		.req_type = PANTHOR_VMSHM_MSG_SYNCOBJ_SNAPSHOT_REQ,
		.rsp_type = PANTHOR_VMSHM_MSG_SYNCOBJ_SNAPSHOT_RSP,
		.req_size = sizeof(struct panthor_vmshm_syncobj_snapshot_req),
		.name = "SYNCOBJ_SNAPSHOT",
		.handler = panthor_proxy_handle_syncobj_snapshot,
	},
	{
		.req_type = PANTHOR_VMSHM_MSG_COMPLETION_PAGE_REQ,
		.rsp_type = PANTHOR_VMSHM_MSG_COMPLETION_PAGE_RSP,
		.req_size = sizeof(struct panthor_vmshm_completion_page_req),
		.name = "COMPLETION_PAGE",
		.handler = panthor_proxy_handle_completion_page,
	},
	{
		.req_type = PANTHOR_VMSHM_MSG_SNAPSHOT_WATCH_REQ,
		.rsp_type = PANTHOR_VMSHM_MSG_SNAPSHOT_WATCH_RSP,
		.req_size = sizeof(struct panthor_vmshm_syncobj_snapshot_req),
		.name = "SNAPSHOT_WATCH",
		.handler = panthor_proxy_handle_syncobj_snapshot,
	},
	{
		.req_type = PANTHOR_VMSHM_MSG_OPEN_SESSION_REQ,
		.rsp_type = PANTHOR_VMSHM_MSG_OPEN_SESSION_RSP,
		.req_size = sizeof(struct panthor_vmshm_open_session_req),
		.name = "OPEN_SESSION",
		.handler = panthor_proxy_handle_open_session,
	},
	{
		.req_type = PANTHOR_VMSHM_MSG_CLOSE_SESSION_REQ,
		.rsp_type = PANTHOR_VMSHM_MSG_CLOSE_SESSION_RSP,
		.req_size = sizeof(struct panthor_vmshm_close_session_req),
		.name = "CLOSE_SESSION",
		.handler = panthor_proxy_handle_close_session,
	},
	{
		.req_type = PANTHOR_VMSHM_MSG_DEV_QUERY_REQ,
		.rsp_type = PANTHOR_VMSHM_MSG_DEV_QUERY_RSP,
		.req_size = sizeof(struct panthor_vmshm_dev_query_req),
		.name = "DEV_QUERY",
		.handler = panthor_proxy_handle_dev_query,
	},
	{
		.req_type = PANTHOR_VMSHM_MSG_VM_CREATE_REQ,
		.rsp_type = PANTHOR_VMSHM_MSG_VM_CREATE_RSP,
		.req_size = sizeof(struct panthor_vmshm_vm_create_req),
		.name = "VM_CREATE",
		.handler = panthor_proxy_handle_vm_create,
	},
	{
		.req_type = PANTHOR_VMSHM_MSG_VM_DESTROY_REQ,
		.rsp_type = PANTHOR_VMSHM_MSG_VM_DESTROY_RSP,
		.req_size = sizeof(struct panthor_vmshm_vm_destroy_req),
		.name = "VM_DESTROY",
		.handler = panthor_proxy_handle_vm_destroy,
	},
	{
		.req_type = PANTHOR_VMSHM_MSG_VM_BIND_REQ,
		.rsp_type = PANTHOR_VMSHM_MSG_VM_BIND_RSP,
		.req_size = sizeof(struct panthor_vmshm_vm_bind_req),
		.name = "VM_BIND",
		.handler = panthor_proxy_handle_vm_bind,
	},
	{
		.req_type = PANTHOR_VMSHM_MSG_VM_GET_STATE_REQ,
		.rsp_type = PANTHOR_VMSHM_MSG_VM_GET_STATE_RSP,
		.req_size = sizeof(struct panthor_vmshm_vm_get_state_req),
		.name = "VM_GET_STATE",
		.handler = panthor_proxy_handle_vm_get_state,
	},
	{
		.req_type = PANTHOR_VMSHM_MSG_BO_CREATE_REQ,
		.rsp_type = PANTHOR_VMSHM_MSG_BO_CREATE_RSP,
		.req_size = sizeof(struct panthor_vmshm_bo_create_req),
		.name = "BO_CREATE",
		.handler = panthor_proxy_handle_bo_create,
	},
	{
		.req_type = PANTHOR_VMSHM_MSG_BO_DESTROY_REQ,
		.rsp_type = PANTHOR_VMSHM_MSG_BO_DESTROY_RSP,
		.req_size = sizeof(struct panthor_vmshm_bo_destroy_req),
		.name = "BO_DESTROY",
		.handler = panthor_proxy_handle_bo_destroy,
	},
	{
		.req_type = PANTHOR_VMSHM_MSG_SYNCOBJ_CREATE_REQ,
		.rsp_type = PANTHOR_VMSHM_MSG_SYNCOBJ_CREATE_RSP,
		.req_size = sizeof(struct panthor_vmshm_syncobj_create_req),
		.name = "SYNCOBJ_CREATE",
		.handler = panthor_proxy_handle_syncobj_create,
	},
	{
		.req_type = PANTHOR_VMSHM_MSG_SYNCOBJ_DESTROY_REQ,
		.rsp_type = PANTHOR_VMSHM_MSG_SYNCOBJ_DESTROY_RSP,
		.req_size = sizeof(struct panthor_vmshm_syncobj_destroy_req),
		.name = "SYNCOBJ_DESTROY",
		.handler = panthor_proxy_handle_syncobj_destroy,
	},
	{
		.req_type = PANTHOR_VMSHM_MSG_SYNCOBJ_WAIT_REQ,
		.rsp_type = PANTHOR_VMSHM_MSG_SYNCOBJ_WAIT_RSP,
		.req_size = sizeof(struct panthor_vmshm_syncobj_wait_req),
		.name = "SYNCOBJ_WAIT",
		.handler = panthor_proxy_handle_syncobj_wait,
	},
	{
		.req_type = PANTHOR_VMSHM_MSG_SYNCOBJ_TRANSFER_REQ,
		.rsp_type = PANTHOR_VMSHM_MSG_SYNCOBJ_TRANSFER_RSP,
		.req_size = sizeof(struct panthor_vmshm_syncobj_transfer_req),
		.name = "SYNCOBJ_TRANSFER",
		.handler = panthor_proxy_handle_syncobj_transfer,
	},
	{
		.req_type = PANTHOR_VMSHM_MSG_SYNCOBJ_TIMELINE_WAIT_REQ,
		.rsp_type = PANTHOR_VMSHM_MSG_SYNCOBJ_TIMELINE_WAIT_RSP,
		.req_size = sizeof(struct panthor_vmshm_syncobj_timeline_wait_req),
		.name = "SYNCOBJ_TIMELINE_WAIT",
		.handler = panthor_proxy_handle_syncobj_timeline_wait,
	},
	{
		.req_type = PANTHOR_VMSHM_MSG_SYNCOBJ_RESET_REQ,
		.rsp_type = PANTHOR_VMSHM_MSG_SYNCOBJ_RESET_RSP,
		.req_size = sizeof(struct panthor_vmshm_syncobj_array_req),
		.name = "SYNCOBJ_RESET",
		.handler = panthor_proxy_handle_syncobj_reset,
	},
	{
		.req_type = PANTHOR_VMSHM_MSG_SYNCOBJ_SIGNAL_REQ,
		.rsp_type = PANTHOR_VMSHM_MSG_SYNCOBJ_SIGNAL_RSP,
		.req_size = sizeof(struct panthor_vmshm_syncobj_array_req),
		.name = "SYNCOBJ_SIGNAL",
		.handler = panthor_proxy_handle_syncobj_signal,
	},
	{
		.req_type = PANTHOR_VMSHM_MSG_SYNCOBJ_TIMELINE_SIGNAL_REQ,
		.rsp_type = PANTHOR_VMSHM_MSG_SYNCOBJ_TIMELINE_SIGNAL_RSP,
		.req_size = sizeof(struct panthor_vmshm_syncobj_timeline_array_req),
		.name = "SYNCOBJ_TIMELINE_SIGNAL",
		.handler = panthor_proxy_handle_syncobj_timeline_signal,
	},
	{
		.req_type = PANTHOR_VMSHM_MSG_SYNCOBJ_QUERY_REQ,
		.rsp_type = PANTHOR_VMSHM_MSG_SYNCOBJ_QUERY_RSP,
		.req_size = sizeof(struct panthor_vmshm_syncobj_timeline_array_req),
		.name = "SYNCOBJ_QUERY",
		.handler = panthor_proxy_handle_syncobj_query,
	},
	{
		.req_type = PANTHOR_VMSHM_MSG_GROUP_CREATE_REQ,
		.rsp_type = PANTHOR_VMSHM_MSG_GROUP_CREATE_RSP,
		.req_size = sizeof(struct panthor_vmshm_group_create_req),
		.name = "GROUP_CREATE",
		.handler = panthor_proxy_handle_group_create,
	},
	{
		.req_type = PANTHOR_VMSHM_MSG_GROUP_DESTROY_REQ,
		.rsp_type = PANTHOR_VMSHM_MSG_GROUP_DESTROY_RSP,
		.req_size = sizeof(struct panthor_vmshm_group_destroy_req),
		.name = "GROUP_DESTROY",
		.handler = panthor_proxy_handle_group_destroy,
	},
	{
		.req_type = PANTHOR_VMSHM_MSG_GROUP_GET_STATE_REQ,
		.rsp_type = PANTHOR_VMSHM_MSG_GROUP_GET_STATE_RSP,
		.req_size = sizeof(struct panthor_vmshm_group_get_state_req),
		.name = "GROUP_GET_STATE",
		.handler = panthor_proxy_handle_group_get_state,
	},
	{
		.req_type = PANTHOR_VMSHM_MSG_PLAN_REGISTER_REQ,
		.rsp_type = PANTHOR_VMSHM_MSG_PLAN_REGISTER_RSP,
		.req_size = sizeof(struct panthor_vmshm_plan_register_req),
		.name = "PLAN_REGISTER",
		.handler = panthor_proxy_handle_plan_register,
	},
	{
		.req_type = PANTHOR_VMSHM_MSG_PLAN_EXEC_REQ,
		.rsp_type = PANTHOR_VMSHM_MSG_PLAN_EXEC_RSP,
		.req_size = sizeof(struct panthor_vmshm_plan_exec_req),
		.name = "PLAN_EXEC",
		.handler = panthor_proxy_handle_plan_exec,
	},
	{
		.req_type = PANTHOR_VMSHM_MSG_TILER_HEAP_CREATE_REQ,
		.rsp_type = PANTHOR_VMSHM_MSG_TILER_HEAP_CREATE_RSP,
		.req_size = sizeof(struct panthor_vmshm_tiler_heap_create_req),
		.name = "TILER_HEAP_CREATE",
		.handler = panthor_proxy_handle_tiler_heap_create,
	},
	{
		.req_type = PANTHOR_VMSHM_MSG_TILER_HEAP_DESTROY_REQ,
		.rsp_type = PANTHOR_VMSHM_MSG_TILER_HEAP_DESTROY_RSP,
		.req_size = sizeof(struct panthor_vmshm_tiler_heap_destroy_req),
		.name = "TILER_HEAP_DESTROY",
		.handler = panthor_proxy_handle_tiler_heap_destroy,
	},
};

static const struct panthor_proxy_msg_handler *
panthor_proxy_find_handler(u32 type)
{
	u32 i;

	for (i = 0; i < ARRAY_SIZE(panthor_proxy_handlers); i++) {
		if (panthor_proxy_handlers[i].req_type == type)
			return &panthor_proxy_handlers[i];
	}

	return NULL;
}

static u32
panthor_proxy_handler_index(const struct panthor_proxy_msg_handler *handler)
{
	return handler ? (u32)(handler - panthor_proxy_handlers) :
			 PANTHOR_PROXY_RPC_STATS_MAX;
}

static void
panthor_proxy_rpc_stats_dump_snapshot(const struct panthor_proxy_rpc_stats *stats)
{
	u32 i;

	if (!stats)
		return;

	pr_info("panthor-proxy: [MZH][PANTHOR_PROXY_RPC_STATS_SUMMARY] bad_rx=%llu unknown_type=%llu bad_len=%llu\n",
		stats->bad_rx, stats->unknown_type, stats->bad_len);
	if (stats->response_calls)
		pr_info("panthor-proxy: [MZH][PANTHOR_PROXY_RESPONSE_STATS] calls=%llu errors=%llu total_ns=%llu avg_ns=%llu max_ns=%llu\n",
			stats->response_calls, stats->response_errors, stats->response_ns,
			div64_u64(stats->response_ns, stats->response_calls),
			stats->response_max_ns);

	for (i = 0; i < ARRAY_SIZE(panthor_proxy_handlers) &&
		    i < PANTHOR_PROXY_RPC_STATS_MAX; i++) {
		const struct panthor_proxy_rpc_stat *stat = &stats->op[i];
		u64 avg_ns;

		if (!stat->calls)
			continue;

		avg_ns = div64_u64(stat->total_ns, stat->calls);
		pr_info("panthor-proxy: [MZH][PANTHOR_PROXY_RPC_STATS] op=%s type=0x%x calls=%llu total_ns=%llu avg_ns=%llu max_ns=%llu\n",
			panthor_proxy_handlers[i].name,
			panthor_proxy_handlers[i].req_type, stat->calls,
			stat->total_ns, avg_ns, stat->max_ns);
	}
}

static int panthor_proxy_rx_handler(const struct vmshm_comm_rx *rx, void *priv)
{
	const struct panthor_proxy_msg_handler *handler;
	u64 start_ns = 0;

	if (!rx) {
		panthor_proxy_rpc_stats_record_bad_rx();
		return -EPROTO;
	}

	handler = panthor_proxy_find_handler(rx->type);
	if (!handler) {
		panthor_proxy_rpc_stats_record_unknown_type();
		return -ENOENT;
	}

	if (rx->len != handler->req_size) {
		panthor_proxy_rpc_stats_record_bad_len();
		return -EPROTO;
	}

	if (panthor_proxy_rpc_stats_is_enabled())
		start_ns = ktime_get_ns();
	handler->handler(rx->proxy_channel, rx, rx->payload);
	if (start_ns)
		panthor_proxy_rpc_stats_record_op(
			panthor_proxy_handler_index(handler),
			ktime_get_ns() - start_ns);

	return 0;
}

static int panthor_proxy_rpc_stats_set(const char *val,
				       const struct kernel_param *kp)
{
	struct panthor_proxy_rpc_stats snapshot = { 0 };
	unsigned long flags;
	bool enabled;
	bool dump = false;
	int ret;

	ret = kstrtobool(val, &enabled);
	if (ret)
		return ret;

	spin_lock_irqsave(&panthor_proxy_rpc_stats_lock, flags);
	if (enabled && !panthor_proxy_rpc_stats_enabled) {
		memset(&panthor_proxy_rpc_stats_data, 0,
		       sizeof(panthor_proxy_rpc_stats_data));
	} else if (!enabled && panthor_proxy_rpc_stats_enabled) {
		snapshot = panthor_proxy_rpc_stats_data;
		dump = true;
	}
	WRITE_ONCE(panthor_proxy_rpc_stats_enabled, enabled);
	spin_unlock_irqrestore(&panthor_proxy_rpc_stats_lock, flags);

	if (dump)
		panthor_proxy_rpc_stats_dump_snapshot(&snapshot);

	return 0;
}

static const struct kernel_param_ops panthor_proxy_rpc_stats_param_ops = {
	.set = panthor_proxy_rpc_stats_set,
	.get = param_get_bool,
};

module_param_cb(rpc_stats, &panthor_proxy_rpc_stats_param_ops,
		&panthor_proxy_rpc_stats_enabled, 0644);
MODULE_PARM_DESC(rpc_stats,
		 "Collect aggregated vmshm RPC handler latency stats");

static int __init panthor_proxy_init(void)
{
	int ret, i;

	mutex_init(&panthor_proxy_submit_sched.lock);
	INIT_LIST_HEAD(&panthor_proxy_submit_sched.runnable_sessions);
	INIT_WORK(&panthor_proxy_submit_sched.work,
		  panthor_proxy_submit_sched_work);
	atomic_set(&panthor_proxy_submit_sched.queued, 0);

	for (i = 0; i < ARRAY_SIZE(panthor_proxy_handlers); i++) {
		ret = proxy_comm_vmshm_register_handler(
			panthor_proxy_handlers[i].req_type,
			panthor_proxy_rx_handler, NULL);
		if (ret) {
			pr_warn("panthor-proxy: handler %s register failed type=0x%x ret=%d\n",
				panthor_proxy_handlers[i].name,
				panthor_proxy_handlers[i].req_type, ret);
			goto err_unregister_handlers;
		}
	}

	pr_info("panthor-proxy: vmshm handler registered\n");
	return 0;

err_unregister_handlers:
	while (--i >= 0)
		proxy_comm_vmshm_unregister_handler(
			panthor_proxy_handlers[i].req_type,
			panthor_proxy_rx_handler, NULL);

	return ret;
}

static void __exit panthor_proxy_exit(void)
{
	int i;

	for (i = ARRAY_SIZE(panthor_proxy_handlers) - 1; i >= 0; i--)
		proxy_comm_vmshm_unregister_handler(
			panthor_proxy_handlers[i].req_type,
			panthor_proxy_rx_handler, NULL);
	panthor_proxy_sessions_destroy_all();
	cancel_work_sync(&panthor_proxy_submit_sched.work);
}

module_init(panthor_proxy_init);
module_exit(panthor_proxy_exit);

MODULE_DESCRIPTION("Panthor proxy VM vmshm bridge");
MODULE_LICENSE("GPL and additional rights");
