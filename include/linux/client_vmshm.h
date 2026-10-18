/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_CLIENT_VMSHM_H
#define _LINUX_CLIENT_VMSHM_H

#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/types.h>
#include <linux/vmshm_manager.h>

struct client_vmshm_object;

struct client_vmshm_lookup_params {
	u64 handle;
	u64 grant_id;
	u32 lookup;
	u32 requester_vmid;
	u32 required_perms;
	u32 flags;
};

#if IS_ENABLED(CONFIG_CLIENT_VMSHM_MANAGER) || defined(CLIENT_VMSHM_MANAGER_BUILD)
int client_vmshm_manager_bind(void *base, phys_addr_t gpa, size_t size,
			      u32 client_vmid);
void client_vmshm_manager_unbind(void *base);
int client_vmshm_manager_get(const struct client_vmshm_lookup_params *params,
			     struct client_vmshm_object **out);
void client_vmshm_manager_put(struct client_vmshm_object *obj);
const struct vmshm_manager_desc *
client_vmshm_object_desc(const struct client_vmshm_object *obj);
phys_addr_t client_vmshm_object_gpa(const struct client_vmshm_object *obj);
const void *client_vmshm_object_kva(const struct client_vmshm_object *obj);
#else
static inline int client_vmshm_manager_bind(void *base, phys_addr_t gpa,
					 size_t size, u32 client_vmid)
{
	return -ENODEV;
}

static inline void client_vmshm_manager_unbind(void *base) {}

static inline int
client_vmshm_manager_get(const struct client_vmshm_lookup_params *params,
			 struct client_vmshm_object **out)
{
	return -ENODEV;
}

static inline void client_vmshm_manager_put(struct client_vmshm_object *obj) {}

static inline const struct vmshm_manager_desc *
client_vmshm_object_desc(const struct client_vmshm_object *obj)
{
	return NULL;
}

static inline const void *client_vmshm_object_kva(const struct client_vmshm_object *obj)
{
	return NULL;
}

static inline phys_addr_t
client_vmshm_object_gpa(const struct client_vmshm_object *obj)
{
	return 0;
}

#endif

#endif /* _LINUX_CLIENT_VMSHM_H */
