/* SPDX-License-Identifier: GPL-2.0 or MIT */
#ifndef _LINUX_PANTHOR_VMSHM_H
#define _LINUX_PANTHOR_VMSHM_H

#include <linux/build_bug.h>
#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/types.h>

#include <drm/panthor_drm.h>

#define PANTHOR_VMSHM_ABI_VERSION	1
#define PANTHOR_VMSHM_POC_CLIENT_VMID	1

#define PANTHOR_VMSHM_MSG_OPEN_SESSION_REQ	0x50544f52U /* "PTOR" */
#define PANTHOR_VMSHM_MSG_OPEN_SESSION_RSP	0x50544f53U /* "PTOS" */
#define PANTHOR_VMSHM_MSG_CLOSE_SESSION_REQ	0x50544352U /* "PTCR" */
#define PANTHOR_VMSHM_MSG_CLOSE_SESSION_RSP	0x50544353U /* "PTCS" */
#define PANTHOR_VMSHM_MSG_DEV_QUERY_REQ	0x50545152U /* "PTQR" */
#define PANTHOR_VMSHM_MSG_DEV_QUERY_RSP	0x50545153U /* "PTQS" */
#define PANTHOR_VMSHM_MSG_VM_CREATE_REQ	0x50545652U /* "PTVR" */
#define PANTHOR_VMSHM_MSG_VM_CREATE_RSP	0x50545653U /* "PTVS" */
#define PANTHOR_VMSHM_MSG_VM_DESTROY_REQ	0x50545752U /* "PTWR" */
#define PANTHOR_VMSHM_MSG_VM_DESTROY_RSP	0x50545753U /* "PTWS" */
#define PANTHOR_VMSHM_MSG_VM_BIND_REQ	0x50545852U /* "PTXR" */
#define PANTHOR_VMSHM_MSG_VM_BIND_RSP	0x50545853U /* "PTXS" */
#define PANTHOR_VMSHM_MSG_VM_GET_STATE_REQ	0x50545952U /* "PTYR" */
#define PANTHOR_VMSHM_MSG_VM_GET_STATE_RSP	0x50545953U /* "PTYS" */
#define PANTHOR_VMSHM_MSG_BO_CREATE_REQ	0x50544252U /* "PTBR" */
#define PANTHOR_VMSHM_MSG_BO_CREATE_RSP	0x50544253U /* "PTBS" */
#define PANTHOR_VMSHM_MSG_BO_DESTROY_REQ	0x50544452U /* "PTDR" */
#define PANTHOR_VMSHM_MSG_BO_DESTROY_RSP	0x50544453U /* "PTDS" */
#define PANTHOR_VMSHM_MSG_SYNCOBJ_CREATE_REQ	0x50545352U /* "PTSR" */
#define PANTHOR_VMSHM_MSG_SYNCOBJ_CREATE_RSP	0x50545353U /* "PTSS" */
#define PANTHOR_VMSHM_MSG_SYNCOBJ_DESTROY_REQ	0x50545452U /* "PTTR" */
#define PANTHOR_VMSHM_MSG_SYNCOBJ_DESTROY_RSP	0x50545453U /* "PTTS" */
#define PANTHOR_VMSHM_MSG_SYNCOBJ_WAIT_REQ	0x50545552U /* "PTUR" */
#define PANTHOR_VMSHM_MSG_SYNCOBJ_WAIT_RSP	0x50545553U /* "PTUS" */
#define PANTHOR_VMSHM_MSG_SYNCOBJ_TRANSFER_REQ	0x50545a52U /* "PTZR" */
#define PANTHOR_VMSHM_MSG_SYNCOBJ_TRANSFER_RSP	0x50545a53U /* "PTZS" */
#define PANTHOR_VMSHM_MSG_SYNCOBJ_TIMELINE_WAIT_REQ	0x50546152U /* "PTaR" */
#define PANTHOR_VMSHM_MSG_SYNCOBJ_TIMELINE_WAIT_RSP	0x50546153U /* "PTaS" */
#define PANTHOR_VMSHM_MSG_SYNCOBJ_RESET_REQ	0x50546252U /* "PTbR" */
#define PANTHOR_VMSHM_MSG_SYNCOBJ_RESET_RSP	0x50546253U /* "PTbS" */
#define PANTHOR_VMSHM_MSG_SYNCOBJ_SIGNAL_REQ	0x50546352U /* "PTcR" */
#define PANTHOR_VMSHM_MSG_SYNCOBJ_SIGNAL_RSP	0x50546353U /* "PTcS" */
#define PANTHOR_VMSHM_MSG_SYNCOBJ_TIMELINE_SIGNAL_REQ	0x50546452U /* "PTdR" */
#define PANTHOR_VMSHM_MSG_SYNCOBJ_TIMELINE_SIGNAL_RSP	0x50546453U /* "PTdS" */
#define PANTHOR_VMSHM_MSG_SYNCOBJ_QUERY_REQ	0x50546552U /* "PTeR" */
#define PANTHOR_VMSHM_MSG_SYNCOBJ_QUERY_RSP	0x50546553U /* "PTeS" */
#define PANTHOR_VMSHM_MSG_GROUP_CREATE_REQ	0x50546752U /* "PTgR" */
#define PANTHOR_VMSHM_MSG_GROUP_CREATE_RSP	0x50546753U /* "PTgS" */
#define PANTHOR_VMSHM_MSG_GROUP_DESTROY_REQ	0x50546852U /* "PThR" */
#define PANTHOR_VMSHM_MSG_GROUP_DESTROY_RSP	0x50546853U /* "PThS" */
#define PANTHOR_VMSHM_MSG_GROUP_GET_STATE_REQ	0x50546952U /* "PTiR" */
#define PANTHOR_VMSHM_MSG_GROUP_GET_STATE_RSP	0x50546953U /* "PTiS" */
#define PANTHOR_VMSHM_MSG_TILER_HEAP_CREATE_REQ	0x50546a52U /* "PTjR" */
#define PANTHOR_VMSHM_MSG_TILER_HEAP_CREATE_RSP	0x50546a53U /* "PTjS" */
#define PANTHOR_VMSHM_MSG_TILER_HEAP_DESTROY_REQ	0x50546b52U /* "PTkR" */
#define PANTHOR_VMSHM_MSG_TILER_HEAP_DESTROY_RSP	0x50546b53U /* "PTkS" */
#define PANTHOR_VMSHM_MSG_GROUP_SUBMIT_REQ	0x50546c52U /* "PTlR" */
#define PANTHOR_VMSHM_MSG_GROUP_SUBMIT_RSP	0x50546c53U /* "PTlS" */

#define PANTHOR_VMSHM_DEV_QUERY_F_DATA	(1U << 0)
#define PANTHOR_VMSHM_MAX_QUERY_DATA	sizeof(struct drm_panthor_gpu_info)
#define PANTHOR_VMSHM_MAX_VM_BIND_OPS	8
#define PANTHOR_VMSHM_MAX_VM_BIND_SYNCS	8
#define PANTHOR_VMSHM_MAX_SYNCOBJ_WAIT_HANDLES	16
#define PANTHOR_VMSHM_MAX_GROUP_QUEUES	16
#define PANTHOR_VMSHM_MAX_GROUP_SUBMITS	4
#define PANTHOR_VMSHM_MAX_GROUP_SUBMIT_SYNCS	16
#define PANTHOR_VMSHM_VM_BIND_FAILED_OP_NONE	((__u32)~0U)
/* vmshm_comm uses 512-byte message slots with a 36-byte transport header. */
#define PANTHOR_VMSHM_COMM_PAYLOAD_LIMIT	476

struct proxy_vmshm_object;

struct panthor_vmshm_open_session_req {
	__u32 version;
	__u32 flags;
};

struct panthor_vmshm_open_session_rsp {
	__s32 ret;
	__u32 pad;
	__u64 session_id;
	__u64 feature_bits;
};

struct panthor_vmshm_close_session_req {
	__u64 session_id;
	__u32 flags;
	__u32 pad;
};

struct panthor_vmshm_close_session_rsp {
	__s32 ret;
	__u32 pad;
};

struct panthor_vmshm_dev_query_req {
	__u64 session_id;
	__u32 type;
	__u32 size;
	__u32 flags;
	__u32 pad;
};

struct panthor_vmshm_dev_query_rsp {
	__s32 ret;
	__u32 type;
	__u32 size;
	__u32 data_len;
	__u8 data[PANTHOR_VMSHM_MAX_QUERY_DATA];
};

struct panthor_vmshm_vm_create_req {
	__u64 session_id;
	__u32 flags;
	__u32 pad;
	__u64 user_va_range;
};

struct panthor_vmshm_vm_create_rsp {
	__s32 ret;
	__u32 client_vm_id;
	__u32 proxy_vm_id;
	__u32 pad;
	__u64 user_va_range;
};

struct panthor_vmshm_vm_destroy_req {
	__u64 session_id;
	__u32 client_vm_id;
	__u32 pad;
};

struct panthor_vmshm_vm_destroy_rsp {
	__s32 ret;
	__u32 proxy_vm_id;
};

struct panthor_vmshm_vm_bind_op {
	__u32 flags;
	__u32 client_bo_handle;
	__u64 bo_offset;
	__u64 va;
	__u64 size;
	__u32 sync_start;
	__u32 sync_count;
};

struct panthor_vmshm_vm_bind_sync {
	__u32 flags;
	__u32 client_syncobj_handle;
	__u64 timeline_value;
};

struct panthor_vmshm_vm_bind_req {
	__u64 session_id;
	__u32 client_vm_id;
	__u32 flags;
	__u32 op_count;
	__u32 sync_count;
	struct panthor_vmshm_vm_bind_op ops[PANTHOR_VMSHM_MAX_VM_BIND_OPS];
	struct panthor_vmshm_vm_bind_sync syncs[PANTHOR_VMSHM_MAX_VM_BIND_SYNCS];
};

struct panthor_vmshm_vm_bind_rsp {
	__s32 ret;
	__u32 proxy_vm_id;
	__u32 op_count;
	__u32 failed_op;
};

struct panthor_vmshm_vm_get_state_req {
	__u64 session_id;
	__u32 client_vm_id;
	__u32 pad;
};

struct panthor_vmshm_vm_get_state_rsp {
	__s32 ret;
	__u32 proxy_vm_id;
	__u32 state;
	__u32 pad;
};

struct panthor_vmshm_bo_create_req {
	__u64 session_id;
	__u64 size;
	__u32 flags;
	__u32 client_exclusive_vm_id;
};

struct panthor_vmshm_bo_create_rsp {
	__s32 ret;
	__u32 client_bo_handle;
	__u32 proxy_bo_handle;
	__u32 pad;
	__u64 size;
	__u64 payload_handle;
	__u64 payload_offset;
	__u64 payload_size;
};

struct panthor_vmshm_bo_destroy_req {
	__u64 session_id;
	__u32 client_bo_handle;
	__u32 pad;
};

struct panthor_vmshm_bo_destroy_rsp {
	__s32 ret;
	__u32 proxy_bo_handle;
};

struct panthor_vmshm_syncobj_create_req {
	__u64 session_id;
	__u32 flags;
	__u32 pad;
};

struct panthor_vmshm_syncobj_create_rsp {
	__s32 ret;
	__u32 client_syncobj_handle;
	__u32 proxy_syncobj_handle;
	__u32 pad;
};

struct panthor_vmshm_syncobj_destroy_req {
	__u64 session_id;
	__u32 client_syncobj_handle;
	__u32 pad;
};

struct panthor_vmshm_syncobj_destroy_rsp {
	__s32 ret;
	__u32 proxy_syncobj_handle;
};

struct panthor_vmshm_syncobj_wait_req {
	__u64 session_id;
	__s64 timeout_rel_nsec;
	__u64 deadline_rel_nsec;
	__u32 count_handles;
	__u32 flags;
	__u32 handles[PANTHOR_VMSHM_MAX_SYNCOBJ_WAIT_HANDLES];
};

struct panthor_vmshm_syncobj_wait_rsp {
	__s32 ret;
	__u32 first_signaled;
};

struct panthor_vmshm_syncobj_transfer_req {
	__u64 session_id;
	__u32 src_handle;
	__u32 dst_handle;
	__u64 src_point;
	__u64 dst_point;
	__u32 flags;
	__u32 pad;
};

struct panthor_vmshm_syncobj_transfer_rsp {
	__s32 ret;
	__u32 pad;
};

struct panthor_vmshm_syncobj_timeline_wait_req {
	__u64 session_id;
	__s64 timeout_rel_nsec;
	__u64 deadline_rel_nsec;
	__u32 count_handles;
	__u32 flags;
	__u32 handles[PANTHOR_VMSHM_MAX_SYNCOBJ_WAIT_HANDLES];
	__u64 points[PANTHOR_VMSHM_MAX_SYNCOBJ_WAIT_HANDLES];
};

struct panthor_vmshm_syncobj_timeline_wait_rsp {
	__s32 ret;
	__u32 first_signaled;
};

struct panthor_vmshm_syncobj_array_req {
	__u64 session_id;
	__u32 count_handles;
	__u32 pad;
	__u32 handles[PANTHOR_VMSHM_MAX_SYNCOBJ_WAIT_HANDLES];
};

struct panthor_vmshm_syncobj_array_rsp {
	__s32 ret;
	__u32 pad;
};

struct panthor_vmshm_syncobj_timeline_array_req {
	__u64 session_id;
	__u32 count_handles;
	__u32 flags;
	__u32 handles[PANTHOR_VMSHM_MAX_SYNCOBJ_WAIT_HANDLES];
	__u64 points[PANTHOR_VMSHM_MAX_SYNCOBJ_WAIT_HANDLES];
};

struct panthor_vmshm_syncobj_timeline_array_rsp {
	__s32 ret;
	__u32 pad;
	__u64 points[PANTHOR_VMSHM_MAX_SYNCOBJ_WAIT_HANDLES];
};

struct panthor_vmshm_group_create_req {
	__u64 session_id;
	__u32 client_vm_id;
	__u32 queue_count;
	__u8 max_compute_cores;
	__u8 max_fragment_cores;
	__u8 max_tiler_cores;
	__u8 priority;
	__u32 pad;
	__u64 compute_core_mask;
	__u64 fragment_core_mask;
	__u64 tiler_core_mask;
	struct drm_panthor_queue_create queues[PANTHOR_VMSHM_MAX_GROUP_QUEUES];
};

struct panthor_vmshm_group_create_rsp {
	__s32 ret;
	__u32 client_group_handle;
	__u32 proxy_group_handle;
	__u32 proxy_vm_id;
};

struct panthor_vmshm_group_destroy_req {
	__u64 session_id;
	__u32 client_group_handle;
	__u32 pad;
};

struct panthor_vmshm_group_destroy_rsp {
	__s32 ret;
	__u32 proxy_group_handle;
};

struct panthor_vmshm_group_get_state_req {
	__u64 session_id;
	__u32 client_group_handle;
	__u32 pad;
};

struct panthor_vmshm_group_get_state_rsp {
	__s32 ret;
	__u32 proxy_group_handle;
	__u32 state;
	__u32 fatal_queues;
};

struct panthor_vmshm_tiler_heap_create_req {
	__u64 session_id;
	__u32 client_vm_id;
	__u32 initial_chunk_count;
	__u32 chunk_size;
	__u32 max_chunks;
	__u32 target_in_flight;
	__u32 pad;
};

struct panthor_vmshm_tiler_heap_create_rsp {
	__s32 ret;
	__u32 client_heap_handle;
	__u32 proxy_heap_handle;
	__u32 proxy_vm_id;
	__u64 tiler_heap_ctx_gpu_va;
	__u64 first_heap_chunk_gpu_va;
};

struct panthor_vmshm_tiler_heap_destroy_req {
	__u64 session_id;
	__u32 client_heap_handle;
	__u32 pad;
};

struct panthor_vmshm_tiler_heap_destroy_rsp {
	__s32 ret;
	__u32 proxy_heap_handle;
};

struct panthor_vmshm_group_submit_job {
	__u32 queue_index;
	__u32 stream_size;
	__u64 stream_addr;
	__u32 latest_flush;
	__u32 sync_start;
	__u32 sync_count;
	__u32 pad;
};

struct panthor_vmshm_group_submit_sync {
	__u32 flags;
	__u32 client_syncobj_handle;
	__u64 timeline_value;
};

struct panthor_vmshm_group_submit_req {
	__u64 session_id;
	__u32 client_group_handle;
	__u32 job_count;
	__u32 sync_count;
	__u32 pad;
	struct panthor_vmshm_group_submit_job jobs[PANTHOR_VMSHM_MAX_GROUP_SUBMITS];
	struct panthor_vmshm_group_submit_sync syncs[PANTHOR_VMSHM_MAX_GROUP_SUBMIT_SYNCS];
};

struct panthor_vmshm_group_submit_rsp {
	__s32 ret;
	__u32 proxy_group_handle;
	__u32 job_count;
	__u32 sync_count;
};

static_assert(sizeof(struct panthor_vmshm_group_create_req) <=
	      PANTHOR_VMSHM_COMM_PAYLOAD_LIMIT);
static_assert(sizeof(struct panthor_vmshm_vm_bind_req) <=
	      PANTHOR_VMSHM_COMM_PAYLOAD_LIMIT);
static_assert(sizeof(struct panthor_vmshm_group_submit_req) <=
	      PANTHOR_VMSHM_COMM_PAYLOAD_LIMIT);

struct panthor_vmshm_session;

#if IS_ENABLED(CONFIG_DRM_PANTHOR)
int panthor_vmshm_session_open(struct panthor_vmshm_session **session);
void panthor_vmshm_session_close(struct panthor_vmshm_session *session);
int panthor_vmshm_dev_query(const struct panthor_vmshm_dev_query_req *req,
			    struct panthor_vmshm_dev_query_rsp *rsp);
int panthor_vmshm_vm_create(struct panthor_vmshm_session *session,
			    struct drm_panthor_vm_create *args);
int panthor_vmshm_vm_destroy(struct panthor_vmshm_session *session,
			     __u32 vm_id);
int panthor_vmshm_vm_bind(struct panthor_vmshm_session *session, __u32 vm_id,
			  struct drm_panthor_vm_bind_op *ops, __u32 op_count,
			  const struct drm_panthor_sync_op *syncs,
			  const __u32 *sync_starts,
			  const __u32 *sync_counts, __u32 sync_count,
			  __u32 flags,
			  __u32 *failed_op);
int panthor_vmshm_vm_get_state(struct panthor_vmshm_session *session,
			       __u32 vm_id, __u32 *state);
int panthor_vmshm_bo_create(struct panthor_vmshm_session *session,
			    struct drm_panthor_bo_create *args);
int panthor_vmshm_bo_create_from_payload(struct panthor_vmshm_session *session,
					 struct drm_panthor_bo_create *args,
					 struct proxy_vmshm_object *payload);
int panthor_vmshm_bo_destroy(struct panthor_vmshm_session *session,
			     __u32 bo_handle);
int panthor_vmshm_syncobj_create(struct panthor_vmshm_session *session,
				 __u32 flags, __u32 *handle);
int panthor_vmshm_syncobj_destroy(struct panthor_vmshm_session *session,
				  __u32 handle);
int panthor_vmshm_syncobj_wait(struct panthor_vmshm_session *session,
			       const __u32 *handles, __u32 count_handles,
			       __s64 timeout_nsec, __u32 flags,
			       __u64 deadline_nsec, __u32 *first_signaled);
int panthor_vmshm_syncobj_transfer(struct panthor_vmshm_session *session,
				   struct drm_syncobj_transfer *args);
int panthor_vmshm_syncobj_timeline_wait(
	struct panthor_vmshm_session *session, const __u32 *handles,
	const __u64 *points, __u32 count_handles, __s64 timeout_nsec,
	__u32 flags, __u64 deadline_nsec, __u32 *first_signaled);
int panthor_vmshm_syncobj_reset(struct panthor_vmshm_session *session,
				const __u32 *handles, __u32 count_handles);
int panthor_vmshm_syncobj_signal(struct panthor_vmshm_session *session,
				 const __u32 *handles, __u32 count_handles);
int panthor_vmshm_syncobj_timeline_signal(
	struct panthor_vmshm_session *session, const __u32 *handles,
	const __u64 *points, __u32 count_handles, __u32 flags);
int panthor_vmshm_syncobj_query(struct panthor_vmshm_session *session,
				const __u32 *handles, __u64 *points,
				__u32 count_handles, __u32 flags);
int panthor_vmshm_group_create(struct panthor_vmshm_session *session,
			       struct drm_panthor_group_create *args,
			       const struct drm_panthor_queue_create *queues);
int panthor_vmshm_group_destroy(struct panthor_vmshm_session *session,
				__u32 group_handle);
int panthor_vmshm_group_get_state(struct panthor_vmshm_session *session,
				  struct drm_panthor_group_get_state *args);
int panthor_vmshm_group_submit(struct panthor_vmshm_session *session,
			       __u32 group_handle,
			       const struct drm_panthor_queue_submit *jobs,
			       const struct drm_panthor_sync_op *syncs,
			       const __u32 *sync_starts,
			       const __u32 *sync_counts,
			       __u32 job_count);
int panthor_vmshm_tiler_heap_create(struct panthor_vmshm_session *session,
				    struct drm_panthor_tiler_heap_create *args);
int panthor_vmshm_tiler_heap_destroy(struct panthor_vmshm_session *session,
				     __u32 heap_handle);
#else
static inline int
panthor_vmshm_session_open(struct panthor_vmshm_session **session)
{
	if (session)
		*session = NULL;

	return -ENODEV;
}

static inline void
panthor_vmshm_session_close(struct panthor_vmshm_session *session)
{
}

static inline int
panthor_vmshm_dev_query(const struct panthor_vmshm_dev_query_req *req,
			struct panthor_vmshm_dev_query_rsp *rsp)
{
	if (rsp)
		rsp->ret = -ENODEV;

	return 0;
}

static inline int
panthor_vmshm_vm_create(struct panthor_vmshm_session *session,
			struct drm_panthor_vm_create *args)
{
	return -ENODEV;
}

static inline int
panthor_vmshm_vm_destroy(struct panthor_vmshm_session *session, __u32 vm_id)
{
	return -ENODEV;
}

static inline int
panthor_vmshm_vm_bind(struct panthor_vmshm_session *session, __u32 vm_id,
		      struct drm_panthor_vm_bind_op *ops, __u32 op_count,
		      const struct drm_panthor_sync_op *syncs,
		      const __u32 *sync_starts, const __u32 *sync_counts,
		      __u32 sync_count, __u32 flags,
		      __u32 *failed_op)
{
	if (failed_op)
		*failed_op = PANTHOR_VMSHM_VM_BIND_FAILED_OP_NONE;

	return -ENODEV;
}

static inline int
panthor_vmshm_vm_get_state(struct panthor_vmshm_session *session, __u32 vm_id,
			   __u32 *state)
{
	return -ENODEV;
}

static inline int
panthor_vmshm_bo_create(struct panthor_vmshm_session *session,
			struct drm_panthor_bo_create *args)
{
	return -ENODEV;
}

static inline int
panthor_vmshm_bo_create_from_payload(struct panthor_vmshm_session *session,
				     struct drm_panthor_bo_create *args,
				     struct proxy_vmshm_object *payload)
{
	return -ENODEV;
}

static inline int
panthor_vmshm_bo_destroy(struct panthor_vmshm_session *session, __u32 bo_handle)
{
	return -ENODEV;
}

static inline int
panthor_vmshm_syncobj_create(struct panthor_vmshm_session *session,
			     __u32 flags, __u32 *handle)
{
	return -ENODEV;
}

static inline int
panthor_vmshm_syncobj_destroy(struct panthor_vmshm_session *session,
			      __u32 handle)
{
	return -ENODEV;
}

static inline int
panthor_vmshm_syncobj_wait(struct panthor_vmshm_session *session,
			   const __u32 *handles, __u32 count_handles,
			   __s64 timeout_nsec, __u32 flags,
			   __u64 deadline_nsec, __u32 *first_signaled)
{
	return -ENODEV;
}

static inline int
panthor_vmshm_syncobj_transfer(struct panthor_vmshm_session *session,
			       struct drm_syncobj_transfer *args)
{
	return -ENODEV;
}

static inline int panthor_vmshm_syncobj_timeline_wait(
	struct panthor_vmshm_session *session, const __u32 *handles,
	const __u64 *points, __u32 count_handles, __s64 timeout_nsec,
	__u32 flags, __u64 deadline_nsec, __u32 *first_signaled)
{
	return -ENODEV;
}

static inline int
panthor_vmshm_syncobj_reset(struct panthor_vmshm_session *session,
			    const __u32 *handles, __u32 count_handles)
{
	return -ENODEV;
}

static inline int
panthor_vmshm_syncobj_signal(struct panthor_vmshm_session *session,
			     const __u32 *handles, __u32 count_handles)
{
	return -ENODEV;
}

static inline int
panthor_vmshm_syncobj_timeline_signal(
	struct panthor_vmshm_session *session, const __u32 *handles,
	const __u64 *points, __u32 count_handles, __u32 flags)
{
	return -ENODEV;
}

static inline int
panthor_vmshm_syncobj_query(struct panthor_vmshm_session *session,
			    const __u32 *handles, __u64 *points,
			    __u32 count_handles, __u32 flags)
{
	return -ENODEV;
}

static inline int
panthor_vmshm_group_create(struct panthor_vmshm_session *session,
			   struct drm_panthor_group_create *args,
			   const struct drm_panthor_queue_create *queues)
{
	return -ENODEV;
}

static inline int
panthor_vmshm_group_destroy(struct panthor_vmshm_session *session,
			    __u32 group_handle)
{
	return -ENODEV;
}

static inline int
panthor_vmshm_group_get_state(struct panthor_vmshm_session *session,
			      struct drm_panthor_group_get_state *args)
{
	return -ENODEV;
}

static inline int
panthor_vmshm_group_submit(struct panthor_vmshm_session *session,
			   __u32 group_handle,
			   const struct drm_panthor_queue_submit *jobs,
			   const struct drm_panthor_sync_op *syncs,
			   const __u32 *sync_starts,
			   const __u32 *sync_counts,
			   __u32 job_count)
{
	return -ENODEV;
}

static inline int
panthor_vmshm_tiler_heap_create(struct panthor_vmshm_session *session,
				struct drm_panthor_tiler_heap_create *args)
{
	return -ENODEV;
}

static inline int
panthor_vmshm_tiler_heap_destroy(struct panthor_vmshm_session *session,
				 __u32 heap_handle)
{
	return -ENODEV;
}
#endif

#endif /* _LINUX_PANTHOR_VMSHM_H */
