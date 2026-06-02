/* SPDX-License-Identifier: GPL-2.0 or MIT */
#ifndef _LINUX_PANTHOR_VMSHM_H
#define _LINUX_PANTHOR_VMSHM_H

#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/types.h>

#include <drm/panthor_drm.h>

#define PANTHOR_VMSHM_ABI_VERSION	1

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

#define PANTHOR_VMSHM_DEV_QUERY_F_DATA	(1U << 0)
#define PANTHOR_VMSHM_MAX_QUERY_DATA	sizeof(struct drm_panthor_gpu_info)

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
#endif

#endif /* _LINUX_PANTHOR_VMSHM_H */
