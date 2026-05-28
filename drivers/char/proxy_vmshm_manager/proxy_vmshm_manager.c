// SPDX-License-Identifier: GPL-2.0
/*
 * proxy_vmshm_manager - vmshm 共享内存的内核态对象管理器。
 *
 * proxy VM 持有所有可信元数据。shared memslot 只保存 payload，
 * 也就是未来可能授权给其他 VM 访问的对象内容。
 *
 * manager 主要负责三件事：
 * - 用私有 buddy allocator 从 vmshm window 中切分 payload 内存；
 * - 把 object/grant 等元数据保存在 proxy 私有内核内存中；
 * - 在其他路径使用共享对象前，统一做 handle、owner 和生命周期检查。
 *
 * 当前阶段暂不在 manager 内解释 read/write/mmap 权限；perms 字段只作为
 * 未来协议的保留元数据保存。
 */

#define PROXY_VMSHM_MANAGER_BUILD

#include <linux/atomic.h>
#include <linux/bitmap.h>
#include <linux/delay.h>
#include <linux/idr.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/list.h>
#include <linux/log2.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/numa.h>
#include <linux/proxy_vmshm.h>
#include <linux/refcount.h>
#include <linux/slab.h>
#include <linux/sizes.h>
#include <linux/vmshm_comm.h>
#include <linux/vmshm_manager.h>
#include <linux/xarray.h>

#include "proxy_vmshm_manager.h"

#define PROXY_VMSHM_RESERVE_SIZE	SZ_1M
#define PROXY_VMSHM_MIN_ALLOC_ORDER	0
#define PROXY_VMSHM_ZERO_INLINE_MAX	SZ_64K
#define PROXY_VMSHM_MAX_ALIGN		SZ_2M
#define PROXY_VMSHM_SLAB_MAX_SIZE	SZ_2K
#define PROXY_VMSHM_SG_MAX_SEGS		256
#define PROXY_VMSHM_MANAGER_SEND_RETRIES	1000
#define PROXY_VMSHM_MANAGER_SEND_WAIT_US	1000

static const size_t proxy_vmshm_slab_sizes[] = {
	8, 16, 32, 64, 128, 256, 512, 1024, 2048,
};

#define PROXY_VMSHM_SLAB_NR_CLASSES ARRAY_SIZE(proxy_vmshm_slab_sizes)

static int proxy_vmshm_manager_rx_handler(const struct vmshm_comm_rx *rx,
					  void *priv);

/*
 * 这里的 page 指 buddy 的最小管理单位，大小等于内核 PAGE_SIZE；
 * 它不是 struct page，也不保证永远是 4 KiB，取决于内核页大小配置。
 *
 * 一个 block 由 2^order 个这样的 page 组成。只有 block 的第一页才是
 * 合法的分配/释放 head，其余页标记为 TAIL。这样如果误释放大块内部
 * 的某一页，可以尽早被检查出来。
 */
enum proxy_vmshm_block_state {
	PROXY_VMSHM_BLOCK_TAIL = 0,
	PROXY_VMSHM_BLOCK_FREE,
	PROXY_VMSHM_BLOCK_USED,
};

struct proxy_vmshm_buddy_page {
	struct list_head node;
	unsigned int order;
	enum proxy_vmshm_block_state state;
};

struct proxy_vmshm_buddy {
	/* page 0 相对 vmshm window 起点的偏移。 */
	u64 base_offset;
	unsigned long nr_pages;
	unsigned long free_pages;
	unsigned int max_order;
	struct list_head *free_area;
	struct proxy_vmshm_buddy_page *pages;
};

struct proxy_vmshm_slab;

enum proxy_vmshm_backing_kind {
	PROXY_VMSHM_BACKING_BUDDY = 0,
	PROXY_VMSHM_BACKING_SLAB,
	PROXY_VMSHM_BACKING_SG,
};

enum proxy_vmshm_slab_state {
	PROXY_VMSHM_SLAB_DETACHED = 0,
	PROXY_VMSHM_SLAB_PARTIAL,
	PROXY_VMSHM_SLAB_FULL,
};

struct proxy_vmshm_slab_cache {
	size_t object_size;
	unsigned int order;
	unsigned int objects_per_slab;
	struct list_head partial;
	struct list_head full;
};

struct proxy_vmshm_slab {
	struct list_head node;
	struct proxy_vmshm_slab_cache *cache;
	enum proxy_vmshm_slab_state state;
	u64 offset;
	void *kva;
	phys_addr_t gpa;
	size_t size;
	unsigned int order;
	unsigned int inuse;
	unsigned int total;
	unsigned long *bitmap;
};

struct proxy_vmshm_sg_segment {
	u64 logical_offset;
	u64 offset;
	void *kva;
	phys_addr_t gpa;
	size_t size;
	unsigned int order;
};

struct proxy_vmshm_sg_backing {
	unsigned int nr_segs;
	unsigned int nr_pages;
	size_t total_size;
	struct proxy_vmshm_sg_segment *segs;
};

struct proxy_vmshm_backing {
	enum proxy_vmshm_backing_kind kind;
	u64 offset;
	void *kva;
	phys_addr_t gpa;
	size_t alloc_size;
	unsigned int order;
	struct proxy_vmshm_slab *slab;
	unsigned int slab_index;
	struct proxy_vmshm_sg_backing sg;
};

enum proxy_vmshm_object_state {
	/* 对象仍在 mgr->objects 中可见，并且当前没有 active grant。 */
	PROXY_VMSHM_OBJECT_ALLOCATED = 0,
	/* 对象仍可见，且至少有一个 grant 指向它。 */
	PROXY_VMSHM_OBJECT_GRANTED,
	/* 对象正在从所有 lookup 路径中摘除。 */
	PROXY_VMSHM_OBJECT_REVOKING,
	/*
	 * 对象已经不在 object table 中，新的 lookup 找不到它；
	 * 但仍有人 pin 住它，所以 metadata 还不能释放。
	 */
	PROXY_VMSHM_OBJECT_ZOMBIE,
};

struct proxy_vmshm_object {
	/* id 可能被复用；generation 用来组成抗 ABA 的外部 handle。 */
	u32 id;
	u32 generation;
	u32 type;
	u64 offset;
	size_t size;
	size_t alloc_size;
	unsigned int order;
	u32 flags;
	u32 owner_vmid;
	u32 perms;
	u32 active_grants;
	enum proxy_vmshm_object_state state;
	/* 保护 object 元数据本身的生命周期。 */
	refcount_t refcnt;
	/* 保护 shared backing，防止仍在使用时被归还给 buddy。 */
	atomic_t pin_count;
	void *kva;
	phys_addr_t gpa;
	bool backing_released;
	enum proxy_vmshm_backing_kind backing_kind;
	struct proxy_vmshm_slab *slab;
	unsigned int slab_index;
	struct proxy_vmshm_sg_backing sg;
	struct list_head grants;
};

struct proxy_vmshm_grant {
	u32 id;
	u32 object_id;
	u32 target_vmid;
	u32 perms;
	u32 generation;
	struct proxy_vmshm_object *obj;
	struct list_head obj_node;
};

struct proxy_vmshm_manager {
	/* proxy VM 内核中映射到的 vmshm window。 */
	void *base;
	phys_addr_t gpa;
	size_t size;
	/* 可分配 payload 区间；alloc_start 之前的空间预留不用。 */
	u64 alloc_start;
	size_t alloc_size;

	/*
	 * 串行化 object/grant table、object 状态、refcnt/pin_count 以及
	 * buddy 元数据。后续如果锁竞争明显，可以再拆成更细粒度的锁。
	 */
	struct mutex lock;
	struct proxy_vmshm_buddy buddy;
	struct xarray objects;
	struct xarray grants;
	struct ida object_ids;
	struct ida grant_ids;
	struct proxy_vmshm_slab_cache slab_caches[PROXY_VMSHM_SLAB_NR_CLASSES];
	u32 next_object_generation;
	u32 next_grant_generation;
	bool ready;
};

static struct proxy_vmshm_manager proxy_vmshm_mgr;
static DEFINE_MUTEX(proxy_vmshm_init_lock);
static struct kmem_cache *proxy_vmshm_obj_cache;
static struct kmem_cache *proxy_vmshm_grant_cache;

static u64 proxy_vmshm_make_obj_handle(u32 id, u32 generation)
{
	return ((u64)generation << 32) | id;
}

static u32 proxy_vmshm_obj_index(u64 handle)
{
	return lower_32_bits(handle);
}

static u32 proxy_vmshm_obj_handle_generation(u64 handle)
{
	return upper_32_bits(handle);
}

static u64 proxy_vmshm_make_grant_id(u32 id, u32 generation)
{
	return ((u64)generation << 32) | id;
}

static u32 proxy_vmshm_grant_index(u64 grant_id)
{
	return lower_32_bits(grant_id);
}

static u32 proxy_vmshm_grant_generation(u64 grant_id)
{
	return upper_32_bits(grant_id);
}

static unsigned long proxy_vmshm_buddy_order_pages(unsigned int order)
{
	return 1UL << order;
}

static void proxy_vmshm_buddy_mark_block(struct proxy_vmshm_buddy *buddy,
					 unsigned long page_idx,
					 unsigned int order,
					 enum proxy_vmshm_block_state state)
{
	unsigned long nr_pages = proxy_vmshm_buddy_order_pages(order);
	unsigned long i;

	/*
	 * head 记录 alloc/free 真正依赖的 block 状态。TAIL 页只保留
	 * 足够的调试信息，用来识别错误释放。
	 */
	buddy->pages[page_idx].order = order;
	buddy->pages[page_idx].state = state;

	for (i = 1; i < nr_pages; i++) {
		buddy->pages[page_idx + i].order = order;
		buddy->pages[page_idx + i].state = PROXY_VMSHM_BLOCK_TAIL;
	}
}

static void proxy_vmshm_buddy_insert(struct proxy_vmshm_buddy *buddy,
				     unsigned long page_idx,
				     unsigned int order)
{
	struct proxy_vmshm_buddy_page *page = &buddy->pages[page_idx];

	/* 把一个 free block head 插入对应 order 的 free list。 */
	proxy_vmshm_buddy_mark_block(buddy, page_idx, order,
				     PROXY_VMSHM_BLOCK_FREE);
	INIT_LIST_HEAD(&page->node);
	list_add(&page->node, &buddy->free_area[order]);
	buddy->free_pages += proxy_vmshm_buddy_order_pages(order);
}

static void proxy_vmshm_buddy_remove(struct proxy_vmshm_buddy *buddy,
				     unsigned long page_idx)
{
	struct proxy_vmshm_buddy_page *page = &buddy->pages[page_idx];

	/* 只有 free block head 才能出现在 free list 中。 */
	if (WARN_ON(page->state != PROXY_VMSHM_BLOCK_FREE))
		return;

	list_del_init(&page->node);
	page->state = PROXY_VMSHM_BLOCK_USED;
	buddy->free_pages -= proxy_vmshm_buddy_order_pages(page->order);
}

static int proxy_vmshm_buddy_add_initial_block(struct proxy_vmshm_buddy *buddy,
					       unsigned long page_idx,
					       unsigned int order)
{
	if (WARN_ON(page_idx >= buddy->nr_pages ||
		    page_idx + proxy_vmshm_buddy_order_pages(order) >
			    buddy->nr_pages))
		return -EINVAL;

	proxy_vmshm_buddy_insert(buddy, page_idx, order);
	return 0;
}

static int proxy_vmshm_buddy_init(struct proxy_vmshm_buddy *buddy,
				  u64 base_offset, size_t size)
{
	unsigned long page_idx = 0, remaining, nr_pages;
	unsigned int i;
	int ret;

	memset(buddy, 0, sizeof(*buddy));

	nr_pages = size >> PAGE_SHIFT;
	if (!nr_pages)
		return -EINVAL;

	buddy->base_offset = base_offset;
	buddy->nr_pages = nr_pages;
	buddy->max_order = ilog2(nr_pages);

	buddy->free_area = kcalloc(buddy->max_order + 1,
				   sizeof(*buddy->free_area), GFP_KERNEL);
	if (!buddy->free_area)
		return -ENOMEM;

	buddy->pages = kcalloc(nr_pages, sizeof(*buddy->pages), GFP_KERNEL);
	if (!buddy->pages) {
		ret = -ENOMEM;
		goto err_free_area;
	}

	for (i = 0; i <= buddy->max_order; i++)
		INIT_LIST_HEAD(&buddy->free_area[i]);

	for (page_idx = 0; page_idx < nr_pages; page_idx++)
		INIT_LIST_HEAD(&buddy->pages[page_idx].node);
	for (page_idx = 0; page_idx < nr_pages; page_idx++)
		buddy->pages[page_idx].state = PROXY_VMSHM_BLOCK_TAIL;

	/*
	 * 可分配区间不一定是 2 的幂。这里把它拆成若干个“最大且对齐”的
	 * 2^order block，作为 private buddy 的初始 free blocks。
	 */
	page_idx = 0;
	remaining = nr_pages;
	while (remaining) {
		unsigned int align_order, size_order, order;

		if (page_idx)
			align_order = __ffs(page_idx);
		else
			align_order = buddy->max_order;

		size_order = ilog2(remaining);
		order = min3(align_order, size_order, buddy->max_order);

		ret = proxy_vmshm_buddy_add_initial_block(buddy, page_idx, order);
		if (ret)
			goto err_free_pages;

		page_idx += proxy_vmshm_buddy_order_pages(order);
		remaining -= proxy_vmshm_buddy_order_pages(order);
	}

	return 0;

err_free_pages:
	kfree(buddy->pages);
	buddy->pages = NULL;
err_free_area:
	kfree(buddy->free_area);
	buddy->free_area = NULL;
	return ret;
}

static void proxy_vmshm_buddy_destroy(struct proxy_vmshm_buddy *buddy)
{
	kfree(buddy->pages);
	kfree(buddy->free_area);
	memset(buddy, 0, sizeof(*buddy));
}

static int proxy_vmshm_buddy_alloc(struct proxy_vmshm_buddy *buddy,
				   unsigned int order, u64 *offset)
{
	struct proxy_vmshm_buddy_page *page;
	unsigned long page_idx;
	unsigned int cur_order;

	if (order > buddy->max_order)
		return -ENOMEM;

	for (cur_order = order; cur_order <= buddy->max_order; cur_order++) {
		if (list_empty(&buddy->free_area[cur_order]))
			continue;

		page = list_first_entry(&buddy->free_area[cur_order],
					struct proxy_vmshm_buddy_page, node);
		page_idx = page - buddy->pages;
		proxy_vmshm_buddy_remove(buddy, page_idx);

		/* 保留左半块继续拆分，把每次拆出的右半块放回低一级 free list。 */
		while (cur_order > order) {
			unsigned long buddy_idx;

			cur_order--;
			buddy_idx = page_idx + proxy_vmshm_buddy_order_pages(cur_order);
			proxy_vmshm_buddy_insert(buddy, buddy_idx, cur_order);
		}

		page = &buddy->pages[page_idx];
		proxy_vmshm_buddy_mark_block(buddy, page_idx, order,
					     PROXY_VMSHM_BLOCK_USED);
		INIT_LIST_HEAD(&page->node);
		*offset = buddy->base_offset + (page_idx << PAGE_SHIFT);
		return 0;
	}

	return -ENOMEM;
}

static void proxy_vmshm_buddy_free(struct proxy_vmshm_buddy *buddy,
				   u64 offset, unsigned int order)
{
	struct proxy_vmshm_buddy_page *page;
	unsigned long page_idx;

	if (WARN_ON(offset < buddy->base_offset))
		return;

	page_idx = (offset - buddy->base_offset) >> PAGE_SHIFT;
	if (WARN_ON(page_idx >= buddy->nr_pages || order > buddy->max_order))
		return;

	page = &buddy->pages[page_idx];
	if (WARN_ON(page->state != PROXY_VMSHM_BLOCK_USED))
		return;

	if (WARN_ON(page->order != order))
		return;

	/*
	 * 只和“同 order 且空闲”的 buddy 合并。由于 free block 必须有合法
	 * head 状态，这里也会自然拒绝对 TAIL 页的错误释放。
	 */
	while (order < buddy->max_order) {
		struct proxy_vmshm_buddy_page *buddy_page;
		unsigned long buddy_idx;

		buddy_idx = page_idx ^ proxy_vmshm_buddy_order_pages(order);
		if (buddy_idx >= buddy->nr_pages)
			break;

		buddy_page = &buddy->pages[buddy_idx];
		if (buddy_page->state != PROXY_VMSHM_BLOCK_FREE ||
		    buddy_page->order != order)
			break;

		proxy_vmshm_buddy_remove(buddy, buddy_idx);
		page_idx = min(page_idx, buddy_idx);
		order++;
	}

	proxy_vmshm_buddy_insert(buddy, page_idx, order);
}

static int proxy_vmshm_size_align_to_order(size_t size, size_t align,
					   unsigned int *order,
					   size_t *alloc_size);

static void proxy_vmshm_slab_init(struct proxy_vmshm_manager *mgr)
{
	unsigned int i;

	for (i = 0; i < PROXY_VMSHM_SLAB_NR_CLASSES; i++) {
		struct proxy_vmshm_slab_cache *cache = &mgr->slab_caches[i];

		cache->object_size = proxy_vmshm_slab_sizes[i];
		cache->order = 0;
		cache->objects_per_slab = PAGE_SIZE / cache->object_size;
		INIT_LIST_HEAD(&cache->partial);
		INIT_LIST_HEAD(&cache->full);
	}
}

static void proxy_vmshm_slab_detach_locked(struct proxy_vmshm_slab *slab)
{
	if (slab->state == PROXY_VMSHM_SLAB_DETACHED)
		return;

	list_del_init(&slab->node);
	slab->state = PROXY_VMSHM_SLAB_DETACHED;
}

static void proxy_vmshm_slab_move_locked(struct proxy_vmshm_slab *slab,
					 enum proxy_vmshm_slab_state state)
{
	struct proxy_vmshm_slab_cache *cache = slab->cache;

	if (slab->state == state)
		return;

	proxy_vmshm_slab_detach_locked(slab);

	switch (state) {
	case PROXY_VMSHM_SLAB_PARTIAL:
		list_add(&slab->node, &cache->partial);
		break;
	case PROXY_VMSHM_SLAB_FULL:
		list_add(&slab->node, &cache->full);
		break;
	case PROXY_VMSHM_SLAB_DETACHED:
		break;
	}

	slab->state = state;
}

static void proxy_vmshm_slab_destroy_locked(struct proxy_vmshm_manager *mgr,
					    struct proxy_vmshm_slab *slab)
{
	proxy_vmshm_slab_detach_locked(slab);
	proxy_vmshm_buddy_free(&mgr->buddy, slab->offset, slab->order);
	bitmap_free(slab->bitmap);
	kfree(slab);
}

static int proxy_vmshm_slab_select_cache(struct proxy_vmshm_manager *mgr,
					 size_t size, size_t align,
					 struct proxy_vmshm_slab_cache **out)
{
	size_t need;
	unsigned int i;

	if (!size)
		return -EINVAL;

	if (size > PROXY_VMSHM_SLAB_MAX_SIZE)
		return -ENOENT;

	if (!align)
		align = 1;
	if (!is_power_of_2(align))
		return -EINVAL;
	if (align > PROXY_VMSHM_SLAB_MAX_SIZE)
		return -ENOENT;

	need = max(size, align);
	for (i = 0; i < PROXY_VMSHM_SLAB_NR_CLASSES; i++) {
		if (mgr->slab_caches[i].object_size >= need) {
			*out = &mgr->slab_caches[i];
			return 0;
		}
	}

	return -ENOENT;
}

static struct proxy_vmshm_slab *
proxy_vmshm_slab_create_locked(struct proxy_vmshm_manager *mgr,
			       struct proxy_vmshm_slab_cache *cache,
			       gfp_t gfp)
{
	struct proxy_vmshm_slab *slab;
	u64 offset;
	int ret;

	slab = kzalloc(sizeof(*slab), gfp);
	if (!slab)
		return ERR_PTR(-ENOMEM);

	slab->bitmap = bitmap_zalloc(cache->objects_per_slab, gfp);
	if (!slab->bitmap) {
		kfree(slab);
		return ERR_PTR(-ENOMEM);
	}

	ret = proxy_vmshm_buddy_alloc(&mgr->buddy, cache->order, &offset);
	if (ret) {
		bitmap_free(slab->bitmap);
		kfree(slab);
		return ERR_PTR(ret);
	}

	INIT_LIST_HEAD(&slab->node);
	slab->cache = cache;
	slab->state = PROXY_VMSHM_SLAB_DETACHED;
	slab->offset = offset;
	slab->kva = (void *)((u8 *)mgr->base + offset);
	slab->gpa = mgr->gpa + offset;
	slab->size = (size_t)PAGE_SIZE << cache->order;
	slab->order = cache->order;
	slab->total = cache->objects_per_slab;

	return slab;
}

static int proxy_vmshm_slab_alloc_locked(struct proxy_vmshm_manager *mgr,
					 size_t size, size_t align,
					 struct proxy_vmshm_backing *backing,
					 gfp_t gfp)
{
	struct proxy_vmshm_slab_cache *cache;
	struct proxy_vmshm_slab *slab;
	unsigned int index;
	int ret;

	ret = proxy_vmshm_slab_select_cache(mgr, size, align, &cache);
	if (ret)
		return ret;

	if (!list_empty(&cache->partial)) {
		slab = list_first_entry(&cache->partial,
					struct proxy_vmshm_slab, node);
	} else {
		slab = proxy_vmshm_slab_create_locked(mgr, cache, gfp);
		if (IS_ERR(slab))
			return PTR_ERR(slab);
	}

	index = find_first_zero_bit(slab->bitmap, slab->total);
	if (WARN_ON(index >= slab->total))
		return -ENOSPC;

	__set_bit(index, slab->bitmap);
	slab->inuse++;

	if (slab->inuse == slab->total)
		proxy_vmshm_slab_move_locked(slab, PROXY_VMSHM_SLAB_FULL);
	else
		proxy_vmshm_slab_move_locked(slab, PROXY_VMSHM_SLAB_PARTIAL);

	backing->kind = PROXY_VMSHM_BACKING_SLAB;
	backing->offset = slab->offset + (index * cache->object_size);
	backing->kva = (void *)((u8 *)slab->kva + (index * cache->object_size));
	backing->gpa = slab->gpa + (index * cache->object_size);
	backing->alloc_size = cache->object_size;
	backing->order = 0;
	backing->slab = slab;
	backing->slab_index = index;
	return 0;
}

static void proxy_vmshm_slab_free_locked(struct proxy_vmshm_manager *mgr,
					 struct proxy_vmshm_slab *slab,
					 unsigned int index)
{
	if (WARN_ON(!slab || index >= slab->total))
		return;

	if (WARN_ON(!test_bit(index, slab->bitmap)))
		return;

	__clear_bit(index, slab->bitmap);
	if (WARN_ON(!slab->inuse))
		return;
	slab->inuse--;

	if (!slab->inuse) {
		proxy_vmshm_slab_destroy_locked(mgr, slab);
		return;
	}

	if (slab->inuse == slab->total)
		proxy_vmshm_slab_move_locked(slab, PROXY_VMSHM_SLAB_FULL);
	else
		proxy_vmshm_slab_move_locked(slab, PROXY_VMSHM_SLAB_PARTIAL);
}

static void proxy_vmshm_slab_destroy_all_locked(struct proxy_vmshm_manager *mgr)
{
	unsigned int i;

	for (i = 0; i < PROXY_VMSHM_SLAB_NR_CLASSES; i++) {
		struct proxy_vmshm_slab_cache *cache = &mgr->slab_caches[i];
		struct proxy_vmshm_slab *slab, *tmp;

		list_for_each_entry_safe(slab, tmp, &cache->partial, node) {
			WARN_ON(slab->inuse);
			proxy_vmshm_slab_destroy_locked(mgr, slab);
		}

		list_for_each_entry_safe(slab, tmp, &cache->full, node) {
			WARN_ON(slab->inuse);
			proxy_vmshm_slab_destroy_locked(mgr, slab);
		}
	}
}

static void proxy_vmshm_sg_free_locked(struct proxy_vmshm_manager *mgr,
				       struct proxy_vmshm_sg_backing *sg)
{
	unsigned int i;

	if (!sg || !sg->segs)
		return;

	for (i = 0; i < sg->nr_segs; i++) {
		struct proxy_vmshm_sg_segment *seg = &sg->segs[i];

		if (!seg->size)
			continue;

		proxy_vmshm_buddy_free(&mgr->buddy, seg->offset, seg->order);
	}

	kfree(sg->segs);
	memset(sg, 0, sizeof(*sg));
}

static void proxy_vmshm_sg_zero(struct proxy_vmshm_sg_backing *sg)
{
	unsigned int i;

	if (!sg || !sg->segs)
		return;

	for (i = 0; i < sg->nr_segs; i++) {
		struct proxy_vmshm_sg_segment *seg = &sg->segs[i];

		if (seg->kva && seg->size)
			memzero_explicit(seg->kva, seg->size);
	}
}

static int proxy_vmshm_sg_alloc_backing_locked(struct proxy_vmshm_manager *mgr,
					       size_t size, size_t align,
					       struct proxy_vmshm_backing *backing,
					       gfp_t gfp)
{
	struct proxy_vmshm_sg_backing sg = {};
	size_t needed, remaining, logical_off;
	unsigned int max_segs;
	int ret = 0;

	if (!size)
		return -EINVAL;

	if (align > PAGE_SIZE)
		return -EINVAL;

	if (size > SIZE_MAX - (PAGE_SIZE - 1))
		return -EOVERFLOW;

	needed = PAGE_ALIGN(size);
	if (!needed)
		return -EOVERFLOW;

	if ((needed >> PAGE_SHIFT) > mgr->buddy.free_pages)
		return -ENOMEM;

	max_segs = min_t(unsigned int, PROXY_VMSHM_SG_MAX_SEGS,
			 needed >> PAGE_SHIFT);
	if (!max_segs)
		return -EOVERFLOW;

	sg.segs = kcalloc(max_segs, sizeof(*sg.segs), gfp);
	if (!sg.segs)
		return -ENOMEM;

	remaining = needed;
	logical_off = 0;
	while (remaining) {
		size_t pages = remaining >> PAGE_SHIFT;
		unsigned int start_order;
		unsigned int order;
		u64 offset = 0;
		bool found = false;

		if (sg.nr_segs >= max_segs) {
			ret = -E2BIG;
			goto err_rollback;
		}

		start_order = min_t(unsigned int, ilog2(pages),
				    mgr->buddy.max_order);
		for (order = start_order; ; order--) {
			ret = proxy_vmshm_buddy_alloc(&mgr->buddy, order,
						      &offset);
			if (!ret) {
				size_t seg_size = (size_t)PAGE_SIZE << order;
				struct proxy_vmshm_sg_segment *seg;

				seg = &sg.segs[sg.nr_segs++];
				seg->logical_offset = logical_off;
				seg->offset = offset;
				seg->kva = (void *)((u8 *)mgr->base + offset);
				seg->gpa = mgr->gpa + offset;
				seg->size = seg_size;
				seg->order = order;

				sg.total_size += seg_size;
				sg.nr_pages += seg_size >> PAGE_SHIFT;
				remaining -= seg_size;
				logical_off += seg_size;
				found = true;
				break;
			}

			if (!order)
				break;
		}

		if (!found) {
			ret = -ENOMEM;
			goto err_rollback;
		}
	}

	backing->kind = PROXY_VMSHM_BACKING_SG;
	backing->alloc_size = needed;
	backing->order = 0;
	backing->sg = sg;
	if (sg.nr_segs) {
		backing->offset = sg.segs[0].offset;
		backing->kva = sg.segs[0].kva;
		backing->gpa = sg.segs[0].gpa;
	}

	return 0;

err_rollback:
	proxy_vmshm_sg_free_locked(mgr, &sg);
	return ret;
}

static int proxy_vmshm_buddy_alloc_backing_locked(struct proxy_vmshm_manager *mgr,
						  size_t size, size_t align,
						  struct proxy_vmshm_backing *backing)
{
	size_t alloc_size;
	unsigned int order;
	u64 offset;
	int ret;

	ret = proxy_vmshm_size_align_to_order(size, align, &order, &alloc_size);
	if (ret)
		return ret;

	ret = proxy_vmshm_buddy_alloc(&mgr->buddy, order, &offset);
	if (ret)
		return ret;

	backing->kind = PROXY_VMSHM_BACKING_BUDDY;
	backing->offset = offset;
	backing->kva = (void *)((u8 *)mgr->base + offset);
	backing->gpa = mgr->gpa + offset;
	backing->alloc_size = alloc_size;
	backing->order = order;
	return 0;
}

static bool proxy_vmshm_params_allow_sg(const struct proxy_vmshm_alloc_params *params)
{
	if (!(params->flags & PROXY_VMSHM_F_ALLOW_SG))
		return false;

	if (params->flags & PROXY_VMSHM_F_CONTIG)
		return false;

	if (params->size <= PROXY_VMSHM_SLAB_MAX_SIZE)
		return false;

	if (params->align > PAGE_SIZE)
		return false;

	if (params->perms & PROXY_VMSHM_PERM_MMAP)
		return false;

	return true;
}

static int proxy_vmshm_alloc_backing_locked(struct proxy_vmshm_manager *mgr,
					    const struct proxy_vmshm_alloc_params *params,
					    struct proxy_vmshm_backing *backing,
					    gfp_t gfp)
{
	int buddy_ret, ret;

	memset(backing, 0, sizeof(*backing));

	ret = proxy_vmshm_slab_alloc_locked(mgr, params->size, params->align,
					    backing, gfp);
	if (ret != -ENOENT)
		return ret;

	buddy_ret = proxy_vmshm_buddy_alloc_backing_locked(mgr, params->size,
							   params->align,
							   backing);
	if (!buddy_ret)
		return 0;

	if (buddy_ret == -ENOMEM && proxy_vmshm_params_allow_sg(params))
		return proxy_vmshm_sg_alloc_backing_locked(mgr, params->size,
							   params->align,
							   backing, gfp);

	return buddy_ret;
}

static void proxy_vmshm_grant_destroy_locked(struct proxy_vmshm_grant *grant)
{
	struct proxy_vmshm_manager *mgr = &proxy_vmshm_mgr;
	struct proxy_vmshm_object *obj = grant->obj;

	/* grant 本质是 lookup capability；删除 table entry 即完成撤销。 */
	xa_erase(&mgr->grants, grant->id);
	ida_free(&mgr->grant_ids, grant->id);
	list_del_init(&grant->obj_node);

	if (obj->active_grants)
		obj->active_grants--;
	if (!obj->active_grants && obj->state == PROXY_VMSHM_OBJECT_GRANTED)
		obj->state = PROXY_VMSHM_OBJECT_ALLOCATED;

	kmem_cache_free(proxy_vmshm_grant_cache, grant);
}

static bool proxy_vmshm_object_state_usable(struct proxy_vmshm_object *obj)
{
	return obj->state == PROXY_VMSHM_OBJECT_ALLOCATED ||
	       obj->state == PROXY_VMSHM_OBJECT_GRANTED;
}

static bool proxy_vmshm_object_backing_releasable_locked(struct proxy_vmshm_object *obj,
							 bool force)
{
	/* 只有最后一个 in-flight pin 释放后，backing 才能归还给 buddy。 */
	return !obj->backing_released &&
	       (force || !atomic_read(&obj->pin_count));
}

static bool proxy_vmshm_object_zero_lockless(struct proxy_vmshm_object *obj)
{
	if (obj->backing_kind == PROXY_VMSHM_BACKING_SLAB)
		return false;

	return obj->alloc_size > PROXY_VMSHM_ZERO_INLINE_MAX;
}

static void proxy_vmshm_object_zero_backing(struct proxy_vmshm_object *obj)
{
	switch (obj->backing_kind) {
	case PROXY_VMSHM_BACKING_BUDDY:
	case PROXY_VMSHM_BACKING_SLAB:
		if (obj->kva && obj->alloc_size)
			memzero_explicit(obj->kva, obj->alloc_size);
		break;
	case PROXY_VMSHM_BACKING_SG:
		proxy_vmshm_sg_zero(&obj->sg);
		break;
	default:
		WARN_ON_ONCE(1);
		break;
	}
}

static void proxy_vmshm_object_release_backing_locked(struct proxy_vmshm_object *obj,
						      bool force,
						      bool already_zeroed)
{
	struct proxy_vmshm_manager *mgr = &proxy_vmshm_mgr;

	if (!proxy_vmshm_object_backing_releasable_locked(obj, force))
		return;

	/* 复用 shared block 前必须清零，避免其他 VM 看到旧对象残留数据。 */
	if (!already_zeroed)
		proxy_vmshm_object_zero_backing(obj);

	switch (obj->backing_kind) {
	case PROXY_VMSHM_BACKING_SLAB:
		proxy_vmshm_slab_free_locked(mgr, obj->slab, obj->slab_index);
		break;
	case PROXY_VMSHM_BACKING_BUDDY:
		proxy_vmshm_buddy_free(&mgr->buddy, obj->offset, obj->order);
		break;
	case PROXY_VMSHM_BACKING_SG:
		proxy_vmshm_sg_free_locked(mgr, &obj->sg);
		break;
	default:
		WARN_ON_ONCE(1);
		break;
	}
	obj->backing_released = true;
}

static void proxy_vmshm_object_put_locked(struct proxy_vmshm_object *obj)
{
	if (!refcount_dec_and_test(&obj->refcnt))
		return;

	WARN_ON(!obj->backing_released);
	kmem_cache_free(proxy_vmshm_obj_cache, obj);
}

static bool proxy_vmshm_object_get_locked(struct proxy_vmshm_object *obj)
{
	return refcount_inc_not_zero(&obj->refcnt);
}

static int proxy_vmshm_object_pin_locked(struct proxy_vmshm_object *obj)
{
	if (!proxy_vmshm_object_state_usable(obj) || obj->backing_released)
		return -ENOENT;

	/*
	 * pin 比普通 metadata ref 更强：除了保护 object 结构体，还阻止
	 * backing 被回收，因为 GPU/backend/transfer 路径可能仍在访问 payload。
	 */
	if (!proxy_vmshm_object_get_locked(obj))
		return -ENOENT;

	atomic_inc(&obj->pin_count);
	return 0;
}

static bool proxy_vmshm_object_begin_free_locked(struct proxy_vmshm_object *obj,
						bool force)
{
	struct proxy_vmshm_manager *mgr = &proxy_vmshm_mgr;
	struct proxy_vmshm_grant *grant, *tmp;

	if (obj->state == PROXY_VMSHM_OBJECT_ZOMBIE)
		return false;

	/*
	 * 先摘除所有公开 lookup 路径。已有 pin 可以继续使用对象，但从这一刻
	 * 开始，新的 object/grant lookup 都找不到它。
	 */
	obj->state = PROXY_VMSHM_OBJECT_REVOKING;
	list_for_each_entry_safe(grant, tmp, &obj->grants, obj_node)
		proxy_vmshm_grant_destroy_locked(grant);

	xa_erase(&mgr->objects, obj->id);
	ida_free(&mgr->object_ids, obj->id);
	obj->state = PROXY_VMSHM_OBJECT_ZOMBIE;

	if (force)
		WARN_ON(atomic_read(&obj->pin_count));

	return true;
}

static void proxy_vmshm_object_finish_free_locked(struct proxy_vmshm_object *obj,
						  bool force,
						  bool already_zeroed)
{
	/* backing 是否可回收判断完成后，再释放 manager 持有的 object 引用。 */
	proxy_vmshm_object_release_backing_locked(obj, force, already_zeroed);
	proxy_vmshm_object_put_locked(obj);
}

/*
 * 调用者必须持有 proxy_vmshm_init_lock 和 mgr->lock。对于大对象，
 * 这里可能临时释放 mgr->lock 做清零，之后再重新加锁返回。
 */
static void proxy_vmshm_object_release_and_put_maybe_unlock(struct proxy_vmshm_object *obj,
							    bool force)
{
	struct proxy_vmshm_manager *mgr = &proxy_vmshm_mgr;
	bool zero_lockless;

	zero_lockless = !force &&
			proxy_vmshm_object_backing_releasable_locked(obj, false) &&
			proxy_vmshm_object_zero_lockless(obj);
	if (!zero_lockless) {
		proxy_vmshm_object_finish_free_locked(obj, force, false);
		return;
	}

	/*
	 * 大 BO 清零可能比较耗时。此时对象已经不可 lookup，所以可以在
	 * mgr->lock 外清零，然后短暂重新加锁，把 block 归还给 buddy。
	 */
	mutex_unlock(&mgr->lock);
	proxy_vmshm_object_zero_backing(obj);
	mutex_lock(&mgr->lock);

	proxy_vmshm_object_finish_free_locked(obj, false, true);
}

static int proxy_vmshm_size_align_to_order(size_t size, size_t align,
					   unsigned int *order,
					   size_t *alloc_size)
{
	size_t align_pages, pages;
	unsigned int align_order, size_order;

	if (!size)
		return -EINVAL;

	if (!align)
		align = PAGE_SIZE;
	align = max_t(size_t, align, PAGE_SIZE);
	if (!is_power_of_2(align) || align > PROXY_VMSHM_MAX_ALIGN)
		return -EINVAL;

	/*
	 * buddy 返回的都是 2^order block。通过提高 order 来满足对齐要求
	 * 简单可靠，前提是 manager 的 base offset 也按最大对齐粒度对齐。
	 */
	pages = DIV_ROUND_UP(size, PAGE_SIZE);
	if (pages > (SIZE_MAX >> PAGE_SHIFT))
		return -EOVERFLOW;

	align_pages = align >> PAGE_SHIFT;
	size_order = order_base_2(pages);
	align_order = order_base_2(align_pages);

	*order = max_t(unsigned int, PROXY_VMSHM_MIN_ALLOC_ORDER,
		       max(size_order, align_order));
	if (*order >= BITS_PER_LONG - PAGE_SHIFT)
		return -EOVERFLOW;

	*alloc_size = (size_t)PAGE_SIZE << *order;
	return 0;
}

static int proxy_vmshm_cache_init(void)
{
	if (proxy_vmshm_obj_cache && proxy_vmshm_grant_cache)
		return 0;

	/* metadata 留在 proxy VM 私有内存；payload 才放在 vmshm 中。 */
	proxy_vmshm_obj_cache = KMEM_CACHE(proxy_vmshm_object,
					   SLAB_HWCACHE_ALIGN);
	if (!proxy_vmshm_obj_cache)
		return -ENOMEM;

	proxy_vmshm_grant_cache = KMEM_CACHE(proxy_vmshm_grant,
					     SLAB_HWCACHE_ALIGN);
	if (!proxy_vmshm_grant_cache) {
		kmem_cache_destroy(proxy_vmshm_obj_cache);
		proxy_vmshm_obj_cache = NULL;
		return -ENOMEM;
	}

	return 0;
}

static void proxy_vmshm_cache_destroy(void)
{
	kmem_cache_destroy(proxy_vmshm_grant_cache);
	proxy_vmshm_grant_cache = NULL;
	kmem_cache_destroy(proxy_vmshm_obj_cache);
	proxy_vmshm_obj_cache = NULL;
}

int proxy_vmshm_manager_init(void *base, phys_addr_t gpa, size_t size)
{
	struct proxy_vmshm_manager *mgr = &proxy_vmshm_mgr;
	size_t original_size = size;
	size_t reserve;
	size_t buddy_size;
	int ret;

	mutex_lock(&proxy_vmshm_init_lock);

	if (mgr->ready) {
		ret = -EBUSY;
		goto out_unlock;
	}

	if (!base || !IS_ALIGNED((unsigned long)base, PAGE_SIZE) ||
	    !IS_ALIGNED((u64)gpa, PAGE_SIZE)) {
		ret = -EINVAL;
		goto out_unlock;
	}

	size &= PAGE_MASK;
	if (size < (2 * PAGE_SIZE)) {
		ret = -EINVAL;
		goto out_unlock;
	}

	ret = proxy_vmshm_cache_init();
	if (ret)
		goto out_unlock;

	mutex_init(&mgr->lock);
	xa_init(&mgr->objects);
	xa_init(&mgr->grants);
	ida_init(&mgr->object_ids);
	ida_init(&mgr->grant_ids);
	proxy_vmshm_slab_init(mgr);

	reserve = min_t(size_t, PROXY_VMSHM_RESERVE_SIZE, size / 16);
	reserve = max_t(size_t, reserve, PAGE_SIZE);
	reserve = PAGE_ALIGN(reserve);
	/* 保证所有基于 order 的对齐都相对 vmshm window 成立。 */
	reserve = ALIGN(reserve, PROXY_VMSHM_MAX_ALIGN);
	if (reserve >= size) {
		ret = -EINVAL;
		goto err_destroy_ids;
	}

	buddy_size = (size - reserve) & PAGE_MASK;
	if (!buddy_size) {
		ret = -EINVAL;
		goto err_destroy_ids;
	}

	ret = proxy_vmshm_buddy_init(&mgr->buddy, reserve, buddy_size);
	if (ret)
		goto err_destroy_ids;

	mgr->base = base;
	mgr->gpa = gpa;
	mgr->size = size;
	/* buddy 只管理 [alloc_start, alloc_start + alloc_size) 这段区间。 */
	mgr->alloc_start = reserve;
	mgr->alloc_size = buddy_size;
	mgr->next_object_generation = 1;
	mgr->next_grant_generation = 1;
	mgr->ready = true;

	if (IS_ENABLED(CONFIG_PROXY_VMSHM_COMM)) {
		ret = proxy_comm_vmshm_register_handler(
			VMSHM_MANAGER_MSG_GET_OBJECT_REQ,
			proxy_vmshm_manager_rx_handler, NULL);
		if (ret)
			goto err_destroy_buddy;
	}

	pr_info("proxy_manager_vmshm: manager ready base=%pa size=0x%zx alloc=[0x%llx,0x%zx] max_order=%u\n",
		&gpa, size, mgr->alloc_start, mgr->alloc_size,
		mgr->buddy.max_order);
	if (original_size != size)
		pr_info("proxy_manager_vmshm: truncated non-page-aligned size from 0x%zx to 0x%zx\n",
			original_size, size);

	mutex_unlock(&proxy_vmshm_init_lock);
	return 0;

err_destroy_buddy:
	mgr->ready = false;
	proxy_vmshm_buddy_destroy(&mgr->buddy);
err_destroy_ids:
	ida_destroy(&mgr->grant_ids);
	ida_destroy(&mgr->object_ids);
	xa_destroy(&mgr->grants);
	xa_destroy(&mgr->objects);
	proxy_vmshm_cache_destroy();
out_unlock:
	mutex_unlock(&proxy_vmshm_init_lock);
	return ret;
}

void proxy_vmshm_manager_destroy(void)
{
	struct proxy_vmshm_manager *mgr = &proxy_vmshm_mgr;
	struct proxy_vmshm_object *obj;
	struct proxy_vmshm_grant *grant;
	unsigned long index;

	/*
	 * destroy 只能在上层停止发起新请求，并释放所有 object pin 后调用。
	 * force 路径会对未释放的 pin 打 WARN，但仍会继续拆掉设备私有状态。
	 */
	mutex_lock(&proxy_vmshm_init_lock);
	if (!mgr->ready) {
		mutex_unlock(&proxy_vmshm_init_lock);
		return;
	}

	if (IS_ENABLED(CONFIG_PROXY_VMSHM_COMM))
		proxy_comm_vmshm_unregister_handler(
			VMSHM_MANAGER_MSG_GET_OBJECT_REQ,
			proxy_vmshm_manager_rx_handler, NULL);

	mutex_lock(&mgr->lock);
	mgr->ready = false;
	while (true) {
		index = 0;
		grant = xa_find(&mgr->grants, &index, ULONG_MAX, XA_PRESENT);
		if (!grant)
			break;
		proxy_vmshm_grant_destroy_locked(grant);
	}

	while (true) {
		index = 0;
		obj = xa_find(&mgr->objects, &index, ULONG_MAX, XA_PRESENT);
		if (!obj)
			break;
		if (proxy_vmshm_object_begin_free_locked(obj, true))
			proxy_vmshm_object_release_and_put_maybe_unlock(obj, true);
	}

	proxy_vmshm_slab_destroy_all_locked(mgr);
	mutex_unlock(&mgr->lock);

	proxy_vmshm_buddy_destroy(&mgr->buddy);
	ida_destroy(&mgr->grant_ids);
	ida_destroy(&mgr->object_ids);
	xa_destroy(&mgr->grants);
	xa_destroy(&mgr->objects);
	proxy_vmshm_cache_destroy();
	mutex_unlock(&proxy_vmshm_init_lock);
}

struct proxy_vmshm_object *
proxy_vmshm_alloc_ext(const struct proxy_vmshm_alloc_params *params,
		      gfp_t gfp)
{
	struct proxy_vmshm_manager *mgr = &proxy_vmshm_mgr;
	struct proxy_vmshm_object *obj;
	struct proxy_vmshm_backing backing;
	u32 generation;
	int id, ret;

	might_sleep();

	if (!params)
		return ERR_PTR(-EINVAL);

	/*
	 * init_lock 用来把普通用户路径和 manager teardown 串行化，包括 slab
	 * cache 销毁；mgr->lock 则保护 allocator 和各类表。
	 */
	mutex_lock(&proxy_vmshm_init_lock);
	if (!proxy_vmshm_obj_cache) {
		ret = -ENODEV;
		goto err_init_unlock;
	}

	obj = kmem_cache_zalloc(proxy_vmshm_obj_cache, gfp);
	if (!obj) {
		ret = -ENOMEM;
		goto err_init_unlock;
	}

	mutex_lock(&mgr->lock);
	if (!mgr->ready) {
		ret = -ENODEV;
		goto err_unlock;
	}

	id = ida_alloc_range(&mgr->object_ids, 1, INT_MAX, gfp);
	if (id < 0) {
		ret = id;
		goto err_unlock;
	}

	ret = proxy_vmshm_alloc_backing_locked(mgr, params, &backing, gfp);
	if (ret)
		goto err_free_id;

	generation = mgr->next_object_generation++;
	if (!generation)
		generation = mgr->next_object_generation++;

	/* id 是 xarray 索引；generation 让导出的 handle 具备 ABA 防护。 */
	obj->id = id;
	obj->generation = generation;
	obj->type = params->type;
	obj->offset = backing.offset;
	obj->size = params->size;
	obj->alloc_size = backing.alloc_size;
	obj->order = backing.order;
	obj->flags = params->flags;
	obj->owner_vmid = params->owner_vmid;
	obj->perms = params->perms;
	obj->state = PROXY_VMSHM_OBJECT_ALLOCATED;
	refcount_set(&obj->refcnt, 1);
	atomic_set(&obj->pin_count, 0);
	obj->kva = backing.kva;
	obj->gpa = backing.gpa;
	obj->backing_kind = backing.kind;
	obj->slab = backing.slab;
	obj->slab_index = backing.slab_index;
	obj->sg = backing.sg;
	INIT_LIST_HEAD(&obj->grants);

	ret = xa_err(xa_store(&mgr->objects, obj->id, obj, gfp));
	if (ret)
		goto err_release_backing;

	mutex_unlock(&mgr->lock);
	mutex_unlock(&proxy_vmshm_init_lock);
	return obj;

err_release_backing:
	proxy_vmshm_object_release_backing_locked(obj, true, false);
err_free_id:
	ida_free(&mgr->object_ids, id);
err_unlock:
	mutex_unlock(&mgr->lock);
	kmem_cache_free(proxy_vmshm_obj_cache, obj);
err_init_unlock:
	mutex_unlock(&proxy_vmshm_init_lock);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(proxy_vmshm_alloc_ext);

struct proxy_vmshm_object *proxy_vmshm_alloc(size_t size, gfp_t gfp)
{
	struct proxy_vmshm_alloc_params params = {
		.owner_vmid = 0,
		.type = PROXY_VMSHM_OBJ_GENERIC,
		.perms = 0,
		.size = size,
		.align = 0,
	};

	return proxy_vmshm_alloc_ext(&params, gfp);
}
EXPORT_SYMBOL_GPL(proxy_vmshm_alloc);

void proxy_vmshm_free(struct proxy_vmshm_object *obj)
{
	struct proxy_vmshm_manager *mgr = &proxy_vmshm_mgr;

	if (!obj)
		return;

	mutex_lock(&proxy_vmshm_init_lock);
	mutex_lock(&mgr->lock);
	if (!mgr->ready || xa_load(&mgr->objects, obj->id) != obj) {
		mutex_unlock(&mgr->lock);
		mutex_unlock(&proxy_vmshm_init_lock);
		return;
	}

	if (proxy_vmshm_object_begin_free_locked(obj, false))
		proxy_vmshm_object_release_and_put_maybe_unlock(obj, false);
	mutex_unlock(&mgr->lock);
	mutex_unlock(&proxy_vmshm_init_lock);
}
EXPORT_SYMBOL_GPL(proxy_vmshm_free);

int proxy_vmshm_free_handle(u64 handle, u32 requester_vmid)
{
	struct proxy_vmshm_manager *mgr = &proxy_vmshm_mgr;
	struct proxy_vmshm_object *obj;
	u32 id = proxy_vmshm_obj_index(handle);
	u32 generation = proxy_vmshm_obj_handle_generation(handle);
	int ret = 0;

	if (!id || !generation)
		return -EINVAL;

	mutex_lock(&proxy_vmshm_init_lock);
	mutex_lock(&mgr->lock);
	if (!mgr->ready) {
		ret = -ENODEV;
		goto out_unlock;
	}

	obj = xa_load(&mgr->objects, id);
	if (!obj) {
		ret = -ENOENT;
		goto out_unlock;
	}

	if (obj->generation != generation) {
		ret = -ESTALE;
		goto out_unlock;
	}

	if (obj->owner_vmid != requester_vmid) {
		ret = -EACCES;
		goto out_unlock;
	}

	if (proxy_vmshm_object_begin_free_locked(obj, false))
		proxy_vmshm_object_release_and_put_maybe_unlock(obj, false);

out_unlock:
	mutex_unlock(&mgr->lock);
	mutex_unlock(&proxy_vmshm_init_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(proxy_vmshm_free_handle);

bool proxy_vmshm_get(struct proxy_vmshm_object *obj)
{
	struct proxy_vmshm_manager *mgr = &proxy_vmshm_mgr;
	bool ret = false;

	if (!obj)
		return false;

	mutex_lock(&mgr->lock);
	if (mgr->ready && xa_load(&mgr->objects, obj->id) == obj &&
	    proxy_vmshm_object_state_usable(obj))
		ret = proxy_vmshm_object_get_locked(obj);
	mutex_unlock(&mgr->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(proxy_vmshm_get);

void proxy_vmshm_put(struct proxy_vmshm_object *obj)
{
	struct proxy_vmshm_manager *mgr = &proxy_vmshm_mgr;

	if (!obj)
		return;

	mutex_lock(&mgr->lock);
	proxy_vmshm_object_put_locked(obj);
	mutex_unlock(&mgr->lock);
}
EXPORT_SYMBOL_GPL(proxy_vmshm_put);

int proxy_vmshm_pin(struct proxy_vmshm_object *obj)
{
	struct proxy_vmshm_manager *mgr = &proxy_vmshm_mgr;
	int ret;

	if (!obj)
		return -EINVAL;

	mutex_lock(&mgr->lock);
	if (!mgr->ready || xa_load(&mgr->objects, obj->id) != obj)
		ret = -ENODEV;
	else
		ret = proxy_vmshm_object_pin_locked(obj);
	mutex_unlock(&mgr->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(proxy_vmshm_pin);

void proxy_vmshm_unpin(struct proxy_vmshm_object *obj)
{
	struct proxy_vmshm_manager *mgr = &proxy_vmshm_mgr;

	if (!obj)
		return;

	mutex_lock(&proxy_vmshm_init_lock);
	mutex_lock(&mgr->lock);
	if (WARN_ON(!atomic_read(&obj->pin_count))) {
		mutex_unlock(&mgr->lock);
		mutex_unlock(&proxy_vmshm_init_lock);
		return;
	}

	if (atomic_dec_and_test(&obj->pin_count) &&
	    obj->state == PROXY_VMSHM_OBJECT_ZOMBIE)
		/*
		 * 这是 free() 之后最后一个 in-flight 用户。现在 backing 可以
		 * 被清零并归还给 buddy。
		 */
		proxy_vmshm_object_release_and_put_maybe_unlock(obj, false);
	else
		proxy_vmshm_object_put_locked(obj);
	mutex_unlock(&mgr->lock);
	mutex_unlock(&proxy_vmshm_init_lock);
}
EXPORT_SYMBOL_GPL(proxy_vmshm_unpin);

int proxy_vmshm_lookup_pin(u64 handle, u32 requester_vmid, u32 required_perms,
			   struct proxy_vmshm_object **out)
{
	struct proxy_vmshm_manager *mgr = &proxy_vmshm_mgr;
	struct proxy_vmshm_object *obj;
	u32 id = proxy_vmshm_obj_index(handle);
	u32 generation = proxy_vmshm_obj_handle_generation(handle);
	int ret;

	if (!out || !id || !generation)
		return -EINVAL;

	(void)required_perms;
	*out = NULL;

	mutex_lock(&mgr->lock);
	if (!mgr->ready) {
		ret = -ENODEV;
		goto out_unlock;
	}

	obj = xa_load(&mgr->objects, id);
	if (!obj) {
		ret = -ENOENT;
		goto out_unlock;
	}

	if (obj->generation != generation) {
		ret = -ESTALE;
		goto out_unlock;
	}

	if (obj->owner_vmid != requester_vmid) {
		ret = -EACCES;
		goto out_unlock;
	}

	/* lookup 成功会返回 pinned object，调用者后续必须 unpin。 */
	ret = proxy_vmshm_object_pin_locked(obj);
	if (!ret)
		*out = obj;

out_unlock:
	mutex_unlock(&mgr->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(proxy_vmshm_lookup_pin);

int proxy_vmshm_lookup(u64 handle, u32 requester_vmid, u32 required_perms,
		       struct proxy_vmshm_object **out)
{
	return proxy_vmshm_lookup_pin(handle, requester_vmid, required_perms,
				      out);
}
EXPORT_SYMBOL_GPL(proxy_vmshm_lookup);

u64 proxy_vmshm_obj_handle(struct proxy_vmshm_object *obj)
{
	return obj ? proxy_vmshm_make_obj_handle(obj->id, obj->generation) : 0;
}
EXPORT_SYMBOL_GPL(proxy_vmshm_obj_handle);

u32 proxy_vmshm_obj_id(struct proxy_vmshm_object *obj)
{
	return obj ? obj->id : 0;
}
EXPORT_SYMBOL_GPL(proxy_vmshm_obj_id);

u32 proxy_vmshm_obj_generation(struct proxy_vmshm_object *obj)
{
	return obj ? obj->generation : 0;
}
EXPORT_SYMBOL_GPL(proxy_vmshm_obj_generation);

u32 proxy_vmshm_obj_type(struct proxy_vmshm_object *obj)
{
	return obj ? obj->type : PROXY_VMSHM_OBJ_GENERIC;
}
EXPORT_SYMBOL_GPL(proxy_vmshm_obj_type);

u32 proxy_vmshm_obj_owner_vmid(struct proxy_vmshm_object *obj)
{
	return obj ? obj->owner_vmid : 0;
}
EXPORT_SYMBOL_GPL(proxy_vmshm_obj_owner_vmid);

u32 proxy_vmshm_obj_perms(struct proxy_vmshm_object *obj)
{
	return obj ? obj->perms : 0;
}
EXPORT_SYMBOL_GPL(proxy_vmshm_obj_perms);

unsigned int proxy_vmshm_obj_order(struct proxy_vmshm_object *obj)
{
	return obj ? obj->order : 0;
}
EXPORT_SYMBOL_GPL(proxy_vmshm_obj_order);

void *proxy_vmshm_obj_kva(struct proxy_vmshm_object *obj)
{
	if (obj && obj->backing_kind == PROXY_VMSHM_BACKING_SG) {
		WARN_ON_ONCE(1);
		return NULL;
	}

	return obj ? obj->kva : NULL;
}
EXPORT_SYMBOL_GPL(proxy_vmshm_obj_kva);

phys_addr_t proxy_vmshm_obj_gpa(struct proxy_vmshm_object *obj)
{
	if (obj && obj->backing_kind == PROXY_VMSHM_BACKING_SG) {
		WARN_ON_ONCE(1);
		return 0;
	}

	return obj ? obj->gpa : 0;
}
EXPORT_SYMBOL_GPL(proxy_vmshm_obj_gpa);

u64 proxy_vmshm_obj_offset(struct proxy_vmshm_object *obj)
{
	if (obj && obj->backing_kind == PROXY_VMSHM_BACKING_SG) {
		WARN_ON_ONCE(1);
		return 0;
	}

	return obj ? obj->offset : 0;
}
EXPORT_SYMBOL_GPL(proxy_vmshm_obj_offset);

size_t proxy_vmshm_obj_size(struct proxy_vmshm_object *obj)
{
	return obj ? obj->size : 0;
}
EXPORT_SYMBOL_GPL(proxy_vmshm_obj_size);

size_t proxy_vmshm_obj_alloc_size(struct proxy_vmshm_object *obj)
{
	return obj ? obj->alloc_size : 0;
}
EXPORT_SYMBOL_GPL(proxy_vmshm_obj_alloc_size);

bool proxy_vmshm_obj_is_sg(struct proxy_vmshm_object *obj)
{
	return obj && obj->backing_kind == PROXY_VMSHM_BACKING_SG;
}
EXPORT_SYMBOL_GPL(proxy_vmshm_obj_is_sg);

bool proxy_vmshm_obj_is_contiguous(struct proxy_vmshm_object *obj)
{
	return obj && obj->backing_kind != PROXY_VMSHM_BACKING_SG;
}
EXPORT_SYMBOL_GPL(proxy_vmshm_obj_is_contiguous);

unsigned int proxy_vmshm_obj_nr_segments(struct proxy_vmshm_object *obj)
{
	if (!obj)
		return 0;

	if (obj->backing_kind == PROXY_VMSHM_BACKING_SG)
		return obj->sg.nr_segs;

	return 1;
}
EXPORT_SYMBOL_GPL(proxy_vmshm_obj_nr_segments);

int proxy_vmshm_obj_get_segment(struct proxy_vmshm_object *obj,
				unsigned int idx,
				struct proxy_vmshm_span *span)
{
	if (!obj || !span)
		return -EINVAL;

	if (obj->backing_kind == PROXY_VMSHM_BACKING_SG) {
		struct proxy_vmshm_sg_segment *seg;

		if (idx >= obj->sg.nr_segs)
			return -ERANGE;

		seg = &obj->sg.segs[idx];
		span->logical_offset = seg->logical_offset;
		span->offset = seg->offset;
		span->kva = seg->kva;
		span->gpa = seg->gpa;
		span->size = seg->size;
		return 0;
	}

	if (idx)
		return -ERANGE;

	span->logical_offset = 0;
	span->offset = obj->offset;
	span->kva = obj->kva;
	span->gpa = obj->gpa;
	span->size = obj->alloc_size;
	return 0;
}
EXPORT_SYMBOL_GPL(proxy_vmshm_obj_get_segment);

static int proxy_vmshm_obj_translate_sg(struct proxy_vmshm_object *obj,
					u64 logical_off, size_t len,
					struct proxy_vmshm_span *spans,
					unsigned int max_spans,
					unsigned int *nr_spans)
{
	unsigned int i, out = 0, required = 0;
	u64 end = logical_off + len;

	for (i = 0; i < obj->sg.nr_segs; i++) {
		struct proxy_vmshm_sg_segment *seg = &obj->sg.segs[i];
		u64 seg_start = seg->logical_offset;
		u64 seg_end = seg_start + seg->size;

		if (end <= seg_start || logical_off >= seg_end)
			continue;

		required++;
	}

	if (max_spans < required) {
		*nr_spans = required;
		return -ENOSPC;
	}

	for (i = 0; i < obj->sg.nr_segs; i++) {
		struct proxy_vmshm_sg_segment *seg = &obj->sg.segs[i];
		u64 seg_start = seg->logical_offset;
		u64 seg_end = seg_start + seg->size;
		u64 start, stop, delta;

		if (end <= seg_start || logical_off >= seg_end)
			continue;

		start = max(logical_off, seg_start);
		stop = min(end, seg_end);
		delta = start - seg_start;

		spans[out].logical_offset = start;
		spans[out].offset = seg->offset + delta;
		spans[out].kva = (void *)((u8 *)seg->kva + delta);
		spans[out].gpa = seg->gpa + delta;
		spans[out].size = stop - start;
		out++;
	}

	*nr_spans = out;
	return 0;
}

int proxy_vmshm_obj_translate(struct proxy_vmshm_object *obj,
			      u64 logical_off, size_t len,
			      struct proxy_vmshm_span *spans,
			      unsigned int max_spans,
			      unsigned int *nr_spans)
{
	u64 end;

	if (!obj || !spans || !nr_spans)
		return -EINVAL;

	*nr_spans = 0;
	if (len > U64_MAX - logical_off)
		return -EOVERFLOW;

	end = logical_off + len;
	if (end > obj->size)
		return -ERANGE;

	if (!len)
		return 0;

	if (obj->backing_kind == PROXY_VMSHM_BACKING_SG)
		return proxy_vmshm_obj_translate_sg(obj, logical_off, len,
						    spans, max_spans,
						    nr_spans);

	if (!max_spans) {
		*nr_spans = 1;
		return -ENOSPC;
	}

	spans[0].logical_offset = logical_off;
	spans[0].offset = obj->offset + logical_off;
	spans[0].kva = (void *)((u8 *)obj->kva + logical_off);
	spans[0].gpa = obj->gpa + logical_off;
	spans[0].size = len;
	*nr_spans = 1;
	return 0;
}
EXPORT_SYMBOL_GPL(proxy_vmshm_obj_translate);

int proxy_vmshm_grant_create(struct proxy_vmshm_object *obj, u32 target_vmid,
			     u32 perms, u64 *grant_id)
{
	struct proxy_vmshm_manager *mgr = &proxy_vmshm_mgr;
	struct proxy_vmshm_grant *grant;
	u32 generation;
	int id, ret;

	if (!obj || !grant_id)
		return -EINVAL;

	mutex_lock(&proxy_vmshm_init_lock);
	if (!proxy_vmshm_grant_cache) {
		ret = -ENODEV;
		goto err_init_unlock;
	}

	grant = kmem_cache_zalloc(proxy_vmshm_grant_cache, GFP_KERNEL);
	if (!grant) {
		ret = -ENOMEM;
		goto err_init_unlock;
	}

	mutex_lock(&mgr->lock);
	if (!mgr->ready || xa_load(&mgr->objects, obj->id) != obj) {
		ret = -ENODEV;
		goto err_unlock;
	}

	if (!proxy_vmshm_object_state_usable(obj)) {
		ret = -EACCES;
		goto err_unlock;
	}

	id = ida_alloc_range(&mgr->grant_ids, 1, INT_MAX, GFP_KERNEL);
	if (id < 0) {
		ret = id;
		goto err_unlock;
	}

	generation = mgr->next_grant_generation++;
	if (!generation)
		generation = mgr->next_grant_generation++;

	grant->id = id;
	grant->object_id = obj->id;
	grant->target_vmid = target_vmid;
	grant->perms = perms;
	grant->generation = generation;
	grant->obj = obj;

	ret = xa_err(xa_store(&mgr->grants, grant->id, grant, GFP_KERNEL));
	if (ret)
		goto err_free_id;

	list_add_tail(&grant->obj_node, &obj->grants);
	obj->active_grants++;
	obj->state = PROXY_VMSHM_OBJECT_GRANTED;
	*grant_id = proxy_vmshm_make_grant_id(grant->id, generation);

	mutex_unlock(&mgr->lock);
	mutex_unlock(&proxy_vmshm_init_lock);
	return 0;

err_free_id:
	ida_free(&mgr->grant_ids, id);
err_unlock:
	mutex_unlock(&mgr->lock);
	kmem_cache_free(proxy_vmshm_grant_cache, grant);
err_init_unlock:
	mutex_unlock(&proxy_vmshm_init_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(proxy_vmshm_grant_create);

int proxy_vmshm_grant_lookup_pin(u64 grant_id, u32 requester_vmid,
				 u32 required_perms,
				 struct proxy_vmshm_object **out)
{
	struct proxy_vmshm_manager *mgr = &proxy_vmshm_mgr;
	struct proxy_vmshm_grant *grant;
	struct proxy_vmshm_object *obj;
	u32 id = proxy_vmshm_grant_index(grant_id);
	u32 generation = proxy_vmshm_grant_generation(grant_id);
	int ret;

	if (!out || !id || !generation)
		return -EINVAL;

	(void)required_perms;
	*out = NULL;

	mutex_lock(&mgr->lock);
	if (!mgr->ready) {
		ret = -ENODEV;
		goto out_unlock;
	}

	grant = xa_load(&mgr->grants, id);
	if (!grant) {
		ret = -ENOENT;
		goto out_unlock;
	}

	if (grant->generation != generation) {
		ret = -ESTALE;
		goto out_unlock;
	}

	if (grant->target_vmid != requester_vmid) {
		ret = -EACCES;
		goto out_unlock;
	}

	obj = grant->obj;
	if (!obj || xa_load(&mgr->objects, obj->id) != obj) {
		ret = -ENOENT;
		goto out_unlock;
	}

	/* grant lookup 也代表正在使用底层 object backing，因此需要 pin。 */
	ret = proxy_vmshm_object_pin_locked(obj);
	if (!ret)
		*out = obj;

out_unlock:
	mutex_unlock(&mgr->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(proxy_vmshm_grant_lookup_pin);

int proxy_vmshm_grant_lookup(u64 grant_id, u32 requester_vmid,
			     u32 required_perms,
			     struct proxy_vmshm_object **out)
{
	return proxy_vmshm_grant_lookup_pin(grant_id, requester_vmid,
					    required_perms, out);
}
EXPORT_SYMBOL_GPL(proxy_vmshm_grant_lookup);

int proxy_vmshm_grant_revoke(u64 grant_id)
{
	struct proxy_vmshm_manager *mgr = &proxy_vmshm_mgr;
	struct proxy_vmshm_grant *grant;
	u32 id = proxy_vmshm_grant_index(grant_id);
	u32 generation = proxy_vmshm_grant_generation(grant_id);
	int ret = 0;

	if (!id || !generation)
		return -EINVAL;

	mutex_lock(&mgr->lock);
	if (!mgr->ready) {
		ret = -ENODEV;
		goto out_unlock;
	}

	grant = xa_load(&mgr->grants, id);
	if (!grant) {
		ret = -ENOENT;
		goto out_unlock;
	}

	if (grant->generation != generation) {
		ret = -ESTALE;
		goto out_unlock;
	}

	proxy_vmshm_grant_destroy_locked(grant);

out_unlock:
	mutex_unlock(&mgr->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(proxy_vmshm_grant_revoke);

static int
proxy_vmshm_manager_send_object_rsp(u64 reply_to,
				    const struct vmshm_manager_get_object_rsp *rsp)
{
	struct vmshm_comm_tx tx = {
		.type = VMSHM_MANAGER_MSG_GET_OBJECT_RSP,
		.reply_to = reply_to,
		.status = rsp->ret,
		.payload = rsp,
		.len = sizeof(*rsp),
	};
	int ret, i;

	for (i = 0; i < PROXY_VMSHM_MANAGER_SEND_RETRIES; i++) {
		ret = proxy_comm_vmshm_send_to_client(&tx);
		if (ret != -EAGAIN)
			return ret;

		usleep_range(PROXY_VMSHM_MANAGER_SEND_WAIT_US,
			     PROXY_VMSHM_MANAGER_SEND_WAIT_US * 2);
	}

	return -ETIMEDOUT;
}

static int
proxy_vmshm_manager_fill_desc(struct proxy_vmshm_object *obj,
			      struct vmshm_manager_desc *desc)
{
	if (!proxy_vmshm_obj_is_contiguous(obj))
		return -EOPNOTSUPP;

	desc->handle = proxy_vmshm_obj_handle(obj);
	desc->id = proxy_vmshm_obj_id(obj);
	desc->generation = proxy_vmshm_obj_generation(obj);
	desc->type = proxy_vmshm_obj_type(obj);
	desc->perms = proxy_vmshm_obj_perms(obj);
	desc->offset = proxy_vmshm_obj_offset(obj);
	desc->size = proxy_vmshm_obj_size(obj);
	desc->alloc_size = proxy_vmshm_obj_alloc_size(obj);
	desc->gpa = proxy_vmshm_obj_gpa(obj);
	desc->flags = VMSHM_MANAGER_DESC_F_CONTIG;
	desc->nr_segments = 1;
	return 0;
}

static int
proxy_vmshm_manager_handle_object_req(
	const struct vmshm_manager_get_object_req *req,
	struct vmshm_manager_get_object_rsp *rsp)
{
	struct proxy_vmshm_object *obj = NULL;
	int ret;

	switch (req->lookup) {
	case VMSHM_MANAGER_LOOKUP_HANDLE:
		ret = proxy_vmshm_lookup_pin(req->handle, req->requester_vmid,
					     req->required_perms, &obj);
		break;
	case VMSHM_MANAGER_LOOKUP_GRANT:
		ret = proxy_vmshm_grant_lookup_pin(req->grant_id,
						   req->requester_vmid,
						   req->required_perms, &obj);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	if (ret)
		return ret;

	ret = proxy_vmshm_manager_fill_desc(obj, &rsp->desc);
	proxy_vmshm_unpin(obj);
	return ret;
}

static int proxy_vmshm_manager_rx_handler(const struct vmshm_comm_rx *rx,
					  void *priv)
{
	struct vmshm_manager_get_object_req req;
	struct vmshm_manager_get_object_rsp rsp;
	int ret;

	if (!rx || rx->len != sizeof(req))
		return -EPROTO;

	memset(&rsp, 0, sizeof(rsp));
	memcpy(&req, rx->payload, sizeof(req));

	ret = proxy_vmshm_manager_handle_object_req(&req, &rsp);
	rsp.ret = ret;

	ret = proxy_vmshm_manager_send_object_rsp(rx->seq, &rsp);
	if (ret)
		pr_warn_ratelimited("proxy_manager_vmshm: response send failed (%d)\n",
				    ret);

	return ret;
}

#ifdef CONFIG_PROXY_VMSHM_MANAGER_SELFTEST
static int proxy_vmshm_debug_check_free_area_locked(struct proxy_vmshm_buddy *buddy,
						    unsigned long *listed_pages)
{
	struct proxy_vmshm_buddy_page *page;
	unsigned long page_idx;
	unsigned int order;

	*listed_pages = 0;

	for (order = 0; order <= buddy->max_order; order++) {
		list_for_each_entry(page, &buddy->free_area[order], node) {
			page_idx = page - buddy->pages;

			if (page_idx >= buddy->nr_pages) {
				pr_err("proxy_manager_vmshm: debug free_area[%u] has out-of-range page\n",
				       order);
				return -EINVAL;
			}

			if (page->state != PROXY_VMSHM_BLOCK_FREE ||
			    page->order != order) {
				pr_err("proxy_manager_vmshm: debug free_area[%u] bad head idx=%lu state=%d order=%u\n",
				       order, page_idx, page->state, page->order);
				return -EINVAL;
			}

			if (page_idx + proxy_vmshm_buddy_order_pages(order) >
			    buddy->nr_pages) {
				pr_err("proxy_manager_vmshm: debug free_area[%u] block idx=%lu exceeds pool\n",
				       order, page_idx);
				return -EINVAL;
			}

			*listed_pages += proxy_vmshm_buddy_order_pages(order);
		}
	}

	return 0;
}

static int proxy_vmshm_debug_check_page_map_locked(struct proxy_vmshm_buddy *buddy,
						   unsigned long *mapped_pages,
						   unsigned long *free_blocks)
{
	unsigned long block_pages, i, page_idx = 0;
	struct proxy_vmshm_buddy_page *head;
	struct proxy_vmshm_buddy_page *tail;

	*mapped_pages = 0;
	*free_blocks = 0;

	while (page_idx < buddy->nr_pages) {
		head = &buddy->pages[page_idx];
		if (head->state != PROXY_VMSHM_BLOCK_FREE) {
			pr_err("proxy_manager_vmshm: debug expected free head at idx=%lu state=%d order=%u\n",
			       page_idx, head->state, head->order);
			return -EINVAL;
		}

		if (head->order > buddy->max_order) {
			pr_err("proxy_manager_vmshm: debug head idx=%lu has bad order=%u\n",
			       page_idx, head->order);
			return -EINVAL;
		}

		block_pages = proxy_vmshm_buddy_order_pages(head->order);
		if (page_idx + block_pages > buddy->nr_pages) {
			pr_err("proxy_manager_vmshm: debug head idx=%lu order=%u exceeds pool\n",
			       page_idx, head->order);
			return -EINVAL;
		}

		for (i = 1; i < block_pages; i++) {
			tail = &buddy->pages[page_idx + i];
			if (tail->state != PROXY_VMSHM_BLOCK_TAIL ||
			    tail->order != head->order) {
				pr_err("proxy_manager_vmshm: debug bad tail idx=%lu state=%d order=%u expected_order=%u\n",
				       page_idx + i, tail->state, tail->order,
				       head->order);
				return -EINVAL;
			}
		}

		*mapped_pages += block_pages;
		(*free_blocks)++;
		page_idx += block_pages;
	}

	return 0;
}

int proxy_vmshm_manager_debug_check_empty(void)
{
	struct proxy_vmshm_manager *mgr = &proxy_vmshm_mgr;
	unsigned long listed_pages, mapped_pages, free_blocks;
	struct proxy_vmshm_buddy *buddy = &mgr->buddy;
	unsigned long index;
	int ret = 0;

	mutex_lock(&mgr->lock);
	if (!mgr->ready) {
		ret = -ENODEV;
		goto out_unlock;
	}

	index = 0;
	if (xa_find(&mgr->objects, &index, ULONG_MAX, XA_PRESENT)) {
		pr_err("proxy_manager_vmshm: debug object xarray is not empty\n");
		ret = -EBUSY;
		goto out_unlock;
	}

	index = 0;
	if (xa_find(&mgr->grants, &index, ULONG_MAX, XA_PRESENT)) {
		pr_err("proxy_manager_vmshm: debug grant xarray is not empty\n");
		ret = -EBUSY;
		goto out_unlock;
	}

	if (buddy->free_pages != buddy->nr_pages) {
		pr_err("proxy_manager_vmshm: debug free_pages mismatch free=%lu total=%lu\n",
		       buddy->free_pages, buddy->nr_pages);
		ret = -EBUSY;
		goto out_unlock;
	}

	ret = proxy_vmshm_debug_check_free_area_locked(buddy, &listed_pages);
	if (ret)
		goto out_unlock;

	if (listed_pages != buddy->nr_pages) {
		pr_err("proxy_manager_vmshm: debug listed free pages mismatch listed=%lu total=%lu\n",
		       listed_pages, buddy->nr_pages);
		ret = -EINVAL;
		goto out_unlock;
	}

	ret = proxy_vmshm_debug_check_page_map_locked(buddy, &mapped_pages,
						      &free_blocks);
	if (ret)
		goto out_unlock;

	if (mapped_pages != buddy->nr_pages) {
		pr_err("proxy_manager_vmshm: debug page map mismatch mapped=%lu total=%lu\n",
		       mapped_pages, buddy->nr_pages);
		ret = -EINVAL;
		goto out_unlock;
	}

	pr_info("proxy_manager_vmshm: debug empty check passed objects=0 grants=0 free_pages=%lu free_blocks=%lu\n",
		buddy->free_pages, free_blocks);

out_unlock:
	mutex_unlock(&mgr->lock);
	return ret;
}
#endif
