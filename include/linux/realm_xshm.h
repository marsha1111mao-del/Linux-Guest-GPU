/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_REALM_XSHM_H
#define _LINUX_REALM_XSHM_H

#include <uapi/linux/realm_xshm.h>

/* Private platform data installed by the Realm kernel after checking the
 * RMM grant. A Host device-tree node cannot create this authorization.
 * The backing stays pinned until the transport is quiesced and removed.
 */
struct realm_xshm_transport {
	u32 client_vmid;
	u64 channel;
	void (*notify)(u64 channel);
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

#endif
