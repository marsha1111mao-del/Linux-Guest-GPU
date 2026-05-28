/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _VMSHM_PROXY_VMSHM_MANAGER_H
#define _VMSHM_PROXY_VMSHM_MANAGER_H

#include <linux/types.h>

int proxy_vmshm_manager_init(void *base, phys_addr_t gpa, size_t size);
void proxy_vmshm_manager_destroy(void);

#if IS_ENABLED(CONFIG_PROXY_VMSHM_MANAGER_SELFTEST) || \
	defined(PROXY_VMSHM_MANAGER_SELFTEST_BUILD)
int proxy_vmshm_manager_selftest_run(void);
int proxy_vmshm_manager_debug_check_empty(void);
#else
static inline int proxy_vmshm_manager_selftest_run(void)
{
	return 0;
}

static inline int proxy_vmshm_manager_debug_check_empty(void)
{
	return 0;
}
#endif

#endif /* _VMSHM_PROXY_VMSHM_MANAGER_H */
