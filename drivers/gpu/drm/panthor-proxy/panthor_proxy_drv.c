// SPDX-License-Identifier: GPL-2.0 or MIT
/*
 * Panthor proxy VM bridge.
 *
 * The proxy VM owns the real Panthor device. This worker consumes Panthor
 * DEV_QUERY requests from proxy_vmshm_comm and sends query results back to the
 * client VM.
 */

#include <linux/delay.h>
#include <linux/atomic.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/refcount.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/vmshm_comm.h>
#include <linux/xarray.h>
#include <linux/panthor_vmshm.h>

#define PANTHOR_PROXY_SEND_RETRIES	1000
#define PANTHOR_PROXY_SEND_WAIT_US	1000
#define PANTHOR_PROXY_MAX_VMS_PER_SESSION	32

struct panthor_proxy_vm {
	u32 client_vm_id;
	u32 proxy_vm_id;
	u64 user_va_range;
};

struct panthor_proxy_session {
	struct list_head node;
	u64 session_id;
	struct panthor_vmshm_session *real_session;
	struct mutex lock;
	struct xarray vms;
	refcount_t refcnt;
	bool closing;
};

typedef void (*panthor_proxy_msg_handler_t)(
	struct proxy_comm_vmshm_channel *channel,
	const struct vmshm_comm_rx *rx,
	const void *req);

struct panthor_proxy_msg_handler {
	u32 req_type;
	u32 rsp_type;
	u32 req_size;
	const char *name;
	panthor_proxy_msg_handler_t handler;
};

static DEFINE_MUTEX(panthor_proxy_sessions_lock);
static LIST_HEAD(panthor_proxy_sessions);
static atomic64_t panthor_proxy_next_session_id = ATOMIC64_INIT(0);

static struct panthor_proxy_session *
panthor_proxy_session_find_locked(u64 session_id)
{
	struct panthor_proxy_session *session;

	list_for_each_entry(session, &panthor_proxy_sessions, node) {
		if (session->session_id == session_id)
			return session;
	}

	return NULL;
}

static int panthor_proxy_session_create(u64 *session_id)
{
	struct panthor_proxy_session *session;
	int ret;

	if (!session_id)
		return -EINVAL;

	session = kzalloc(sizeof(*session), GFP_KERNEL);
	if (!session)
		return -ENOMEM;

	mutex_init(&session->lock);
	xa_init_flags(&session->vms, XA_FLAGS_ALLOC1);
	refcount_set(&session->refcnt, 1);

	ret = panthor_vmshm_session_open(&session->real_session);
	if (ret) {
		xa_destroy(&session->vms);
		kfree(session);
		return ret;
	}

	session->session_id = (u64)atomic64_inc_return(&panthor_proxy_next_session_id);
	if (!session->session_id) {
		panthor_vmshm_session_close(session->real_session);
		xa_destroy(&session->vms);
		kfree(session);
		return -EOVERFLOW;
	}

	mutex_lock(&panthor_proxy_sessions_lock);
	list_add_tail(&session->node, &panthor_proxy_sessions);
	mutex_unlock(&panthor_proxy_sessions_lock);

	*session_id = session->session_id;
	return 0;
}

static void panthor_proxy_session_release(struct panthor_proxy_session *session)
{
	struct panthor_proxy_vm *vm;
	unsigned long i;

	if (!session)
		return;

	xa_for_each(&session->vms, i, vm)
		kfree(vm);
	xa_destroy(&session->vms);
	panthor_vmshm_session_close(session->real_session);
	kfree(session);
}

static void panthor_proxy_session_put(struct panthor_proxy_session *session)
{
	if (session && refcount_dec_and_test(&session->refcnt))
		panthor_proxy_session_release(session);
}

static int panthor_proxy_session_destroy(u64 session_id)
{
	struct panthor_proxy_session *session;

	if (!session_id)
		return -EINVAL;

	mutex_lock(&panthor_proxy_sessions_lock);
	session = panthor_proxy_session_find_locked(session_id);
	if (session) {
		session->closing = true;
		list_del(&session->node);
	}
	mutex_unlock(&panthor_proxy_sessions_lock);

	if (!session)
		return -ENOENT;

	panthor_proxy_session_put(session);
	return 0;
}

static void panthor_proxy_sessions_destroy_all(void)
{
	struct panthor_proxy_session *session, *tmp;
	LIST_HEAD(sessions);

	mutex_lock(&panthor_proxy_sessions_lock);
	list_for_each_entry_safe(session, tmp, &panthor_proxy_sessions, node) {
		session->closing = true;
		list_move_tail(&session->node, &sessions);
	}
	mutex_unlock(&panthor_proxy_sessions_lock);

	list_for_each_entry_safe(session, tmp, &sessions, node) {
		list_del(&session->node);
		panthor_proxy_session_put(session);
	}
}

static struct panthor_proxy_session *panthor_proxy_session_lookup(u64 session_id)
{
	struct panthor_proxy_session *session;

	if (!session_id)
		return NULL;

	mutex_lock(&panthor_proxy_sessions_lock);
	session = panthor_proxy_session_find_locked(session_id);
	if (session && session->closing)
		session = NULL;
	if (session)
		refcount_inc(&session->refcnt);
	mutex_unlock(&panthor_proxy_sessions_lock);

	return session;
}

static bool panthor_proxy_session_exists(u64 session_id)
{
	struct panthor_proxy_session *session;
	bool exists;

	if (!session_id)
		return true;

	session = panthor_proxy_session_lookup(session_id);
	exists = session;
	panthor_proxy_session_put(session);
	return exists;
}

static int
panthor_proxy_session_vm_create(struct panthor_proxy_session *session,
				const struct panthor_vmshm_vm_create_req *req,
				struct panthor_vmshm_vm_create_rsp *rsp)
{
	struct panthor_proxy_vm *vm;
	struct drm_panthor_vm_create args = {
		.flags = req->flags,
		.user_va_range = req->user_va_range,
	};
	u32 client_vm_id;
	int ret;

	if (!session || !rsp)
		return -EINVAL;

	vm = kzalloc(sizeof(*vm), GFP_KERNEL);
	if (!vm)
		return -ENOMEM;

	mutex_lock(&session->lock);
	ret = panthor_vmshm_vm_create(session->real_session, &args);
	if (ret)
		goto err_unlock;

	ret = xa_alloc(&session->vms, &client_vm_id, vm,
		       XA_LIMIT(1, PANTHOR_PROXY_MAX_VMS_PER_SESSION),
		       GFP_KERNEL);
	if (ret)
		goto err_destroy_proxy_vm;

	vm->client_vm_id = client_vm_id;
	vm->proxy_vm_id = args.id;
	vm->user_va_range = args.user_va_range;

	rsp->client_vm_id = client_vm_id;
	rsp->proxy_vm_id = args.id;
	rsp->user_va_range = args.user_va_range;
	mutex_unlock(&session->lock);
	return 0;

err_destroy_proxy_vm:
	panthor_vmshm_vm_destroy(session->real_session, args.id);

err_unlock:
	mutex_unlock(&session->lock);
	kfree(vm);
	return ret;
}

static int
panthor_proxy_session_vm_destroy(struct panthor_proxy_session *session,
				 u32 client_vm_id, u32 *proxy_vm_id)
{
	struct panthor_proxy_vm *vm;
	int ret;

	if (!session || !client_vm_id)
		return -EINVAL;

	mutex_lock(&session->lock);
	vm = xa_erase(&session->vms, client_vm_id);
	if (!vm) {
		mutex_unlock(&session->lock);
		return -EINVAL;
	}

	ret = panthor_vmshm_vm_destroy(session->real_session, vm->proxy_vm_id);
	if (proxy_vm_id)
		*proxy_vm_id = vm->proxy_vm_id;
	mutex_unlock(&session->lock);

	kfree(vm);
	return ret;
}

static int
panthor_proxy_send_rsp(struct proxy_comm_vmshm_channel *channel,
		       u64 reply_to, u32 type, s32 status,
		       const void *payload, u32 len)
{
	struct vmshm_comm_tx tx = {
		.type = type,
		.reply_to = reply_to,
		.status = status,
		.payload = payload,
		.len = len,
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
panthor_proxy_handle_open_session(struct proxy_comm_vmshm_channel *channel,
				  const struct vmshm_comm_rx *rx,
				  const void *payload)
{
	const struct panthor_vmshm_open_session_req *req = payload;
	struct panthor_vmshm_open_session_rsp rsp = { 0 };
	int ret;

	if (req->version != PANTHOR_VMSHM_ABI_VERSION || req->flags) {
		rsp.ret = -EINVAL;
		goto out_send;
	}

	ret = panthor_proxy_session_create(&rsp.session_id);
	if (ret)
		rsp.ret = ret;
	else
		pr_info("panthor-proxy: OPEN_SESSION session=%llu\n",
			rsp.session_id);

out_send:
	ret = panthor_proxy_send_rsp(channel, rx->seq,
				     PANTHOR_VMSHM_MSG_OPEN_SESSION_RSP,
				     rsp.ret, &rsp, sizeof(rsp));
	if (ret)
		pr_warn_ratelimited("panthor-proxy: OPEN_SESSION response send failed (%d)\n",
				    ret);
}

static void
panthor_proxy_handle_close_session(struct proxy_comm_vmshm_channel *channel,
				   const struct vmshm_comm_rx *rx,
				   const void *payload)
{
	const struct panthor_vmshm_close_session_req *req = payload;
	struct panthor_vmshm_close_session_rsp rsp = { 0 };
	int ret;

	if (req->flags || req->pad)
		rsp.ret = -EINVAL;
	else
		rsp.ret = panthor_proxy_session_destroy(req->session_id);
	if (!rsp.ret)
		pr_info("panthor-proxy: CLOSE_SESSION session=%llu\n",
			req->session_id);

	ret = panthor_proxy_send_rsp(channel, rx->seq,
				     PANTHOR_VMSHM_MSG_CLOSE_SESSION_RSP,
				     rsp.ret, &rsp, sizeof(rsp));
	if (ret)
		pr_warn_ratelimited("panthor-proxy: CLOSE_SESSION response send failed (%d)\n",
				    ret);
}

static void
panthor_proxy_handle_dev_query(struct proxy_comm_vmshm_channel *channel,
			       const struct vmshm_comm_rx *rx,
			       const void *payload)
{
	const struct panthor_vmshm_dev_query_req *req = payload;
	struct panthor_vmshm_dev_query_rsp rsp;
	int ret;

	memset(&rsp, 0, sizeof(rsp));
	if (!panthor_proxy_session_exists(req->session_id)) {
		rsp.type = req->type;
		rsp.ret = -ENOENT;
		goto out_send;
	}

	ret = panthor_vmshm_dev_query(req, &rsp);
	if (ret) {
		memset(&rsp, 0, sizeof(rsp));
		rsp.type = req->type;
		rsp.ret = ret;
	}

out_send:
	if (!rsp.ret)
		pr_info("panthor-proxy: DEV_QUERY session=%llu type=%u size=%u data=%u\n",
			req->session_id, req->type, rsp.size, rsp.data_len);

	ret = panthor_proxy_send_rsp(channel, rx->seq,
				     PANTHOR_VMSHM_MSG_DEV_QUERY_RSP,
				     rsp.ret, &rsp, sizeof(rsp));
	if (ret)
		pr_warn_ratelimited("panthor-proxy: DEV_QUERY response send failed (%d)\n",
				    ret);
}

static void
panthor_proxy_handle_vm_create(struct proxy_comm_vmshm_channel *channel,
			       const struct vmshm_comm_rx *rx,
			       const void *payload)
{
	const struct panthor_vmshm_vm_create_req *req = payload;
	struct panthor_vmshm_vm_create_rsp rsp = { 0 };
	struct panthor_proxy_session *session;
	int ret;

	session = panthor_proxy_session_lookup(req->session_id);
	if (!session) {
		rsp.ret = -ENOENT;
		goto out_send;
	}

	ret = panthor_proxy_session_vm_create(session, req, &rsp);
	panthor_proxy_session_put(session);
	if (ret)
		rsp.ret = ret;
	else
		pr_info("panthor-proxy: VM_CREATE session=%llu client_vm=%u proxy_vm=%u user_va_range=0x%llx\n",
			req->session_id, rsp.client_vm_id, rsp.proxy_vm_id,
			rsp.user_va_range);

out_send:
	ret = panthor_proxy_send_rsp(channel, rx->seq,
				     PANTHOR_VMSHM_MSG_VM_CREATE_RSP,
				     rsp.ret, &rsp, sizeof(rsp));
	if (ret)
		pr_warn_ratelimited("panthor-proxy: VM_CREATE response send failed (%d)\n",
				    ret);
}

static void
panthor_proxy_handle_vm_destroy(struct proxy_comm_vmshm_channel *channel,
				const struct vmshm_comm_rx *rx,
				const void *payload)
{
	const struct panthor_vmshm_vm_destroy_req *req = payload;
	struct panthor_vmshm_vm_destroy_rsp rsp = { 0 };
	struct panthor_proxy_session *session;
	int ret;

	if (req->pad) {
		rsp.ret = -EINVAL;
		goto out_send;
	}

	session = panthor_proxy_session_lookup(req->session_id);
	if (!session) {
		rsp.ret = -ENOENT;
		goto out_send;
	}

	rsp.ret = panthor_proxy_session_vm_destroy(session, req->client_vm_id,
						  &rsp.proxy_vm_id);
	panthor_proxy_session_put(session);
	if (!rsp.ret)
		pr_info("panthor-proxy: VM_DESTROY session=%llu client_vm=%u proxy_vm=%u\n",
			req->session_id, req->client_vm_id, rsp.proxy_vm_id);

out_send:
	ret = panthor_proxy_send_rsp(channel, rx->seq,
				     PANTHOR_VMSHM_MSG_VM_DESTROY_RSP,
				     rsp.ret, &rsp, sizeof(rsp));
	if (ret)
		pr_warn_ratelimited("panthor-proxy: VM_DESTROY response send failed (%d)\n",
				    ret);
}

static const struct panthor_proxy_msg_handler panthor_proxy_handlers[] = {
	{
		.req_type = PANTHOR_VMSHM_MSG_OPEN_SESSION_REQ,
		.rsp_type = PANTHOR_VMSHM_MSG_OPEN_SESSION_RSP,
		.req_size = sizeof(struct panthor_vmshm_open_session_req),
		.name = "OPEN_SESSION",
		.handler = panthor_proxy_handle_open_session,
	},
	{
		.req_type = PANTHOR_VMSHM_MSG_CLOSE_SESSION_REQ,
		.rsp_type = PANTHOR_VMSHM_MSG_CLOSE_SESSION_RSP,
		.req_size = sizeof(struct panthor_vmshm_close_session_req),
		.name = "CLOSE_SESSION",
		.handler = panthor_proxy_handle_close_session,
	},
	{
		.req_type = PANTHOR_VMSHM_MSG_DEV_QUERY_REQ,
		.rsp_type = PANTHOR_VMSHM_MSG_DEV_QUERY_RSP,
		.req_size = sizeof(struct panthor_vmshm_dev_query_req),
		.name = "DEV_QUERY",
		.handler = panthor_proxy_handle_dev_query,
	},
	{
		.req_type = PANTHOR_VMSHM_MSG_VM_CREATE_REQ,
		.rsp_type = PANTHOR_VMSHM_MSG_VM_CREATE_RSP,
		.req_size = sizeof(struct panthor_vmshm_vm_create_req),
		.name = "VM_CREATE",
		.handler = panthor_proxy_handle_vm_create,
	},
	{
		.req_type = PANTHOR_VMSHM_MSG_VM_DESTROY_REQ,
		.rsp_type = PANTHOR_VMSHM_MSG_VM_DESTROY_RSP,
		.req_size = sizeof(struct panthor_vmshm_vm_destroy_req),
		.name = "VM_DESTROY",
		.handler = panthor_proxy_handle_vm_destroy,
	},
};

static const struct panthor_proxy_msg_handler *
panthor_proxy_find_handler(u32 type)
{
	u32 i;

	for (i = 0; i < ARRAY_SIZE(panthor_proxy_handlers); i++) {
		if (panthor_proxy_handlers[i].req_type == type)
			return &panthor_proxy_handlers[i];
	}

	return NULL;
}

static int panthor_proxy_rx_handler(const struct vmshm_comm_rx *rx, void *priv)
{
	const struct panthor_proxy_msg_handler *handler;

	if (!rx)
		return -EPROTO;

	handler = panthor_proxy_find_handler(rx->type);
	if (!handler)
		return -ENOENT;

	if (rx->len != handler->req_size)
		return -EPROTO;

	handler->handler(rx->proxy_channel, rx, rx->payload);

	return 0;
}

static int __init panthor_proxy_init(void)
{
	int ret, i;

	for (i = 0; i < ARRAY_SIZE(panthor_proxy_handlers); i++) {
		ret = proxy_comm_vmshm_register_handler(
			panthor_proxy_handlers[i].req_type,
			panthor_proxy_rx_handler, NULL);
		if (ret)
			goto err_unregister_handlers;
	}

	pr_info("panthor-proxy: vmshm handler registered\n");
	return 0;

err_unregister_handlers:
	while (--i >= 0)
		proxy_comm_vmshm_unregister_handler(
			panthor_proxy_handlers[i].req_type,
			panthor_proxy_rx_handler, NULL);

	return ret;
}

static void __exit panthor_proxy_exit(void)
{
	int i;

	for (i = ARRAY_SIZE(panthor_proxy_handlers) - 1; i >= 0; i--)
		proxy_comm_vmshm_unregister_handler(
			panthor_proxy_handlers[i].req_type,
			panthor_proxy_rx_handler, NULL);
	panthor_proxy_sessions_destroy_all();
}

module_init(panthor_proxy_init);
module_exit(panthor_proxy_exit);

MODULE_DESCRIPTION("Panthor proxy VM vmshm bridge");
MODULE_LICENSE("GPL and additional rights");
