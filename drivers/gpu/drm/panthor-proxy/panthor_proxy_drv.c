// SPDX-License-Identifier: GPL-2.0 or MIT
/*
 * Panthor proxy VM bridge.
 *
 * The proxy VM owns the real Panthor device. This worker consumes Panthor
 * DEV_QUERY requests from proxy_vmshm_comm and sends query results back to the
 * client VM.
 */

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/vmshm_comm.h>
#include <linux/panthor_vmshm.h>

#define PANTHOR_PROXY_SEND_RETRIES	1000
#define PANTHOR_PROXY_SEND_WAIT_US	1000

static int
panthor_proxy_send_rsp(struct proxy_comm_vmshm_channel *channel,
		       u64 reply_to,
		       const struct panthor_vmshm_dev_query_rsp *rsp)
{
	struct vmshm_comm_tx tx = {
		.type = PANTHOR_VMSHM_MSG_DEV_QUERY_RSP,
		.reply_to = reply_to,
		.status = rsp->ret,
		.payload = rsp,
		.len = sizeof(*rsp),
	};
	int ret, i;

	for (i = 0; i < PANTHOR_PROXY_SEND_RETRIES; i++) {
		ret = proxy_comm_vmshm_send_to_channel(channel, &tx);
		if (ret != -EAGAIN)
			return ret;

		usleep_range(PANTHOR_PROXY_SEND_WAIT_US,
			     PANTHOR_PROXY_SEND_WAIT_US * 2);
	}

	return -ETIMEDOUT;
}

static void
panthor_proxy_handle_dev_query(struct proxy_comm_vmshm_channel *channel,
			       u64 seq,
			       const struct panthor_vmshm_dev_query_req *req)
{
	struct panthor_vmshm_dev_query_rsp rsp;
	int ret;

	memset(&rsp, 0, sizeof(rsp));
	ret = panthor_vmshm_dev_query(req, &rsp);
	if (ret) {
		memset(&rsp, 0, sizeof(rsp));
		rsp.type = req->type;
		rsp.ret = ret;
	}

	ret = panthor_proxy_send_rsp(channel, seq, &rsp);
	if (ret)
		pr_warn_ratelimited("panthor-proxy: response send failed (%d)\n",
				    ret);
}

static int panthor_proxy_rx_handler(const struct vmshm_comm_rx *rx, void *priv)
{
	struct panthor_vmshm_dev_query_req req;

	if (!rx || rx->len != sizeof(req))
		return -EPROTO;

	memcpy(&req, rx->payload, sizeof(req));
	panthor_proxy_handle_dev_query(rx->proxy_channel, rx->seq, &req);
	return 0;
}

static int __init panthor_proxy_init(void)
{
	int ret;

	ret = proxy_comm_vmshm_register_handler(PANTHOR_VMSHM_MSG_DEV_QUERY_REQ,
						panthor_proxy_rx_handler,
						NULL);
	if (ret)
		return ret;

	pr_info("panthor-proxy: vmshm handler registered\n");
	return 0;
}

static void __exit panthor_proxy_exit(void)
{
	proxy_comm_vmshm_unregister_handler(PANTHOR_VMSHM_MSG_DEV_QUERY_REQ,
					    panthor_proxy_rx_handler, NULL);
}

module_init(panthor_proxy_init);
module_exit(panthor_proxy_exit);

MODULE_DESCRIPTION("Panthor proxy VM vmshm bridge");
MODULE_LICENSE("GPL and additional rights");
