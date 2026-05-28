/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_VMSHM_COMM_H
#define _LINUX_VMSHM_COMM_H

#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/types.h>

/*
 * Generic in-kernel API for vmshm control transport users.
 *
 * The comm layer does not define message semantics. @type, @flags and payload
 * are owned by the upper driver using the channel.
 */
struct vmshm_comm_tx {
	u32 type;
	u32 flags;
	u64 seq;
	u64 reply_to;
	s32 status;
	const void *payload;
	u32 len;
};

struct vmshm_comm_rx {
	u32 type;
	u32 flags;
	u64 seq;
	u64 reply_to;
	s32 status;
	void *payload;
	u32 payload_capacity;
	u32 len;
};

#define VMSHM_COMM_SELFTEST_HELLO_REQ	0x48454c4fU /* "HELO" */
#define VMSHM_COMM_SELFTEST_HELLO_RSP	0x48454c52U /* "HELR" */
#define VMSHM_COMM_SELFTEST_HELLO_PAYLOAD	"hello_world"
#define VMSHM_COMM_SELFTEST_ACK_PAYLOAD		"hello_ack"

typedef int (*proxy_comm_vmshm_rx_handler_t)(const struct vmshm_comm_rx *rx,
					     void *priv);

#if IS_ENABLED(CONFIG_PROXY_VMSHM_COMM)
bool proxy_comm_vmshm_ready(void);
u32 proxy_comm_vmshm_max_payload(void);
int proxy_comm_vmshm_send_to_client(const struct vmshm_comm_tx *tx);
int proxy_comm_vmshm_recv_from_client(struct vmshm_comm_rx *rx);
int proxy_comm_vmshm_register_handler(u32 type,
				      proxy_comm_vmshm_rx_handler_t handler,
				      void *priv);
void proxy_comm_vmshm_unregister_handler(u32 type,
					 proxy_comm_vmshm_rx_handler_t handler,
					 void *priv);
#else
static inline bool proxy_comm_vmshm_ready(void)
{
	return false;
}

static inline u32 proxy_comm_vmshm_max_payload(void)
{
	return 0;
}

static inline int proxy_comm_vmshm_send_to_client(const struct vmshm_comm_tx *tx)
{
	return -ENODEV;
}

static inline int proxy_comm_vmshm_recv_from_client(struct vmshm_comm_rx *rx)
{
	return -ENODEV;
}

static inline int
proxy_comm_vmshm_register_handler(u32 type,
				  proxy_comm_vmshm_rx_handler_t handler,
				  void *priv)
{
	return -ENODEV;
}

static inline void
proxy_comm_vmshm_unregister_handler(u32 type,
				    proxy_comm_vmshm_rx_handler_t handler,
				    void *priv)
{
}
#endif

#if IS_ENABLED(CONFIG_CLIENT_VMSHM_COMM)
bool client_comm_vmshm_ready(void);
u32 client_comm_vmshm_max_payload(void);
int client_comm_vmshm_send_to_proxy(const struct vmshm_comm_tx *tx);
int client_comm_vmshm_recv_from_proxy(struct vmshm_comm_rx *rx);
int client_comm_vmshm_call(u32 req_type, u32 req_flags,
			   const void *req_payload, u32 req_len,
			   u32 rsp_type, void *rsp_payload,
			   u32 rsp_payload_capacity,
			   struct vmshm_comm_rx *rsp_rx);
#else
static inline bool client_comm_vmshm_ready(void)
{
	return false;
}

static inline u32 client_comm_vmshm_max_payload(void)
{
	return 0;
}

static inline int client_comm_vmshm_send_to_proxy(const struct vmshm_comm_tx *tx)
{
	return -ENODEV;
}

static inline int client_comm_vmshm_recv_from_proxy(struct vmshm_comm_rx *rx)
{
	return -ENODEV;
}

static inline int client_comm_vmshm_call(u32 req_type, u32 req_flags,
					 const void *req_payload, u32 req_len,
					 u32 rsp_type, void *rsp_payload,
					 u32 rsp_payload_capacity,
					 struct vmshm_comm_rx *rsp_rx)
{
	return -ENODEV;
}
#endif

#endif /* _LINUX_VMSHM_COMM_H */
