/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PROXY_VMSHM_H
#define _LINUX_PROXY_VMSHM_H

#include <linux/bits.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/gfp.h>
#include <linux/kconfig.h>
#include <linux/types.h>

struct proxy_vmshm_object;

#define PROXY_VMSHM_PERM_CPU_READ	BIT(0)
#define PROXY_VMSHM_PERM_CPU_WRITE	BIT(1)
#define PROXY_VMSHM_PERM_MMAP		BIT(2)
#define PROXY_VMSHM_PERM_GPU_READ	BIT(3)
#define PROXY_VMSHM_PERM_GPU_WRITE	BIT(4)
#define PROXY_VMSHM_PERM_SUBMIT	BIT(5)
#define PROXY_VMSHM_PERM_SIGNAL	BIT(6)

#define PROXY_VMSHM_PERM_READ		PROXY_VMSHM_PERM_CPU_READ
#define PROXY_VMSHM_PERM_WRITE		PROXY_VMSHM_PERM_CPU_WRITE

#define PROXY_VMSHM_F_CONTIG		BIT(0)
#define PROXY_VMSHM_F_ALLOW_SG		BIT(1)

enum proxy_vmshm_object_type {
	PROXY_VMSHM_OBJ_GENERIC = 0,
	PROXY_VMSHM_OBJ_GPU_BO,
	PROXY_VMSHM_OBJ_COMMAND_BO,
	PROXY_VMSHM_OBJ_SUBMIT_RING,
	PROXY_VMSHM_OBJ_EVENT_RING,
	PROXY_VMSHM_OBJ_FENCE_PAGE,
	PROXY_VMSHM_OBJ_TRANSFER_BUFFER,
};

struct proxy_vmshm_alloc_params {
	u32 owner_vmid;
	u32 type;
	u32 flags;
	u32 perms;
	size_t size;
	size_t align;
};

struct proxy_vmshm_desc {
	u64 handle;
	u32 id;
	u32 generation;
	u32 type;
	u64 offset;
	size_t size;
	size_t alloc_size;
	u32 perms;
};

struct proxy_vmshm_span {
	u64 logical_offset;
	u64 offset;
	void *kva;
	phys_addr_t gpa;
	size_t size;
};

#if IS_ENABLED(CONFIG_PROXY_VMSHM_MANAGER) || defined(PROXY_VMSHM_MANAGER_BUILD)
struct proxy_vmshm_object *
proxy_vmshm_alloc_ext(const struct proxy_vmshm_alloc_params *params,
		      gfp_t gfp);
struct proxy_vmshm_object *proxy_vmshm_alloc(size_t size, gfp_t gfp);
void proxy_vmshm_free(struct proxy_vmshm_object *obj);
int proxy_vmshm_free_handle(u64 handle, u32 requester_vmid);

bool proxy_vmshm_get(struct proxy_vmshm_object *obj);
void proxy_vmshm_put(struct proxy_vmshm_object *obj);
int proxy_vmshm_pin(struct proxy_vmshm_object *obj);
void proxy_vmshm_unpin(struct proxy_vmshm_object *obj);

/*
 * Successful lookups return a pinned object. The caller must release it with
 * proxy_vmshm_unpin() when the backing is no longer in flight.
 */
int proxy_vmshm_lookup_pin(u64 handle, u32 requester_vmid,
			   u32 required_perms,
			   struct proxy_vmshm_object **out);
int proxy_vmshm_lookup(u64 handle, u32 requester_vmid, u32 required_perms,
		       struct proxy_vmshm_object **out);

u64 proxy_vmshm_obj_handle(struct proxy_vmshm_object *obj);
u32 proxy_vmshm_obj_id(struct proxy_vmshm_object *obj);
u32 proxy_vmshm_obj_generation(struct proxy_vmshm_object *obj);
u32 proxy_vmshm_obj_type(struct proxy_vmshm_object *obj);
u32 proxy_vmshm_obj_owner_vmid(struct proxy_vmshm_object *obj);
u32 proxy_vmshm_obj_perms(struct proxy_vmshm_object *obj);
unsigned int proxy_vmshm_obj_order(struct proxy_vmshm_object *obj);
void *proxy_vmshm_obj_kva(struct proxy_vmshm_object *obj);
phys_addr_t proxy_vmshm_obj_gpa(struct proxy_vmshm_object *obj);
u64 proxy_vmshm_obj_offset(struct proxy_vmshm_object *obj);
size_t proxy_vmshm_obj_size(struct proxy_vmshm_object *obj);
size_t proxy_vmshm_obj_alloc_size(struct proxy_vmshm_object *obj);
bool proxy_vmshm_obj_is_sg(struct proxy_vmshm_object *obj);
bool proxy_vmshm_obj_is_contiguous(struct proxy_vmshm_object *obj);
unsigned int proxy_vmshm_obj_nr_segments(struct proxy_vmshm_object *obj);
/*
 * The caller must already hold a pin on obj, or otherwise serialize with the
 * manager so the object backing cannot be released while spans are consumed.
 */
int proxy_vmshm_obj_get_segment(struct proxy_vmshm_object *obj,
				unsigned int idx,
				struct proxy_vmshm_span *span);
/*
 * Translate logical object range [logical_off, logical_off + len) into shared
 * memory spans. On -ENOSPC, *nr_spans is set to the required number of spans.
 * The caller must already hold a pin on obj, or otherwise serialize with the
 * manager so the object backing cannot be released while spans are consumed.
 */
int proxy_vmshm_obj_translate(struct proxy_vmshm_object *obj,
			      u64 logical_off, size_t len,
			      struct proxy_vmshm_span *spans,
			      unsigned int max_spans,
			      unsigned int *nr_spans);

int proxy_vmshm_grant_create(struct proxy_vmshm_object *obj, u32 target_vmid,
			     u32 perms, u64 *grant_id);
/* Successful grant lookups also return a pinned object. */
int proxy_vmshm_grant_lookup_pin(u64 grant_id, u32 requester_vmid,
				 u32 required_perms,
				 struct proxy_vmshm_object **out);
int proxy_vmshm_grant_lookup(u64 grant_id, u32 requester_vmid,
			     u32 required_perms,
			     struct proxy_vmshm_object **out);
int proxy_vmshm_grant_revoke(u64 grant_id);
#else
static inline struct proxy_vmshm_object *
proxy_vmshm_alloc_ext(const struct proxy_vmshm_alloc_params *params,
		      gfp_t gfp)
{
	return ERR_PTR(-ENODEV);
}

static inline struct proxy_vmshm_object *
proxy_vmshm_alloc(size_t size, gfp_t gfp)
{
	return ERR_PTR(-ENODEV);
}

static inline void proxy_vmshm_free(struct proxy_vmshm_object *obj) {}

static inline int proxy_vmshm_free_handle(u64 handle, u32 requester_vmid)
{
	return -ENODEV;
}

static inline bool proxy_vmshm_get(struct proxy_vmshm_object *obj)
{
	return false;
}

static inline void proxy_vmshm_put(struct proxy_vmshm_object *obj) {}

static inline int proxy_vmshm_pin(struct proxy_vmshm_object *obj)
{
	return -ENODEV;
}

static inline void proxy_vmshm_unpin(struct proxy_vmshm_object *obj) {}

static inline int proxy_vmshm_lookup_pin(u64 handle, u32 requester_vmid,
					 u32 required_perms,
					 struct proxy_vmshm_object **out)
{
	return -ENODEV;
}

static inline int proxy_vmshm_lookup(u64 handle, u32 requester_vmid,
				     u32 required_perms,
				     struct proxy_vmshm_object **out)
{
	return -ENODEV;
}

static inline u64 proxy_vmshm_obj_handle(struct proxy_vmshm_object *obj)
{
	return 0;
}

static inline u32 proxy_vmshm_obj_id(struct proxy_vmshm_object *obj)
{
	return 0;
}

static inline u32 proxy_vmshm_obj_generation(struct proxy_vmshm_object *obj)
{
	return 0;
}

static inline u32 proxy_vmshm_obj_type(struct proxy_vmshm_object *obj)
{
	return 0;
}

static inline u32 proxy_vmshm_obj_owner_vmid(struct proxy_vmshm_object *obj)
{
	return 0;
}

static inline u32 proxy_vmshm_obj_perms(struct proxy_vmshm_object *obj)
{
	return 0;
}

static inline unsigned int proxy_vmshm_obj_order(struct proxy_vmshm_object *obj)
{
	return 0;
}

static inline void *proxy_vmshm_obj_kva(struct proxy_vmshm_object *obj)
{
	return NULL;
}

static inline phys_addr_t proxy_vmshm_obj_gpa(struct proxy_vmshm_object *obj)
{
	return 0;
}

static inline u64 proxy_vmshm_obj_offset(struct proxy_vmshm_object *obj)
{
	return 0;
}

static inline size_t proxy_vmshm_obj_size(struct proxy_vmshm_object *obj)
{
	return 0;
}

static inline size_t proxy_vmshm_obj_alloc_size(struct proxy_vmshm_object *obj)
{
	return 0;
}

static inline bool proxy_vmshm_obj_is_sg(struct proxy_vmshm_object *obj)
{
	return false;
}

static inline bool proxy_vmshm_obj_is_contiguous(struct proxy_vmshm_object *obj)
{
	return false;
}

static inline unsigned int
proxy_vmshm_obj_nr_segments(struct proxy_vmshm_object *obj)
{
	return 0;
}

static inline int proxy_vmshm_obj_get_segment(struct proxy_vmshm_object *obj,
					      unsigned int idx,
					      struct proxy_vmshm_span *span)
{
	return -ENODEV;
}

static inline int proxy_vmshm_obj_translate(struct proxy_vmshm_object *obj,
					    u64 logical_off, size_t len,
					    struct proxy_vmshm_span *spans,
					    unsigned int max_spans,
					    unsigned int *nr_spans)
{
	return -ENODEV;
}

static inline int proxy_vmshm_grant_create(struct proxy_vmshm_object *obj,
					   u32 target_vmid, u32 perms,
					   u64 *grant_id)
{
	return -ENODEV;
}

static inline int proxy_vmshm_grant_lookup_pin(u64 grant_id, u32 requester_vmid,
					       u32 required_perms,
					       struct proxy_vmshm_object **out)
{
	return -ENODEV;
}

static inline int proxy_vmshm_grant_lookup(u64 grant_id, u32 requester_vmid,
					   u32 required_perms,
					   struct proxy_vmshm_object **out)
{
	return -ENODEV;
}

static inline int proxy_vmshm_grant_revoke(u64 grant_id)
{
	return -ENODEV;
}
#endif

#endif /* _LINUX_PROXY_VMSHM_H */
