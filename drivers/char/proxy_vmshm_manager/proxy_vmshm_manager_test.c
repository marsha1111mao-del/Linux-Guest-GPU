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
#include <linux/string.h>

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

static int proxy_vmshm_selftest_gpu_lifetime(u32 owner)
{
	struct proxy_vmshm_alloc_params params =
		proxy_vmshm_test_params(PAGE_SIZE, PAGE_SIZE, owner);
	struct proxy_vmshm_object *obj, *replacement = NULL, *lookup = NULL;
	u64 old_handle, old_offset;
	bool pinned = false, revoked = false;
	int ret;

	params.type = PROXY_VMSHM_OBJ_GPU_BO;
	params.flags = PROXY_VMSHM_F_CONTIG;
	params.perms = PROXY_VMSHM_PERM_GPU_READ;
	obj = proxy_vmshm_alloc_ext(&params, GFP_KERNEL);
	if (IS_ERR(obj))
		return PTR_ERR(obj);
	old_handle = proxy_vmshm_obj_handle(obj);
	old_offset = proxy_vmshm_obj_offset(obj);
	ret = proxy_vmshm_pin(obj);
	if (ret)
		goto out;
	pinned = true;
	ret = proxy_vmshm_obj_check_gpu_map(obj, PROXY_VMSHM_PERM_GPU_READ);
	if (ret)
		goto out;
	ret = proxy_vmshm_test_expect_err("GPU write escalation",
		proxy_vmshm_obj_check_gpu_map(obj, PROXY_VMSHM_PERM_GPU_READ |
					    PROXY_VMSHM_PERM_GPU_WRITE), -EACCES);
	if (ret)
		goto out;
	ret = proxy_vmshm_lookup_pin(old_handle, owner + 1,
				     PROXY_VMSHM_PERM_GPU_READ, &lookup);
	if (!ret)
		proxy_vmshm_unpin(lookup);
	ret = proxy_vmshm_test_expect_err("foreign GPU owner", ret, -EACCES);
	if (ret)
		goto out;
	memset(proxy_vmshm_obj_kva(obj), 0xa5, PAGE_SIZE);
	proxy_vmshm_free(obj);
	revoked = true;
	ret = proxy_vmshm_test_expect_err("revoked GPU mapping",
		proxy_vmshm_obj_check_gpu_map(obj, PROXY_VMSHM_PERM_GPU_READ), -ENOENT);
	if (ret)
		goto out;
	replacement = proxy_vmshm_alloc_ext(&params, GFP_KERNEL);
	if (IS_ERR(replacement)) {
		ret = PTR_ERR(replacement);
		replacement = NULL;
		goto out;
	}
	if (proxy_vmshm_obj_offset(replacement) == old_offset) {
		pr_err("proxy_manager_vmshm_test: pinned GPU backing was reused\n");
		ret = -EINVAL;
		goto out;
	}
	ret = proxy_vmshm_lookup_pin(old_handle, owner,
				     PROXY_VMSHM_PERM_GPU_READ, &lookup);
	if (!ret)
		proxy_vmshm_unpin(lookup);
	ret = proxy_vmshm_test_expect_err("recycled GPU handle", ret, -ESTALE);
	if (ret)
		goto out;
	proxy_vmshm_free(replacement);
	replacement = NULL;
	proxy_vmshm_unpin(obj);
	pinned = false;
	obj = NULL;
	replacement = proxy_vmshm_alloc_ext(&params, GFP_KERNEL);
	if (IS_ERR(replacement))
		return PTR_ERR(replacement);
	if (proxy_vmshm_obj_offset(replacement) != old_offset ||
	    memchr_inv(proxy_vmshm_obj_kva(replacement), 0, PAGE_SIZE)) {
		pr_err("proxy_manager_vmshm_test: GPU backing reuse/zero mismatch old=%llu new=%llu\n",
			old_offset, proxy_vmshm_obj_offset(replacement));
		ret = -EINVAL;
		goto out;
	}
	proxy_vmshm_free(replacement);
	params.type = PROXY_VMSHM_OBJ_FENCE_PAGE;
	replacement = proxy_vmshm_alloc_ext(&params, GFP_KERNEL);
	if (IS_ERR(replacement))
		return PTR_ERR(replacement);
	ret = proxy_vmshm_pin(replacement);
	if (ret)
		goto out;
	ret = proxy_vmshm_test_expect_err("GPU control-page alias",
		proxy_vmshm_obj_check_gpu_map(replacement, PROXY_VMSHM_PERM_GPU_READ),
		-EACCES);
	proxy_vmshm_unpin(replacement);
out:
	proxy_vmshm_free(replacement);
	if (obj && !revoked)
		proxy_vmshm_free(obj);
	if (pinned)
		proxy_vmshm_unpin(obj);
	if (!ret)
		pr_info("proxy_manager_vmshm_test: GPU isolation PASS owner=%u permissions=1 control=1 stale=1 pin_reuse=1 zero=1\n",
			owner);
	return ret;
}

static int proxy_vmshm_selftest_gpu_fragmentation(u32 owner, size_t capacity)
{
	struct proxy_vmshm_alloc_params params =
		proxy_vmshm_test_params(PAGE_SIZE, PAGE_SIZE, owner);
	struct proxy_vmshm_object *objects[4] = {};
	struct proxy_vmshm_object *replacement = NULL;
	struct proxy_vmshm_span segment;
	size_t half = ((capacity >> PAGE_SHIFT) - 2) / 2 * PAGE_SIZE;
	size_t sizes[] = { half, PAGE_SIZE, capacity - half - 2 * PAGE_SIZE, PAGE_SIZE };
	u64 total = 0;
	unsigned int index, segments = 0;
	bool pinned = false, revoked = false;
	int ret = 0;

	params.type = PROXY_VMSHM_OBJ_GPU_BO;
	params.flags = PROXY_VMSHM_F_CONTIG;
	params.perms = PROXY_VMSHM_PERM_CPU_READ | PROXY_VMSHM_PERM_CPU_WRITE |
		PROXY_VMSHM_PERM_MMAP | PROXY_VMSHM_PERM_GPU_READ;
	for (index = 0; index < ARRAY_SIZE(sizes); index++) {
		params.size = sizes[index];
		objects[index] = proxy_vmshm_alloc_ext(&params, GFP_KERNEL);
		if (IS_ERR(objects[index])) {
			ret = PTR_ERR(objects[index]);
			objects[index] = NULL;
			goto out;
		}
	}
	proxy_vmshm_free(objects[0]);
	objects[0] = NULL;
	proxy_vmshm_free(objects[2]);
	objects[2] = NULL;
	params.size = capacity - 2 * PAGE_SIZE;
	replacement = proxy_vmshm_alloc_ext(&params, GFP_KERNEL);
	if (!IS_ERR(replacement)) {
		ret = -EINVAL;
		goto out;
	}
	ret = proxy_vmshm_test_expect_err("fragmented contiguous BO",
					 PTR_ERR(replacement), -ENOMEM);
	replacement = NULL;
	if (ret)
		goto out;
	params.flags = PROXY_VMSHM_F_ALLOW_SG;
	replacement = proxy_vmshm_alloc_ext(&params, GFP_KERNEL);
	if (IS_ERR(replacement)) {
		ret = PTR_ERR(replacement);
		replacement = NULL;
		goto out;
	}
	segments = proxy_vmshm_obj_nr_segments(replacement);
	if (!proxy_vmshm_obj_is_sg(replacement) || segments < 2 ||
	    proxy_vmshm_obj_alloc_size(replacement) != params.size) {
		ret = -EINVAL;
		goto out;
	}
	ret = proxy_vmshm_pin(replacement);
	if (ret)
		goto out;
	pinned = true;
	ret = proxy_vmshm_obj_check_gpu_map(replacement, PROXY_VMSHM_PERM_GPU_READ);
	if (ret)
		goto out;
	for (index = 0; index < segments; index++) {
		ret = proxy_vmshm_obj_get_segment(replacement, index, &segment);
		if (ret)
			goto out;
		if (segment.logical_offset != total || !segment.size ||
		    segment.size > params.size - total) {
			ret = -EINVAL;
			goto out;
		}
		memset(segment.kva, 0xa5, segment.size);
		total += segment.size;
	}
	if (total != params.size) {
		ret = -EINVAL;
		goto out;
	}
	proxy_vmshm_free(replacement);
	revoked = true;
	ret = proxy_vmshm_test_expect_err("revoked SG mapping",
		proxy_vmshm_obj_check_gpu_map(replacement, PROXY_VMSHM_PERM_GPU_READ), -ENOENT);
	proxy_vmshm_unpin(replacement);
	pinned = false;
	replacement = NULL;
	if (ret)
		goto out;
	proxy_vmshm_free(objects[1]);
	objects[1] = NULL;
	proxy_vmshm_free(objects[3]);
	objects[3] = NULL;
	params.flags = PROXY_VMSHM_F_CONTIG;
	params.size = capacity;
	replacement = proxy_vmshm_alloc_ext(&params, GFP_KERNEL);
	if (IS_ERR(replacement)) {
		ret = PTR_ERR(replacement);
		replacement = NULL;
		goto out;
	}
	revoked = false;
	if (memchr_inv(proxy_vmshm_obj_kva(replacement), 0, capacity))
		ret = -EINVAL;
out:
	if (!revoked)
		proxy_vmshm_free(replacement);
	if (pinned)
		proxy_vmshm_unpin(replacement);
	for (index = 0; index < ARRAY_SIZE(sizes); index++)
		proxy_vmshm_free(objects[index]);
	if (!ret)
		pr_info("proxy_manager_vmshm_test: GPU SG fragmentation PASS owner=%u segments=%u zero=1 revoke=1\n",
			owner, segments);
	return ret;
}

int proxy_vmshm_manager_selftest_permissions(u32 owner, size_t capacity)
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
		ret = proxy_vmshm_selftest_gpu_lifetime(owner);
	if (!ret)
		ret = proxy_vmshm_selftest_gpu_fragmentation(owner, capacity);
	if (!ret)
		pr_info("proxy_manager_vmshm_test: permissions PASS owner=%u\n", owner);
	return ret;
}
