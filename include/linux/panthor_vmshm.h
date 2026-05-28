/* SPDX-License-Identifier: GPL-2.0 or MIT */
#ifndef _LINUX_PANTHOR_VMSHM_H
#define _LINUX_PANTHOR_VMSHM_H

#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/types.h>

#include <drm/panthor_drm.h>

#define PANTHOR_VMSHM_MSG_DEV_QUERY_REQ	0x50545152U /* "PTQR" */
#define PANTHOR_VMSHM_MSG_DEV_QUERY_RSP	0x50545153U /* "PTQS" */

#define PANTHOR_VMSHM_DEV_QUERY_F_DATA	(1U << 0)
#define PANTHOR_VMSHM_MAX_QUERY_DATA	sizeof(struct drm_panthor_gpu_info)

struct panthor_vmshm_dev_query_req {
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

#if IS_ENABLED(CONFIG_DRM_PANTHOR)
int panthor_vmshm_dev_query(const struct panthor_vmshm_dev_query_req *req,
			    struct panthor_vmshm_dev_query_rsp *rsp);
#else
static inline int
panthor_vmshm_dev_query(const struct panthor_vmshm_dev_query_req *req,
			struct panthor_vmshm_dev_query_rsp *rsp)
{
	if (rsp)
		rsp->ret = -ENODEV;

	return 0;
}
#endif

#endif /* _LINUX_PANTHOR_VMSHM_H */
