// SPDX-License-Identifier: GPL-2.0
/*
 * proxy_vmshm_manager_test - lightweight self-tests for proxy_vmshm_manager.
 *
 * 这些测试只通过 include/linux/proxy_vmshm.h 中导出的公开 API 访问
 * manager，刻意不依赖 manager 内部 static 结构。这样可以验证后续
 * backend/协议层实际会使用到的语义：handle、grant、pin/free。
 *
 * 权限用例检查对象权限上限与 grant 的降权，防止查询和转授权提权。
 */

#define PROXY_VMSHM_MANAGER_SELFTEST_BUILD

#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/proxy_vmshm.h>
#include <linux/slab.h>
#include <linux/sizes.h>

#include "proxy_vmshm_manager.h"

#define VMSHM_TEST_TARGET 2

static struct proxy_vmshm_alloc_params
proxy_vmshm_test_params(size_t size, size_t align, u32 owner)
{
	struct proxy_vmshm_alloc_params params = {
		.owner_vmid = owner,
		.type = PROXY_VMSHM_OBJ_GENERIC,
		.flags = 0,
		.perms = 0,
		.size = size,
		.align = align,
	};

	return params;
}

static int proxy_vmshm_test_expect_err(const char *name, int ret, int expected)
{
	if (ret == expected)
		return 0;

	pr_err("proxy_manager_vmshm_test: %s expected %d got %d\n",
	       name, expected, ret);
	return -EINVAL;
}

int proxy_vmshm_manager_selftest_permissions(u32 owner)
{
	struct proxy_vmshm_alloc_params params;
	struct proxy_vmshm_object *obj, *pinned;
	u64 grant = 0;
	u64 handle;
	int ret;

	params = proxy_vmshm_test_params(3 * PAGE_SIZE, PAGE_SIZE,
					 owner);
	params.type = PROXY_VMSHM_OBJ_GPU_BO;
	params.flags = PROXY_VMSHM_F_CONTIG;
	params.perms = PROXY_VMSHM_PERM_CPU_READ | PROXY_VMSHM_PERM_CPU_WRITE;
	obj = proxy_vmshm_alloc_ext(&params, GFP_KERNEL);
	if (IS_ERR(obj))
		return PTR_ERR(obj);
	if (proxy_vmshm_obj_alloc_size(obj) != params.size ||
	    !proxy_vmshm_obj_is_contiguous(obj)) {
		ret = -EINVAL;
		goto out;
	}
	handle = proxy_vmshm_obj_handle(obj);

	ret = proxy_vmshm_lookup_pin(handle, owner,
				     PROXY_VMSHM_PERM_GPU_WRITE, &pinned);
	if (!ret)
		proxy_vmshm_unpin(pinned);
	ret = proxy_vmshm_test_expect_err("object permission escalation", ret,
					  -EACCES);
	if (ret)
		goto out;

	ret = proxy_vmshm_lookup_pin(handle, owner,
				     params.perms, &pinned);
	if (ret)
		goto out;
	proxy_vmshm_unpin(pinned);

	ret = proxy_vmshm_grant_create(obj, VMSHM_TEST_TARGET,
				       PROXY_VMSHM_PERM_GPU_WRITE, &grant);
	if (!ret) {
		proxy_vmshm_grant_revoke(grant);
		grant = 0;
	}
	ret = proxy_vmshm_test_expect_err("grant permission escalation", ret,
					  -EACCES);
	if (ret)
		goto out;

	ret = proxy_vmshm_grant_create(obj, VMSHM_TEST_TARGET,
				       PROXY_VMSHM_PERM_CPU_READ, &grant);
	if (ret)
		goto out;
	ret = proxy_vmshm_grant_lookup_pin(grant, VMSHM_TEST_TARGET,
					   PROXY_VMSHM_PERM_CPU_WRITE, &pinned);
	if (!ret)
		proxy_vmshm_unpin(pinned);
	ret = proxy_vmshm_test_expect_err("read-only grant write lookup", ret,
					  -EACCES);
	if (ret)
		goto out;
	ret = proxy_vmshm_grant_lookup_pin(grant, VMSHM_TEST_TARGET,
					   PROXY_VMSHM_PERM_CPU_READ, &pinned);
	if (!ret)
		proxy_vmshm_unpin(pinned);
out:
	if (grant)
		proxy_vmshm_grant_revoke(grant);
	proxy_vmshm_free(obj);
	if (!ret)
		pr_info("proxy_manager_vmshm_test: permissions PASS owner=%u\n", owner);
	return ret;
}
