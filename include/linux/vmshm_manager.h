/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_VMSHM_MANAGER_H
#define _LINUX_VMSHM_MANAGER_H

#include <linux/bits.h>
#include <linux/types.h>

#define VMSHM_MANAGER_MSG_GET_OBJECT_REQ	0x564d4751U /* "VMGQ" */
#define VMSHM_MANAGER_MSG_GET_OBJECT_RSP	0x564d4752U /* "VMGR" */

#define VMSHM_MANAGER_LOOKUP_HANDLE		1
#define VMSHM_MANAGER_LOOKUP_GRANT		2

#define VMSHM_MANAGER_DESC_F_CONTIG		BIT(0)

struct vmshm_manager_desc {
	__u64 handle;
	__u32 id;
	__u32 generation;
	__u32 type;
	__u32 perms;
	__u64 offset;
	__u64 size;
	__u64 alloc_size;
	__u64 gpa;
	__u32 flags;
	__u32 nr_segments;
};

struct vmshm_manager_get_object_req {
	__u64 handle;
	__u64 grant_id;
	__u32 lookup;
	__u32 requester_vmid;
	__u32 required_perms;
	__u32 flags;
};

struct vmshm_manager_get_object_rsp {
	__s32 ret;
	__u32 reserved;
	struct vmshm_manager_desc desc;
};

#endif /* _LINUX_VMSHM_MANAGER_H */
