/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _VMSHM_PROXY_VMSHM_MANAGER_H
#define _VMSHM_PROXY_VMSHM_MANAGER_H

#include <linux/types.h>

#if IS_ENABLED(CONFIG_PROXY_VMSHM_MANAGER_SELFTEST) || defined(PROXY_VMSHM_MANAGER_SELFTEST_BUILD)
int proxy_vmshm_manager_selftest_permissions(u32 owner, size_t capacity);
#else
static inline int proxy_vmshm_manager_selftest_permissions(u32 owner, size_t capacity)
{
	return 0;
}
#endif
#endif
