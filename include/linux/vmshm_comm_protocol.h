/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_VMSHM_COMM_PROTOCOL_H
#define _LINUX_VMSHM_COMM_PROTOCOL_H

#include <linux/build_bug.h>
#include <linux/stddef.h>
#include <linux/types.h>

/* Shared control-queue ABI. Both peers are currently AArch64/little-endian.
 * Addresses are window-relative offsets, never kernel virtual addresses.
 * Shared descriptors are untrusted: consumers still snapshot and validate
 * them against their private geometry and locally owned ring progress.
 */
#define PROXY_COMM_VMSHM_MAGIC		0x56534350U /* "VSCP" */
#define PROXY_COMM_VMSHM_INIT_MAGIC	0x494e4954U /* "INIT" */
#define PROXY_COMM_VMSHM_VERSION	1
#define PROXY_COMM_VMSHM_MAX_QUEUES	2
#define PROXY_COMM_VMSHM_MAX_OBJECTS	64
#define PROXY_COMM_VMSHM_QUEUE_SIZE	256
#define PROXY_COMM_VMSHM_MSG_SIZE	512
#define PROXY_COMM_VMSHM_ALIGN		64
#define PROXY_COMM_VMSHM_INIT_WAIT_US	1000
#define PROXY_COMM_VMSHM_INIT_RETRIES	1000
#define PROXY_COMM_VMSHM_QUEUE_MAGIC	0x51564350U /* "PCVQ" */
/* Receiver-owned sleep hint, never an authorization or completion proof. */
#define PROXY_COMM_VMSHM_USED_NO_NOTIFY	1U

enum proxy_comm_vmshm_status {
	PROXY_COMM_VMSHM_STATUS_RESET = 0,
	PROXY_COMM_VMSHM_STATUS_INIT = 1,
	PROXY_COMM_VMSHM_STATUS_READY = 2,
	PROXY_COMM_VMSHM_STATUS_ERROR = 3,
};

enum proxy_comm_vmshm_role {
	PROXY_COMM_VMSHM_ROLE_PROXY = 0,
	PROXY_COMM_VMSHM_ROLE_CLIENT = 1,
};

enum proxy_comm_vmshm_direction {
	PROXY_COMM_VMSHM_Q_CLIENT_TO_PROXY = 0,
	PROXY_COMM_VMSHM_Q_PROXY_TO_CLIENT = 1,
};

struct proxy_comm_vmshm_queue {
	u32 queue_obj_id;
	u32 desc_obj_id;
	u32 avail_obj_id;
	u32 used_obj_id;
	u32 msg_pool_obj_id;
	u32 queue_off;
	u32 size;
	u32 msg_size;
	u32 desc_off;
	u32 avail_off;
	u32 used_off;
	u32 msg_pool_off;
	u32 direction;
	u32 flags;
	u32 reserved;
};

struct proxy_comm_vmshm_header {
	u32 magic;
	u32 version;
	u32 header_size;
	u32 total_size;
	u32 generation;
	u32 status[2];
	u32 heap_base_off;
	u32 heap_size;
	u32 next_free_off;
	u32 object_table_off;
	u32 object_count;
	u32 object_capacity;
	u32 queue_count;
	u64 feature_bits;
	struct proxy_comm_vmshm_queue queues[PROXY_COMM_VMSHM_MAX_QUEUES];
};

struct proxy_comm_vmshm_object {
	u32 obj_id;
	u32 type;
	u32 offset;
	u32 size;
	u32 align;
	u32 flags;
	u32 generation;
	u32 reserved;
};

struct proxy_comm_vmshm_queue_object {
	u32 magic;
	u32 queue_id;
	u32 direction;
	u32 queue_size;
	u32 queue_mask;
	u32 msg_size;
	u32 desc_off;
	u32 avail_off;
	u32 used_off;
	u32 msg_pool_off;
	u32 desc_obj_id;
	u32 avail_obj_id;
	u32 used_obj_id;
	u32 msg_pool_obj_id;
	u32 flags;
	u32 reserved;
};

struct proxy_comm_vmshm_desc {
	u32 msg_off;
	u32 msg_capacity;
	u32 msg_len;
	u32 flags;
	u64 seq;
};

struct proxy_comm_vmshm_avail_ring {
	u32 idx;
	u32 flags;
	u16 ring[];
};

struct proxy_comm_vmshm_used_elem {
	u16 desc_id;
	s16 status;
	u32 len;
};

struct proxy_comm_vmshm_used_ring {
	u32 idx;
	u32 flags;
	struct proxy_comm_vmshm_used_elem ring[];
};

struct proxy_comm_vmshm_msg {
	u32 type;
	u32 flags;
	u32 len;
	s32 status;
	u64 seq;
	u64 reply_to;
	u32 reserved;
	u8 payload[];
};

/* Freeze the existing wire layout while changing the backing/notification
 * mechanism. payload starts at byte 36 despite sizeof(msg) being 40.
 */
static_assert(sizeof(struct proxy_comm_vmshm_queue) == 60);
static_assert(sizeof(struct proxy_comm_vmshm_header) == 184);
static_assert(sizeof(struct proxy_comm_vmshm_object) == 32);
static_assert(sizeof(struct proxy_comm_vmshm_queue_object) == 64);
static_assert(sizeof(struct proxy_comm_vmshm_desc) == 24);
static_assert(sizeof(struct proxy_comm_vmshm_avail_ring) == 8);
static_assert(sizeof(struct proxy_comm_vmshm_used_elem) == 8);
static_assert(sizeof(struct proxy_comm_vmshm_used_ring) == 8);
static_assert(sizeof(struct proxy_comm_vmshm_msg) == 40);
static_assert(offsetof(struct proxy_comm_vmshm_msg, payload) == 36);

#endif /* _LINUX_VMSHM_COMM_PROTOCOL_H */
