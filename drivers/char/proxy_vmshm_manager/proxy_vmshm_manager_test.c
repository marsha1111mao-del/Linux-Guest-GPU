// SPDX-License-Identifier: GPL-2.0
/*
 * proxy_vmshm_manager_test - lightweight self-tests for proxy_vmshm_manager.
 *
 * 这些测试只通过 include/linux/proxy_vmshm.h 中导出的公开 API 访问
 * manager，刻意不依赖 manager 内部 static 结构。这样可以验证后续
 * backend/协议层实际会使用到的语义：handle、grant、pin/free。
 *
 * 当前阶段 manager 不检查 read/write/mmap 权限，因此测试也不覆盖这些
 * 权限语义；perms 字段统一传 0，后续阶段再单独补权限测试。
 */

#define PROXY_VMSHM_MANAGER_SELFTEST_BUILD

#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/proxy_vmshm.h>
#include <linux/slab.h>
#include <linux/sizes.h>

#include "proxy_vmshm_manager.h"

#define VMSHM_TEST_OWNER	1
#define VMSHM_TEST_TARGET	2
#define VMSHM_TEST_BAD_VM	99
#define VMSHM_TEST_MAGIC	0x766d7368U
#define VMSHM_TEST_STRESS_NR	64
#define VMSHM_TEST_STRESS_ROUNDS 8
#define VMSHM_TEST_SLAB_NR	128
#define VMSHM_TEST_SLAB_8_NR	513
#define VMSHM_TEST_FRAG_NR	512

struct proxy_vmshm_test_case {
	const char *name;
	int (*fn)(void);
};

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

static int proxy_vmshm_test_invalid_params(void)
{
	struct proxy_vmshm_alloc_params params;
	struct proxy_vmshm_object *obj;

	params = proxy_vmshm_test_params(0, PAGE_SIZE, VMSHM_TEST_OWNER);
	obj = proxy_vmshm_alloc_ext(&params, GFP_KERNEL);
	if (!IS_ERR(obj)) {
		proxy_vmshm_free(obj);
		return -EINVAL;
	}
	if (PTR_ERR(obj) != -EINVAL)
		return proxy_vmshm_test_expect_err("zero-size alloc",
						   PTR_ERR(obj), -EINVAL);

	params = proxy_vmshm_test_params(PAGE_SIZE, PAGE_SIZE * 3,
					 VMSHM_TEST_OWNER);
	obj = proxy_vmshm_alloc_ext(&params, GFP_KERNEL);
	if (!IS_ERR(obj)) {
		proxy_vmshm_free(obj);
		return -EINVAL;
	}
	if (PTR_ERR(obj) != -EINVAL)
		return proxy_vmshm_test_expect_err("bad-align alloc",
						   PTR_ERR(obj), -EINVAL);

	return 0;
}

static int proxy_vmshm_test_basic_object(void)
{
	struct proxy_vmshm_alloc_params params;
	struct proxy_vmshm_object *obj, *pinned;
	u64 handle;
	u32 *payload;
	int ret;

	params = proxy_vmshm_test_params(PAGE_SIZE, PAGE_SIZE,
					 VMSHM_TEST_OWNER);
	obj = proxy_vmshm_alloc_ext(&params, GFP_KERNEL);
	if (IS_ERR(obj))
		return PTR_ERR(obj);

	handle = proxy_vmshm_obj_handle(obj);
	if (!handle || !proxy_vmshm_obj_generation(obj) ||
	    proxy_vmshm_obj_owner_vmid(obj) != VMSHM_TEST_OWNER) {
		ret = -EINVAL;
		goto out_free;
	}

	payload = proxy_vmshm_obj_kva(obj);
	if (!payload) {
		ret = -EINVAL;
		goto out_free;
	}

	*payload = VMSHM_TEST_MAGIC;
	if (*payload != VMSHM_TEST_MAGIC) {
		ret = -EIO;
		goto out_free;
	}

	ret = proxy_vmshm_lookup_pin(handle, VMSHM_TEST_OWNER, 0, &pinned);
	if (ret)
		goto out_free;
	if (pinned != obj) {
		proxy_vmshm_unpin(pinned);
		ret = -EINVAL;
		goto out_free;
	}
	proxy_vmshm_unpin(pinned);

	ret = proxy_vmshm_lookup_pin(handle, VMSHM_TEST_BAD_VM, 0, &pinned);
	if (ret != -EACCES) {
		if (!ret)
			proxy_vmshm_unpin(pinned);
		ret = proxy_vmshm_test_expect_err("bad-owner lookup", ret,
						  -EACCES);
		goto out_free;
	}

	ret = 0;

out_free:
	proxy_vmshm_free(obj);
	return ret;
}

static int proxy_vmshm_test_grant(void)
{
	struct proxy_vmshm_alloc_params params;
	struct proxy_vmshm_object *obj, *pinned;
	u64 grant_handle;
	int ret;

	params = proxy_vmshm_test_params(PAGE_SIZE, PAGE_SIZE,
					 VMSHM_TEST_OWNER);
	obj = proxy_vmshm_alloc_ext(&params, GFP_KERNEL);
	if (IS_ERR(obj))
		return PTR_ERR(obj);

	ret = proxy_vmshm_grant_create(obj, VMSHM_TEST_TARGET, 0,
				       &grant_handle);
	if (ret)
		goto out_free;

	ret = proxy_vmshm_grant_lookup_pin(grant_handle, VMSHM_TEST_TARGET, 0,
					   &pinned);
	if (ret)
		goto out_revoke;
	if (pinned != obj) {
		proxy_vmshm_unpin(pinned);
		ret = -EINVAL;
		goto out_revoke;
	}
	proxy_vmshm_unpin(pinned);

	ret = proxy_vmshm_grant_lookup_pin(grant_handle, VMSHM_TEST_BAD_VM, 0,
					   &pinned);
	if (ret != -EACCES) {
		if (!ret)
			proxy_vmshm_unpin(pinned);
		ret = proxy_vmshm_test_expect_err("bad-target grant lookup",
						  ret, -EACCES);
		goto out_revoke;
	}

	ret = proxy_vmshm_grant_revoke(grant_handle);
	if (ret)
		goto out_free;

	ret = proxy_vmshm_grant_lookup_pin(grant_handle, VMSHM_TEST_TARGET, 0,
					   &pinned);
	if (ret != -ENOENT) {
		if (!ret)
			proxy_vmshm_unpin(pinned);
		ret = proxy_vmshm_test_expect_err("revoked grant lookup", ret,
						  -ENOENT);
		goto out_free;
	}

	ret = 0;
	goto out_free;

out_revoke:
	proxy_vmshm_grant_revoke(grant_handle);
out_free:
	proxy_vmshm_free(obj);
	return ret;
}

static int proxy_vmshm_test_align_and_zero_reuse(void)
{
	struct proxy_vmshm_alloc_params params;
	struct proxy_vmshm_object *obj, *obj2;
	u64 offset, offset2;
	u32 *payload;
	int ret = 0;
	int i;

	params = proxy_vmshm_test_params(PAGE_SIZE, SZ_2M,
					 VMSHM_TEST_OWNER);
	obj = proxy_vmshm_alloc_ext(&params, GFP_KERNEL);
	if (IS_ERR(obj))
		return PTR_ERR(obj);

	offset = proxy_vmshm_obj_offset(obj);
	if (!IS_ALIGNED(offset, SZ_2M)) {
		pr_err("proxy_manager_vmshm_test: align offset 0x%llx is not 2M aligned\n",
		       offset);
		ret = -EINVAL;
		goto out_free;
	}

	payload = proxy_vmshm_obj_kva(obj);
	for (i = 0; i < 16; i++)
		payload[i] = VMSHM_TEST_MAGIC + i;

	proxy_vmshm_free(obj);
	obj = NULL;

	obj2 = proxy_vmshm_alloc_ext(&params, GFP_KERNEL);
	if (IS_ERR(obj2))
		return PTR_ERR(obj2);

	offset2 = proxy_vmshm_obj_offset(obj2);
	payload = proxy_vmshm_obj_kva(obj2);
	if (offset2 == offset) {
		for (i = 0; i < 16; i++) {
			if (payload[i]) {
				pr_err("proxy_manager_vmshm_test: zero reuse failed index=%d value=0x%x\n",
				       i, payload[i]);
				ret = -EIO;
				break;
			}
		}
		pr_info("proxy_manager_vmshm_test: zero reuse checked at offset 0x%llx\n",
			offset2);
	} else {
		pr_info("proxy_manager_vmshm_test: zero reuse skipped, offset changed 0x%llx -> 0x%llx\n",
			offset, offset2);
	}

	proxy_vmshm_free(obj2);
	return ret;

out_free:
	proxy_vmshm_free(obj);
	return ret;
}

static int proxy_vmshm_test_stale_handle(void)
{
	struct proxy_vmshm_alloc_params params;
	struct proxy_vmshm_object *obj, *obj2, *pinned;
	u64 old_handle;
	u32 old_id;
	int ret;

	params = proxy_vmshm_test_params(PAGE_SIZE, PAGE_SIZE,
					 VMSHM_TEST_OWNER);
	obj = proxy_vmshm_alloc_ext(&params, GFP_KERNEL);
	if (IS_ERR(obj))
		return PTR_ERR(obj);

	old_handle = proxy_vmshm_obj_handle(obj);
	old_id = proxy_vmshm_obj_id(obj);
	proxy_vmshm_free(obj);

	obj2 = proxy_vmshm_alloc_ext(&params, GFP_KERNEL);
	if (IS_ERR(obj2))
		return PTR_ERR(obj2);

	ret = proxy_vmshm_lookup_pin(old_handle, VMSHM_TEST_OWNER, 0,
				     &pinned);
	if (proxy_vmshm_obj_id(obj2) == old_id) {
		if (ret != -ESTALE) {
			if (!ret)
				proxy_vmshm_unpin(pinned);
			ret = proxy_vmshm_test_expect_err("stale handle", ret,
							  -ESTALE);
			goto out_free;
		}
	} else if (ret != -ENOENT) {
		if (!ret)
			proxy_vmshm_unpin(pinned);
		ret = proxy_vmshm_test_expect_err("old handle", ret, -ENOENT);
		goto out_free;
	}

	ret = 0;

out_free:
	proxy_vmshm_free(obj2);
	return ret;
}

static int proxy_vmshm_test_pin_free_unpin(void)
{
	struct proxy_vmshm_alloc_params params;
	struct proxy_vmshm_object *obj, *pinned;
	u64 handle;
	int ret;

	params = proxy_vmshm_test_params(PAGE_SIZE * 2, PAGE_SIZE,
					 VMSHM_TEST_OWNER);
	obj = proxy_vmshm_alloc_ext(&params, GFP_KERNEL);
	if (IS_ERR(obj))
		return PTR_ERR(obj);

	handle = proxy_vmshm_obj_handle(obj);
	ret = proxy_vmshm_lookup_pin(handle, VMSHM_TEST_OWNER, 0, &pinned);
	if (ret)
		goto out_free;

	proxy_vmshm_free(obj);

	ret = proxy_vmshm_lookup_pin(handle, VMSHM_TEST_OWNER, 0, &obj);
	if (ret != -ENOENT) {
		if (!ret)
			proxy_vmshm_unpin(obj);
		ret = proxy_vmshm_test_expect_err("zombie lookup", ret,
						  -ENOENT);
		proxy_vmshm_unpin(pinned);
		return ret;
	}

	proxy_vmshm_unpin(pinned);
	return 0;

out_free:
	proxy_vmshm_free(obj);
	return ret;
}

static size_t proxy_vmshm_test_expected_slab_size(size_t size)
{
	size_t object_size = 8;

	while (object_size < size)
		object_size <<= 1;

	return object_size;
}

static int proxy_vmshm_test_slab_small_objects(void)
{
	static const size_t sizes[] = {
		1, 8, 9, 16, 17, 32, 33, 64, 65, 128, 129,
		256, 257, 512, 513, 1024, 1025, 2048,
	};
	struct proxy_vmshm_object *objects[VMSHM_TEST_SLAB_NR];
	struct proxy_vmshm_alloc_params params;
	u8 *payload;
	size_t size, expected;
	size_t j;
	int i, ret = 0;

	memset(objects, 0, sizeof(objects));

	for (i = 0; i < VMSHM_TEST_SLAB_NR; i++) {
		size = sizes[i % ARRAY_SIZE(sizes)];
		params = proxy_vmshm_test_params(size, 0, VMSHM_TEST_OWNER);
		objects[i] = proxy_vmshm_alloc_ext(&params, GFP_KERNEL);
		if (IS_ERR(objects[i])) {
			ret = PTR_ERR(objects[i]);
			objects[i] = NULL;
			goto out_free;
		}

		expected = proxy_vmshm_test_expected_slab_size(size);
		if (proxy_vmshm_obj_alloc_size(objects[i]) != expected) {
			pr_err("proxy_manager_vmshm_test: slab alloc_size mismatch size=%zu expected=%zu got=%zu\n",
			       size, expected,
			       proxy_vmshm_obj_alloc_size(objects[i]));
			ret = -EINVAL;
			goto out_free;
		}

		payload = proxy_vmshm_obj_kva(objects[i]);
		if (!payload) {
			ret = -EINVAL;
			goto out_free;
		}

		memset(payload, i, size);
		for (j = 0; j < size; j++) {
			if (payload[j] != (u8)i) {
				pr_err("proxy_manager_vmshm_test: slab payload mismatch obj=%d byte=%zu\n",
				       i, j);
				ret = -EIO;
				goto out_free;
			}
		}
	}

	for (i = 1; i < VMSHM_TEST_SLAB_NR; i += 2) {
		proxy_vmshm_free(objects[i]);
		objects[i] = NULL;
	}

	for (i = 1; i < VMSHM_TEST_SLAB_NR; i += 2) {
		params = proxy_vmshm_test_params(64, 0, VMSHM_TEST_OWNER);
		objects[i] = proxy_vmshm_alloc_ext(&params, GFP_KERNEL);
		if (IS_ERR(objects[i])) {
			ret = PTR_ERR(objects[i]);
			objects[i] = NULL;
			goto out_free;
		}

		if (proxy_vmshm_obj_alloc_size(objects[i]) != 64) {
			ret = -EINVAL;
			goto out_free;
		}
	}

	pr_info("proxy_manager_vmshm_test: slab small-object allocation checked (%d objects)\n",
		VMSHM_TEST_SLAB_NR);

out_free:
	for (i = 0; i < VMSHM_TEST_SLAB_NR; i++) {
		proxy_vmshm_free(objects[i]);
		objects[i] = NULL;
	}
	return ret;
}

static int proxy_vmshm_test_slab_tiny_classes(void)
{
	static const struct {
		size_t size;
		size_t align;
		size_t expected;
	} cases[] = {
		{ 1, 0, 8 },
		{ 8, 0, 8 },
		{ 9, 0, 16 },
		{ 16, 0, 16 },
		{ 8, 16, 16 },
		{ 8, 64, 64 },
	};
	struct proxy_vmshm_object **tiny;
	struct proxy_vmshm_alloc_params params;
	struct proxy_vmshm_object *obj;
	int i, ret = 0;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		params = proxy_vmshm_test_params(cases[i].size, cases[i].align,
						 VMSHM_TEST_OWNER);
		obj = proxy_vmshm_alloc_ext(&params, GFP_KERNEL);
		if (IS_ERR(obj))
			return PTR_ERR(obj);

		if (proxy_vmshm_obj_alloc_size(obj) != cases[i].expected) {
			pr_err("proxy_manager_vmshm_test: tiny slab case %d expected=%zu got=%zu\n",
			       i, cases[i].expected,
			       proxy_vmshm_obj_alloc_size(obj));
			ret = -EINVAL;
		}

		proxy_vmshm_free(obj);
		if (ret)
			return ret;
	}

	tiny = kcalloc(VMSHM_TEST_SLAB_8_NR, sizeof(*tiny), GFP_KERNEL);
	if (!tiny)
		return -ENOMEM;

	for (i = 0; i < VMSHM_TEST_SLAB_8_NR; i++) {
		params = proxy_vmshm_test_params(8, 0, VMSHM_TEST_OWNER);
		tiny[i] = proxy_vmshm_alloc_ext(&params, GFP_KERNEL);
		if (IS_ERR(tiny[i])) {
			ret = PTR_ERR(tiny[i]);
			tiny[i] = NULL;
			goto out_free;
		}

		if (proxy_vmshm_obj_alloc_size(tiny[i]) != 8) {
			ret = -EINVAL;
			goto out_free;
		}
	}

	pr_info("proxy_manager_vmshm_test: 8B slab allocation checked (%d objects)\n",
		VMSHM_TEST_SLAB_8_NR);

out_free:
	for (i = 0; i < VMSHM_TEST_SLAB_8_NR; i++) {
		proxy_vmshm_free(tiny[i]);
		tiny[i] = NULL;
	}
	kfree(tiny);
	return ret;
}

static int proxy_vmshm_test_sg_fallback(void)
{
	struct proxy_vmshm_object **blocks;
	struct proxy_vmshm_object *obj = NULL, *pinned = NULL;
	struct proxy_vmshm_alloc_params params;
	struct proxy_vmshm_span spans[2];
	struct proxy_vmshm_span seg0, seg1;
	unsigned int nr_spans;
	u64 handle;
	int i, nr_blocks = 0, ret = 0;

	blocks = kcalloc(VMSHM_TEST_FRAG_NR, sizeof(*blocks), GFP_KERNEL);
	if (!blocks)
		return -ENOMEM;

	params = proxy_vmshm_test_params(SZ_64K, PAGE_SIZE, VMSHM_TEST_OWNER);
	params.flags = PROXY_VMSHM_F_ALLOW_SG;
	obj = proxy_vmshm_alloc_ext(&params, GFP_KERNEL);
	if (IS_ERR(obj))
		return PTR_ERR(obj);
	if (!proxy_vmshm_obj_is_contiguous(obj) || proxy_vmshm_obj_is_sg(obj)) {
		proxy_vmshm_free(obj);
		return -EINVAL;
	}
	proxy_vmshm_free(obj);
	obj = NULL;

	params = proxy_vmshm_test_params(64, 0, VMSHM_TEST_OWNER);
	params.flags = PROXY_VMSHM_F_ALLOW_SG;
	obj = proxy_vmshm_alloc_ext(&params, GFP_KERNEL);
	if (IS_ERR(obj))
		return PTR_ERR(obj);
	if (proxy_vmshm_obj_alloc_size(obj) != 64 ||
	    !proxy_vmshm_obj_is_contiguous(obj) || proxy_vmshm_obj_is_sg(obj)) {
		proxy_vmshm_free(obj);
		return -EINVAL;
	}
	proxy_vmshm_free(obj);
	obj = NULL;

	params = proxy_vmshm_test_params(SZ_1M, PAGE_SIZE, VMSHM_TEST_OWNER);
	params.flags = PROXY_VMSHM_F_CONTIG;
	for (i = 0; i < VMSHM_TEST_FRAG_NR; i++) {
		blocks[i] = proxy_vmshm_alloc_ext(&params, GFP_KERNEL);
		if (IS_ERR(blocks[i])) {
			if (PTR_ERR(blocks[i]) == -ENOMEM) {
				blocks[i] = NULL;
				break;
			}

			ret = PTR_ERR(blocks[i]);
			blocks[i] = NULL;
			goto out_free_blocks;
		}
		nr_blocks++;
	}

	if (nr_blocks < 4) {
		ret = -ENOMEM;
		goto out_free_blocks;
	}

	for (i = 0; i < nr_blocks; i += 2) {
		proxy_vmshm_free(blocks[i]);
		blocks[i] = NULL;
	}

	params = proxy_vmshm_test_params(SZ_2M, PAGE_SIZE, VMSHM_TEST_OWNER);
	params.flags = PROXY_VMSHM_F_ALLOW_SG;
	params.perms = PROXY_VMSHM_PERM_MMAP;
	obj = proxy_vmshm_alloc_ext(&params, GFP_KERNEL);
	if (!IS_ERR(obj)) {
		proxy_vmshm_free(obj);
		obj = NULL;
		ret = -EINVAL;
		goto out_free_blocks;
	}
	if (PTR_ERR(obj) != -ENOMEM) {
		ret = proxy_vmshm_test_expect_err("sg mmap fallback",
						  PTR_ERR(obj), -ENOMEM);
		obj = NULL;
		goto out_free_blocks;
	}
	obj = NULL;

	params = proxy_vmshm_test_params(SZ_2M, PAGE_SIZE, VMSHM_TEST_OWNER);
	params.flags = PROXY_VMSHM_F_ALLOW_SG;
	obj = proxy_vmshm_alloc_ext(&params, GFP_KERNEL);
	if (IS_ERR(obj)) {
		ret = PTR_ERR(obj);
		obj = NULL;
		goto out_free_blocks;
	}

	if (!proxy_vmshm_obj_is_sg(obj) ||
	    proxy_vmshm_obj_nr_segments(obj) < 2 ||
	    proxy_vmshm_obj_alloc_size(obj) != SZ_2M) {
		ret = -EINVAL;
		goto out_free_obj;
	}

	ret = proxy_vmshm_obj_get_segment(obj, 0, &seg0);
	if (ret)
		goto out_free_obj;
	ret = proxy_vmshm_obj_get_segment(obj, 1, &seg1);
	if (ret)
		goto out_free_obj;
	if (seg0.logical_offset != 0 ||
	    seg1.logical_offset != seg0.size) {
		ret = -EINVAL;
		goto out_free_obj;
	}

	ret = proxy_vmshm_obj_translate(obj, seg0.size - 128, 256, spans,
					ARRAY_SIZE(spans), &nr_spans);
	if (ret || nr_spans != 2) {
		ret = ret ?: -EINVAL;
		goto out_free_obj;
	}

	ret = proxy_vmshm_obj_translate(obj, seg0.size - 128, 256, spans, 1,
					&nr_spans);
	if (ret != -ENOSPC || nr_spans != 2) {
		ret = proxy_vmshm_test_expect_err("sg translate short spans",
						  ret, -ENOSPC);
		goto out_free_obj;
	}

	ret = proxy_vmshm_obj_translate(obj, SZ_2M - 128, 256, spans,
					ARRAY_SIZE(spans), &nr_spans);
	if (ret != -ERANGE) {
		ret = proxy_vmshm_test_expect_err("sg translate range", ret,
						  -ERANGE);
		goto out_free_obj;
	}

	handle = proxy_vmshm_obj_handle(obj);
	ret = proxy_vmshm_lookup_pin(handle, VMSHM_TEST_OWNER, 0, &pinned);
	if (ret)
		goto out_free_obj;
	if (pinned != obj) {
		ret = -EINVAL;
		goto out_unpin;
	}

	proxy_vmshm_free(obj);
	obj = NULL;

	ret = proxy_vmshm_lookup_pin(handle, VMSHM_TEST_OWNER, 0, &obj);
	if (ret != -ENOENT) {
		if (!ret)
			proxy_vmshm_unpin(obj);
		ret = proxy_vmshm_test_expect_err("sg zombie lookup", ret,
						  -ENOENT);
		goto out_unpin;
	}

	ret = proxy_vmshm_obj_translate(pinned, 0, PAGE_SIZE, spans,
					ARRAY_SIZE(spans), &nr_spans);
	if (ret || nr_spans != 1)
		ret = ret ?: -EINVAL;

out_unpin:
	proxy_vmshm_unpin(pinned);
	pinned = NULL;
out_free_obj:
	proxy_vmshm_free(obj);
out_free_blocks:
	for (i = 0; i < nr_blocks; i++) {
		proxy_vmshm_free(blocks[i]);
		blocks[i] = NULL;
	}
	kfree(blocks);

	return ret;
}

static int proxy_vmshm_test_stress_alloc_free(void)
{
	struct proxy_vmshm_alloc_params params;
	struct proxy_vmshm_object *objects[VMSHM_TEST_STRESS_NR];
	size_t size;
	int i, round, ret;

	for (round = 0; round < VMSHM_TEST_STRESS_ROUNDS; round++) {
		memset(objects, 0, sizeof(objects));

		for (i = 0; i < VMSHM_TEST_STRESS_NR; i++) {
			size = PAGE_SIZE + (((i + round) % 17) * 1536);
			params = proxy_vmshm_test_params(size, PAGE_SIZE,
							 VMSHM_TEST_OWNER);
			objects[i] = proxy_vmshm_alloc_ext(&params, GFP_KERNEL);
			if (IS_ERR(objects[i])) {
				ret = PTR_ERR(objects[i]);
				objects[i] = NULL;
				goto out_free;
			}
		}

		for (i = round & 1; i < VMSHM_TEST_STRESS_NR; i += 2) {
			proxy_vmshm_free(objects[i]);
			objects[i] = NULL;
		}

		for (i = round & 1; i < VMSHM_TEST_STRESS_NR; i += 2) {
			size = PAGE_SIZE << ((i + round) % 4);
			params = proxy_vmshm_test_params(size, PAGE_SIZE,
							 VMSHM_TEST_OWNER);
			objects[i] = proxy_vmshm_alloc_ext(&params, GFP_KERNEL);
			if (IS_ERR(objects[i])) {
				ret = PTR_ERR(objects[i]);
				objects[i] = NULL;
				goto out_free;
			}
		}

		for (i = 0; i < VMSHM_TEST_STRESS_NR; i++) {
			proxy_vmshm_free(objects[i]);
			objects[i] = NULL;
		}

		pr_info("proxy_manager_vmshm_test: stress round %d/%d passed\n",
			round + 1, VMSHM_TEST_STRESS_ROUNDS);
	}

	return 0;

out_free:
	for (i = 0; i < VMSHM_TEST_STRESS_NR; i++) {
		proxy_vmshm_free(objects[i]);
		objects[i] = NULL;
	}
	return ret;
}

static int proxy_vmshm_test_run_one(const struct proxy_vmshm_test_case *test)
{
	int ret;

	pr_info("proxy_manager_vmshm_test: RUN  %s\n", test->name);
	ret = test->fn();
	if (ret)
		pr_err("proxy_manager_vmshm_test: FAIL %s (%d)\n", test->name, ret);
	else
		pr_info("proxy_manager_vmshm_test: PASS %s\n", test->name);

	return ret;
}

static int proxy_vmshm_test_manager_empty_stats(void)
{
	return proxy_vmshm_manager_debug_check_empty();
}

int proxy_vmshm_manager_selftest_run(void)
{
	static const struct proxy_vmshm_test_case tests[] = {
		{ "invalid-params", proxy_vmshm_test_invalid_params },
		{ "basic-object", proxy_vmshm_test_basic_object },
		{ "grant", proxy_vmshm_test_grant },
		{ "align-and-zero-reuse", proxy_vmshm_test_align_and_zero_reuse },
		{ "stale-handle", proxy_vmshm_test_stale_handle },
		{ "pin-free-unpin", proxy_vmshm_test_pin_free_unpin },
		{ "slab-small-objects", proxy_vmshm_test_slab_small_objects },
		{ "slab-tiny-classes", proxy_vmshm_test_slab_tiny_classes },
		{ "sg-fallback", proxy_vmshm_test_sg_fallback },
		{ "stress-alloc-free", proxy_vmshm_test_stress_alloc_free },
		{ "manager-empty-stats", proxy_vmshm_test_manager_empty_stats },
	};
	int i;
	int ret;

	pr_info("proxy_manager_vmshm_test: start (%zu cases)\n", ARRAY_SIZE(tests));

	for (i = 0; i < ARRAY_SIZE(tests); i++) {
		ret = proxy_vmshm_test_run_one(&tests[i]);
		if (ret) {
			pr_err("proxy_manager_vmshm_test: failed at case %d/%zu\n",
			       i + 1, ARRAY_SIZE(tests));
			return ret;
		}
	}

	pr_info("proxy_manager_vmshm_test: all %zu cases passed\n", ARRAY_SIZE(tests));
	return 0;
}
