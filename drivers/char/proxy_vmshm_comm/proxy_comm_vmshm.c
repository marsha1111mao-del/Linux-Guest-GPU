// SPDX-License-Identifier: GPL-2.0
/*
 * proxy_comm_vmshm - shared-memory communication proxy skeleton.
 *
 * This driver owns a separate vmshm window described by:
 *
 *   compatible = "proxy_comm_vmshm";
 *
 * This driver places a small virtqueue-like control transport in this memory
 * so two VMs can exchange control messages without involving GPU BO backing.
 * The first transport version uses a simple shared-memory arena allocator to
 * create queue/ring/descriptor/message-pool objects during setup. Runtime
 * message flow then reuses fixed descriptor indexes inside each queue.
 */

#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/idr.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ioctl.h>
#include <linux/kthread.h>
#include <linux/list.h>
#include <linux/log2.h>
#include <linux/mm.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/rwsem.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/vmshm_comm.h>
#include <linux/workqueue.h>

#define PROXY_COMM_VMSHM_NAME		"proxy_comm_vmshm"
#define PROXY_COMM_VMSHM_IOC_MAGIC	'C'

#define PROXY_COMM_VMSHM_IOC_GET_SIZE \
	_IOR(PROXY_COMM_VMSHM_IOC_MAGIC, 1, __u64)
#define PROXY_COMM_VMSHM_IOC_GET_BASE \
	_IOR(PROXY_COMM_VMSHM_IOC_MAGIC, 2, __u64)
#define PROXY_COMM_VMSHM_IOC_GET_INFO \
	_IOR(PROXY_COMM_VMSHM_IOC_MAGIC, 3, struct proxy_comm_vmshm_info)

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
#define PROXY_COMM_VMSHM_SELFTEST_REQ	0x54534551U /* "TSEQ" */
#define PROXY_COMM_VMSHM_SELFTEST_RSP	0x54534552U /* "TSER" */
#define PROXY_COMM_VMSHM_MAX_HANDLERS	32
#define PROXY_COMM_VMSHM_MAX_CHANNELS	32
#define PROXY_COMM_VMSHM_DISPATCH_EMPTY_WAIT_MS	1
#define PROXY_COMM_VMSHM_DISPATCH_WAIT_MS	20

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

enum proxy_comm_vmshm_obj_type {
	PROXY_COMM_VMSHM_OBJ_NONE = 0,
	PROXY_COMM_VMSHM_OBJ_QUEUE,
	PROXY_COMM_VMSHM_OBJ_DESC_TABLE,
	PROXY_COMM_VMSHM_OBJ_AVAIL_RING,
	PROXY_COMM_VMSHM_OBJ_USED_RING,
	PROXY_COMM_VMSHM_OBJ_MSG_POOL,
	PROXY_COMM_VMSHM_OBJ_IDX_TABLE,
	PROXY_COMM_VMSHM_OBJ_BITMAP,
	PROXY_COMM_VMSHM_OBJ_PRIVATE_SCRATCH,
};

/*
 * Shared-memory protocol. All fields are little-endian neutral for now because
 * both peers are expected to be same-architecture VMs in the current prototype.
 * If this becomes a stable cross-architecture ABI, convert these to __le32/64.
 *
 * All addresses stored here are offsets relative to the start of the
 * proxy_comm_vmshm shared window. Do not store kernel virtual addresses in the
 * shared protocol area.
 */
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

struct proxy_comm_vmshm_info {
	__u64 gpa;
	__u64 size;
	__u32 header_size;
	__u32 queue_count;
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
	phys_addr_t gpa;
	resource_size_t size;
	u32 client_vmid;
	void __iomem *doorbell;
	resource_size_t doorbell_size;
	int irq;
	bool irq_notify;
	struct work_struct rx_work;
	u64 perf_noop_count;
	dev_t devt;
	struct cdev cdev;
	struct device *dev;
	struct mutex lock;
	atomic_t open_cnt;
	struct task_struct *dispatch_task;
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
	struct proxy_comm_vmshm_header *h = d->hdr;
	u32 table_size;

	if (!h)
		return NULL;

	table_size = h->object_capacity * sizeof(struct proxy_comm_vmshm_object);
	return proxy_comm_vmshm_ptr(d, h->object_table_off, table_size);
}

static int proxy_comm_vmshm_alloc_object(struct proxy_comm_vmshm_dev *d,
					 u32 type, u32 size, u32 align,
					 u32 flags, u32 *obj_id, u32 *offset)
{
	struct proxy_comm_vmshm_header *h = d->hdr;
	struct proxy_comm_vmshm_object *objects, *obj;
	u32 aligned, end;

	if (!size || !align || !is_power_of_2(align))
		return -EINVAL;
	if (!h || !obj_id || !offset)
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
	struct proxy_comm_vmshm_header *h = d->hdr;
	struct proxy_comm_vmshm_queue *q;

	if (!h || queue_id >= h->queue_count)
		return NULL;

	q = &h->queues[queue_id];
	return proxy_comm_vmshm_ptr(d, q->queue_off, sizeof(*q));
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
	return 0;
}

static int proxy_comm_vmshm_setup_queue(struct proxy_comm_vmshm_dev *d,
					u32 queue_id, u32 direction)
{
	struct proxy_comm_vmshm_header *h = d->hdr;
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

	qobj = proxy_comm_vmshm_ptr(d, queue_off, sizeof(*qobj));
	if (!qobj)
		return -EINVAL;

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

	return proxy_comm_vmshm_queue_reset(d, queue_id);
}

static int proxy_comm_vmshm_build_layout(struct proxy_comm_vmshm_dev *d)
{
	struct proxy_comm_vmshm_header *h = d->hdr;
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
	h->status[PROXY_COMM_VMSHM_ROLE_PROXY] = PROXY_COMM_VMSHM_STATUS_READY;
	WRITE_ONCE(h->magic, PROXY_COMM_VMSHM_MAGIC);
	return 0;
}

static int proxy_comm_vmshm_validate_layout(struct proxy_comm_vmshm_dev *d)
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
	if (!proxy_comm_vmshm_range_valid(d, h->object_table_off, table_size))
		return -EINVAL;
	if (!proxy_comm_vmshm_range_valid(d, h->heap_base_off, h->heap_size))
		return -EINVAL;

	for (i = 0; i < h->queue_count; i++) {
		struct proxy_comm_vmshm_queue_object *q;

		q = proxy_comm_vmshm_queue_obj(d, i);
		if (!q || q->magic != PROXY_COMM_VMSHM_QUEUE_MAGIC ||
		    q->queue_size != PROXY_COMM_VMSHM_QUEUE_SIZE ||
		    q->msg_size != PROXY_COMM_VMSHM_MSG_SIZE ||
		    !is_power_of_2(q->queue_size))
			return -EINVAL;
	}

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
		magic = READ_ONCE(h->magic);
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

		return proxy_comm_vmshm_validate_layout(d);
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

	avail_idx = READ_ONCE(avail->idx);
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
	smp_store_release(&avail->idx, avail_idx + 1);
	return 0;
}

static int proxy_comm_vmshm_queue_recv(struct proxy_comm_vmshm_dev *d,
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

	q = proxy_comm_vmshm_queue_obj(d, queue_id);
	if (!q)
		return -EINVAL;

	descs = proxy_comm_vmshm_descs(d, q);
	avail = proxy_comm_vmshm_avail(d, q);
	used = proxy_comm_vmshm_used(d, q);
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

	msg = proxy_comm_vmshm_ptr(d, desc->msg_off, desc->msg_len);
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

static int proxy_comm_vmshm_selftest_objects(struct proxy_comm_vmshm_dev *d)
{
	struct proxy_comm_vmshm_header *h = d->hdr;
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

	return d->base && h &&
	       READ_ONCE(h->magic) == PROXY_COMM_VMSHM_MAGIC &&
	       READ_ONCE(h->status[PROXY_COMM_VMSHM_ROLE_PROXY]) ==
		       PROXY_COMM_VMSHM_STATUS_READY;
}

static void proxy_comm_vmshm_kick_client(struct proxy_comm_vmshm_dev *d)
{
	if (d->irq_notify && d->doorbell)
		writel(1, d->doorbell);
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

bool proxy_comm_vmshm_channel_ready(struct proxy_comm_vmshm_channel *channel)
{
	struct proxy_comm_vmshm_dev *d =
		proxy_comm_vmshm_dev_from_channel(channel);

	return d && proxy_comm_vmshm_ready_locked(d);
}
EXPORT_SYMBOL_GPL(proxy_comm_vmshm_channel_ready);

static u32 proxy_comm_vmshm_max_payload_for_dev(struct proxy_comm_vmshm_dev *d)
{
	struct proxy_comm_vmshm_queue_object *q;

	if (!d || !proxy_comm_vmshm_ready_locked(d))
		return 0;

	q = proxy_comm_vmshm_queue_obj(d, PROXY_COMM_VMSHM_Q_CLIENT_TO_PROXY);
	if (!q || q->msg_size <= sizeof(struct proxy_comm_vmshm_msg))
		return 0;

	return q->msg_size - sizeof(struct proxy_comm_vmshm_msg);
}

u32 proxy_comm_vmshm_channel_max_payload(struct proxy_comm_vmshm_channel *channel)
{
	return proxy_comm_vmshm_max_payload_for_dev(
		proxy_comm_vmshm_dev_from_channel(channel));
}
EXPORT_SYMBOL_GPL(proxy_comm_vmshm_channel_max_payload);

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
		proxy_comm_vmshm_kick_client(d);

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

static void proxy_comm_vmshm_drain_rx(struct proxy_comm_vmshm_dev *d,
				      bool sleep_when_empty)
{
	for (;;) {
		int ret = proxy_comm_vmshm_dispatch_one(d);

		if (!ret)
			continue;
		if (ret == -ENOENT) {
			if (sleep_when_empty)
				msleep(PROXY_COMM_VMSHM_DISPATCH_EMPTY_WAIT_MS);
			return;
		}
		if (ret == -ENODEV) {
			if (sleep_when_empty)
				msleep(PROXY_COMM_VMSHM_DISPATCH_WAIT_MS);
			return;
		}

		pr_warn_ratelimited("proxy_comm_vmshm: dispatch recv failed (%d)\n",
				    ret);
		if (sleep_when_empty)
			msleep(PROXY_COMM_VMSHM_DISPATCH_EMPTY_WAIT_MS);
		return;
	}
}

static void proxy_comm_vmshm_rx_work(struct work_struct *work)
{
	struct proxy_comm_vmshm_dev *d =
		container_of(work, struct proxy_comm_vmshm_dev, rx_work);

	proxy_comm_vmshm_drain_rx(d, false);
}

static irqreturn_t proxy_comm_vmshm_irq(int irq, void *data)
{
	struct proxy_comm_vmshm_dev *d = data;

	proxy_comm_vmshm_queue_rx(d);
	return IRQ_HANDLED;
}

static int proxy_comm_vmshm_dispatch_thread(void *data)
{
	struct proxy_comm_vmshm_dev *d = data;

	while (!kthread_should_stop()) {
		proxy_comm_vmshm_drain_rx(d, true);
		cond_resched();
	}

	return 0;
}

static int proxy_comm_vmshm_start_dispatcher_locked(struct proxy_comm_vmshm_dev *d)
{
	if (!d || !d->hdr)
		return 0;
	if (d->irq_notify)
		return 0;
	if (d->dispatch_task)
		return 0;

	d->dispatch_task = kthread_run(proxy_comm_vmshm_dispatch_thread, d,
				       "proxy-vmshm-dispatch/%d", d->minor);
	if (IS_ERR(d->dispatch_task)) {
		int ret = PTR_ERR(d->dispatch_task);

		d->dispatch_task = NULL;
		return ret;
	}

	return 0;
}

static int proxy_comm_vmshm_start_dispatchers_locked(void)
{
	struct proxy_comm_vmshm_dev *d;
	int ret;

	lockdep_assert_held(&proxy_comm_vmshm_channels_lock);

	list_for_each_entry(d, &proxy_comm_vmshm_channels, node) {
		ret = proxy_comm_vmshm_start_dispatcher_locked(d);
		if (ret)
			return ret;
	}

	return 0;
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
			ret = proxy_comm_vmshm_start_dispatchers_locked();
			if (ret) {
				proxy_comm_vmshm_handlers[i].handler = NULL;
				proxy_comm_vmshm_handlers[i].priv = NULL;
				proxy_comm_vmshm_handlers[i].type = 0;
				proxy_comm_vmshm_handler_count--;
			} else {
				proxy_comm_vmshm_schedule_all_locked();
			}
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

static void proxy_comm_vmshm_notify_init(struct platform_device *pdev,
					 struct proxy_comm_vmshm_dev *d)
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

	ret = devm_request_irq(&pdev->dev, irq, proxy_comm_vmshm_irq, 0,
			       PROXY_COMM_VMSHM_NAME, d);
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

static int proxy_comm_vmshm_open(struct inode *inode, struct file *filp)
{
	struct proxy_comm_vmshm_dev *d =
		container_of(inode->i_cdev, struct proxy_comm_vmshm_dev, cdev);

	if (atomic_inc_return(&d->open_cnt) < 0) {
		atomic_dec(&d->open_cnt);
		return -EMFILE;
	}

	filp->private_data = d;
	return 0;
}

static int proxy_comm_vmshm_release(struct inode *inode, struct file *filp)
{
	struct proxy_comm_vmshm_dev *d = filp->private_data;

	atomic_dec(&d->open_cnt);
	return 0;
}

static loff_t proxy_comm_vmshm_llseek(struct file *filp, loff_t offset,
				      int whence)
{
	struct proxy_comm_vmshm_dev *d = filp->private_data;
	loff_t newpos;

	switch (whence) {
	case SEEK_SET:
		newpos = offset;
		break;
	case SEEK_CUR:
		newpos = filp->f_pos + offset;
		break;
	case SEEK_END:
		newpos = d->size + offset;
		break;
	default:
		return -EINVAL;
	}

	if (newpos < 0 || newpos > d->size)
		return -EINVAL;

	filp->f_pos = newpos;
	return newpos;
}

static ssize_t proxy_comm_vmshm_read(struct file *filp, char __user *buf,
				     size_t count, loff_t *ppos)
{
	struct proxy_comm_vmshm_dev *d = filp->private_data;
	loff_t pos = *ppos;
	size_t avail;

	if (pos >= d->size)
		return 0;

	avail = d->size - pos;
	if (count > avail)
		count = avail;

	mutex_lock(&d->lock);
	if (copy_to_user(buf, (u8 *)d->base + pos, count))
		count = -EFAULT;
	else
		*ppos = pos + count;
	mutex_unlock(&d->lock);

	return count;
}

static ssize_t proxy_comm_vmshm_write(struct file *filp,
				      const char __user *buf,
				      size_t count, loff_t *ppos)
{
	struct proxy_comm_vmshm_dev *d = filp->private_data;
	loff_t pos = *ppos;
	size_t avail;

	if (pos >= d->size)
		return -ENOSPC;

	avail = d->size - pos;
	if (count > avail)
		count = avail;

	mutex_lock(&d->lock);
	if (copy_from_user((u8 *)d->base + pos, buf, count))
		count = -EFAULT;
	else
		*ppos = pos + count;
	mutex_unlock(&d->lock);

	return count;
}

static long proxy_comm_vmshm_ioctl(struct file *filp, unsigned int cmd,
				   unsigned long arg)
{
	struct proxy_comm_vmshm_dev *d = filp->private_data;

	switch (cmd) {
	case PROXY_COMM_VMSHM_IOC_GET_SIZE: {
		__u64 sz = d->size;

		if (copy_to_user((__u64 __user *)arg, &sz, sizeof(sz)))
			return -EFAULT;
		return 0;
	}
	case PROXY_COMM_VMSHM_IOC_GET_BASE: {
		__u64 base = d->gpa;

		if (copy_to_user((__u64 __user *)arg, &base, sizeof(base)))
			return -EFAULT;
		return 0;
	}
	case PROXY_COMM_VMSHM_IOC_GET_INFO: {
		struct proxy_comm_vmshm_info info = {
			.gpa = d->gpa,
			.size = d->size,
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

static const struct file_operations proxy_comm_vmshm_fops = {
	.owner		= THIS_MODULE,
	.open		= proxy_comm_vmshm_open,
	.release	= proxy_comm_vmshm_release,
	.read		= proxy_comm_vmshm_read,
	.write		= proxy_comm_vmshm_write,
	.llseek		= proxy_comm_vmshm_llseek,
	.unlocked_ioctl = proxy_comm_vmshm_ioctl,
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
	struct resource *res;
	int ret;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev, "missing 'reg' resource in DT node\n");
		return -ENODEV;
	}

	d = devm_kzalloc(&pdev->dev, sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;

	d->channel.dev = d;
	INIT_LIST_HEAD(&d->node);
	d->gpa = res->start;
	d->size = resource_size(res);
	if (pdev->dev.of_node)
		of_property_read_u32(pdev->dev.of_node, "vmshm-client-vmid",
				     &d->client_vmid);
	if (!d->client_vmid)
		d->client_vmid = 1;
	d->minor = -1;
	mutex_init(&d->lock);
	atomic_set(&d->open_cnt, 0);
	d->irq = -1;
	INIT_WORK(&d->rx_work, proxy_comm_vmshm_rx_work);
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

	ret = proxy_comm_vmshm_init_layout(d);
	if (ret) {
		dev_err(&pdev->dev, "shared protocol init failed (%d)\n", ret);
		goto err_unmap;
	}

	proxy_comm_vmshm_notify_init(pdev, d);

	dev_info(&pdev->dev,
		 "protocol %s generation=%u objects=%u heap=[0x%x,0x%x]\n",
		 d->layout_owner ? "initialized" : "attached",
		 d->hdr->generation, d->hdr->object_count,
		 d->hdr->heap_base_off, d->hdr->heap_size);

	if (d->layout_owner) {
		ret = proxy_comm_vmshm_selftest_run(d);
		if (ret)
			goto err_unmap;
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

	down_read(&proxy_comm_vmshm_handler_rwsem);
	if (proxy_comm_vmshm_handler_count) {
		ret = proxy_comm_vmshm_start_dispatcher_locked(d);
		if (ret) {
			up_read(&proxy_comm_vmshm_handler_rwsem);
			goto err_unregister_channel;
		}
	}
	up_read(&proxy_comm_vmshm_handler_rwsem);
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
	if (d->dispatch_task) {
		kthread_stop(d->dispatch_task);
		d->dispatch_task = NULL;
	}
	cancel_work_sync(&d->rx_work);
	memunmap(d->base);
	d->base = NULL;
	return ret;
}

static void proxy_comm_vmshm_remove(struct platform_device *pdev)
{
	struct proxy_comm_vmshm_dev *d = platform_get_drvdata(pdev);

	mutex_lock(&proxy_comm_vmshm_channels_lock);
	if (d->registered) {
		d->registered = false;
		list_del_init(&d->node);
	}
	mutex_unlock(&proxy_comm_vmshm_channels_lock);

	if (d->dispatch_task) {
		kthread_stop(d->dispatch_task);
		d->dispatch_task = NULL;
	}
	cancel_work_sync(&d->rx_work);
	proxy_comm_vmshm_chardev_unregister(d);
	if (d->base) {
		memunmap(d->base);
		d->base = NULL;
	}
}

static const struct of_device_id proxy_comm_vmshm_of_match[] = {
	{ .compatible = "proxy_comm_vmshm" },
	{ }
};

static struct platform_driver proxy_comm_vmshm_driver = {
	.probe = proxy_comm_vmshm_probe,
	.remove_new = proxy_comm_vmshm_remove,
	.driver = {
		.name = PROXY_COMM_VMSHM_NAME,
		.of_match_table = proxy_comm_vmshm_of_match,
	},
};

builtin_platform_driver(proxy_comm_vmshm_driver);
