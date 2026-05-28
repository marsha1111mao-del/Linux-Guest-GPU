/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_CLIENT_VMSHM_H
#define _LINUX_CLIENT_VMSHM_H

#include <linux/errno.h>
#include <linux/ioctl.h>
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

struct client_vmshm_manager_info {
	__u64 gpa;
	__u64 size;
};

struct client_vmshm_manager_user_lookup {
	__u64 handle;
	__u64 grant_id;
	__u32 lookup;
	__u32 requester_vmid;
	__u32 required_perms;
	__u32 flags;
	struct vmshm_manager_desc desc;
};

#define CLIENT_VMSHM_MANAGER_IOC_MAGIC		'v'
#define CLIENT_VMSHM_MANAGER_IOC_GET_INFO \
	_IOR(CLIENT_VMSHM_MANAGER_IOC_MAGIC, 1, \
	     struct client_vmshm_manager_info)
#define CLIENT_VMSHM_MANAGER_IOC_GET_OBJECT \
	_IOWR(CLIENT_VMSHM_MANAGER_IOC_MAGIC, 2, \
	      struct client_vmshm_manager_user_lookup)

#if IS_ENABLED(CONFIG_CLIENT_VMSHM_MANAGER) || defined(CLIENT_VMSHM_MANAGER_BUILD)
bool client_vmshm_manager_ready(void);
int client_vmshm_manager_get(const struct client_vmshm_lookup_params *params,
			     struct client_vmshm_object **out);
void client_vmshm_manager_put(struct client_vmshm_object *obj);
const struct vmshm_manager_desc *
client_vmshm_object_desc(const struct client_vmshm_object *obj);
void *client_vmshm_object_kva(const struct client_vmshm_object *obj);
phys_addr_t client_vmshm_object_gpa(const struct client_vmshm_object *obj);
int client_vmshm_manager_map_offset(u64 offset, size_t size,
				    void **kva, phys_addr_t *gpa);
#else
static inline bool client_vmshm_manager_ready(void)
{
	return false;
}

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

static inline void *
client_vmshm_object_kva(const struct client_vmshm_object *obj)
{
	return NULL;
}

static inline phys_addr_t
client_vmshm_object_gpa(const struct client_vmshm_object *obj)
{
	return 0;
}

static inline int client_vmshm_manager_map_offset(u64 offset, size_t size,
						  void **kva, phys_addr_t *gpa)
{
	return -ENODEV;
}
#endif

#endif /* _LINUX_CLIENT_VMSHM_H */
