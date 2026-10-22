// SPDX-License-Identifier: GPL-2.0
/*
 * proxy_comm_vmshm - RMM-authorized Realm queue transport.
 * Pool binding supplies authenticated channel identity and mapped queue pages.
 * Notifications use RMM IRQ subscription; queue data is validated privately.
 * The character device only holds a lifetime reference for unbind checks.
 */

#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/idr.h>
#include <linux/io.h>
#include <linux/list.h>
#include <linux/log2.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/refcount.h>
#include <linux/realm_xshm.h>
#include <asm/rsi.h>
#include <linux/rwsem.h>
#include <linux/slab.h>
#include <linux/vmshm_comm.h>
#include <linux/vmshm_comm_protocol.h>
#include <linux/workqueue.h>

#define PROXY_COMM_VMSHM_NAME		"proxy_comm_vmshm"
#define PROXY_COMM_VMSHM_SELFTEST_REQ	0x54534551U /* "TSEQ" */
#define PROXY_COMM_VMSHM_SELFTEST_RSP	0x54534552U /* "TSER" */
#define PROXY_COMM_VMSHM_MAX_HANDLERS	32
#define PROXY_COMM_VMSHM_MAX_CHANNELS	32

enum proxy_comm_vmshm_obj_type {
	PROXY_COMM_VMSHM_OBJ_QUEUE = 1,
	PROXY_COMM_VMSHM_OBJ_DESC_TABLE,
	PROXY_COMM_VMSHM_OBJ_AVAIL_RING,
	PROXY_COMM_VMSHM_OBJ_USED_RING,
	PROXY_COMM_VMSHM_OBJ_MSG_POOL,
};


struct proxy_comm_vmshm_dev;

struct proxy_comm_vmshm_channel {
	struct proxy_comm_vmshm_dev *dev;
};

struct proxy_comm_vmshm_dev {
	struct proxy_comm_vmshm_channel channel;
	struct list_head node;
	void *base;
	struct proxy_comm_vmshm_header *hdr;
	/* Validated, private geometry and locally owned ring progress. */
	struct proxy_comm_vmshm_header layout;
	struct proxy_comm_vmshm_queue_object queues[PROXY_COMM_VMSHM_MAX_QUEUES];
	u32 tx_avail[PROXY_COMM_VMSHM_MAX_QUEUES];
	u32 rx_used[PROXY_COMM_VMSHM_MAX_QUEUES];
	phys_addr_t gpa;
	resource_size_t size;
	u32 client_vmid;
	bool irq_notify;
	struct work_struct rx_work;
	u64 perf_noop_count;
	u64 realm_response_notifies;
	u64 realm_response_suppressed;
	atomic64_t trace_irq;
	u64 trace_work_irq, trace_work_start;
	dev_t devt;
	struct cdev cdev;
	struct device *dev;
	struct mutex lock;
	refcount_t users;
	bool quiescing;
	const struct realm_xshm_transport *realm;
	int minor;
	bool layout_owner;
	bool registered;
	bool cdev_added;
};

struct proxy_comm_vmshm_handler_slot {
	u32 type;
	proxy_comm_vmshm_rx_handler_t handler;
	void *priv;
};

static DEFINE_MUTEX(proxy_comm_vmshm_channels_lock);
static LIST_HEAD(proxy_comm_vmshm_channels);
static DECLARE_RWSEM(proxy_comm_vmshm_handler_rwsem);
static struct proxy_comm_vmshm_handler_slot
	proxy_comm_vmshm_handlers[PROXY_COMM_VMSHM_MAX_HANDLERS];
static unsigned int proxy_comm_vmshm_handler_count;
static DEFINE_MUTEX(proxy_comm_vmshm_chardev_lock);
static DEFINE_IDA(proxy_comm_vmshm_minor_ida);
static dev_t proxy_comm_vmshm_devt;
static struct class *proxy_comm_vmshm_class;
static unsigned int proxy_comm_vmshm_chardev_users;

static void proxy_comm_vmshm_rx_work(struct work_struct *work);

static void proxy_comm_vmshm_queue_rx(struct proxy_comm_vmshm_dev *d)
{
	if (!d)
		return;

	schedule_work(&d->rx_work);
}

static bool proxy_comm_vmshm_range_valid(struct proxy_comm_vmshm_dev *d,
					 u32 offset, u32 size)
{
	if (offset > d->size)
		return false;

	return size <= d->size - offset;
}

static void *proxy_comm_vmshm_ptr(struct proxy_comm_vmshm_dev *d,
				  u32 offset, u32 size)
{
	if (!proxy_comm_vmshm_range_valid(d, offset, size))
		return NULL;

	return (void *)((u8 *)d->base + offset);
}

static struct proxy_comm_vmshm_object *
proxy_comm_vmshm_object_table(struct proxy_comm_vmshm_dev *d)
{
	struct proxy_comm_vmshm_header *h = &d->layout;
	u32 table_size;

	table_size = h->object_capacity * sizeof(struct proxy_comm_vmshm_object);
	return proxy_comm_vmshm_ptr(d, h->object_table_off, table_size);
}

static int proxy_comm_vmshm_alloc_object(struct proxy_comm_vmshm_dev *d,
					 u32 type, u32 size, u32 align,
					 u32 flags, u32 *obj_id, u32 *offset)
{
	struct proxy_comm_vmshm_header *h = &d->layout;
	struct proxy_comm_vmshm_object *objects, *obj;
	u32 aligned, end;

	if (!size || !align || !is_power_of_2(align))
		return -EINVAL;
	if (!obj_id || !offset)
		return -EINVAL;

	objects = proxy_comm_vmshm_object_table(d);
	if (!objects)
		return -EINVAL;
	if (h->object_count >= h->object_capacity)
		return -ENOSPC;

	aligned = ALIGN(h->next_free_off, align);
	if (aligned < h->next_free_off)
		return -EOVERFLOW;
	if (!proxy_comm_vmshm_range_valid(d, aligned, size))
		return -ENOSPC;

	end = aligned + size;
	if (end < aligned)
		return -EOVERFLOW;

	obj = &objects[h->object_count];
	obj->obj_id = h->object_count + 1;
	obj->type = type;
	obj->offset = aligned;
	obj->size = size;
	obj->align = align;
	obj->flags = flags;
	obj->generation = h->generation;

	memset(proxy_comm_vmshm_ptr(d, aligned, size), 0, size);

	h->object_count++;
	h->next_free_off = end;
	*obj_id = obj->obj_id;
	*offset = aligned;
	return 0;
}

static struct proxy_comm_vmshm_queue_object *
proxy_comm_vmshm_queue_obj(struct proxy_comm_vmshm_dev *d, u32 queue_id)
{
	if (queue_id >= PROXY_COMM_VMSHM_MAX_QUEUES ||
	    d->queues[queue_id].magic != PROXY_COMM_VMSHM_QUEUE_MAGIC)
		return NULL;
	return &d->queues[queue_id];
}

static struct proxy_comm_vmshm_desc *
proxy_comm_vmshm_descs(struct proxy_comm_vmshm_dev *d,
		       struct proxy_comm_vmshm_queue_object *q)
{
	return proxy_comm_vmshm_ptr(d, q->desc_off,
				    q->queue_size * sizeof(struct proxy_comm_vmshm_desc));
}

static struct proxy_comm_vmshm_avail_ring *
proxy_comm_vmshm_avail(struct proxy_comm_vmshm_dev *d,
		       struct proxy_comm_vmshm_queue_object *q)
{
	u32 size = sizeof(struct proxy_comm_vmshm_avail_ring) +
		   q->queue_size * sizeof(u16);

	return proxy_comm_vmshm_ptr(d, q->avail_off, size);
}

static struct proxy_comm_vmshm_used_ring *
proxy_comm_vmshm_used(struct proxy_comm_vmshm_dev *d,
		      struct proxy_comm_vmshm_queue_object *q)
{
	u32 size = sizeof(struct proxy_comm_vmshm_used_ring) +
		   q->queue_size * sizeof(struct proxy_comm_vmshm_used_elem);

	return proxy_comm_vmshm_ptr(d, q->used_off, size);
}

static int proxy_comm_vmshm_queue_reset(struct proxy_comm_vmshm_dev *d,
					u32 queue_id)
{
	struct proxy_comm_vmshm_queue_object *q;
	struct proxy_comm_vmshm_desc *desc;
	struct proxy_comm_vmshm_avail_ring *avail;
	struct proxy_comm_vmshm_used_ring *used;
	void *msg_pool;
	u32 desc_size, avail_size, used_size, msg_pool_size;

	q = proxy_comm_vmshm_queue_obj(d, queue_id);
	if (!q || q->magic != PROXY_COMM_VMSHM_QUEUE_MAGIC)
		return -EINVAL;

	desc_size = q->queue_size * sizeof(*desc);
	avail_size = sizeof(*avail) + q->queue_size * sizeof(u16);
	used_size = sizeof(*used) + q->queue_size * sizeof(*used->ring);
	msg_pool_size = q->queue_size * q->msg_size;

	desc = proxy_comm_vmshm_ptr(d, q->desc_off, desc_size);
	avail = proxy_comm_vmshm_ptr(d, q->avail_off, avail_size);
	used = proxy_comm_vmshm_ptr(d, q->used_off, used_size);
	msg_pool = proxy_comm_vmshm_ptr(d, q->msg_pool_off, msg_pool_size);
	if (!desc || !avail || !used || !msg_pool)
		return -EINVAL;

	memset(desc, 0, desc_size);
	memset(avail, 0, avail_size);
	memset(used, 0, used_size);
	memset(msg_pool, 0, msg_pool_size);
	d->tx_avail[queue_id] = 0;
	d->rx_used[queue_id] = 0;
	return 0;
}

static int proxy_comm_vmshm_setup_queue(struct proxy_comm_vmshm_dev *d,
					u32 queue_id, u32 direction)
{
	struct proxy_comm_vmshm_header *h = &d->layout;
	struct proxy_comm_vmshm_queue_object *qobj;
	struct proxy_comm_vmshm_queue *qdesc;
	u32 queue_obj_id, desc_obj_id, avail_obj_id, used_obj_id;
	u32 msg_pool_obj_id, queue_off, desc_off, avail_off, used_off;
	u32 msg_pool_off, desc_size, avail_size, used_size, msg_pool_size;
	int ret;

	desc_size = PROXY_COMM_VMSHM_QUEUE_SIZE *
		    sizeof(struct proxy_comm_vmshm_desc);
	avail_size = sizeof(struct proxy_comm_vmshm_avail_ring) +
		     PROXY_COMM_VMSHM_QUEUE_SIZE * sizeof(u16);
	used_size = sizeof(struct proxy_comm_vmshm_used_ring) +
		    PROXY_COMM_VMSHM_QUEUE_SIZE *
			    sizeof(struct proxy_comm_vmshm_used_elem);
	msg_pool_size = PROXY_COMM_VMSHM_QUEUE_SIZE *
			PROXY_COMM_VMSHM_MSG_SIZE;

	ret = proxy_comm_vmshm_alloc_object(d, PROXY_COMM_VMSHM_OBJ_QUEUE,
					    sizeof(*qobj),
					    PROXY_COMM_VMSHM_ALIGN, 0,
					    &queue_obj_id, &queue_off);
	if (ret)
		return ret;

	ret = proxy_comm_vmshm_alloc_object(d, PROXY_COMM_VMSHM_OBJ_DESC_TABLE,
					    desc_size, PROXY_COMM_VMSHM_ALIGN,
					    0, &desc_obj_id, &desc_off);
	if (ret)
		return ret;

	ret = proxy_comm_vmshm_alloc_object(d, PROXY_COMM_VMSHM_OBJ_AVAIL_RING,
					    avail_size, PROXY_COMM_VMSHM_ALIGN,
					    0, &avail_obj_id, &avail_off);
	if (ret)
		return ret;

	ret = proxy_comm_vmshm_alloc_object(d, PROXY_COMM_VMSHM_OBJ_USED_RING,
					    used_size, PROXY_COMM_VMSHM_ALIGN,
					    0, &used_obj_id, &used_off);
	if (ret)
		return ret;

	ret = proxy_comm_vmshm_alloc_object(d, PROXY_COMM_VMSHM_OBJ_MSG_POOL,
					    msg_pool_size,
					    PROXY_COMM_VMSHM_ALIGN, 0,
					    &msg_pool_obj_id, &msg_pool_off);
	if (ret)
		return ret;

	qobj = &d->queues[queue_id];

	qobj->magic = PROXY_COMM_VMSHM_QUEUE_MAGIC;
	qobj->queue_id = queue_id;
	qobj->direction = direction;
	qobj->queue_size = PROXY_COMM_VMSHM_QUEUE_SIZE;
	qobj->queue_mask = PROXY_COMM_VMSHM_QUEUE_SIZE - 1;
	qobj->msg_size = PROXY_COMM_VMSHM_MSG_SIZE;
	qobj->desc_off = desc_off;
	qobj->avail_off = avail_off;
	qobj->used_off = used_off;
	qobj->msg_pool_off = msg_pool_off;
	qobj->desc_obj_id = desc_obj_id;
	qobj->avail_obj_id = avail_obj_id;
	qobj->used_obj_id = used_obj_id;
	qobj->msg_pool_obj_id = msg_pool_obj_id;

	qdesc = &h->queues[queue_id];
	qdesc->queue_obj_id = queue_obj_id;
	qdesc->desc_obj_id = desc_obj_id;
	qdesc->avail_obj_id = avail_obj_id;
	qdesc->used_obj_id = used_obj_id;
	qdesc->msg_pool_obj_id = msg_pool_obj_id;
	qdesc->queue_off = queue_off;
	qdesc->size = PROXY_COMM_VMSHM_QUEUE_SIZE;
	qdesc->msg_size = PROXY_COMM_VMSHM_MSG_SIZE;
	qdesc->desc_off = desc_off;
	qdesc->avail_off = avail_off;
	qdesc->used_off = used_off;
	qdesc->msg_pool_off = msg_pool_off;
	qdesc->direction = direction;

	memcpy(proxy_comm_vmshm_ptr(d, queue_off, sizeof(*qobj)), qobj, sizeof(*qobj));
	return proxy_comm_vmshm_queue_reset(d, queue_id);
}

static int proxy_comm_vmshm_build_layout(struct proxy_comm_vmshm_dev *d)
{
	struct proxy_comm_vmshm_header *h = &d->layout;
	u32 object_table_off, object_table_size, heap_base;
	int ret;

	memset(d->base, 0, d->size);

	h->magic = PROXY_COMM_VMSHM_INIT_MAGIC;
	h->version = PROXY_COMM_VMSHM_VERSION;
	h->header_size = sizeof(*h);
	h->total_size = d->size;
	h->generation = 1;
	h->status[PROXY_COMM_VMSHM_ROLE_PROXY] = PROXY_COMM_VMSHM_STATUS_INIT;
	h->status[PROXY_COMM_VMSHM_ROLE_CLIENT] = PROXY_COMM_VMSHM_STATUS_RESET;
	h->queue_count = PROXY_COMM_VMSHM_MAX_QUEUES;
	h->feature_bits = 0;

	object_table_off = ALIGN(sizeof(*h), PROXY_COMM_VMSHM_ALIGN);
	object_table_size = PROXY_COMM_VMSHM_MAX_OBJECTS *
			    sizeof(struct proxy_comm_vmshm_object);
	heap_base = ALIGN(object_table_off + object_table_size,
			  PROXY_COMM_VMSHM_ALIGN);
	if (!proxy_comm_vmshm_range_valid(d, heap_base, 1))
		return -ENOSPC;

	h->object_table_off = object_table_off;
	h->object_capacity = PROXY_COMM_VMSHM_MAX_OBJECTS;
	h->object_count = 0;
	h->heap_base_off = heap_base;
	h->heap_size = d->size - heap_base;
	h->next_free_off = heap_base;

	ret = proxy_comm_vmshm_setup_queue(d, 0,
					   PROXY_COMM_VMSHM_Q_CLIENT_TO_PROXY);
	if (ret)
		return ret;

	ret = proxy_comm_vmshm_setup_queue(d, 1,
					   PROXY_COMM_VMSHM_Q_PROXY_TO_CLIENT);
	if (ret)
		return ret;

	smp_wmb();
	/* Publish READY only after the private initialization checks finish. */
	memcpy(d->hdr, h, sizeof(*h));
	return 0;
}

static int proxy_comm_vmshm_validate_layout(struct proxy_comm_vmshm_dev *d)
{
	struct proxy_comm_vmshm_header snapshot, *h = &snapshot;
	u32 table_size, i;

	if (d->size < sizeof(snapshot))
		return -EINVAL;
	memcpy(h, d->hdr, sizeof(*h));
	barrier();
	if (h->magic != PROXY_COMM_VMSHM_MAGIC ||
	    h->version != PROXY_COMM_VMSHM_VERSION ||
	    h->header_size != sizeof(*h) ||
	    h->queue_count != PROXY_COMM_VMSHM_MAX_QUEUES ||
	    h->total_size > d->size ||
	    h->object_count > h->object_capacity ||
	    h->object_capacity > PROXY_COMM_VMSHM_MAX_OBJECTS)
		return -EINVAL;
	table_size = h->object_capacity * sizeof(struct proxy_comm_vmshm_object);
	if (!proxy_comm_vmshm_range_valid(d, h->object_table_off, table_size) ||
	    !proxy_comm_vmshm_range_valid(d, h->heap_base_off, h->heap_size))
		return -EINVAL;
	for (i = 0; i < PROXY_COMM_VMSHM_MAX_QUEUES; i++) {
		struct proxy_comm_vmshm_queue_object *q = &d->queues[i];
		void *shared = proxy_comm_vmshm_ptr(d, h->queues[i].queue_off,
						 sizeof(*q));

		if (!shared)
			return -EINVAL;
		memcpy(q, shared, sizeof(*q));
		barrier();
		if (q->magic != PROXY_COMM_VMSHM_QUEUE_MAGIC ||
		    q->queue_id != i || q->direction != i ||
		    q->queue_size != PROXY_COMM_VMSHM_QUEUE_SIZE ||
		    q->queue_mask != PROXY_COMM_VMSHM_QUEUE_SIZE - 1 ||
		    q->msg_size != PROXY_COMM_VMSHM_MSG_SIZE ||
		    !IS_ALIGNED(q->desc_off, 8) ||
		    !IS_ALIGNED(q->avail_off, 4) ||
		    !IS_ALIGNED(q->used_off, 4) ||
		    !IS_ALIGNED(q->msg_pool_off, 8) ||
		    !proxy_comm_vmshm_descs(d, q) ||
		    !proxy_comm_vmshm_avail(d, q) ||
		    !proxy_comm_vmshm_used(d, q) ||
		    !proxy_comm_vmshm_ptr(d, q->msg_pool_off,
					 q->queue_size * q->msg_size))
			return -EINVAL;
		if (smp_load_acquire(&proxy_comm_vmshm_avail(d, q)->idx) != 0 ||
		    smp_load_acquire(&proxy_comm_vmshm_used(d, q)->idx) != 0)
			return -EBUSY;
	}
	d->layout = snapshot;
	return 0;
}

static int proxy_comm_vmshm_init_layout(struct proxy_comm_vmshm_dev *d)
{
	struct proxy_comm_vmshm_header *h = d->base;
	u32 magic;
	int i, ret;

	if (d->size > U32_MAX)
		return -EOVERFLOW;

	d->hdr = h;
	d->layout_owner = false;

	for (i = 0; i < PROXY_COMM_VMSHM_INIT_RETRIES; i++) {
		magic = smp_load_acquire(&h->magic);
		if (magic == PROXY_COMM_VMSHM_MAGIC)
			return proxy_comm_vmshm_validate_layout(d);

		if (magic == PROXY_COMM_VMSHM_INIT_MAGIC) {
			usleep_range(PROXY_COMM_VMSHM_INIT_WAIT_US,
				     PROXY_COMM_VMSHM_INIT_WAIT_US * 2);
			continue;
		}

		if (cmpxchg(&h->magic, magic,
			    PROXY_COMM_VMSHM_INIT_MAGIC) != magic)
			continue;

		d->layout_owner = true;
		ret = proxy_comm_vmshm_build_layout(d);
		if (ret) {
			WRITE_ONCE(h->magic, 0);
			return ret;
		}

		/* The owner built private geometry; READY follows its selftests. */
		return 0;
	}

	return -ETIMEDOUT;
}

static int proxy_comm_vmshm_queue_send(struct proxy_comm_vmshm_dev *d,
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

	q = proxy_comm_vmshm_queue_obj(d, queue_id);
	if (!q)
		return -EINVAL;
	if (tx->len > q->msg_size - sizeof(*msg))
		return -EMSGSIZE;
	if (tx->len && !tx->payload)
		return -EINVAL;

	descs = proxy_comm_vmshm_descs(d, q);
	avail = proxy_comm_vmshm_avail(d, q);
	used = proxy_comm_vmshm_used(d, q);
	if (!descs || !avail || !used)
		return -EINVAL;

	avail_idx = d->tx_avail[queue_id];
	used_idx = smp_load_acquire(&used->idx);
	if (avail_idx - used_idx >= q->queue_size)
		return -EAGAIN;

	ring_idx = avail_idx & q->queue_mask;
	desc_id = ring_idx;
	msg_off = q->msg_pool_off + desc_id * q->msg_size;
	msg = proxy_comm_vmshm_ptr(d, msg_off, q->msg_size);
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
	d->tx_avail[queue_id] = avail_idx + 1;
	smp_store_release(&avail->idx, avail_idx + 1);
	return 0;
}

static int proxy_comm_vmshm_queue_recv(struct proxy_comm_vmshm_dev *d,
				       u32 queue_id,
				       struct vmshm_comm_rx *rx)
{
	struct proxy_comm_vmshm_queue_object *q;
	struct proxy_comm_vmshm_desc *descs, descriptor, *desc = &descriptor;
	struct proxy_comm_vmshm_avail_ring *avail;
	struct proxy_comm_vmshm_used_ring *used;
	u8 snapshot[PROXY_COMM_VMSHM_MSG_SIZE] __aligned(8);
	struct proxy_comm_vmshm_msg *msg = (void *)snapshot;
	void *shared;
	u32 avail_idx, used_idx, ring_idx, desc_id, expected_msg_off;
	int ret = -EINVAL;

	if (!rx)
		return -EINVAL;
	rx->len = 0;

	q = proxy_comm_vmshm_queue_obj(d, queue_id);
	if (!q)
		return -EINVAL;

	descs = proxy_comm_vmshm_descs(d, q);
	avail = proxy_comm_vmshm_avail(d, q);
	used = proxy_comm_vmshm_used(d, q);
	if (!descs || !avail || !used)
		return -EINVAL;

	used_idx = d->rx_used[queue_id];
	avail_idx = smp_load_acquire(&avail->idx);
	if (used_idx == avail_idx)
		return -ENOENT;
	if (avail_idx - used_idx > q->queue_size)
		return -EPROTO;

	ring_idx = used_idx & q->queue_mask;
	desc_id = READ_ONCE(avail->ring[ring_idx]);
	if (desc_id >= q->queue_size)
		goto out_used;

	memcpy(desc, &descs[desc_id], sizeof(*desc));
	barrier();

	expected_msg_off = q->msg_pool_off + desc_id * q->msg_size;
	if (desc->msg_off != expected_msg_off ||
	    desc->msg_capacity != q->msg_size ||
	    desc->msg_len < sizeof(*msg) ||
	    desc->msg_len > q->msg_size)
		goto out_used;

	shared = proxy_comm_vmshm_ptr(d, expected_msg_off, sizeof(snapshot));
	if (!shared)
		goto out_used;
	/* Copy a fixed bounded slot before inspecting peer-controlled lengths. */
	memcpy(snapshot, shared, sizeof(snapshot));
	barrier();

	if (msg->len > q->msg_size - sizeof(*msg) ||
	    sizeof(*msg) + msg->len != desc->msg_len)
		goto out_used;

	if (msg->len) {
		if (!rx->payload || msg->len > rx->payload_capacity) {
			ret = -EMSGSIZE;
			goto out_used;
		}
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
	d->rx_used[queue_id] = used_idx + 1;
	smp_store_release(&used->idx, used_idx + 1);
	return ret;
}

static int proxy_comm_vmshm_selftest_objects(struct proxy_comm_vmshm_dev *d)
{
	struct proxy_comm_vmshm_header *h = &d->layout;
	struct proxy_comm_vmshm_object *objects;
	u32 i, j;

	objects = proxy_comm_vmshm_object_table(d);
	if (!objects)
		return -EINVAL;

	for (i = 0; i < h->object_count; i++) {
		struct proxy_comm_vmshm_object *a = &objects[i];
		u32 a_end;

		if (!a->obj_id || !a->type || !a->size || !a->align ||
		    !is_power_of_2(a->align))
			return -EINVAL;
		if (!IS_ALIGNED(a->offset, a->align))
			return -EINVAL;
		if (!proxy_comm_vmshm_range_valid(d, a->offset, a->size))
			return -EINVAL;

		a_end = a->offset + a->size;
		if (a_end < a->offset)
			return -EOVERFLOW;

		for (j = i + 1; j < h->object_count; j++) {
			struct proxy_comm_vmshm_object *b = &objects[j];
			u32 b_end = b->offset + b->size;

			if (b_end < b->offset)
				return -EOVERFLOW;
			if (a->offset < b_end && b->offset < a_end)
				return -EINVAL;
		}
	}

	pr_info("proxy_comm_vmshm: object table checked (%u objects)\n",
		h->object_count);
	return 0;
}

static int proxy_comm_vmshm_selftest_req_rsp(struct proxy_comm_vmshm_dev *d)
{
	struct vmshm_comm_rx rx = {};
	struct vmshm_comm_tx tx = {};
	int ret;

	ret = proxy_comm_vmshm_queue_reset(d, 0);
	if (ret)
		return ret;
	ret = proxy_comm_vmshm_queue_reset(d, 1);
	if (ret)
		return ret;

	tx.type = PROXY_COMM_VMSHM_SELFTEST_REQ;
	tx.seq = 1;
	ret = proxy_comm_vmshm_queue_send(d, 0, &tx);
	if (ret)
		return ret;

	ret = proxy_comm_vmshm_queue_recv(d, 0, &rx);
	if (ret)
		return ret;
	if (rx.type != PROXY_COMM_VMSHM_SELFTEST_REQ || rx.seq != 1)
		return -EINVAL;

	memset(&tx, 0, sizeof(tx));
	tx.type = PROXY_COMM_VMSHM_SELFTEST_RSP;
	tx.seq = 2;
	tx.reply_to = rx.seq;
	ret = proxy_comm_vmshm_queue_send(d, 1, &tx);
	if (ret)
		return ret;

	ret = proxy_comm_vmshm_queue_recv(d, 1, &rx);
	if (ret)
		return ret;
	if (rx.type != PROXY_COMM_VMSHM_SELFTEST_RSP || rx.reply_to != 1)
		return -EINVAL;

	pr_info("proxy_comm_vmshm: generic request/response queue test passed\n");
	return 0;
}

static int proxy_comm_vmshm_selftest_wrap(struct proxy_comm_vmshm_dev *d)
{
	struct vmshm_comm_rx rx = {};
	struct vmshm_comm_tx tx = {
		.type = PROXY_COMM_VMSHM_SELFTEST_REQ,
	};
	u32 i;
	int ret;

	ret = proxy_comm_vmshm_queue_reset(d, 0);
	if (ret)
		return ret;

	for (i = 0; i < PROXY_COMM_VMSHM_QUEUE_SIZE + 16; i++) {
		tx.seq = i + 1;
		ret = proxy_comm_vmshm_queue_send(d, 0, &tx);
		if (ret)
			return ret;

		ret = proxy_comm_vmshm_queue_recv(d, 0, &rx);
		if (ret)
			return ret;
		if (rx.type != PROXY_COMM_VMSHM_SELFTEST_REQ || rx.seq != i + 1)
			return -EINVAL;
	}

	pr_info("proxy_comm_vmshm: index wrap-around checked\n");
	return 0;
}

static int proxy_comm_vmshm_selftest_full(struct proxy_comm_vmshm_dev *d)
{
	struct vmshm_comm_rx rx = {};
	struct vmshm_comm_tx tx = {
		.type = PROXY_COMM_VMSHM_SELFTEST_REQ,
	};
	u32 i;
	int ret;

	ret = proxy_comm_vmshm_queue_reset(d, 0);
	if (ret)
		return ret;

	for (i = 0; i < PROXY_COMM_VMSHM_QUEUE_SIZE; i++) {
		tx.seq = i + 1;
		ret = proxy_comm_vmshm_queue_send(d, 0, &tx);
		if (ret)
			return ret;
	}

	tx.seq = 0xdead;
	ret = proxy_comm_vmshm_queue_send(d, 0, &tx);
	if (ret != -EAGAIN)
		return -EINVAL;

	for (i = 0; i < PROXY_COMM_VMSHM_QUEUE_SIZE; i++) {
		ret = proxy_comm_vmshm_queue_recv(d, 0, &rx);
		if (ret)
			return ret;
	}

	pr_info("proxy_comm_vmshm: queue full/drain checked\n");
	return 0;
}

static int proxy_comm_vmshm_selftest_invalid_desc(struct proxy_comm_vmshm_dev *d)
{
	struct proxy_comm_vmshm_queue_object *q;
	struct proxy_comm_vmshm_avail_ring *avail;
	struct proxy_comm_vmshm_used_ring *used;
	struct vmshm_comm_rx rx = {};
	int ret;

	ret = proxy_comm_vmshm_queue_reset(d, 0);
	if (ret)
		return ret;

	q = proxy_comm_vmshm_queue_obj(d, 0);
	if (!q)
		return -EINVAL;

	avail = proxy_comm_vmshm_avail(d, q);
	used = proxy_comm_vmshm_used(d, q);
	if (!avail || !used)
		return -EINVAL;

	WRITE_ONCE(avail->ring[0], q->queue_size + 1);
	smp_store_release(&avail->idx, 1);

	ret = proxy_comm_vmshm_queue_recv(d, 0, &rx);
	if (ret != -EINVAL)
		return -EINVAL;
	if (READ_ONCE(used->idx) != 1)
		return -EINVAL;

	pr_info("proxy_comm_vmshm: invalid descriptor checked\n");
	return 0;
}

static int proxy_comm_vmshm_selftest_invalid_msg(struct proxy_comm_vmshm_dev *d)
{
	struct proxy_comm_vmshm_queue_object *q;
	struct proxy_comm_vmshm_desc *desc;
	struct proxy_comm_vmshm_avail_ring *avail;
	struct proxy_comm_vmshm_used_ring *used;
	struct proxy_comm_vmshm_msg *msg;
	struct vmshm_comm_rx rx = {};
	int ret;

	ret = proxy_comm_vmshm_queue_reset(d, 0);
	if (ret)
		return ret;

	q = proxy_comm_vmshm_queue_obj(d, 0);
	if (!q)
		return -EINVAL;

	desc = proxy_comm_vmshm_descs(d, q);
	avail = proxy_comm_vmshm_avail(d, q);
	used = proxy_comm_vmshm_used(d, q);
	if (!desc || !avail || !used)
		return -EINVAL;

	msg = proxy_comm_vmshm_ptr(d, q->msg_pool_off, q->msg_size);
	if (!msg)
		return -EINVAL;

	memset(msg, 0, q->msg_size);
	msg->type = 0xffff;
	msg->len = q->msg_size;
	msg->seq = 1;

	desc[0].msg_off = q->msg_pool_off;
	desc[0].msg_capacity = q->msg_size;
	desc[0].msg_len = sizeof(*msg);
	desc[0].seq = 1;

	smp_wmb();
	WRITE_ONCE(avail->ring[0], 0);
	smp_store_release(&avail->idx, 1);

	ret = proxy_comm_vmshm_queue_recv(d, 0, &rx);
	if (ret != -EINVAL)
		return -EINVAL;
	if (READ_ONCE(used->idx) != 1)
		return -EINVAL;

	pr_info("proxy_comm_vmshm: invalid message length checked\n");
	return 0;
}

static int proxy_comm_vmshm_selftest_private_geometry(struct proxy_comm_vmshm_dev *d)
{
	struct proxy_comm_vmshm_queue_object *q = &d->queues[0];
	struct proxy_comm_vmshm_queue_object *shared;
	struct proxy_comm_vmshm_header saved_header = *d->hdr;
	struct proxy_comm_vmshm_queue_object saved_queue;
	struct proxy_comm_vmshm_avail_ring *avail = proxy_comm_vmshm_avail(d, q);
	struct proxy_comm_vmshm_used_ring *used = proxy_comm_vmshm_used(d, q);
	u32 value = 0x12345678, received = 0;
	struct vmshm_comm_tx tx = { .type = PROXY_COMM_VMSHM_SELFTEST_REQ,
		.payload = &value, .len = sizeof(value) };
	struct vmshm_comm_rx rx = { .payload = &received,
		.payload_capacity = sizeof(received) };
	int ret;

	shared = proxy_comm_vmshm_ptr(d, d->layout.queues[0].queue_off, sizeof(*shared));
	if (!shared || !avail || !used)
		return -EINVAL;
	saved_queue = *shared;
	ret = proxy_comm_vmshm_queue_reset(d, 0);
	if (ret)
		return ret;
	/* A peer may overwrite every shared layout/progress field it can map. */
	shared->queue_size = U32_MAX;
	shared->queue_mask = U32_MAX;
	shared->msg_size = U32_MAX;
	shared->desc_off = U32_MAX;
	d->hdr->queue_count = U32_MAX;
	d->hdr->queues[0].queue_off = U32_MAX;
	WRITE_ONCE(avail->idx, U32_MAX);
	ret = proxy_comm_vmshm_queue_send(d, 0, &tx);
	if (ret)
		goto restore;
	WRITE_ONCE(used->idx, U32_MAX);
	ret = proxy_comm_vmshm_queue_recv(d, 0, &rx);
	if (!ret && (received != value || rx.len != sizeof(value) ||
		     d->tx_avail[0] != 1 || d->rx_used[0] != 1))
		ret = -EINVAL;
restore:
	*d->hdr = saved_header;
	*shared = saved_queue;
	proxy_comm_vmshm_queue_reset(d, 0);
	if (ret)
		return ret;
	/* Exercise actual u32 progress wrap, not merely ring-mask wrap. */
	d->tx_avail[0] = U32_MAX - 1;
	d->rx_used[0] = U32_MAX - 1;
	WRITE_ONCE(avail->idx, U32_MAX - 1);
	WRITE_ONCE(used->idx, U32_MAX - 1);
	for (unsigned int i = 0; i < 4; i++) {
		ret = proxy_comm_vmshm_queue_send(d, 0, &tx);
		if (!ret)
			ret = proxy_comm_vmshm_queue_recv(d, 0, &rx);
		if (ret || received != value)
			return ret ? ret : -EINVAL;
	}
	if (d->tx_avail[0] != 2 || d->rx_used[0] != 2)
		return -EINVAL;
	proxy_comm_vmshm_queue_reset(d, 0);
	/* A producer cannot advertise more occupied slots than the ring holds. */
	smp_store_release(&avail->idx, q->queue_size + 1);
	ret = proxy_comm_vmshm_queue_recv(d, 0, &rx);
	proxy_comm_vmshm_queue_reset(d, 0);
	if (ret != -EPROTO)
		return -EINVAL;
	pr_info("proxy_comm_vmshm: VMSHM_BOUNDARY_CHECK=PASS geometry=private own_indices=private wrap=u32 producer_overflow=rejected\n");
	return 0;
}

static int proxy_comm_vmshm_selftest_run(struct proxy_comm_vmshm_dev *d)
{
	int ret;

	pr_info("proxy_comm_vmshm: selftest start\n");

	ret = proxy_comm_vmshm_selftest_objects(d);
	if (ret)
		goto out;
	ret = proxy_comm_vmshm_selftest_req_rsp(d);
	if (ret)
		goto out;
	ret = proxy_comm_vmshm_selftest_wrap(d);
	if (ret)
		goto out;
	ret = proxy_comm_vmshm_selftest_full(d);
	if (ret)
		goto out;
	ret = proxy_comm_vmshm_selftest_invalid_desc(d);
	if (ret)
		goto out;
	ret = proxy_comm_vmshm_selftest_invalid_msg(d);
	if (ret)
		goto out;
	ret = proxy_comm_vmshm_selftest_private_geometry(d);
	if (ret)
		goto out;

	proxy_comm_vmshm_queue_reset(d, 0);
	proxy_comm_vmshm_queue_reset(d, 1);
	pr_info("proxy_comm_vmshm: selftest passed\n");
	return 0;

out:
	pr_err("proxy_comm_vmshm: selftest failed (%d)\n", ret);
	return ret;
}

static bool proxy_comm_vmshm_ready_locked(struct proxy_comm_vmshm_dev *d)
{
	struct proxy_comm_vmshm_header *h = d->hdr;

	return !READ_ONCE(d->quiescing) && d->base && h &&
	       READ_ONCE(h->magic) == PROXY_COMM_VMSHM_MAGIC &&
	       READ_ONCE(h->status[PROXY_COMM_VMSHM_ROLE_PROXY]) ==
		       PROXY_COMM_VMSHM_STATUS_READY;
}

static void proxy_comm_vmshm_kick_client(struct proxy_comm_vmshm_dev *d,
					const struct vmshm_comm_tx *tx)
{
	if (d->realm) {
		struct proxy_comm_vmshm_queue_object *q =
			proxy_comm_vmshm_queue_obj(d, PROXY_COMM_VMSHM_Q_PROXY_TO_CLIENT);
		struct proxy_comm_vmshm_used_ring *used =
			q ? proxy_comm_vmshm_used(d, q) : NULL;

		/* Pair response publication with the caller's enable/ARM/recheck. */
		smp_mb();
		if (used && (READ_ONCE(used->flags) & PROXY_COMM_VMSHM_USED_NO_NOTIFY)) {
			d->realm_response_suppressed++;
			return;
		}
		d->realm_response_notifies++;
		d->realm->notify(d->realm->channel, tx->type, tx->reply_to);
	}
}

static void proxy_comm_vmshm_schedule_all_locked(void)
{
	struct proxy_comm_vmshm_dev *d;

	lockdep_assert_held(&proxy_comm_vmshm_channels_lock);

	list_for_each_entry(d, &proxy_comm_vmshm_channels, node) {
		if (d->irq_notify && proxy_comm_vmshm_ready_locked(d))
			proxy_comm_vmshm_queue_rx(d);
	}
}

static struct proxy_comm_vmshm_dev *
proxy_comm_vmshm_dev_from_channel(struct proxy_comm_vmshm_channel *channel)
{
	if (!channel)
		return NULL;

	return channel->dev;
}

bool proxy_comm_vmshm_channel_get(struct proxy_comm_vmshm_channel *channel)
{
	struct proxy_comm_vmshm_dev *d = proxy_comm_vmshm_dev_from_channel(channel);
	bool ready;

	if (!d)
		return false;
	mutex_lock(&d->lock);
	ready = proxy_comm_vmshm_ready_locked(d);
	if (ready)
		refcount_inc(&d->users);
	mutex_unlock(&d->lock);
	return ready;
}
EXPORT_SYMBOL_GPL(proxy_comm_vmshm_channel_get);

void proxy_comm_vmshm_channel_put(struct proxy_comm_vmshm_channel *channel)
{
	struct proxy_comm_vmshm_dev *d = proxy_comm_vmshm_dev_from_channel(channel);

	if (d)
		refcount_dec(&d->users);
}
EXPORT_SYMBOL_GPL(proxy_comm_vmshm_channel_put);

int proxy_comm_vmshm_quiesce(struct device *dev)
{
	struct proxy_comm_vmshm_dev *d = dev_get_drvdata(dev);
	int ret = 0;

	if (!d)
		return -ENODEV;
	/* Exclude a handler creating a new consumer while checking refs. */
	down_write(&proxy_comm_vmshm_handler_rwsem);
	mutex_lock(&d->lock);
	if (refcount_read(&d->users) != 1)
		ret = -EBUSY;
	else
		WRITE_ONCE(d->quiescing, true);
	mutex_unlock(&d->lock);
	up_write(&proxy_comm_vmshm_handler_rwsem);
	return ret;
}
EXPORT_SYMBOL_GPL(proxy_comm_vmshm_quiesce);

u32 proxy_comm_vmshm_channel_client_vmid(struct proxy_comm_vmshm_channel *channel)
{
	struct proxy_comm_vmshm_dev *d =
		proxy_comm_vmshm_dev_from_channel(channel);

	return d ? d->client_vmid : 0;
}
EXPORT_SYMBOL_GPL(proxy_comm_vmshm_channel_client_vmid);

int proxy_comm_vmshm_send_to_channel(struct proxy_comm_vmshm_channel *channel,
				     const struct vmshm_comm_tx *tx)
{
	struct proxy_comm_vmshm_dev *d =
		proxy_comm_vmshm_dev_from_channel(channel);
	int ret;

	if (!d)
		return -ENODEV;

	mutex_lock(&d->lock);
	if (!proxy_comm_vmshm_ready_locked(d)) {
		ret = -ENODEV;
		goto out_unlock;
	}

	ret = proxy_comm_vmshm_queue_send(d, PROXY_COMM_VMSHM_Q_PROXY_TO_CLIENT,
					  tx);
	if (!ret)
		proxy_comm_vmshm_kick_client(d, tx);

out_unlock:
	mutex_unlock(&d->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(proxy_comm_vmshm_send_to_channel);

static int proxy_comm_vmshm_recv_from_dev(struct proxy_comm_vmshm_dev *d,
					  struct vmshm_comm_rx *rx)
{
	int ret;

	if (!d)
		return -ENODEV;

	mutex_lock(&d->lock);
	if (!proxy_comm_vmshm_ready_locked(d)) {
		ret = -ENODEV;
		goto out_unlock;
	}

	ret = proxy_comm_vmshm_queue_recv(d,
					  PROXY_COMM_VMSHM_Q_CLIENT_TO_PROXY,
					  rx);
	if (!ret)
		rx->proxy_channel = &d->channel;

out_unlock:
	mutex_unlock(&d->lock);
	return ret;
}

static int proxy_comm_vmshm_dispatch_one(struct proxy_comm_vmshm_dev *d)
{
	u8 payload[PROXY_COMM_VMSHM_MSG_SIZE - sizeof(struct proxy_comm_vmshm_msg)];
	proxy_comm_vmshm_rx_handler_t handler = NULL;
	struct vmshm_comm_rx rx = {
		.payload = payload,
		.payload_capacity = sizeof(payload),
	};
	void *priv = NULL;
	int ret, i;

	down_read(&proxy_comm_vmshm_handler_rwsem);
	if (!proxy_comm_vmshm_handler_count) {
		up_read(&proxy_comm_vmshm_handler_rwsem);
		return -ENOENT;
	}

	if (!proxy_comm_vmshm_ready_locked(d)) {
		up_read(&proxy_comm_vmshm_handler_rwsem);
		return -ENODEV;
	}

	memset(payload, 0, sizeof(payload));
	ret = proxy_comm_vmshm_recv_from_dev(d, &rx);
	if (ret) {
		up_read(&proxy_comm_vmshm_handler_rwsem);
		return ret;
	}
	if (realm_xshm_trace_enabled())
		realm_xshm_trace_receive(d->realm->channel, rx.seq, rx.type,
					 d->trace_work_irq, d->trace_work_start);

	for (i = 0; i < PROXY_COMM_VMSHM_MAX_HANDLERS; i++) {
		if (proxy_comm_vmshm_handlers[i].handler &&
		    proxy_comm_vmshm_handlers[i].type == rx.type) {
			handler = proxy_comm_vmshm_handlers[i].handler;
			priv = proxy_comm_vmshm_handlers[i].priv;
			break;
		}
	}

	if (!handler) {
		pr_debug("proxy_comm_vmshm: no handler for type 0x%x\n",
		 rx.type);
		up_read(&proxy_comm_vmshm_handler_rwsem);
		return 0;
	}

	ret = handler(&rx, priv);
	up_read(&proxy_comm_vmshm_handler_rwsem);
	if (ret)
		pr_warn_ratelimited("proxy_comm_vmshm: handler 0x%x failed (%d)\n",
				    rx.type, ret);

	return 0;
}

static void proxy_comm_vmshm_drain_rx(struct proxy_comm_vmshm_dev *d)
{
	bool rechecked = false;

	for (;;) {
		int ret = proxy_comm_vmshm_dispatch_one(d);

		if (!ret) {
			rechecked = false;
			continue;
		}
		if (ret == -ENOENT && d->realm && d->irq_notify && !rechecked) {
			ret = d->realm->arm(d->realm->channel);
			if (ret < 0)
				return;
			rechecked = ret == 1;
			/* Publication before ARM cannot be lost: inspect once more. */
			continue;
		}
		if (ret == -ENOENT) {
			return;
		}
		if (ret == -ENODEV) {
			return;
		}

		pr_warn_ratelimited("proxy_comm_vmshm: dispatch recv failed (%d)\n",
				    ret);
		return;
	}
}

static void proxy_comm_vmshm_rx_work(struct work_struct *work)
{
	struct proxy_comm_vmshm_dev *d =
		container_of(work, struct proxy_comm_vmshm_dev, rx_work);

	if (realm_xshm_trace_enabled()) {
		d->trace_work_start = realm_xshm_counter();
		d->trace_work_irq = atomic64_xchg(&d->trace_irq, 0);
	}
	proxy_comm_vmshm_drain_rx(d);
}

static void proxy_comm_vmshm_receive(void *data)
{
	struct proxy_comm_vmshm_dev *d = data;

	if (realm_xshm_trace_enabled())
		atomic64_set(&d->trace_irq, realm_xshm_counter());
	proxy_comm_vmshm_queue_rx(d);
}

int proxy_comm_vmshm_register_handler(u32 type,
				      proxy_comm_vmshm_rx_handler_t handler,
				      void *priv)
{
	int ret = -ENOSPC;
	int i;

	if (!type || !handler)
		return -EINVAL;

	down_write(&proxy_comm_vmshm_handler_rwsem);
	for (i = 0; i < PROXY_COMM_VMSHM_MAX_HANDLERS; i++) {
		if (proxy_comm_vmshm_handlers[i].handler &&
		    proxy_comm_vmshm_handlers[i].type == type) {
			if (proxy_comm_vmshm_handlers[i].handler == handler &&
			    proxy_comm_vmshm_handlers[i].priv == priv)
				ret = 0;
			else
				ret = -EEXIST;
			goto out_unlock;
		}
	}

	for (i = 0; i < PROXY_COMM_VMSHM_MAX_HANDLERS; i++) {
		if (!proxy_comm_vmshm_handlers[i].handler) {
			proxy_comm_vmshm_handlers[i].type = type;
			proxy_comm_vmshm_handlers[i].handler = handler;
			proxy_comm_vmshm_handlers[i].priv = priv;
			proxy_comm_vmshm_handler_count++;
			mutex_lock(&proxy_comm_vmshm_channels_lock);
			ret = 0;
			proxy_comm_vmshm_schedule_all_locked();
			mutex_unlock(&proxy_comm_vmshm_channels_lock);
			goto out_unlock;
		}
	}

out_unlock:
	up_write(&proxy_comm_vmshm_handler_rwsem);
	return ret;
}
EXPORT_SYMBOL_GPL(proxy_comm_vmshm_register_handler);

void proxy_comm_vmshm_unregister_handler(u32 type,
					 proxy_comm_vmshm_rx_handler_t handler,
					 void *priv)
{
	int i;

	down_write(&proxy_comm_vmshm_handler_rwsem);
	for (i = 0; i < PROXY_COMM_VMSHM_MAX_HANDLERS; i++) {
		if (proxy_comm_vmshm_handlers[i].handler == handler &&
		    proxy_comm_vmshm_handlers[i].type == type &&
		    proxy_comm_vmshm_handlers[i].priv == priv) {
			proxy_comm_vmshm_handlers[i].handler = NULL;
			proxy_comm_vmshm_handlers[i].priv = NULL;
			proxy_comm_vmshm_handlers[i].type = 0;
			proxy_comm_vmshm_handler_count--;
			break;
		}
	}
	up_write(&proxy_comm_vmshm_handler_rwsem);
}
EXPORT_SYMBOL_GPL(proxy_comm_vmshm_unregister_handler);

#ifdef CONFIG_PROXY_VMSHM_COMM_HELLO_SELFTEST
static int
proxy_comm_vmshm_hello_send_ack(struct proxy_comm_vmshm_channel *channel,
				u64 reply_to)
{
	static const char ack[] = VMSHM_COMM_SELFTEST_ACK_PAYLOAD;
	struct vmshm_comm_tx tx = {
		.type = VMSHM_COMM_SELFTEST_HELLO_RSP,
		.reply_to = reply_to,
		.payload = ack,
		.len = sizeof(ack),
	};
	int ret, i;

	for (i = 0; i < PROXY_COMM_VMSHM_INIT_RETRIES; i++) {
		ret = proxy_comm_vmshm_send_to_channel(channel, &tx);
		if (ret != -EAGAIN)
			return ret;

		usleep_range(PROXY_COMM_VMSHM_INIT_WAIT_US,
			     PROXY_COMM_VMSHM_INIT_WAIT_US * 2);
	}

	return -ETIMEDOUT;
}

static int proxy_comm_vmshm_hello_handler(const struct vmshm_comm_rx *rx,
					  void *priv)
{
	char msg[sizeof(VMSHM_COMM_SELFTEST_HELLO_PAYLOAD)];
	int ret;

	if (!rx || rx->len != sizeof(msg))
		return -EPROTO;

	memcpy(msg, rx->payload, sizeof(msg));
	if (memcmp(msg, VMSHM_COMM_SELFTEST_HELLO_PAYLOAD, sizeof(msg)))
		return -EINVAL;

	pr_info("proxy_comm_vmshm: hello selftest received: %s\n", msg);

	ret = proxy_comm_vmshm_hello_send_ack(rx->proxy_channel, rx->seq);
	if (ret)
		pr_warn("proxy_comm_vmshm: hello selftest ACK failed (%d)\n",
			ret);

	return ret;
}

static int proxy_comm_vmshm_hello_selftest_register(void)
{
	int ret;

	ret = proxy_comm_vmshm_register_handler(VMSHM_COMM_SELFTEST_HELLO_REQ,
						proxy_comm_vmshm_hello_handler,
						NULL);
	if (ret)
		pr_warn("proxy_comm_vmshm: hello selftest handler register failed (%d)\n",
			ret);
	else
		pr_info("proxy_comm_vmshm: hello selftest handler registered\n");

	return ret;
}
#else
static int proxy_comm_vmshm_hello_selftest_register(void)
{
	return 0;
}
#endif

#ifdef CONFIG_PROXY_VMSHM_COMM_PERF_SELFTEST
static int
proxy_comm_vmshm_perf_send_noop_rsp(struct proxy_comm_vmshm_channel *channel,
				    u64 reply_to)
{
	struct vmshm_comm_tx tx = {
		.type = VMSHM_COMM_PERF_NOOP_RSP,
		.reply_to = reply_to,
	};
	int ret, i;

	for (i = 0; i < PROXY_COMM_VMSHM_INIT_RETRIES; i++) {
		ret = proxy_comm_vmshm_send_to_channel(channel, &tx);
		if (ret != -EAGAIN)
			return ret;

		usleep_range(PROXY_COMM_VMSHM_INIT_WAIT_US,
			     PROXY_COMM_VMSHM_INIT_WAIT_US * 2);
	}

	return -ETIMEDOUT;
}

static int
proxy_comm_vmshm_perf_send_report_rsp(struct proxy_comm_vmshm_channel *channel,
				      u64 reply_to)
{
	struct proxy_comm_vmshm_dev *d =
		proxy_comm_vmshm_dev_from_channel(channel);
	struct vmshm_comm_perf_report report = {
	};
	struct vmshm_comm_tx tx = {
		.type = VMSHM_COMM_PERF_REPORT_RSP,
		.reply_to = reply_to,
		.payload = &report,
		.len = sizeof(report),
	};
	int ret, i;

	if (!d)
		return -ENODEV;

	report.noop_count = d->perf_noop_count;

	for (i = 0; i < PROXY_COMM_VMSHM_INIT_RETRIES; i++) {
		ret = proxy_comm_vmshm_send_to_channel(channel, &tx);
		if (ret != -EAGAIN)
			return ret;

		usleep_range(PROXY_COMM_VMSHM_INIT_WAIT_US,
			     PROXY_COMM_VMSHM_INIT_WAIT_US * 2);
	}

	return -ETIMEDOUT;
}

static int proxy_comm_vmshm_perf_noop_handler(const struct vmshm_comm_rx *rx,
					      void *priv)
{
	int ret;

	if (!rx || rx->len)
		return -EPROTO;

	if (!rx->proxy_channel)
		return -ENODEV;

	proxy_comm_vmshm_dev_from_channel(rx->proxy_channel)->perf_noop_count++;
	ret = proxy_comm_vmshm_perf_send_noop_rsp(rx->proxy_channel, rx->seq);
	if (ret)
		pr_warn_ratelimited("proxy_comm_vmshm: perf noop response failed (%d)\n",
				    ret);

	return ret;
}

static int proxy_comm_vmshm_perf_report_handler(const struct vmshm_comm_rx *rx,
						void *priv)
{
	int ret;

	if (!rx || rx->len)
		return -EPROTO;

	ret = proxy_comm_vmshm_perf_send_report_rsp(rx->proxy_channel,
						   rx->seq);
	if (ret)
		pr_warn("proxy_comm_vmshm: perf report response failed (%d)\n",
			ret);

	return ret;
}

static int proxy_comm_vmshm_perf_selftest_register(void)
{
	int ret;

	ret = proxy_comm_vmshm_register_handler(VMSHM_COMM_PERF_NOOP_REQ,
						proxy_comm_vmshm_perf_noop_handler,
						NULL);
	if (ret) {
		pr_warn("proxy_comm_vmshm: perf noop handler register failed (%d)\n",
			ret);
		return ret;
	}

	ret = proxy_comm_vmshm_register_handler(VMSHM_COMM_PERF_REPORT_REQ,
						proxy_comm_vmshm_perf_report_handler,
						NULL);
	if (ret) {
		proxy_comm_vmshm_unregister_handler(VMSHM_COMM_PERF_NOOP_REQ,
						    proxy_comm_vmshm_perf_noop_handler,
						    NULL);
		pr_warn("proxy_comm_vmshm: perf report handler register failed (%d)\n",
			ret);
		return ret;
	}

	pr_info("proxy_comm_vmshm: perf selftest handlers registered\n");
	return 0;
}
#else
static int proxy_comm_vmshm_perf_selftest_register(void)
{
	return 0;
}
#endif

static int proxy_comm_vmshm_notify_init(struct platform_device *pdev,
                                          struct proxy_comm_vmshm_dev *d)
{
	int ret = d->realm->subscribe(d->realm->channel, proxy_comm_vmshm_receive, d);

	if (ret)
		return ret;
	d->irq_notify = true;
	dev_info(&pdev->dev, "RMM event IRQ enabled channel=%llu\n", d->realm->channel);
	return 0;
}

static void proxy_comm_vmshm_notify_stop(struct platform_device *pdev,
                                         struct proxy_comm_vmshm_dev *d)
{
	if (d->irq_notify)
		d->realm->unsubscribe(d->realm->channel);
}

static int proxy_comm_vmshm_open(struct inode *inode, struct file *filp)
{
	struct proxy_comm_vmshm_dev *d =
		container_of(inode->i_cdev, struct proxy_comm_vmshm_dev, cdev);

	if (!proxy_comm_vmshm_channel_get(&d->channel))
		return -ENODEV;

	filp->private_data = d;
	return 0;
}

static int proxy_comm_vmshm_release(struct inode *inode, struct file *filp)
{
	struct proxy_comm_vmshm_dev *d = filp->private_data;

	proxy_comm_vmshm_channel_put(&d->channel);
	return 0;
}

static const struct file_operations proxy_comm_vmshm_fops = {
	.owner		= THIS_MODULE,
	.open		= proxy_comm_vmshm_open,
	.release	= proxy_comm_vmshm_release,
};

static void proxy_comm_vmshm_chardev_unregister(struct proxy_comm_vmshm_dev *d)
{
	if (d->dev) {
		device_destroy(proxy_comm_vmshm_class, d->devt);
		d->dev = NULL;
	}

	if (d->cdev_added) {
		cdev_del(&d->cdev);
		d->cdev_added = false;
	}

	mutex_lock(&proxy_comm_vmshm_chardev_lock);
	if (d->minor >= 0) {
		ida_free(&proxy_comm_vmshm_minor_ida, d->minor);
		d->minor = -1;
	}
	if (proxy_comm_vmshm_chardev_users) {
		proxy_comm_vmshm_chardev_users--;
		if (!proxy_comm_vmshm_chardev_users) {
			class_destroy(proxy_comm_vmshm_class);
			proxy_comm_vmshm_class = NULL;
			unregister_chrdev_region(proxy_comm_vmshm_devt,
						 PROXY_COMM_VMSHM_MAX_CHANNELS);
			proxy_comm_vmshm_devt = 0;
		}
	}
	mutex_unlock(&proxy_comm_vmshm_chardev_lock);
}

static int proxy_comm_vmshm_chardev_register(struct platform_device *pdev,
					     struct proxy_comm_vmshm_dev *d)
{
	int ret;

	mutex_lock(&proxy_comm_vmshm_chardev_lock);
	if (!proxy_comm_vmshm_devt) {
		ret = alloc_chrdev_region(&proxy_comm_vmshm_devt, 0,
					  PROXY_COMM_VMSHM_MAX_CHANNELS,
					  PROXY_COMM_VMSHM_NAME);
		if (ret)
			goto out_unlock;
	}

	if (!proxy_comm_vmshm_class) {
		proxy_comm_vmshm_class = class_create(PROXY_COMM_VMSHM_NAME);
		if (IS_ERR(proxy_comm_vmshm_class)) {
			ret = PTR_ERR(proxy_comm_vmshm_class);
			proxy_comm_vmshm_class = NULL;
			goto err_unregister_chrdev;
		}
	}

	ret = ida_alloc_range(&proxy_comm_vmshm_minor_ida, 0,
			      PROXY_COMM_VMSHM_MAX_CHANNELS - 1, GFP_KERNEL);
	if (ret < 0)
		goto err_destroy_unused_class;

	d->minor = ret;
	d->devt = MKDEV(MAJOR(proxy_comm_vmshm_devt), d->minor);
	proxy_comm_vmshm_chardev_users++;
	mutex_unlock(&proxy_comm_vmshm_chardev_lock);

	cdev_init(&d->cdev, &proxy_comm_vmshm_fops);
	d->cdev.owner = THIS_MODULE;
	ret = cdev_add(&d->cdev, d->devt, 1);
	if (ret)
		goto err_unregister;
	d->cdev_added = true;

	if (d->minor)
		d->dev = device_create(proxy_comm_vmshm_class, &pdev->dev,
				       d->devt, NULL, PROXY_COMM_VMSHM_NAME "%d",
				       d->minor);
	else
		d->dev = device_create(proxy_comm_vmshm_class, &pdev->dev,
				       d->devt, NULL, PROXY_COMM_VMSHM_NAME);
	if (IS_ERR(d->dev)) {
		ret = PTR_ERR(d->dev);
		d->dev = NULL;
		goto err_cdev_del;
	}

	dev_info(&pdev->dev, "/dev/%s registered (major %d minor %d)\n",
		 dev_name(d->dev), MAJOR(d->devt), d->minor);
	return 0;

err_cdev_del:
	if (d->cdev_added) {
		cdev_del(&d->cdev);
		d->cdev_added = false;
	}
err_unregister:
	proxy_comm_vmshm_chardev_unregister(d);
	return ret;

err_destroy_unused_class:
	if (!proxy_comm_vmshm_chardev_users) {
		class_destroy(proxy_comm_vmshm_class);
		proxy_comm_vmshm_class = NULL;
	}
err_unregister_chrdev:
	if (!proxy_comm_vmshm_chardev_users) {
		unregister_chrdev_region(proxy_comm_vmshm_devt,
					 PROXY_COMM_VMSHM_MAX_CHANNELS);
		proxy_comm_vmshm_devt = 0;
	}
out_unlock:
	mutex_unlock(&proxy_comm_vmshm_chardev_lock);
	return ret;
}

static int proxy_comm_vmshm_probe(struct platform_device *pdev)
{
	struct proxy_comm_vmshm_dev *d;
	const struct realm_xshm_transport *realm = dev_get_platdata(&pdev->dev);
	struct resource *res;
	int ret;

	/* Realm mappings must come from the kernel's authenticated RMM grant,
	 * never from Host-supplied memslots/device-tree identifiers. */
	if (!is_realm_world() || !realm || !realm->client_vmid ||
				!realm->channel || !realm->notify || !realm->subscribe ||
				!realm->unsubscribe || !realm->arm)
		return -EPERM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev, "missing authenticated pool resource\n");
		return -ENODEV;
	}

	d = devm_kzalloc(&pdev->dev, sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;

	d->channel.dev = d;
	INIT_LIST_HEAD(&d->node);
	d->gpa = res->start;
	d->size = resource_size(res);
	d->client_vmid = realm->client_vmid;
	d->realm = realm;
	d->minor = -1;
	mutex_init(&d->lock);
	refcount_set(&d->users, 1);
	INIT_WORK(&d->rx_work, proxy_comm_vmshm_rx_work);
	atomic64_set(&d->trace_irq, 0);
	platform_set_drvdata(pdev, d);

	if (d->size < sizeof(struct proxy_comm_vmshm_header)) {
		dev_err(&pdev->dev, "shared window too small: 0x%pa\n",
			&d->size);
		return -EINVAL;
	}

	dev_info(&pdev->dev, "GPA 0x%pa size 0x%pa client_vmid=%u\n",
		 &d->gpa, &d->size, d->client_vmid);

	d->base = memremap(d->gpa, d->size, MEMREMAP_WB);
	if (!d->base) {
		dev_err(&pdev->dev, "memremap failed\n");
		return -ENOMEM;
	}

	dev_info(&pdev->dev, "mapped -> kernel VA %p\n", d->base);

	/* RMM provider is the protocol owner; never adopt peer-written layout. */
	memset(d->base, 0, d->size);
	ret = proxy_comm_vmshm_init_layout(d);
	if (ret) {
		dev_err(&pdev->dev, "shared protocol init failed (%d)\n", ret);
		goto err_unmap;
	}

	ret = proxy_comm_vmshm_notify_init(pdev, d);
	if (ret)
		goto err_unmap;

	dev_info(&pdev->dev,
		 "protocol %s generation=%u objects=%u heap=[0x%x,0x%x]\n",
		 d->layout_owner ? "initialized" : "attached",
		 d->hdr->generation, d->hdr->object_count,
		 d->hdr->heap_base_off, d->hdr->heap_size);

	if (d->layout_owner) {
		ret = proxy_comm_vmshm_selftest_run(d);
		if (ret)
			goto err_unmap;
		d->hdr->status[PROXY_COMM_VMSHM_ROLE_PROXY] =
			PROXY_COMM_VMSHM_STATUS_READY;
		smp_store_release(&d->hdr->magic, PROXY_COMM_VMSHM_MAGIC);
	}

	ret = proxy_comm_vmshm_chardev_register(pdev, d);
	if (ret) {
		dev_err(&pdev->dev, "chardev register failed (%d)\n", ret);
		goto err_unmap;
	}

	mutex_lock(&proxy_comm_vmshm_channels_lock);
	list_add_tail(&d->node, &proxy_comm_vmshm_channels);
	d->registered = true;
	mutex_unlock(&proxy_comm_vmshm_channels_lock);

	ret = proxy_comm_vmshm_hello_selftest_register();
	if (ret)
		goto err_unregister_channel;

	ret = proxy_comm_vmshm_perf_selftest_register();
	if (ret)
		goto err_unregister_channel;

	if (d->irq_notify)
		proxy_comm_vmshm_queue_rx(d);

	return 0;

err_unregister_channel:
	mutex_lock(&proxy_comm_vmshm_channels_lock);
	if (d->registered) {
		d->registered = false;
		list_del_init(&d->node);
	}
	mutex_unlock(&proxy_comm_vmshm_channels_lock);
	proxy_comm_vmshm_chardev_unregister(d);
err_unmap:
	proxy_comm_vmshm_notify_stop(pdev, d);
	cancel_work_sync(&d->rx_work);
	memunmap(d->base);
	d->base = NULL;
	return ret;
}

static void proxy_comm_vmshm_remove(struct platform_device *pdev)
{
	struct proxy_comm_vmshm_dev *d = platform_get_drvdata(pdev);

	WARN_ON(refcount_read(&d->users) != 1 || !d->quiescing);
	proxy_comm_vmshm_notify_stop(pdev, d);

	mutex_lock(&proxy_comm_vmshm_channels_lock);
	if (d->registered) {
		d->registered = false;
		list_del_init(&d->node);
	}
	mutex_unlock(&proxy_comm_vmshm_channels_lock);

	cancel_work_sync(&d->rx_work);
	if (d->realm)
		dev_info(&pdev->dev, "[REALM_RESPONSE_NOTIFY_STATS] sent=%llu suppressed=%llu\n",
			 d->realm_response_notifies, d->realm_response_suppressed);
	proxy_comm_vmshm_chardev_unregister(d);
	if (d->base) {
		memunmap(d->base);
		d->base = NULL;
	}
}

static struct platform_driver proxy_comm_vmshm_driver = {
	.probe = proxy_comm_vmshm_probe,
	.remove_new = proxy_comm_vmshm_remove,
	.driver = {
		.name = PROXY_COMM_VMSHM_NAME,
		.suppress_bind_attrs = true,
	},
};

builtin_platform_driver(proxy_comm_vmshm_driver);
