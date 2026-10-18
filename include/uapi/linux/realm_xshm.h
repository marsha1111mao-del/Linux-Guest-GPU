/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_REALM_XSHM_H
#define _UAPI_LINUX_REALM_XSHM_H

#include <linux/ioctl.h>
#include <linux/types.h>

enum realm_xshm_command {
	REALM_XSHM_REGISTER,
	REALM_XSHM_OFFER,
	REALM_XSHM_ACCEPT,
	REALM_XSHM_CLOSE,
	REALM_XSHM_NOTIFY,
	REALM_XSHM_ARM,
	REALM_XSHM_ACK,
	REALM_XSHM_QUERY,
	REALM_XSHM_UNREGISTER,
	REALM_XSHM_MEM_OFFER,
	REALM_XSHM_MEM_ATTACH,
	REALM_XSHM_MEM_DETACH,
	REALM_XSHM_MEM_REVOKE,
	REALM_XSHM_MEM_STATUS,
	REALM_XSHM_MEM_CONTINUE,
	/* [channel, provider endpoint, borrower endpoint, caller-local IPA]. */
	REALM_XSHM_MEM_DESCRIBE,
	/* Bind the RMM event IRQ to this Realm's REC0. */
	REALM_XSHM_IRQ_BIND,
	/* Acknowledge current events and arm atomically; recheck the queue after. */
	REALM_XSHM_ACK_ARM,
};

/* Experimental RMM ABI; REGISTER capabilities describe supported operations.
 * IRQ delivery requires CAP_EVENT_IRQ and IRQ_BIND. Ownership is Realm-wide.
 * Memory commands use sequence as the local IPA, and MEM_OFFER uses size.
 * Buffers must be pinned and prepared by a trusted Realm kernel consumer:
 * source = private DATA, target = unassigned protected RAM L3 entries.
 * This raw diagnostic interface neither allocates nor pins user buffers.
 */
struct realm_xshm_call {
	__u64 command;
	__u64 id;
	__u64 sequence;
	union {
		__u64 reserved;
		__u64 size;
	};
	__u64 status;
	__u64 result[4];
};

#define REALM_XSHM_MEMORY_ACTIVE 5U
#define REALM_XSHM_CAP_MEMORY_IDENTITY 8U
#define REALM_XSHM_CAP_EVENT_IRQ 16U
#define REALM_XSHM_CAP_ACK_ARM 32U
#define REALM_XSHM_IRQ_INTID 127U

enum realm_xshm_pool_operation {
	REALM_XSHM_POOL_SOURCE,
	REALM_XSHM_POOL_TARGET,
	REALM_XSHM_POOL_INFO,
	REALM_XSHM_POOL_READ,
	REALM_XSHM_POOL_WRITE,
	REALM_XSHM_POOL_BIND,
	REALM_XSHM_POOL_UNBIND,
	/* Queue plus GPU payload allocator, using the same authenticated grant. */
	REALM_XSHM_POOL_BIND_GPU,
};

/* Up to eight persistent pools per Realm. No DMA state transition is used:
 * these pages remain in protected IPA space. Target access requires an ACTIVE grant.
 * For existing pools, ipa selects the local pool. Zero requires a single pool,
 * except BIND/UNBIND which may resolve it from the authenticated grant. */
struct realm_xshm_pool {
	__u64 operation;
	__u64 size;
	__u64 offset;
	__u64 value;
	__u64 grant;
	__u64 ipa;
};
#define REALM_XSHM_POOL _IOWR('X', 0x02, struct realm_xshm_pool)

#define REALM_XSHM_CALL _IOWR('X', 0x01, struct realm_xshm_call)

#endif
