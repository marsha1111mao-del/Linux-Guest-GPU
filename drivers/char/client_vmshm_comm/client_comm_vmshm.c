// SPDX-License-Identifier: GPL-2.0
/*
 * client_comm_vmshm - client/frontend side of the vmshm control transport.
 *
 * The proxy VM owns the shared protocol layout. A client VM only maps its
 * single communication memslot, waits for the proxy side to publish a valid
 * header, validates the queue/object layout, and then exposes the region for
 * debugging and future in-kernel RPC users.
 */

#include <linux/cdev.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ioctl.h>
#include <linux/list.h>
#include <linux/log2.h>
#include <linux/mm.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/vmshm_comm.h>
#include <linux/workqueue.h>

#define CLIENT_COMM_VMSHM_NAME		"client_comm_vmshm"
#define CLIENT_COMM_VMSHM_IOC_MAGIC	'C'

#define CLIENT_COMM_VMSHM_IOC_GET_SIZE \
	_IOR(CLIENT_COMM_VMSHM_IOC_MAGIC, 1, __u64)
#define CLIENT_COMM_VMSHM_IOC_GET_BASE \
	_IOR(CLIENT_COMM_VMSHM_IOC_MAGIC, 2, __u64)
#define CLIENT_COMM_VMSHM_IOC_GET_INFO \
	_IOR(CLIENT_COMM_VMSHM_IOC_MAGIC, 3, struct client_comm_vmshm_info)

#define PROXY_COMM_VMSHM_MAGIC		0x56534350U /* "VSCP" */
#define PROXY_COMM_VMSHM_INIT_MAGIC	0x494e4954U /* "INIT" */
#define PROXY_COMM_VMSHM_VERSION	1
#define PROXY_COMM_VMSHM_MAX_QUEUES	2
#define PROXY_COMM_VMSHM_MAX_OBJECTS	64
#define PROXY_COMM_VMSHM_QUEUE_SIZE	256
#define PROXY_COMM_VMSHM_MSG_SIZE	512
#define PROXY_COMM_VMSHM_INIT_WAIT_US	1000
#define PROXY_COMM_VMSHM_INIT_RETRIES	1000
#define PROXY_COMM_VMSHM_QUEUE_MAGIC	0x51564350U /* "PCVQ" */
#define CLIENT_COMM_VMSHM_RPC_WAIT_US	1000
#define CLIENT_COMM_VMSHM_RPC_RETRIES	1000
#define CLIENT_COMM_VMSHM_RPC_TIMEOUT_MS	1000

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

struct client_comm_vmshm_info {
	__u64 gpa;
	__u64 size;
	__u32 header_size;
	__u32 queue_count;
};

struct client_comm_vmshm_dev {
	void *base;
	struct proxy_comm_vmshm_header *hdr;
	phys_addr_t gpa;
	resource_size_t size;
	void __iomem *doorbell;
	resource_size_t doorbell_size;
	int irq;
	bool irq_notify;
	struct work_struct rx_work;
	spinlock_t waiters_lock;
	struct list_head waiters;
	dev_t devt;
	struct cdev cdev;
	struct class *class;
	struct device *dev;
	struct mutex lock;
	struct mutex rpc_lock;
	atomic_t open_cnt;
	atomic64_t next_seq;
};

struct client_comm_vmshm_waiter {
	struct list_head node;
	struct completion done;
	u64 seq;
	u32 rsp_type;
	void *rsp_payload;
	u32 rsp_payload_capacity;
	struct vmshm_comm_rx rsp_rx;
	int status;
};

static struct client_comm_vmshm_dev client_comm_vmshm_dev = {
	.lock = __MUTEX_INITIALIZER(client_comm_vmshm_dev.lock),
	.rpc_lock = __MUTEX_INITIALIZER(client_comm_vmshm_dev.rpc_lock),
};

static void client_comm_vmshm_rx_work(struct work_struct *work);

static bool client_comm_vmshm_range_valid(struct client_comm_vmshm_dev *d,
					  u32 offset, u32 size)
{
	if (offset > d->size)
		return false;

	return size <= d->size - offset;
}

static void *client_comm_vmshm_ptr(struct client_comm_vmshm_dev *d,
				   u32 offset, u32 size)
{
	if (!client_comm_vmshm_range_valid(d, offset, size))
		return NULL;

	return (void *)((u8 *)d->base + offset);
}

static struct proxy_comm_vmshm_queue_object *
client_comm_vmshm_queue_obj(struct client_comm_vmshm_dev *d, u32 queue_id)
{
	struct proxy_comm_vmshm_header *h = d->hdr;
	struct proxy_comm_vmshm_queue *q;

	if (!h || queue_id >= h->queue_count)
		return NULL;

	q = &h->queues[queue_id];
	return client_comm_vmshm_ptr(d, q->queue_off, sizeof(*q));
}

static struct proxy_comm_vmshm_desc *
client_comm_vmshm_descs(struct client_comm_vmshm_dev *d,
			struct proxy_comm_vmshm_queue_object *q)
{
	return client_comm_vmshm_ptr(d, q->desc_off,
				     q->queue_size * sizeof(struct proxy_comm_vmshm_desc));
}

static struct proxy_comm_vmshm_avail_ring *
client_comm_vmshm_avail(struct client_comm_vmshm_dev *d,
			struct proxy_comm_vmshm_queue_object *q)
{
	u32 size = sizeof(struct proxy_comm_vmshm_avail_ring) +
		   q->queue_size * sizeof(u16);

	return client_comm_vmshm_ptr(d, q->avail_off, size);
}

static struct proxy_comm_vmshm_used_ring *
client_comm_vmshm_used(struct client_comm_vmshm_dev *d,
		       struct proxy_comm_vmshm_queue_object *q)
{
	u32 size = sizeof(struct proxy_comm_vmshm_used_ring) +
		   q->queue_size * sizeof(struct proxy_comm_vmshm_used_elem);

	return client_comm_vmshm_ptr(d, q->used_off, size);
}

static int client_comm_vmshm_validate_layout(struct client_comm_vmshm_dev *d)
{
	struct proxy_comm_vmshm_header *h = d->hdr;
	u32 table_size;
	u32 i;

	if (READ_ONCE(h->magic) != PROXY_COMM_VMSHM_MAGIC)
		return -EINVAL;
	if (h->version != PROXY_COMM_VMSHM_VERSION ||
	    h->header_size != sizeof(*h) ||
	    h->queue_count != PROXY_COMM_VMSHM_MAX_QUEUES)
		return -EINVAL;
	if (h->total_size > d->size ||
	    h->object_count > h->object_capacity ||
	    h->object_capacity > PROXY_COMM_VMSHM_MAX_OBJECTS)
		return -EINVAL;

	table_size = h->object_capacity * sizeof(struct proxy_comm_vmshm_object);
	if (!client_comm_vmshm_range_valid(d, h->object_table_off, table_size))
		return -EINVAL;
	if (!client_comm_vmshm_range_valid(d, h->heap_base_off, h->heap_size))
		return -EINVAL;

	for (i = 0; i < h->queue_count; i++) {
		struct proxy_comm_vmshm_queue_object *q;

		q = client_comm_vmshm_queue_obj(d, i);
		if (!q || q->magic != PROXY_COMM_VMSHM_QUEUE_MAGIC ||
		    q->queue_size != PROXY_COMM_VMSHM_QUEUE_SIZE ||
		    q->msg_size != PROXY_COMM_VMSHM_MSG_SIZE ||
		    !is_power_of_2(q->queue_size))
			return -EINVAL;
	}

	return 0;
}

static int client_comm_vmshm_attach_layout(struct client_comm_vmshm_dev *d)
{
	struct proxy_comm_vmshm_header *h = d->base;
	u32 magic;
	int i, ret;

	if (d->size > U32_MAX)
		return -EOVERFLOW;

	d->hdr = h;
	for (i = 0; i < PROXY_COMM_VMSHM_INIT_RETRIES; i++) {
		magic = READ_ONCE(h->magic);
		if (magic == PROXY_COMM_VMSHM_MAGIC) {
			ret = client_comm_vmshm_validate_layout(d);
			if (ret)
				return ret;

			smp_wmb();
			h->status[PROXY_COMM_VMSHM_ROLE_CLIENT] =
				PROXY_COMM_VMSHM_STATUS_READY;
			return 0;
		}

		if (magic == PROXY_COMM_VMSHM_INIT_MAGIC || !magic) {
			usleep_range(PROXY_COMM_VMSHM_INIT_WAIT_US,
				     PROXY_COMM_VMSHM_INIT_WAIT_US * 2);
			continue;
		}

		return -EINVAL;
	}

	return -ETIMEDOUT;
}

static int client_comm_vmshm_queue_send(struct client_comm_vmshm_dev *d,
					u32 queue_id,
					const struct vmshm_comm_tx *tx)
{
	struct proxy_comm_vmshm_queue_object *q;
	struct proxy_comm_vmshm_desc *descs, *desc;
	struct proxy_comm_vmshm_avail_ring *avail;
	struct proxy_comm_vmshm_used_ring *used;
	struct proxy_comm_vmshm_msg *msg;
	u32 avail_idx, used_idx, ring_idx, desc_id, msg_off, msg_len;

	if (!tx)
		return -EINVAL;

	q = client_comm_vmshm_queue_obj(d, queue_id);
	if (!q)
		return -EINVAL;
	if (tx->len > q->msg_size - sizeof(*msg))
		return -EMSGSIZE;
	if (tx->len && !tx->payload)
		return -EINVAL;

	descs = client_comm_vmshm_descs(d, q);
	avail = client_comm_vmshm_avail(d, q);
	used = client_comm_vmshm_used(d, q);
	if (!descs || !avail || !used)
		return -EINVAL;

	avail_idx = READ_ONCE(avail->idx);
	used_idx = smp_load_acquire(&used->idx);
	if (avail_idx - used_idx >= q->queue_size)
		return -EAGAIN;

	ring_idx = avail_idx & q->queue_mask;
	desc_id = ring_idx;
	msg_off = q->msg_pool_off + desc_id * q->msg_size;
	msg = client_comm_vmshm_ptr(d, msg_off, q->msg_size);
	if (!msg)
		return -EINVAL;

	memset(msg, 0, q->msg_size);
	msg->type = tx->type;
	msg->flags = tx->flags;
	msg->len = tx->len;
	msg->seq = tx->seq;
	msg->reply_to = tx->reply_to;
	msg->status = tx->status;
	if (tx->len)
		memcpy(msg->payload, tx->payload, tx->len);

	msg_len = sizeof(*msg) + tx->len;
	desc = &descs[desc_id];
	desc->msg_off = msg_off;
	desc->msg_capacity = q->msg_size;
	desc->msg_len = msg_len;
	desc->flags = 0;
	desc->seq = tx->seq;

	smp_wmb();
	WRITE_ONCE(avail->ring[ring_idx], desc_id);
	smp_store_release(&avail->idx, avail_idx + 1);
	return 0;
}

static int client_comm_vmshm_queue_recv(struct client_comm_vmshm_dev *d,
					u32 queue_id,
					struct vmshm_comm_rx *rx)
{
	struct proxy_comm_vmshm_queue_object *q;
	struct proxy_comm_vmshm_desc *descs, *desc = NULL;
	struct proxy_comm_vmshm_avail_ring *avail;
	struct proxy_comm_vmshm_used_ring *used;
	struct proxy_comm_vmshm_msg *msg = NULL;
	u32 avail_idx, used_idx, ring_idx, desc_id, expected_msg_off;
	int ret = -EINVAL;

	if (!rx)
		return -EINVAL;
	rx->len = 0;

	q = client_comm_vmshm_queue_obj(d, queue_id);
	if (!q)
		return -EINVAL;

	descs = client_comm_vmshm_descs(d, q);
	avail = client_comm_vmshm_avail(d, q);
	used = client_comm_vmshm_used(d, q);
	if (!descs || !avail || !used)
		return -EINVAL;

	used_idx = READ_ONCE(used->idx);
	avail_idx = smp_load_acquire(&avail->idx);
	if (used_idx == avail_idx)
		return -ENOENT;

	ring_idx = used_idx & q->queue_mask;
	desc_id = READ_ONCE(avail->ring[ring_idx]);
	if (desc_id >= q->queue_size)
		goto out_used;

	desc = &descs[desc_id];
	smp_rmb();

	expected_msg_off = q->msg_pool_off + desc_id * q->msg_size;
	if (desc->msg_off != expected_msg_off ||
	    desc->msg_capacity != q->msg_size ||
	    desc->msg_len < sizeof(*msg) ||
	    desc->msg_len > q->msg_size)
		goto out_used;

	msg = client_comm_vmshm_ptr(d, desc->msg_off, desc->msg_len);
	if (!msg)
		goto out_used;

	if (msg->len > q->msg_size - sizeof(*msg) ||
	    sizeof(*msg) + msg->len != desc->msg_len)
		goto out_used;

	if (msg->len) {
		if (!rx->payload || msg->len > rx->payload_capacity)
			return -EMSGSIZE;
		memcpy(rx->payload, msg->payload, msg->len);
	}

	rx->type = msg->type;
	rx->flags = msg->flags;
	rx->len = msg->len;
	rx->seq = msg->seq;
	rx->reply_to = msg->reply_to;
	rx->status = msg->status;
	ret = 0;

out_used:
	used->ring[ring_idx].desc_id = desc_id;
	used->ring[ring_idx].status = ret;
	used->ring[ring_idx].len = ret ? 0 : desc->msg_len;
	smp_store_release(&used->idx, used_idx + 1);
	return ret;
}

static bool client_comm_vmshm_ready_locked(struct client_comm_vmshm_dev *d)
{
	struct proxy_comm_vmshm_header *h = d->hdr;

	return d->base && h &&
	       READ_ONCE(h->magic) == PROXY_COMM_VMSHM_MAGIC &&
	       READ_ONCE(h->status[PROXY_COMM_VMSHM_ROLE_CLIENT]) ==
		       PROXY_COMM_VMSHM_STATUS_READY;
}

static void client_comm_vmshm_kick_proxy(struct client_comm_vmshm_dev *d)
{
	if (d->irq_notify && d->doorbell)
		writel(1, d->doorbell);
}

bool client_comm_vmshm_ready(void)
{
	return client_comm_vmshm_ready_locked(&client_comm_vmshm_dev);
}
EXPORT_SYMBOL_GPL(client_comm_vmshm_ready);

u32 client_comm_vmshm_max_payload(void)
{
	struct client_comm_vmshm_dev *d = &client_comm_vmshm_dev;
	struct proxy_comm_vmshm_queue_object *q;

	if (!client_comm_vmshm_ready_locked(d))
		return 0;

	q = client_comm_vmshm_queue_obj(d, PROXY_COMM_VMSHM_Q_CLIENT_TO_PROXY);
	if (!q || q->msg_size <= sizeof(struct proxy_comm_vmshm_msg))
		return 0;

	return q->msg_size - sizeof(struct proxy_comm_vmshm_msg);
}
EXPORT_SYMBOL_GPL(client_comm_vmshm_max_payload);

int client_comm_vmshm_send_to_proxy(const struct vmshm_comm_tx *tx)
{
	struct client_comm_vmshm_dev *d = &client_comm_vmshm_dev;
	int ret;

	mutex_lock(&d->lock);
	if (!client_comm_vmshm_ready_locked(d)) {
		ret = -ENODEV;
		goto out_unlock;
	}

	ret = client_comm_vmshm_queue_send(d,
					   PROXY_COMM_VMSHM_Q_CLIENT_TO_PROXY,
					   tx);
	if (!ret)
		client_comm_vmshm_kick_proxy(d);

out_unlock:
	mutex_unlock(&d->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(client_comm_vmshm_send_to_proxy);

int client_comm_vmshm_recv_from_proxy(struct vmshm_comm_rx *rx)
{
	struct client_comm_vmshm_dev *d = &client_comm_vmshm_dev;
	int ret;

	mutex_lock(&d->lock);
	if (!client_comm_vmshm_ready_locked(d)) {
		ret = -ENODEV;
		goto out_unlock;
	}

	ret = client_comm_vmshm_queue_recv(d,
					   PROXY_COMM_VMSHM_Q_PROXY_TO_CLIENT,
					   rx);

out_unlock:
	mutex_unlock(&d->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(client_comm_vmshm_recv_from_proxy);

static void client_comm_vmshm_rx_work(struct work_struct *work)
{
	struct client_comm_vmshm_dev *d =
		container_of(work, struct client_comm_vmshm_dev, rx_work);
	u8 payload[PROXY_COMM_VMSHM_MSG_SIZE - sizeof(struct proxy_comm_vmshm_msg)];
	struct vmshm_comm_rx rx = {
		.payload = payload,
		.payload_capacity = sizeof(payload),
	};
	struct client_comm_vmshm_waiter *waiter = NULL;
	int ret;

	for (;;) {
		memset(payload, 0, sizeof(payload));
		ret = client_comm_vmshm_recv_from_proxy(&rx);
		if (ret == -ENOENT || ret == -ENODEV)
			break;
		if (ret) {
			pr_warn_ratelimited("client_comm_vmshm: IRQ recv failed (%d)\n",
					    ret);
			break;
		}

		spin_lock(&d->waiters_lock);
		list_for_each_entry(waiter, &d->waiters, node) {
			if (waiter->seq == rx.reply_to &&
			    waiter->rsp_type == rx.type)
				goto found_waiter;
		}
		waiter = NULL;

found_waiter:
		if (waiter) {
			if (rx.len > waiter->rsp_payload_capacity) {
				waiter->status = -EMSGSIZE;
			} else {
				if (rx.len)
					memcpy(waiter->rsp_payload, payload, rx.len);
				waiter->rsp_rx = rx;
				waiter->rsp_rx.payload = waiter->rsp_payload;
				waiter->rsp_rx.payload_capacity =
					waiter->rsp_payload_capacity;
				waiter->status = 0;
			}
			list_del_init(&waiter->node);
			complete(&waiter->done);
		}
		spin_unlock(&d->waiters_lock);

		if (!waiter)
			pr_debug("client_comm_vmshm: dropping unexpected rsp type=0x%x reply_to=%llu\n",
				 rx.type, rx.reply_to);
	}
}

static irqreturn_t client_comm_vmshm_irq(int irq, void *data)
{
	struct client_comm_vmshm_dev *d = data;

	schedule_work(&d->rx_work);
	return IRQ_HANDLED;
}

int client_comm_vmshm_call(u32 req_type, u32 req_flags,
			   const void *req_payload, u32 req_len,
			   u32 rsp_type, void *rsp_payload,
			   u32 rsp_payload_capacity,
			   struct vmshm_comm_rx *rsp_rx)
{
	struct client_comm_vmshm_dev *d = &client_comm_vmshm_dev;
	struct vmshm_comm_tx tx = {
		.type = req_type,
		.flags = req_flags,
		.payload = req_payload,
		.len = req_len,
	};
	u64 seq;
	int ret, i;

	if (req_len && !req_payload)
		return -EINVAL;
	if (rsp_payload_capacity && !rsp_payload)
		return -EINVAL;

	mutex_lock(&d->rpc_lock);
	if (!client_comm_vmshm_ready()) {
		ret = -ENODEV;
		goto out_unlock;
	}

	seq = (u64)atomic64_inc_return(&d->next_seq);
	tx.seq = seq;

	if (d->irq_notify) {
		struct client_comm_vmshm_waiter waiter = {
			.seq = seq,
			.rsp_type = rsp_type,
			.rsp_payload = rsp_payload,
			.rsp_payload_capacity = rsp_payload_capacity,
			.status = -ETIMEDOUT,
		};
		unsigned long timeout;

		INIT_LIST_HEAD(&waiter.node);
		init_completion(&waiter.done);
		if (rsp_payload && rsp_payload_capacity)
			memset(rsp_payload, 0, rsp_payload_capacity);

		spin_lock(&d->waiters_lock);
		list_add_tail(&waiter.node, &d->waiters);
		spin_unlock(&d->waiters_lock);

		ret = client_comm_vmshm_send_to_proxy(&tx);
		if (ret) {
			spin_lock(&d->waiters_lock);
			if (!list_empty(&waiter.node))
				list_del_init(&waiter.node);
			spin_unlock(&d->waiters_lock);
			goto out_unlock;
		}

		timeout = msecs_to_jiffies(CLIENT_COMM_VMSHM_RPC_TIMEOUT_MS);
		if (!wait_for_completion_timeout(&waiter.done, timeout)) {
			bool completed;

			spin_lock(&d->waiters_lock);
			if (!list_empty(&waiter.node))
				list_del_init(&waiter.node);
			completed = list_empty(&waiter.node);
			spin_unlock(&d->waiters_lock);

			ret = completed ? waiter.status : -ETIMEDOUT;
			if (!ret && rsp_rx)
				*rsp_rx = waiter.rsp_rx;
			goto out_unlock;
		}

		ret = waiter.status;
		if (!ret && rsp_rx)
			*rsp_rx = waiter.rsp_rx;
		goto out_unlock;
	}

	ret = client_comm_vmshm_send_to_proxy(&tx);
	if (ret)
		goto out_unlock;

	for (i = 0; i < CLIENT_COMM_VMSHM_RPC_RETRIES; i++) {
		struct vmshm_comm_rx rx = {
			.payload = rsp_payload,
			.payload_capacity = rsp_payload_capacity,
		};

		if (rsp_payload && rsp_payload_capacity)
			memset(rsp_payload, 0, rsp_payload_capacity);

		ret = client_comm_vmshm_recv_from_proxy(&rx);
		if (ret == -ENOENT) {
			usleep_range(CLIENT_COMM_VMSHM_RPC_WAIT_US,
				     CLIENT_COMM_VMSHM_RPC_WAIT_US * 2);
			continue;
		}
		if (ret)
			goto out_unlock;

		if (rx.type != rsp_type || rx.reply_to != seq)
			continue;

		if (rsp_rx)
			*rsp_rx = rx;
		ret = 0;
		goto out_unlock;
	}

	ret = -ETIMEDOUT;

out_unlock:
	mutex_unlock(&d->rpc_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(client_comm_vmshm_call);

#ifdef CONFIG_CLIENT_VMSHM_COMM_HELLO_SELFTEST
static int client_comm_vmshm_hello_selftest_run(void)
{
	static const char hello[] = VMSHM_COMM_SELFTEST_HELLO_PAYLOAD;
	char ack[sizeof(VMSHM_COMM_SELFTEST_ACK_PAYLOAD)];
	struct vmshm_comm_rx rx;
	int ret;

	memset(ack, 0, sizeof(ack));
	ret = client_comm_vmshm_call(VMSHM_COMM_SELFTEST_HELLO_REQ, 0,
				     hello, sizeof(hello),
				     VMSHM_COMM_SELFTEST_HELLO_RSP,
				     ack, sizeof(ack), &rx);
	if (ret) {
		pr_warn("client_comm_vmshm: hello selftest request failed (%d)\n",
			ret);
		return ret;
	}

	if (rx.len != sizeof(ack) ||
	    memcmp(ack, VMSHM_COMM_SELFTEST_ACK_PAYLOAD, sizeof(ack))) {
		pr_warn("client_comm_vmshm: hello selftest got bad ACK\n");
		return -EPROTO;
	}

	pr_info("client_comm_vmshm: hello selftest sent %s and got %s\n",
		hello, ack);
	return 0;
}
#else
static int client_comm_vmshm_hello_selftest_run(void)
{
	return 0;
}
#endif

static void client_comm_vmshm_notify_init(struct platform_device *pdev,
					  struct client_comm_vmshm_dev *d)
{
	struct device_node *np = pdev->dev.of_node;
	u64 doorbell[2];
	int irq, ret;

	if (!np ||
	    of_property_read_u64_array(np, "vmshm-doorbell-reg", doorbell, 2)) {
		dev_info(&pdev->dev,
			 "irq notify unavailable, falling back to polling\n");
		return;
	}

	irq = platform_get_irq_optional(pdev, 0);
	if (irq < 0) {
		dev_info(&pdev->dev,
			 "missing irq for notify, falling back to polling\n");
		return;
	}

	d->doorbell = devm_ioremap(&pdev->dev, doorbell[0], doorbell[1]);
	if (!d->doorbell) {
		dev_info(&pdev->dev,
			 "doorbell ioremap failed, falling back to polling\n");
		return;
	}

	ret = devm_request_irq(&pdev->dev, irq, client_comm_vmshm_irq, 0,
			       CLIENT_COMM_VMSHM_NAME, d);
	if (ret) {
		dev_info(&pdev->dev,
			 "request_irq failed (%d), falling back to polling\n",
			 ret);
		d->doorbell = NULL;
		return;
	}

	d->irq = irq;
	d->doorbell_size = doorbell[1];
	d->irq_notify = true;
	dev_info(&pdev->dev,
		 "irq notify enabled irq=%d doorbell=0x%llx size=0x%llx\n",
		 irq, doorbell[0], doorbell[1]);
}

static int client_comm_vmshm_open(struct inode *inode, struct file *filp)
{
	if (atomic_inc_return(&client_comm_vmshm_dev.open_cnt) < 0) {
		atomic_dec(&client_comm_vmshm_dev.open_cnt);
		return -EMFILE;
	}

	filp->private_data = &client_comm_vmshm_dev;
	return 0;
}

static int client_comm_vmshm_release(struct inode *inode, struct file *filp)
{
	atomic_dec(&client_comm_vmshm_dev.open_cnt);
	return 0;
}

static loff_t client_comm_vmshm_llseek(struct file *filp, loff_t offset,
				       int whence)
{
	loff_t newpos;

	switch (whence) {
	case SEEK_SET:
		newpos = offset;
		break;
	case SEEK_CUR:
		newpos = filp->f_pos + offset;
		break;
	case SEEK_END:
		newpos = client_comm_vmshm_dev.size + offset;
		break;
	default:
		return -EINVAL;
	}

	if (newpos < 0 || newpos > client_comm_vmshm_dev.size)
		return -EINVAL;

	filp->f_pos = newpos;
	return newpos;
}

static ssize_t client_comm_vmshm_read(struct file *filp, char __user *buf,
				      size_t count, loff_t *ppos)
{
	loff_t pos = *ppos;
	size_t avail;

	if (pos >= client_comm_vmshm_dev.size)
		return 0;

	avail = client_comm_vmshm_dev.size - pos;
	if (count > avail)
		count = avail;

	mutex_lock(&client_comm_vmshm_dev.lock);
	if (copy_to_user(buf, (u8 *)client_comm_vmshm_dev.base + pos, count))
		count = -EFAULT;
	else
		*ppos = pos + count;
	mutex_unlock(&client_comm_vmshm_dev.lock);

	return count;
}

static ssize_t client_comm_vmshm_write(struct file *filp,
				       const char __user *buf,
				       size_t count, loff_t *ppos)
{
	loff_t pos = *ppos;
	size_t avail;

	if (pos >= client_comm_vmshm_dev.size)
		return -ENOSPC;

	avail = client_comm_vmshm_dev.size - pos;
	if (count > avail)
		count = avail;

	mutex_lock(&client_comm_vmshm_dev.lock);
	if (copy_from_user((u8 *)client_comm_vmshm_dev.base + pos, buf, count))
		count = -EFAULT;
	else
		*ppos = pos + count;
	mutex_unlock(&client_comm_vmshm_dev.lock);

	return count;
}

static long client_comm_vmshm_ioctl(struct file *filp, unsigned int cmd,
				    unsigned long arg)
{
	switch (cmd) {
	case CLIENT_COMM_VMSHM_IOC_GET_SIZE: {
		__u64 sz = client_comm_vmshm_dev.size;

		if (copy_to_user((__u64 __user *)arg, &sz, sizeof(sz)))
			return -EFAULT;
		return 0;
	}
	case CLIENT_COMM_VMSHM_IOC_GET_BASE: {
		__u64 base = client_comm_vmshm_dev.gpa;

		if (copy_to_user((__u64 __user *)arg, &base, sizeof(base)))
			return -EFAULT;
		return 0;
	}
	case CLIENT_COMM_VMSHM_IOC_GET_INFO: {
		struct client_comm_vmshm_info info = {
			.gpa = client_comm_vmshm_dev.gpa,
			.size = client_comm_vmshm_dev.size,
			.header_size = sizeof(struct proxy_comm_vmshm_header),
			.queue_count = PROXY_COMM_VMSHM_MAX_QUEUES,
		};

		if (copy_to_user((void __user *)arg, &info, sizeof(info)))
			return -EFAULT;
		return 0;
	}
	default:
		return -ENOTTY;
	}
}

static const struct file_operations client_comm_vmshm_fops = {
	.owner		= THIS_MODULE,
	.open		= client_comm_vmshm_open,
	.release	= client_comm_vmshm_release,
	.read		= client_comm_vmshm_read,
	.write		= client_comm_vmshm_write,
	.llseek		= client_comm_vmshm_llseek,
	.unlocked_ioctl = client_comm_vmshm_ioctl,
};

static int client_comm_vmshm_probe(struct platform_device *pdev)
{
	struct resource *res;
	int ret;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev, "missing 'reg' resource in DT node\n");
		return -ENODEV;
	}

	client_comm_vmshm_dev.gpa = res->start;
	client_comm_vmshm_dev.size = resource_size(res);
	mutex_init(&client_comm_vmshm_dev.lock);
	mutex_init(&client_comm_vmshm_dev.rpc_lock);
	atomic_set(&client_comm_vmshm_dev.open_cnt, 0);
	atomic64_set(&client_comm_vmshm_dev.next_seq, 0);
	client_comm_vmshm_dev.irq_notify = false;
	client_comm_vmshm_dev.doorbell = NULL;
	client_comm_vmshm_dev.irq = -1;
	INIT_WORK(&client_comm_vmshm_dev.rx_work, client_comm_vmshm_rx_work);
	INIT_LIST_HEAD(&client_comm_vmshm_dev.waiters);
	spin_lock_init(&client_comm_vmshm_dev.waiters_lock);

	if (client_comm_vmshm_dev.size <
	    sizeof(struct proxy_comm_vmshm_header)) {
		dev_err(&pdev->dev, "shared window too small: 0x%pa\n",
			&client_comm_vmshm_dev.size);
		return -EINVAL;
	}

	dev_info(&pdev->dev, "GPA 0x%pa size 0x%pa\n",
		 &client_comm_vmshm_dev.gpa, &client_comm_vmshm_dev.size);

	client_comm_vmshm_dev.base = memremap(client_comm_vmshm_dev.gpa,
					      client_comm_vmshm_dev.size,
					      MEMREMAP_WB);
	if (!client_comm_vmshm_dev.base) {
		dev_err(&pdev->dev, "memremap failed\n");
		return -ENOMEM;
	}

	ret = client_comm_vmshm_attach_layout(&client_comm_vmshm_dev);
	if (ret) {
		dev_err(&pdev->dev, "shared protocol attach failed (%d)\n", ret);
		goto err_unmap;
	}

	client_comm_vmshm_notify_init(pdev, &client_comm_vmshm_dev);

	dev_info(&pdev->dev,
		 "attached protocol generation=%u objects=%u heap=[0x%x,0x%x]\n",
		 client_comm_vmshm_dev.hdr->generation,
		 client_comm_vmshm_dev.hdr->object_count,
		 client_comm_vmshm_dev.hdr->heap_base_off,
		 client_comm_vmshm_dev.hdr->heap_size);

	ret = client_comm_vmshm_hello_selftest_run();
	if (ret)
		goto err_unmap;

	ret = alloc_chrdev_region(&client_comm_vmshm_dev.devt, 0, 1,
				  CLIENT_COMM_VMSHM_NAME);
	if (ret < 0) {
		dev_err(&pdev->dev, "alloc_chrdev_region failed (%d)\n", ret);
		goto err_unmap;
	}

	cdev_init(&client_comm_vmshm_dev.cdev, &client_comm_vmshm_fops);
	client_comm_vmshm_dev.cdev.owner = THIS_MODULE;
	ret = cdev_add(&client_comm_vmshm_dev.cdev,
		       client_comm_vmshm_dev.devt, 1);
	if (ret < 0) {
		dev_err(&pdev->dev, "cdev_add failed (%d)\n", ret);
		goto err_unregister_chrdev;
	}

	client_comm_vmshm_dev.class = class_create(CLIENT_COMM_VMSHM_NAME);
	if (IS_ERR(client_comm_vmshm_dev.class)) {
		ret = PTR_ERR(client_comm_vmshm_dev.class);
		dev_err(&pdev->dev, "class_create failed (%d)\n", ret);
		goto err_cdev_del;
	}

	client_comm_vmshm_dev.dev = device_create(client_comm_vmshm_dev.class,
						  &pdev->dev,
						  client_comm_vmshm_dev.devt,
						  NULL,
						  CLIENT_COMM_VMSHM_NAME);
	if (IS_ERR(client_comm_vmshm_dev.dev)) {
		ret = PTR_ERR(client_comm_vmshm_dev.dev);
		dev_err(&pdev->dev, "device_create failed (%d)\n", ret);
		goto err_class_destroy;
	}

	dev_info(&pdev->dev, "/dev/%s registered (major %d)\n",
		 CLIENT_COMM_VMSHM_NAME, MAJOR(client_comm_vmshm_dev.devt));
	return 0;

err_class_destroy:
	class_destroy(client_comm_vmshm_dev.class);
err_cdev_del:
	cdev_del(&client_comm_vmshm_dev.cdev);
err_unregister_chrdev:
	unregister_chrdev_region(client_comm_vmshm_dev.devt, 1);
err_unmap:
	cancel_work_sync(&client_comm_vmshm_dev.rx_work);
	memunmap(client_comm_vmshm_dev.base);
	client_comm_vmshm_dev.base = NULL;
	return ret;
}

static const struct of_device_id client_comm_vmshm_of_match[] = {
	{ .compatible = "client_comm_vmshm" },
	{ }
};

static struct platform_driver client_comm_vmshm_driver = {
	.probe = client_comm_vmshm_probe,
	.driver = {
		.name = CLIENT_COMM_VMSHM_NAME,
		.of_match_table = client_comm_vmshm_of_match,
	},
};

builtin_platform_driver(client_comm_vmshm_driver);
