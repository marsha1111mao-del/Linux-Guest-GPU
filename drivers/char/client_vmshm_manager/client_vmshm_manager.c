// SPDX-License-Identifier: GPL-2.0
/*
 * client_vmshm_manager - client VM view of proxy-managed vmshm objects.
 *
 * The client VM never allocates or frees from this shared window. It only maps
 * the window, asks the proxy VM for object descriptors, and translates returned
 * offsets into local kernel mappings for other client-side drivers.
 */

#define CLIENT_VMSHM_MANAGER_BUILD

#include <linux/atomic.h>
#include <linux/client_vmshm.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/refcount.h>
#include <linux/slab.h>
#include <linux/vmshm_comm.h>

struct client_vmshm_object {
	struct vmshm_manager_desc desc;
	void *kva;
	phys_addr_t gpa;
	refcount_t refcnt;
};

struct client_vmshm_manager_dev {
	void *base;
	phys_addr_t gpa;
	resource_size_t size;
	u32 client_vmid;
	struct mutex lock;
};

static struct client_vmshm_manager_dev client_vmshm_mgr = {
	.lock = __MUTEX_INITIALIZER(client_vmshm_mgr.lock),
};

int client_vmshm_manager_bind(void *base, phys_addr_t gpa, size_t size,
			      u32 client_vmid)
{
	struct client_vmshm_manager_dev *d = &client_vmshm_mgr;
	int ret = 0;

	if (!base || !size || !client_vmid || !PAGE_ALIGNED(gpa) ||
	    !PAGE_ALIGNED(size))
		return -EINVAL;
	mutex_lock(&d->lock);
	if (d->base)
		ret = -EBUSY;
	else {
		d->gpa = gpa;
		d->size = size;
		d->client_vmid = client_vmid;
		d->base = base;
	}
	mutex_unlock(&d->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(client_vmshm_manager_bind);

/* The communication queue has been quiesced first. Each returned object
 * owns a queue reference, including objects obtained outside a DRM file.
 */
void client_vmshm_manager_unbind(void *base)
{
	struct client_vmshm_manager_dev *d = &client_vmshm_mgr;

	mutex_lock(&d->lock);
	if (WARN_ON(d->base != base)) {
		mutex_unlock(&d->lock);
		return;
	}
	d->base = NULL;
	d->size = 0;
	mutex_unlock(&d->lock);
}
EXPORT_SYMBOL_GPL(client_vmshm_manager_unbind);

static bool client_vmshm_range_valid(struct client_vmshm_manager_dev *d,
				     u64 offset, u64 size)
{
	if (!d->base)
		return false;
	if (offset > d->size)
		return false;

	return size <= d->size - offset;
}

static int client_vmshm_validate_desc(struct client_vmshm_manager_dev *d,
				      const struct vmshm_manager_desc *desc)
{
	if (!(desc->flags & VMSHM_MANAGER_DESC_F_CONTIG))
		return -EOPNOTSUPP;
	if (desc->nr_segments != 1)
		return -EOPNOTSUPP;
	if (desc->size > desc->alloc_size)
		return -EINVAL;
	if (!client_vmshm_range_valid(d, desc->offset, desc->alloc_size))
		return -ERANGE;

	return 0;
}

int client_vmshm_manager_get(const struct client_vmshm_lookup_params *params,
			     struct client_vmshm_object **out)
{
	struct client_vmshm_manager_dev *d = &client_vmshm_mgr;
	struct vmshm_manager_get_object_req req;
	struct vmshm_manager_get_object_rsp rsp;
	struct vmshm_comm_rx rx;
	struct client_vmshm_object *obj;
	int ret;

	if (!params || !out)
		return -EINVAL;
	if (params->lookup != VMSHM_MANAGER_LOOKUP_HANDLE &&
	    params->lookup != VMSHM_MANAGER_LOOKUP_GRANT)
		return -EINVAL;

	*out = NULL;
	if (!client_comm_vmshm_get())
		return -ENODEV;
	mutex_lock(&d->lock);
	if (!d->base) {
		ret = -ENODEV;
		goto out_unlock;
	}
	memset(&req, 0, sizeof(req));
	req.handle = params->handle;
	req.grant_id = params->grant_id;
	req.lookup = params->lookup;
	req.requester_vmid = params->requester_vmid ?: d->client_vmid;
	req.required_perms = params->required_perms;
	req.flags = params->flags;

	ret = client_comm_vmshm_call(VMSHM_MANAGER_MSG_GET_OBJECT_REQ, 0,
				     &req, sizeof(req),
				     VMSHM_MANAGER_MSG_GET_OBJECT_RSP,
				     &rsp, sizeof(rsp), &rx);
	if (ret)
		goto out_unlock;
	if (rx.len != sizeof(rsp)) {
		ret = -EPROTO;
		goto out_unlock;
	}
	if (rsp.ret) {
		ret = rsp.ret;
		goto out_unlock;
	}

	ret = client_vmshm_validate_desc(d, &rsp.desc);
	if (ret)
		goto out_unlock;

	obj = kzalloc(sizeof(*obj), GFP_KERNEL);
	if (!obj) {
		ret = -ENOMEM;
		goto out_unlock;
	}

	obj->desc = rsp.desc;
	obj->desc.gpa = d->gpa + rsp.desc.offset;
	obj->kva = (void *)((u8 *)d->base + rsp.desc.offset);
	obj->gpa = d->gpa + rsp.desc.offset;
	refcount_set(&obj->refcnt, 1);
	*out = obj;
	mutex_unlock(&d->lock);
	return 0;
out_unlock:
	mutex_unlock(&d->lock);
	client_comm_vmshm_put();
	return ret;
}
EXPORT_SYMBOL_GPL(client_vmshm_manager_get);

void client_vmshm_manager_put(struct client_vmshm_object *obj)
{
	if (obj && refcount_dec_and_test(&obj->refcnt)) {
		kfree(obj);
		client_comm_vmshm_put();
	}
}
EXPORT_SYMBOL_GPL(client_vmshm_manager_put);

const struct vmshm_manager_desc *
client_vmshm_object_desc(const struct client_vmshm_object *obj)
{
	return obj ? &obj->desc : NULL;
}
EXPORT_SYMBOL_GPL(client_vmshm_object_desc);

phys_addr_t client_vmshm_object_gpa(const struct client_vmshm_object *obj)
{
	return obj ? obj->gpa : 0;
}
EXPORT_SYMBOL_GPL(client_vmshm_object_gpa);

const void *client_vmshm_object_kva(const struct client_vmshm_object *obj)
{
	return obj ? obj->kva : NULL;
}
EXPORT_SYMBOL_GPL(client_vmshm_object_kva);
