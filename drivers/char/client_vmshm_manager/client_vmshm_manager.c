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
#include <linux/proxy_vmshm.h>
#include <linux/refcount.h>
#include <linux/slab.h>
#include <linux/vmshm_comm.h>

struct client_vmshm_object {
	struct vmshm_manager_desc desc;
	struct vmshm_manager_segment *segments;
	void *kva;
	phys_addr_t pool_gpa;
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
	if (desc->flags & ~VMSHM_MANAGER_DESC_F_CONTIG)
		return -EOPNOTSUPP;
	if (!desc->nr_segments || desc->nr_segments > VMSHM_MANAGER_MAX_SEGMENTS ||
	    !desc->size || desc->size > desc->alloc_size)
		return -EINVAL;
	if (desc->flags & VMSHM_MANAGER_DESC_F_CONTIG) {
		if (desc->nr_segments != 1)
			return -EINVAL;
		if (!client_vmshm_range_valid(d, desc->offset, desc->alloc_size))
			return -ERANGE;
	} else if (desc->type != PROXY_VMSHM_OBJ_GPU_BO ||
		   !PAGE_ALIGNED(desc->alloc_size) || desc->alloc_size > d->size) {
		return -EINVAL;
	}

	return 0;
}

static int client_vmshm_request_segment(const struct vmshm_manager_get_object_req *req,
				      struct vmshm_manager_get_object_rsp *rsp)
{
	struct vmshm_comm_rx rx;
	int ret;

	ret = client_comm_vmshm_call(VMSHM_MANAGER_MSG_GET_OBJECT_REQ, 0,
				     req, sizeof(*req),
				     VMSHM_MANAGER_MSG_GET_OBJECT_RSP,
				     rsp, sizeof(*rsp), &rx);
	if (ret)
		return ret;
	if (rx.len != sizeof(*rsp))
		return -EPROTO;
	return rsp->ret;
}

static int client_vmshm_validate_segments(struct client_vmshm_manager_dev *d,
					const struct client_vmshm_object *obj)
{
	u64 total = 0;
	unsigned int index, previous;

	if (obj->segments[0].offset != obj->desc.offset)
		return -EINVAL;
	for (index = 0; index < obj->desc.nr_segments; index++) {
		const struct vmshm_manager_segment *segment = &obj->segments[index];

		if (!segment->size || !client_vmshm_range_valid(d, segment->offset,
							      segment->size) ||
		    segment->size > obj->desc.alloc_size - total)
			return -ERANGE;
		if (!(obj->desc.flags & VMSHM_MANAGER_DESC_F_CONTIG) &&
		    (!PAGE_ALIGNED(segment->offset) || !PAGE_ALIGNED(segment->size)))
			return -EINVAL;
		for (previous = 0; previous < index; previous++) {
			const struct vmshm_manager_segment *other = &obj->segments[previous];

			if (segment->offset < other->offset + other->size &&
			    other->offset < segment->offset + segment->size)
				return -EINVAL;
		}
		total += segment->size;
	}
	return total == obj->desc.alloc_size ? 0 : -EINVAL;
}

int client_vmshm_manager_get(const struct client_vmshm_lookup_params *params,
			     struct client_vmshm_object **out)
{
	struct client_vmshm_manager_dev *d = &client_vmshm_mgr;
	struct vmshm_manager_get_object_req req;
	struct vmshm_manager_get_object_rsp rsp;
	struct client_vmshm_object *obj;
	unsigned int index;
	int ret;

	if (!params || !out || params->flags)
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
	ret = client_vmshm_request_segment(&req, &rsp);
	if (ret)
		goto out_unlock;

	ret = client_vmshm_validate_desc(d, &rsp.desc);
	if (ret)
		goto out_unlock;

	obj = kzalloc(sizeof(*obj), GFP_KERNEL);
	if (!obj) {
		ret = -ENOMEM;
		goto out_unlock;
	}

	obj->desc = rsp.desc;
	obj->segments = kcalloc(obj->desc.nr_segments, sizeof(*obj->segments), GFP_KERNEL);
	if (!obj->segments) {
		ret = -ENOMEM;
		goto out_free_object;
	}
	obj->segments[0] = rsp.segment;
	for (index = 1; index < obj->desc.nr_segments; index++) {
		req.segment_index = index;
		ret = client_vmshm_request_segment(&req, &rsp);
		if (ret)
			goto out_free_object;
		if (memcmp(&obj->desc, &rsp.desc, sizeof(rsp.desc))) {
			ret = -ESTALE;
			goto out_free_object;
		}
		obj->segments[index] = rsp.segment;
	}
	ret = client_vmshm_validate_segments(d, obj);
	if (ret)
		goto out_free_object;
	obj->desc.gpa = d->gpa + obj->desc.offset;
	if (obj->desc.flags & VMSHM_MANAGER_DESC_F_CONTIG)
		obj->kva = (u8 *)d->base + obj->desc.offset;
	obj->pool_gpa = d->gpa;
	refcount_set(&obj->refcnt, 1);
	*out = obj;
	mutex_unlock(&d->lock);
	return 0;
out_free_object:
	kfree(obj->segments);
	kfree(obj);
out_unlock:
	mutex_unlock(&d->lock);
	client_comm_vmshm_put();
	return ret;
}
EXPORT_SYMBOL_GPL(client_vmshm_manager_get);

void client_vmshm_manager_put(struct client_vmshm_object *obj)
{
	if (obj && refcount_dec_and_test(&obj->refcnt)) {
		kfree(obj->segments);
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

int client_vmshm_object_mmap(const struct client_vmshm_object *obj,
			    struct vm_area_struct *vma, u64 offset)
{
	u64 size = vma->vm_end - vma->vm_start;
	u64 logical = 0, end;
	unsigned int index;
	int ret;

	if (!obj || obj->desc.type != PROXY_VMSHM_OBJ_GPU_BO ||
	    !(obj->desc.perms & PROXY_VMSHM_PERM_MMAP) || !size ||
	    !PAGE_ALIGNED(offset) || offset > obj->desc.alloc_size ||
	    size > obj->desc.alloc_size - offset)
		return -EINVAL;
	end = offset + size;
	for (index = 0; index < obj->desc.nr_segments; index++) {
		const struct vmshm_manager_segment *segment = &obj->segments[index];
		u64 start = max(offset, logical);
		u64 stop = min(end, logical + segment->size);
		phys_addr_t gpa = obj->pool_gpa + segment->offset + start - logical;

		if (stop > start) {
			ret = remap_pfn_range(vma, vma->vm_start + start - offset,
					      gpa >> PAGE_SHIFT, stop - start,
					      vma->vm_page_prot);
			if (ret)
				return ret;
		}
		logical += segment->size;
		if (logical >= end)
			break;
	}
	return 0;
}
EXPORT_SYMBOL_GPL(client_vmshm_object_mmap);

const void *client_vmshm_object_kva(const struct client_vmshm_object *obj)
{
	return obj ? obj->kva : NULL;
}
EXPORT_SYMBOL_GPL(client_vmshm_object_kva);
