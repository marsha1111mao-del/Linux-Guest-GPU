/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_REALM_XSHM_H
#define _LINUX_REALM_XSHM_H

#include <uapi/linux/realm_xshm.h>
#include <linux/kconfig.h>

/* Private platform data installed by the Realm kernel after checking the
 * RMM grant. A Host device-tree node cannot create this authorization.
 * The backing stays pinned until the transport is quiesced and removed.
 */
struct realm_xshm_transport {
	u32 client_vmid;
	u64 channel;
	void (*notify)(u64 channel, u32 type, u64 sequence);
	/* RX callbacks run in hard IRQ context and may only schedule work;
	 * unsubscribe synchronizes callbacks before transport removal.
	 */
	int (*subscribe)(u64 channel, void (*receive)(void *), void *data);
	void (*unsubscribe)(u64 channel);
	/* 1 = armed, 0 = notification raced, negative = channel unavailable.
	 * The consumer must recheck its queue after arming, before sleeping.
	 */
	int (*arm)(u64 channel);
};

#if IS_ENABLED(CONFIG_REALM_XSHM)
bool realm_xshm_trace_enabled(void);
u64 realm_xshm_counter(void);
void realm_xshm_trace_receive(u64 channel, u64 sequence, u32 type,
			      u64 irq, u64 work);
void realm_xshm_trace_completion(u64 session, u64 cookie, u32 path, u32 kind,
				const u64 ticks[5]);
#else
static inline bool realm_xshm_trace_enabled(void) { return false; }
static inline u64 realm_xshm_counter(void) { return 0; }
static inline void realm_xshm_trace_receive(u64 channel, u64 sequence, u32 type,
					  u64 irq, u64 work) { }
static inline void realm_xshm_trace_completion(u64 session, u64 cookie, u32 path,
					     u32 kind, const u64 ticks[5]) { }
#endif

#endif
