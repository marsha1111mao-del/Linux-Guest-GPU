// SPDX-License-Identifier: GPL-2.0-only
/*
 * CPU-agnostic ARM page table allocator.
 *
 * Copyright (C) 2014 ARM Limited
 *
 * Author: Will Deacon <will.deacon@arm.com>
 */

#define pr_fmt(fmt) "arm-lpae io-pgtable: " fmt
#include <linux/highmem.h>
#include <linux/atomic.h>
#include <linux/bitops.h>
#include <linux/cc_platform.h>
#include <linux/io-pgtable.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/set_memory.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/dma-mapping.h>
#include <linux/arm-smccc.h>
#include <linux/mutex.h>
#include <asm/barrier.h>
#include <linux/hashtable.h>
#include "io-pgtable-arm.h"
#include "iommu-pages.h"

#define ARM_LPAE_MAX_ADDR_BITS 52
#define ARM_LPAE_S2_MAX_CONCAT_PAGES 16
#define ARM_LPAE_MAX_LEVELS 4

/* Struct accessors */
#define io_pgtable_to_data(x) container_of((x), struct arm_lpae_io_pgtable, iop)

#define io_pgtable_ops_to_data(x) \
	io_pgtable_to_data(io_pgtable_ops_to_pgtable(x))

/*
 * Calculate the right shift amount to get to the portion describing level l
 * in a virtual address mapped by the pagetable in d.
 */
#define ARM_LPAE_LVL_SHIFT(l, d)                               \
	(((ARM_LPAE_MAX_LEVELS - (l)) * (d)->bits_per_level) + \
	 ilog2(sizeof(arm_lpae_iopte)))

#define ARM_LPAE_GRANULE(d) (sizeof(arm_lpae_iopte) << (d)->bits_per_level)
#define ARM_LPAE_PGD_SIZE(d) (sizeof(arm_lpae_iopte) << (d)->pgd_bits)

#define ARM_LPAE_PTES_PER_TABLE(d) \
	(ARM_LPAE_GRANULE(d) >> ilog2(sizeof(arm_lpae_iopte)))

/*
 * Calculate the index at level l used to map virtual address a using the
 * pagetable in d.
 */
#define ARM_LPAE_PGD_IDX(l, d) \
	((l) == (d)->start_level ? (d)->pgd_bits - (d)->bits_per_level : 0)

#define ARM_LPAE_LVL_IDX(a, l, d)                 \
	(((u64)(a) >> ARM_LPAE_LVL_SHIFT(l, d)) & \
	 ((1 << ((d)->bits_per_level + ARM_LPAE_PGD_IDX(l, d))) - 1))

/* Calculate the block/page mapping size at level l for pagetable in d. */
#define ARM_LPAE_BLOCK_SIZE(l, d) (1ULL << ARM_LPAE_LVL_SHIFT(l, d))

/* Page table bits */
#define ARM_LPAE_PTE_TYPE_SHIFT 0
#define ARM_LPAE_PTE_TYPE_MASK 0x3

#define ARM_LPAE_PTE_TYPE_BLOCK 1
#define ARM_LPAE_PTE_TYPE_TABLE 3
#define ARM_LPAE_PTE_TYPE_PAGE 3

#define ARM_LPAE_PTE_ADDR_MASK GENMASK_ULL(47, 12)

#define ARM_LPAE_PTE_NSTABLE (((arm_lpae_iopte)1) << 63)
#define ARM_LPAE_PTE_XN (((arm_lpae_iopte)3) << 53)
#define ARM_LPAE_PTE_DBM (((arm_lpae_iopte)1) << 51)
#define ARM_LPAE_PTE_AF (((arm_lpae_iopte)1) << 10)
#define ARM_LPAE_PTE_SH_NS (((arm_lpae_iopte)0) << 8)
#define ARM_LPAE_PTE_SH_OS (((arm_lpae_iopte)2) << 8)
#define ARM_LPAE_PTE_SH_IS (((arm_lpae_iopte)3) << 8)
#define ARM_LPAE_PTE_NS (((arm_lpae_iopte)1) << 5)
#define ARM_LPAE_PTE_VALID (((arm_lpae_iopte)1) << 0)

#define ARM_LPAE_PTE_ATTR_LO_MASK (((arm_lpae_iopte)0x3ff) << 2)
/* Ignore the contiguous bit for block splitting */
#define ARM_LPAE_PTE_ATTR_HI_MASK (ARM_LPAE_PTE_XN | ARM_LPAE_PTE_DBM)
#define ARM_LPAE_PTE_ATTR_MASK \
	(ARM_LPAE_PTE_ATTR_LO_MASK | ARM_LPAE_PTE_ATTR_HI_MASK)
/* Software bit for solving coherency races */
#define ARM_LPAE_PTE_SW_SYNC (((arm_lpae_iopte)1) << 55)

/* Stage-1 PTE */
#define ARM_LPAE_PTE_AP_UNPRIV (((arm_lpae_iopte)1) << 6)
#define ARM_LPAE_PTE_AP_RDONLY_BIT 7
#define ARM_LPAE_PTE_AP_RDONLY \
	(((arm_lpae_iopte)1) << ARM_LPAE_PTE_AP_RDONLY_BIT)
#define ARM_LPAE_PTE_AP_WR_CLEAN_MASK \
	(ARM_LPAE_PTE_AP_RDONLY | ARM_LPAE_PTE_DBM)
#define ARM_LPAE_PTE_ATTRINDX_SHIFT 2
#define ARM_LPAE_PTE_nG (((arm_lpae_iopte)1) << 11)

/* Stage-2 PTE */
#define ARM_LPAE_PTE_HAP_FAULT (((arm_lpae_iopte)0) << 6)
#define ARM_LPAE_PTE_HAP_READ (((arm_lpae_iopte)1) << 6)
#define ARM_LPAE_PTE_HAP_WRITE (((arm_lpae_iopte)2) << 6)
#define ARM_LPAE_PTE_MEMATTR_OIWB (((arm_lpae_iopte)0xf) << 2)
#define ARM_LPAE_PTE_MEMATTR_NC (((arm_lpae_iopte)0x5) << 2)
#define ARM_LPAE_PTE_MEMATTR_DEV (((arm_lpae_iopte)0x1) << 2)

/* Register bits */
#define ARM_LPAE_VTCR_SL0_MASK 0x3

#define ARM_LPAE_TCR_T0SZ_SHIFT 0

#define ARM_LPAE_VTCR_PS_SHIFT 16
#define ARM_LPAE_VTCR_PS_MASK 0x7

#define ARM_LPAE_MAIR_ATTR_SHIFT(n) ((n) << 3)
#define ARM_LPAE_MAIR_ATTR_MASK 0xff
#define ARM_LPAE_MAIR_ATTR_DEVICE 0x04
#define ARM_LPAE_MAIR_ATTR_NC 0x44
#define ARM_LPAE_MAIR_ATTR_INC_OWBRWA 0xf4
#define ARM_LPAE_MAIR_ATTR_WBRWA 0xff
#define ARM_LPAE_MAIR_ATTR_IDX_NC 0
#define ARM_LPAE_MAIR_ATTR_IDX_CACHE 1
#define ARM_LPAE_MAIR_ATTR_IDX_DEV 2
#define ARM_LPAE_MAIR_ATTR_IDX_INC_OCACHE 3

#define ARM_MALI_LPAE_TTBR_ADRMODE_TABLE (3u << 0)
#define ARM_MALI_LPAE_TTBR_READ_INNER BIT(2)
#define ARM_MALI_LPAE_TTBR_SHARE_OUTER BIT(4)

#define ARM_MALI_LPAE_MEMATTR_IMP_DEF 0x88ULL
#define ARM_MALI_LPAE_MEMATTR_WRITE_ALLOC 0x8DULL
#define PANTHOR_GPA2HPA_MAX_ENTRIES (PAGE_SIZE / sizeof(u64))

enum panthor_gpa2hpa_cache_mode {
	PANTHOR_GPA2HPA_CACHE_NONE,
	PANTHOR_GPA2HPA_CACHE_TABLE,
};

/* IOPTE accessors */
#define iopte_deref(pte, d) __va(iopte_to_paddr(pte, d))

#define iopte_type(pte) \
	(((pte) >> ARM_LPAE_PTE_TYPE_SHIFT) & ARM_LPAE_PTE_TYPE_MASK)

#define iopte_prot(pte) ((pte) & ARM_LPAE_PTE_ATTR_MASK)

#define iopte_writeable_dirty(pte) \
	(((pte) & ARM_LPAE_PTE_AP_WR_CLEAN_MASK) == ARM_LPAE_PTE_DBM)

#define iopte_set_writeable_clean(ptep) \
	set_bit(ARM_LPAE_PTE_AP_RDONLY_BIT, (unsigned long *)(ptep))

/* Panthor shadow page tables store host PAs in GPU-visible descriptors, while
 * the guest CPU still needs a virtual pointer when walking those tables.
 */
struct panthor_table_pte_mapping {
	u64 hpa;
	u64 gva;
	struct hlist_node node;
};

struct panthor_gpa_hpa_mapping {
	u64 gpa_page;
	u64 hpa_page;
	struct hlist_node node;
};

struct panthor_passthrough_stats {
	u64 gpa2hpa_batch_calls;
	u64 gpa2hpa_entries;
	u64 gpa2hpa_hvc_calls;
	u64 gpa2hpa_hvc_entries;
	u64 gpa2hpa_cache_hits;
	u64 gpa2hpa_cache_misses;
	u64 map_2m_attempts;
	u64 map_2m_blocks;
	u64 map_2m_fallback_tables;
	u64 map_2m_hpa_unaligned;
	u64 map_2m_hpa_discontig;
	u64 pt_timing_samples;
	u64 gpa2hpa_total_ns;
	u64 gpa2hpa_max_ns;
	u64 gpa2hpa_lock_wait_ns;
	u64 gpa2hpa_scan_ns;
	u64 gpa2hpa_prep_ns;
	u64 gpa2hpa_hvc_ns;
	u64 gpa2hpa_hvc_max_ns;
	u64 gpa2hpa_result_ns;
	u64 install_table_calls;
	u64 install_table_total_ns;
	u64 install_table_max_ns;
	u64 install_table_translate_ns;
	u64 install_table_store_ns;
	u64 install_table_sync_ns;
	u64 map_2m_total_ns;
	u64 map_2m_max_ns;
	u64 map_2m_alloc_hpas_ns;
	u64 map_2m_translate_ns;
	u64 map_2m_check_ns;
	u64 map_2m_block_install_ns;
	u64 map_2m_fallback_alloc_ns;
	u64 map_2m_fallback_fill_ns;
	u64 map_2m_fallback_sync_ns;
	u64 map_2m_fallback_install_ns;
	u64 map_2m_free_hpas_ns;
	u64 init_pte_total_ns;
	u64 init_pte_max_ns;
	u64 init_pte_alloc_hpas_ns;
	u64 init_pte_translate_ns;
	u64 init_pte_fill_ns;
	u64 init_pte_sync_ns;
	u64 init_pte_free_hpas_ns;
};

struct arm_lpae_io_pgtable {
	struct io_pgtable iop;

	int pgd_bits;
	int start_level;
	int bits_per_level;

	void *pgd;
	struct page *panthor_gpa2hpa_page;
	struct mutex panthor_gpa2hpa_lock;
	DECLARE_HASHTABLE(panthor_table_pte_map, 10);
	DECLARE_HASHTABLE(panthor_gpa_hpa_map, 12);
	struct panthor_passthrough_stats panthor_stats;
};

static bool panthor_pt_timing_enabled;
module_param_named(panthor_pt_timing, panthor_pt_timing_enabled, bool, 0644);
MODULE_PARM_DESC(panthor_pt_timing,
		 "Enable Panthor passthrough io-pgtable aggregate timing diagnostics");

static inline bool panthor_pt_timing_active(void)
{
	return unlikely(READ_ONCE(panthor_pt_timing_enabled));
}

static inline u64 panthor_pt_timing_start(bool enabled)
{
	return enabled ? ktime_get_ns() : 0;
}

static inline void panthor_pt_timing_accum(u64 *field, u64 start_ns)
{
	if (start_ns)
		*field += ktime_get_ns() - start_ns;
}

static inline void panthor_pt_timing_accum_max(u64 *field, u64 *max_field,
					       u64 start_ns)
{
	u64 delta;

	if (!start_ns)
		return;

	delta = ktime_get_ns() - start_ns;
	*field += delta;
	if (delta > *max_field)
		*max_field = delta;
}

typedef u64 arm_lpae_iopte;

static int store_mapping(struct arm_lpae_io_pgtable *data, u64 hpa, u64 gva)
{
	struct panthor_table_pte_mapping *entry;

	hash_for_each_possible(data->panthor_table_pte_map, entry, node, hpa) {
		if (entry->hpa == hpa) {
			entry->gva = gva;
			return 0;
		}
	}

	entry = kmalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;

	entry->gva = gva;
	entry->hpa = hpa;
	hash_add(data->panthor_table_pte_map, &entry->node, hpa);
	return 0;
}

static arm_lpae_iopte *lookup_mapping(struct arm_lpae_io_pgtable *data, u64 hpa)
{
	struct panthor_table_pte_mapping *entry;

	hash_for_each_possible(data->panthor_table_pte_map, entry, node, hpa) {
		if (entry->hpa == hpa)
			return (arm_lpae_iopte *)entry->gva;
	}

	return NULL;
}

static void remove_mapping(struct arm_lpae_io_pgtable *data, u64 hpa)
{
	struct panthor_table_pte_mapping *entry;

	hash_for_each_possible(data->panthor_table_pte_map, entry, node, hpa) {
		if (entry->hpa == hpa) {
			hash_del(&entry->node);
			kfree(entry);
			return;
		}
	}
}

static int panthor_gpa_hpa_cache_lookup(struct arm_lpae_io_pgtable *data,
					phys_addr_t gpa, phys_addr_t *hpa)
{
	struct panthor_gpa_hpa_mapping *entry;
	u64 gpa_page = gpa & PAGE_MASK;

	hash_for_each_possible(data->panthor_gpa_hpa_map, entry, node,
			       gpa_page) {
		if (entry->gpa_page == gpa_page) {
			*hpa = entry->hpa_page | (gpa & ~PAGE_MASK);
			return 0;
		}
	}

	return -ENOENT;
}

static int panthor_gpa_hpa_cache_store(struct arm_lpae_io_pgtable *data,
				       phys_addr_t gpa, phys_addr_t hpa)
{
	struct panthor_gpa_hpa_mapping *entry;
	u64 gpa_page = gpa & PAGE_MASK;
	u64 hpa_page = hpa & PAGE_MASK;

	hash_for_each_possible(data->panthor_gpa_hpa_map, entry, node,
			       gpa_page) {
		if (entry->gpa_page == gpa_page) {
			entry->hpa_page = hpa_page;
			return 0;
		}
	}

	entry = kmalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;

	entry->gpa_page = gpa_page;
	entry->hpa_page = hpa_page;
	hash_add(data->panthor_gpa_hpa_map, &entry->node, gpa_page);
	return 0;
}

static void panthor_gpa_hpa_cache_free(struct arm_lpae_io_pgtable *data)
{
	struct panthor_gpa_hpa_mapping *entry;
	struct hlist_node *tmp;
	int slot;

	hash_for_each_safe(data->panthor_gpa_hpa_map, slot, tmp, entry,
			   node) {
		hash_del(&entry->node);
		kfree(entry);
	}
}

static long kvm_hypercall_gpa_to_hpa_batch(u64 addr_array, u64 count)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_invoke(ARM_SMCCC_VENDOR_HYP_GPA_TO_HPA_FUNC_ID,
			     addr_array, count, &res);

	return res.a0;
}

static int panthor_realm_share_gpa2hpa_page(struct page *page)
{
	void *addr = page_address(page);
	int ret;

	if (!cc_platform_has(CC_ATTR_MEM_ENCRYPT))
		return 0;

	if (!addr)
		return -EINVAL;

	ret = set_memory_decrypted((unsigned long)addr, 1);
	if (ret) {
		pr_err("[MZH][GPA2HPA] failed to share Realm exchange page ret=%d; leaking page\n",
		       ret);
		return ret;
	}

	memset(addr, 0, PAGE_SIZE);
	return 0;
}

static void panthor_realm_free_gpa2hpa_page(struct page *page)
{
	void *addr;
	int ret;

	if (!page)
		return;

	if (!cc_platform_has(CC_ATTR_MEM_ENCRYPT)) {
		__free_page(page);
		return;
	}

	addr = page_address(page);
	if (!addr) {
		pr_err("[MZH][GPA2HPA] cannot protect exchange page without direct map; leaking page\n");
		return;
	}

	ret = set_memory_encrypted((unsigned long)addr, 1);
	if (ret) {
		pr_err("[MZH][GPA2HPA] failed to protect exchange page ret=%d; leaking page\n",
		       ret);
		return;
	}

	__free_page(page);
}

static int panthor_gpa_to_hpa_batch(struct arm_lpae_io_pgtable *data,
				    phys_addr_t gpa, size_t stride,
				    int num_entries, phys_addr_t *hpas,
				    enum panthor_gpa2hpa_cache_mode cache_mode)
{
	u64 *array;
	long ret;
	bool use_cache = cache_mode != PANTHOR_GPA2HPA_CACHE_NONE;
	bool timing = panthor_pt_timing_active();
	u64 total_start = panthor_pt_timing_start(timing);
	u64 phase_start;
	int i, miss_count = 0;

	if (!num_entries || num_entries > PAGE_SIZE / sizeof(*array))
		return -EINVAL;

	if (!data->panthor_gpa2hpa_page)
		return -ENOMEM;

	phase_start = panthor_pt_timing_start(timing);
	mutex_lock(&data->panthor_gpa2hpa_lock);
	panthor_pt_timing_accum(&data->panthor_stats.gpa2hpa_lock_wait_ns,
				phase_start);
	data->panthor_stats.gpa2hpa_batch_calls++;
	data->panthor_stats.gpa2hpa_entries += num_entries;
	if (timing)
		data->panthor_stats.pt_timing_samples++;

	phase_start = panthor_pt_timing_start(timing);
	for (i = 0; i < num_entries; i++) {
		phys_addr_t in_gpa = gpa + i * stride;
		int cache_ret;

		if (use_cache) {
			cache_ret = panthor_gpa_hpa_cache_lookup(data, in_gpa,
								 &hpas[i]);
			if (!cache_ret) {
				data->panthor_stats.gpa2hpa_cache_hits++;
				continue;
			}
			data->panthor_stats.gpa2hpa_cache_misses++;
		}

		hpas[miss_count++] = in_gpa & PAGE_MASK;
	}
	panthor_pt_timing_accum(&data->panthor_stats.gpa2hpa_scan_ns,
				phase_start);

	if (!miss_count)
		goto out_unlock;

	array = page_address(data->panthor_gpa2hpa_page);
	phase_start = panthor_pt_timing_start(timing);
	for (i = 0; i < miss_count; i++)
		array[i] = hpas[i];

	data->panthor_stats.gpa2hpa_hvc_calls++;
	data->panthor_stats.gpa2hpa_hvc_entries += miss_count;
	flush_dcache_page(data->panthor_gpa2hpa_page);
	wmb();
	panthor_pt_timing_accum(&data->panthor_stats.gpa2hpa_prep_ns,
				phase_start);

	phase_start = panthor_pt_timing_start(timing);
	ret = kvm_hypercall_gpa_to_hpa_batch(
		page_to_phys(data->panthor_gpa2hpa_page), miss_count);
	rmb();
	panthor_pt_timing_accum_max(&data->panthor_stats.gpa2hpa_hvc_ns,
				    &data->panthor_stats.gpa2hpa_hvc_max_ns,
				    phase_start);

	if (ret) {
		pr_err("[MZH][GPA2HPA] HVC failed ret=%ld gpa=%pa entries=%d\n",
		       ret, &gpa, miss_count);
		goto out_unlock_ret;
	}

	phase_start = panthor_pt_timing_start(timing);
	for (i = 0; i < miss_count; i++) {
		phys_addr_t in_gpa = hpas[i];
		phys_addr_t hpa = array[i];

		if (!hpa || hpa >> 52) {
			pr_err("[MZH][GPA2HPA] invalid HPA gpa=%pa hpa=%pa\n",
			       &in_gpa, &hpa);
			ret = -EINVAL;
			goto out_unlock_ret;
		}

		if (use_cache) {
			ret = panthor_gpa_hpa_cache_store(data, in_gpa, hpa);
			if (ret)
				goto out_unlock_ret;
		}

		if (!use_cache)
			hpa |= ((gpa + i * stride) & ~PAGE_MASK);

		hpas[i] = hpa;
	}

	if (use_cache) {
		for (i = 0; i < num_entries; i++) {
			phys_addr_t in_gpa = gpa + i * stride;

			ret = panthor_gpa_hpa_cache_lookup(data, in_gpa,
							   &hpas[i]);
			if (ret)
				goto out_unlock_ret;
		}
	}
	panthor_pt_timing_accum(&data->panthor_stats.gpa2hpa_result_ns,
				phase_start);

out_unlock:
	ret = 0;
out_unlock_ret:
	mutex_unlock(&data->panthor_gpa2hpa_lock);
	panthor_pt_timing_accum_max(&data->panthor_stats.gpa2hpa_total_ns,
				    &data->panthor_stats.gpa2hpa_max_ns,
				    total_start);
	return ret;
}

static int panthor_gpa_to_hpa(struct arm_lpae_io_pgtable *data,
			      phys_addr_t gpa, phys_addr_t *hpa)
{
	return panthor_gpa_to_hpa_batch(data, gpa, PAGE_SIZE, 1, hpa,
					PANTHOR_GPA2HPA_CACHE_TABLE);
}

static arm_lpae_iopte *
panthor_table_from_pte(struct arm_lpae_io_pgtable *data, arm_lpae_iopte pte,
		       int lvl, unsigned long iova, const char *op)
{
	u64 hpa = pte & ARM_LPAE_PTE_ADDR_MASK;
	arm_lpae_iopte *table = lookup_mapping(data, hpa);

	if (!table)
		pr_err("[MZH][PTW][%s] missing table mapping lvl=%d iova=%lx pte=%llx hpa=%llx\n",
		       op, lvl, iova, pte, hpa);

	return table;
}

static inline bool iopte_leaf(arm_lpae_iopte pte, int lvl,
			      enum io_pgtable_fmt fmt)
{
	if (lvl == (ARM_LPAE_MAX_LEVELS - 1) && fmt != ARM_MALI_LPAE)
		return iopte_type(pte) == ARM_LPAE_PTE_TYPE_PAGE;

	return iopte_type(pte) == ARM_LPAE_PTE_TYPE_BLOCK;
}

static inline bool iopte_table(arm_lpae_iopte pte, int lvl)
{
	if (lvl == (ARM_LPAE_MAX_LEVELS - 1))
		return false;
	return iopte_type(pte) == ARM_LPAE_PTE_TYPE_TABLE;
}

static arm_lpae_iopte paddr_to_iopte(phys_addr_t paddr,
				     struct arm_lpae_io_pgtable *data)
{
	arm_lpae_iopte pte = paddr;

	/* Of the bits which overlap, either 51:48 or 15:12 are always RES0 */
	return (pte | (pte >> (48 - 12))) & ARM_LPAE_PTE_ADDR_MASK;
}

static phys_addr_t iopte_to_paddr(arm_lpae_iopte pte,
				  struct arm_lpae_io_pgtable *data)
{
	u64 paddr = pte & ARM_LPAE_PTE_ADDR_MASK;

	if (ARM_LPAE_GRANULE(data) < SZ_64K)
		return paddr;

	/* Rotate the packed high-order bits back to the top */
	return (paddr | (paddr << (48 - 12))) & (ARM_LPAE_PTE_ADDR_MASK << 4);
}

static bool selftest_running = false;

static dma_addr_t __arm_lpae_dma_addr(void *pages)
{
	return (dma_addr_t)virt_to_phys(pages);
}

static void *__arm_lpae_alloc_pages(size_t size, gfp_t gfp,
				    struct io_pgtable_cfg *cfg, void *cookie)
{
	struct device *dev = cfg->iommu_dev;
	int order = get_order(size);
	dma_addr_t dma;
	void *pages;

	VM_BUG_ON((gfp & __GFP_HIGHMEM));

	if (cfg->alloc)
		pages = cfg->alloc(cookie, size, gfp);
	else
		pages = iommu_alloc_pages_node(dev_to_node(dev), gfp, order);

	if (!pages)
		return NULL;

	if (!cfg->coherent_walk) {
		dma = dma_map_single(dev, pages, size, DMA_TO_DEVICE);
		if (dma_mapping_error(dev, dma))
			goto out_free;
		/*
		 * We depend on the IOMMU being able to work with any physical
		 * address directly, so if the DMA layer suggests otherwise by
		 * translating or truncating them, that bodes very badly...
		 */
		if (dma != virt_to_phys(pages))
			goto out_unmap;
	}

	return pages;

out_unmap:
	dev_err(dev,
		"Cannot accommodate DMA translation for IOMMU page tables\n");
	dma_unmap_single(dev, dma, size, DMA_TO_DEVICE);

out_free:
	if (cfg->free)
		cfg->free(cookie, pages, size);
	else
		iommu_free_pages(pages, order);

	return NULL;
}

static void __arm_lpae_free_pages(void *pages, size_t size,
				  struct io_pgtable_cfg *cfg, void *cookie)
{
	if (!cfg->coherent_walk)
		dma_unmap_single(cfg->iommu_dev, __arm_lpae_dma_addr(pages),
				 size, DMA_TO_DEVICE);

	if (cfg->free)
		cfg->free(cookie, pages, size);
	else
		iommu_free_pages(pages, get_order(size));
}

static void __arm_lpae_sync_pte(arm_lpae_iopte *ptep, int num_entries,
				struct io_pgtable_cfg *cfg)
{
	dma_sync_single_for_device(cfg->iommu_dev, __arm_lpae_dma_addr(ptep),
				   sizeof(*ptep) * num_entries, DMA_TO_DEVICE);
}
static void __arm_lpae_clear_pte(arm_lpae_iopte *ptep,
				 struct io_pgtable_cfg *cfg, int num_entries)
{
	for (int i = 0; i < num_entries; i++)
		ptep[i] = 0;

	if (!cfg->coherent_walk && num_entries)
		__arm_lpae_sync_pte(ptep, num_entries, cfg);
}

static size_t __arm_lpae_unmap(struct arm_lpae_io_pgtable *data,
			       struct iommu_iotlb_gather *gather,
			       unsigned long iova, size_t size, size_t pgcount,
			       int lvl, arm_lpae_iopte *ptep);
static void __arm_lpae_init_pte(struct arm_lpae_io_pgtable *data,
				phys_addr_t paddr, arm_lpae_iopte prot,
				int lvl, int num_entries,
				arm_lpae_iopte *ptep);
static size_t __arm_panthor_lpae_unmap(struct arm_lpae_io_pgtable *data,
				       struct iommu_iotlb_gather *gather,
				       unsigned long iova, size_t size,
				       size_t pgcount, int lvl,
				       arm_lpae_iopte *ptep);
static int arm_panthor_lpae_init_pte(struct arm_lpae_io_pgtable *data,
				     unsigned long iova, phys_addr_t paddr,
				     arm_lpae_iopte prot, int lvl,
				     int num_entries, arm_lpae_iopte *ptep);
static int
arm_panthor_lpae_install_table(arm_lpae_iopte *table, arm_lpae_iopte *ptep,
			       arm_lpae_iopte curr,
			       struct arm_lpae_io_pgtable *data,
			       arm_lpae_iopte *oldp);

static int arm_panthor_lpae_init_2m_block_or_pages(
	struct arm_lpae_io_pgtable *data, unsigned long iova, phys_addr_t paddr,
	arm_lpae_iopte prot, int lvl, arm_lpae_iopte *ptep, gfp_t gfp)
{
	struct io_pgtable_cfg *cfg = &data->iop.cfg;
	const int pages_per_block = SZ_2M / PAGE_SIZE;
	arm_lpae_iopte leaf_prot = prot;
	phys_addr_t *hpas, hpa_base;
	arm_lpae_iopte curr, pte;
	bool block_ok = true;
	bool timing = panthor_pt_timing_active();
	u64 total_start = panthor_pt_timing_start(timing);
	u64 phase_start;
	int ret, i;

	if (ARM_LPAE_BLOCK_SIZE(lvl, data) != SZ_2M)
		return arm_panthor_lpae_init_pte(data, iova, paddr, prot, lvl,
						 1, ptep);

	data->panthor_stats.map_2m_attempts++;
	curr = READ_ONCE(*ptep);
	if (curr) {
		ret = -EEXIST;
		goto out_record_total;
	}

	phase_start = panthor_pt_timing_start(timing);
	hpas = kmalloc_array(pages_per_block, sizeof(*hpas), gfp);
	panthor_pt_timing_accum(&data->panthor_stats.map_2m_alloc_hpas_ns,
				phase_start);
	if (!hpas) {
		ret = -ENOMEM;
		goto out_record_total;
	}

	phase_start = panthor_pt_timing_start(timing);
	ret = panthor_gpa_to_hpa_batch(data, paddr, PAGE_SIZE,
				       pages_per_block, hpas,
				       PANTHOR_GPA2HPA_CACHE_NONE);
	panthor_pt_timing_accum(&data->panthor_stats.map_2m_translate_ns,
				phase_start);
	if (ret)
		goto out_free_hpas;

	phase_start = panthor_pt_timing_start(timing);
	hpa_base = hpas[0];
	if (!IS_ALIGNED(hpa_base, SZ_2M)) {
		data->panthor_stats.map_2m_hpa_unaligned++;
		block_ok = false;
	} else {
		for (i = 1; i < pages_per_block; i++) {
			if (hpas[i] != hpa_base + (phys_addr_t)i * PAGE_SIZE) {
				data->panthor_stats.map_2m_hpa_discontig++;
				block_ok = false;
				break;
			}
		}
	}
	panthor_pt_timing_accum(&data->panthor_stats.map_2m_check_ns,
				phase_start);

	if (block_ok) {
		phase_start = panthor_pt_timing_start(timing);
		pte = prot | ARM_LPAE_PTE_TYPE_BLOCK |
		      paddr_to_iopte(hpa_base, data);
		WRITE_ONCE(*ptep, pte);
		if (!cfg->coherent_walk)
			__arm_lpae_sync_pte(ptep, 1, cfg);
		data->panthor_stats.map_2m_blocks++;
		panthor_pt_timing_accum(
			&data->panthor_stats.map_2m_block_install_ns,
			phase_start);
		goto out_free_hpas;
	}

	if (data->iop.fmt != ARM_MALI_LPAE &&
	    lvl + 1 == ARM_LPAE_MAX_LEVELS - 1)
		leaf_prot |= ARM_LPAE_PTE_TYPE_PAGE;
	else
		leaf_prot |= ARM_LPAE_PTE_TYPE_BLOCK;

	{
		size_t tblsz = ARM_LPAE_GRANULE(data);
		arm_lpae_iopte *tablep, old;

		phase_start = panthor_pt_timing_start(timing);
		tablep = __arm_lpae_alloc_pages(tblsz, gfp, cfg,
						data->iop.cookie);
		panthor_pt_timing_accum(
			&data->panthor_stats.map_2m_fallback_alloc_ns,
			phase_start);
		if (!tablep) {
			ret = -ENOMEM;
			goto out_free_hpas;
		}

		phase_start = panthor_pt_timing_start(timing);
		for (i = 0; i < pages_per_block; i++)
			tablep[i] = leaf_prot | paddr_to_iopte(hpas[i], data);
		panthor_pt_timing_accum(
			&data->panthor_stats.map_2m_fallback_fill_ns,
			phase_start);

		phase_start = panthor_pt_timing_start(timing);
		if (!cfg->coherent_walk)
			__arm_lpae_sync_pte(tablep, pages_per_block, cfg);
		panthor_pt_timing_accum(
			&data->panthor_stats.map_2m_fallback_sync_ns,
			phase_start);

		phase_start = panthor_pt_timing_start(timing);
		ret = arm_panthor_lpae_install_table(tablep, ptep, 0, data,
						     &old);
		panthor_pt_timing_accum(
			&data->panthor_stats.map_2m_fallback_install_ns,
			phase_start);
		if (ret) {
			__arm_lpae_free_pages(tablep, tblsz, cfg,
					      data->iop.cookie);
			goto out_free_hpas;
		}

		if (old) {
			__arm_lpae_free_pages(tablep, tblsz, cfg,
					      data->iop.cookie);
			ret = -EEXIST;
		} else {
			data->panthor_stats.map_2m_fallback_tables++;
		}
	}

out_free_hpas:
	phase_start = panthor_pt_timing_start(timing);
	kfree(hpas);
	panthor_pt_timing_accum(&data->panthor_stats.map_2m_free_hpas_ns,
				phase_start);
out_record_total:
	panthor_pt_timing_accum_max(&data->panthor_stats.map_2m_total_ns,
				    &data->panthor_stats.map_2m_max_ns,
				    total_start);
	return ret;
}

static int arm_panthor_lpae_init_2m_blocks_or_pages(
	struct arm_lpae_io_pgtable *data, unsigned long iova, phys_addr_t paddr,
	arm_lpae_iopte prot, int lvl, int num_entries, arm_lpae_iopte *ptep,
	gfp_t gfp, size_t *mapped)
{
	int i, ret = 0;

	for (i = 0; i < num_entries; i++) {
		ret = arm_panthor_lpae_init_2m_block_or_pages(
			data, iova + (unsigned long)i * SZ_2M,
			paddr + (phys_addr_t)i * SZ_2M, prot, lvl, &ptep[i],
			gfp);
		if (ret)
			break;

		*mapped += SZ_2M;
	}

	return ret;
}

static int __arm_panthor_lpae_init_pte(struct arm_lpae_io_pgtable *data,
				       phys_addr_t paddr, arm_lpae_iopte prot,
				       int lvl, int num_entries,
				       arm_lpae_iopte *ptep)
{
	//panthor init pte
	int i;
	size_t sz = ARM_LPAE_BLOCK_SIZE(lvl, data);
	arm_lpae_iopte pte = prot;
	struct io_pgtable_cfg *cfg = &data->iop.cfg;
	phys_addr_t *hpas;
	int ret;
	bool timing = panthor_pt_timing_active();
	u64 total_start = panthor_pt_timing_start(timing);
	u64 phase_start;

	if (data->iop.fmt != ARM_MALI_LPAE && lvl == ARM_LPAE_MAX_LEVELS - 1)
		pte |= ARM_LPAE_PTE_TYPE_PAGE;
	else
		pte |= ARM_LPAE_PTE_TYPE_BLOCK;

	phase_start = panthor_pt_timing_start(timing);
	hpas = kcalloc(num_entries, sizeof(*hpas), GFP_KERNEL);
	panthor_pt_timing_accum(&data->panthor_stats.init_pte_alloc_hpas_ns,
				phase_start);
	if (!hpas) {
		ret = -ENOMEM;
		goto out_record_total;
	}

	phase_start = panthor_pt_timing_start(timing);
	ret = panthor_gpa_to_hpa_batch(data, paddr, sz, num_entries, hpas,
				       PANTHOR_GPA2HPA_CACHE_NONE);
	panthor_pt_timing_accum(&data->panthor_stats.init_pte_translate_ns,
				phase_start);
	if (ret)
		goto out_free_hpas;

	phase_start = panthor_pt_timing_start(timing);
	for (i = 0; i < num_entries; i++) {
		u64 raw_hpa = hpas[i]; // 原始的 Host Physical Address (HPA)
		u64 pte_val =
			pte |
			paddr_to_iopte(raw_hpa, data); // 最终写入页表的 PTE 值

		ptep[i] = pte_val;

		pr_debug("[MZH][PTE] gpa=%pa hpa=%pa pte=%llx lvl=%d prot=%llx\n",
			 &(phys_addr_t){ paddr + i * sz },
			 &(phys_addr_t){ raw_hpa }, (u64)pte_val, lvl, prot);
	}
	panthor_pt_timing_accum(&data->panthor_stats.init_pte_fill_ns,
				phase_start);

	phase_start = panthor_pt_timing_start(timing);
	if (!cfg->coherent_walk)
		__arm_lpae_sync_pte(ptep, num_entries, cfg);
	panthor_pt_timing_accum(&data->panthor_stats.init_pte_sync_ns,
				phase_start);

out_free_hpas:
	phase_start = panthor_pt_timing_start(timing);
	kfree(hpas);
	panthor_pt_timing_accum(&data->panthor_stats.init_pte_free_hpas_ns,
				phase_start);
out_record_total:
	panthor_pt_timing_accum_max(&data->panthor_stats.init_pte_total_ns,
				    &data->panthor_stats.init_pte_max_ns,
				    total_start);
	return ret;
}

static void __arm_lpae_init_pte(struct arm_lpae_io_pgtable *data,
				phys_addr_t paddr, arm_lpae_iopte prot, int lvl,
				int num_entries, arm_lpae_iopte *ptep)
{
	arm_lpae_iopte pte = prot;
	struct io_pgtable_cfg *cfg = &data->iop.cfg;
	size_t sz = ARM_LPAE_BLOCK_SIZE(lvl, data);
	int i;

	if (data->iop.fmt != ARM_MALI_LPAE && lvl == ARM_LPAE_MAX_LEVELS - 1)
		pte |= ARM_LPAE_PTE_TYPE_PAGE;
	else
		pte |= ARM_LPAE_PTE_TYPE_BLOCK;

	for (i = 0; i < num_entries; i++)
		ptep[i] = pte | paddr_to_iopte(paddr + i * sz, data);

	if (!cfg->coherent_walk)
		__arm_lpae_sync_pte(ptep, num_entries, cfg);
}

static int arm_panthor_lpae_init_pte(struct arm_lpae_io_pgtable *data,
				     unsigned long iova, phys_addr_t paddr,
				     arm_lpae_iopte prot, int lvl,
				     int num_entries, arm_lpae_iopte *ptep)
{
	int i;

	for (i = 0; i < num_entries; i++)
		if (iopte_leaf(ptep[i], lvl, data->iop.fmt)) {
			/* We require an unmap first */
			WARN_ON(!selftest_running);
			return -EEXIST;
		} else if (iopte_type(ptep[i]) == ARM_LPAE_PTE_TYPE_TABLE) {
			/*
			 * We need to unmap and free the old table before
			 * overwriting it with a block entry.
			 */
			arm_lpae_iopte *tblp;
			size_t sz = ARM_LPAE_BLOCK_SIZE(lvl, data);

			tblp = ptep - ARM_LPAE_LVL_IDX(iova, lvl, data);
			if (__arm_lpae_unmap(data, NULL, iova + i * sz, sz, 1,
					     lvl, tblp) != sz) {
				WARN_ON(1);
				return -EINVAL;
			}
		}

	return __arm_panthor_lpae_init_pte(data, paddr, prot, lvl, num_entries,
					   ptep);
}

static int arm_lpae_init_pte(struct arm_lpae_io_pgtable *data,
			     unsigned long iova, phys_addr_t paddr,
			     arm_lpae_iopte prot, int lvl, int num_entries,
			     arm_lpae_iopte *ptep)
{
	int i;

	for (i = 0; i < num_entries; i++)
		if (iopte_leaf(ptep[i], lvl, data->iop.fmt)) {
			/* We require an unmap first */
			WARN_ON(!selftest_running);
			return -EEXIST;
		} else if (iopte_type(ptep[i]) == ARM_LPAE_PTE_TYPE_TABLE) {
			/*
			 * We need to unmap and free the old table before
			 * overwriting it with a block entry.
			 */
			arm_lpae_iopte *tblp;
			size_t sz = ARM_LPAE_BLOCK_SIZE(lvl, data);

			tblp = ptep - ARM_LPAE_LVL_IDX(iova, lvl, data);
			if (__arm_lpae_unmap(data, NULL, iova + i * sz, sz, 1,
					     lvl, tblp) != sz) {
				WARN_ON(1);
				return -EINVAL;
			}
		}

	__arm_lpae_init_pte(data, paddr, prot, lvl, num_entries, ptep);
	return 0;
}

static arm_lpae_iopte arm_lpae_install_table(arm_lpae_iopte *table,
					     arm_lpae_iopte *ptep,
					     arm_lpae_iopte curr,
					     struct arm_lpae_io_pgtable *data)
{
	arm_lpae_iopte old, new;
	struct io_pgtable_cfg *cfg = &data->iop.cfg;
	new = paddr_to_iopte(__pa(table), data) | ARM_LPAE_PTE_TYPE_TABLE;
	if (cfg->quirks & IO_PGTABLE_QUIRK_ARM_NS)
		new |= ARM_LPAE_PTE_NSTABLE;

	/*
	 * Ensure the table itself is visible before its PTE can be.
	 * Whilst we could get away with cmpxchg64_release below, this
	 * doesn't have any ordering semantics when !CONFIG_SMP.
	 */
	dma_wmb();

	old = cmpxchg64_relaxed(ptep, curr, new);

	if (cfg->coherent_walk || (old & ARM_LPAE_PTE_SW_SYNC))
		return old;

	/* Even if it's not ours, there's no point waiting; just kick it */
	__arm_lpae_sync_pte(ptep, 1, cfg);
	if (old == curr)
		WRITE_ONCE(*ptep, new | ARM_LPAE_PTE_SW_SYNC);

	return old;
}

static int
arm_panthor_lpae_install_table(arm_lpae_iopte *table, arm_lpae_iopte *ptep,
			       arm_lpae_iopte curr,
			       struct arm_lpae_io_pgtable *data,
			       arm_lpae_iopte *oldp)
{
	//panthor install table
	//L1HPA=HVC(L1GPA)
	struct io_pgtable_cfg *cfg = &data->iop.cfg;
	arm_lpae_iopte panthor_old, panthor_new;
	phys_addr_t gpa = __pa(table);
	phys_addr_t hpa;
	int ret;
	bool timing = panthor_pt_timing_active();
	u64 total_start = panthor_pt_timing_start(timing);
	u64 phase_start;

	phase_start = panthor_pt_timing_start(timing);
	ret = panthor_gpa_to_hpa(data, gpa, &hpa);
	panthor_pt_timing_accum(&data->panthor_stats.install_table_translate_ns,
				phase_start);
	if (ret)
		goto out_record_total;

	//gpa->gpa
	//gva->(arm_lpae_iopte *)table
	//hpa->hpa
	//map hpa->gva

	//build new pte
	panthor_new = paddr_to_iopte(hpa, data) | ARM_LPAE_PTE_TYPE_TABLE;

	if (cfg->quirks & IO_PGTABLE_QUIRK_ARM_NS)
		panthor_new |= ARM_LPAE_PTE_NSTABLE;
	dma_wmb();
	phase_start = panthor_pt_timing_start(timing);
	ret = store_mapping(data, panthor_new & ARM_LPAE_PTE_ADDR_MASK, (u64)table);
	panthor_pt_timing_accum(&data->panthor_stats.install_table_store_ns,
				phase_start);
	if (ret) {
		pr_err("[MZH][TABLE] store mapping failed ret=%d gpa=%pa hpa=%pa table=%px\n",
		       ret, &gpa, &hpa, table);
		goto out_record_total;
	}
	//cptep=L0[ptep]
	panthor_old = cmpxchg64_relaxed(ptep, curr, panthor_new);
	if (panthor_old != curr)
		remove_mapping(data, panthor_new & ARM_LPAE_PTE_ADDR_MASK);
	*oldp = panthor_old;
	if (cfg->coherent_walk || (panthor_old & ARM_LPAE_PTE_SW_SYNC))
		goto out_record_total;
	phase_start = panthor_pt_timing_start(timing);
	__arm_lpae_sync_pte(ptep, 1, cfg);
	if (panthor_old == curr)
		WRITE_ONCE(*ptep, panthor_new | ARM_LPAE_PTE_SW_SYNC);
	panthor_pt_timing_accum(&data->panthor_stats.install_table_sync_ns,
				phase_start);
out_record_total:
	if (timing)
		data->panthor_stats.install_table_calls++;
	panthor_pt_timing_accum_max(&data->panthor_stats.install_table_total_ns,
				    &data->panthor_stats.install_table_max_ns,
				    total_start);
	return ret;
}

static int __arm_lpae_map(struct arm_lpae_io_pgtable *data, unsigned long iova,
			  phys_addr_t paddr, size_t size, size_t pgcount,
			  arm_lpae_iopte prot, int lvl, arm_lpae_iopte *ptep,
			  gfp_t gfp, size_t *mapped)
{
	arm_lpae_iopte *cptep, pte;
	size_t block_size = ARM_LPAE_BLOCK_SIZE(lvl, data);
	size_t tblsz = ARM_LPAE_GRANULE(data);
	struct io_pgtable_cfg *cfg = &data->iop.cfg;
	int ret = 0, num_entries, max_entries, map_idx_start;

	/* Find our entry at the current level */
	map_idx_start = ARM_LPAE_LVL_IDX(iova, lvl, data);
	ptep += map_idx_start;

	/* If we can install a leaf entry at this level, then do so */
	if (size == block_size) {
		max_entries = ARM_LPAE_PTES_PER_TABLE(data) - map_idx_start;
		num_entries = min_t(int, pgcount, max_entries);
		ret = arm_lpae_init_pte(data, iova, paddr, prot, lvl,
					num_entries, ptep);
		if (!ret)
			*mapped += num_entries * size;

		return ret;
	}

	/* We can't allocate tables at the final level */
	if (WARN_ON(lvl >= ARM_LPAE_MAX_LEVELS - 1))
		return -EINVAL;

	/* Grab a pointer to the next level */
	pte = READ_ONCE(*ptep);
	if (!pte) {
		cptep = __arm_lpae_alloc_pages(tblsz, gfp, cfg,
					       data->iop.cookie);
		if (!cptep)
			return -ENOMEM;

		pte = arm_lpae_install_table(cptep, ptep, 0, data);
		if (pte)
			__arm_lpae_free_pages(cptep, tblsz, cfg,
					      data->iop.cookie);
	} else if (!cfg->coherent_walk && !(pte & ARM_LPAE_PTE_SW_SYNC)) {
		__arm_lpae_sync_pte(ptep, 1, cfg);
	}

	if (pte && !iopte_leaf(pte, lvl, data->iop.fmt)) {
		cptep = iopte_deref(pte, data);
	} else if (pte) {
		/* We require an unmap first */
		WARN_ON(!selftest_running);
		return -EEXIST;
	}

	/* Rinse, repeat */
	return __arm_lpae_map(data, iova, paddr, size, pgcount, prot, lvl + 1,
			      cptep, gfp, mapped);
}

static int __arm_panthor_lpae_map(struct arm_lpae_io_pgtable *data,
				  unsigned long iova, phys_addr_t paddr,
				  size_t size, size_t pgcount,
				  arm_lpae_iopte prot, int lvl,
				  arm_lpae_iopte *ptep, gfp_t gfp,
				  size_t *mapped)
{
	arm_lpae_iopte *cptep, pte;
	size_t block_size = ARM_LPAE_BLOCK_SIZE(lvl, data);
	size_t tblsz = ARM_LPAE_GRANULE(data);
	struct io_pgtable_cfg *cfg = &data->iop.cfg;
	int ret = 0, num_entries, max_entries, map_idx_start;
	/* Find our entry at the current level */
	map_idx_start = ARM_LPAE_LVL_IDX(iova, lvl, data);
	ptep += map_idx_start;
	/* If we can install a leaf entry at this level, then do so */
	if (size == block_size) {
		if (block_size == SZ_2M) {
			size_t mapped_2m = 0;

			max_entries = ARM_LPAE_PTES_PER_TABLE(data) -
				      map_idx_start;
			num_entries = min_t(int, pgcount, max_entries);
			ret = arm_panthor_lpae_init_2m_blocks_or_pages(
				data, iova, paddr, prot, lvl, num_entries, ptep,
				gfp, &mapped_2m);
			*mapped += mapped_2m;

			return ret;
		}

		max_entries = ARM_LPAE_PTES_PER_TABLE(data) - map_idx_start;
		num_entries = min_t(int, pgcount, max_entries);
		ret = arm_panthor_lpae_init_pte(data, iova, paddr, prot, lvl,
						num_entries, ptep);
		if (!ret)
			*mapped += num_entries * size;

		return ret;
	}

	/* We can't allocate tables at the final level */
	if (WARN_ON(lvl >= ARM_LPAE_MAX_LEVELS - 1))
		return -EINVAL;

	/* Grab a pointer to the next level */
	pte = READ_ONCE(*ptep);
	if (!pte) {
		cptep = __arm_lpae_alloc_pages(tblsz, gfp, cfg,
					       data->iop.cookie);
		pr_debug("[MZH][alloc table pte][gva]:%llx\n", (u64)cptep);
		if (!cptep)
			return -ENOMEM;
		ret = arm_panthor_lpae_install_table(cptep, ptep, 0, data,
						     &pte);
		if (ret) {
			__arm_lpae_free_pages(cptep, tblsz, cfg,
					      data->iop.cookie);
			return ret;
		}
		if (pte) {
			__arm_lpae_free_pages(cptep, tblsz, cfg,
					      data->iop.cookie);
		}
	} else if (!cfg->coherent_walk && !(pte & ARM_LPAE_PTE_SW_SYNC)) {
		__arm_lpae_sync_pte(ptep, 1, cfg);
	}

	if (pte && !iopte_leaf(pte, lvl, data->iop.fmt)) {
		// hpa = iopte_to_paddr(pte, data);
		cptep = panthor_table_from_pte(data, pte, lvl, iova, "map");
		if (!cptep)
			return -EINVAL;
	} else if (pte) {
		/* We require an unmap first */
		WARN_ON(!selftest_running);
		return -EEXIST;
	}

	/* Rinse, repeat */
	return __arm_panthor_lpae_map(data, iova, paddr, size, pgcount, prot,
				      lvl + 1, cptep, gfp, mapped);
}

static arm_lpae_iopte arm_lpae_prot_to_pte(struct arm_lpae_io_pgtable *data,
					   int prot)
{
	arm_lpae_iopte pte;

	if (data->iop.fmt == ARM_64_LPAE_S1 ||
	    data->iop.fmt == ARM_64_PANATHOR_LPAE_S1 ||
	    data->iop.fmt == ARM_32_LPAE_S1) {
		pte = ARM_LPAE_PTE_nG;
		if (!(prot & IOMMU_WRITE) && (prot & IOMMU_READ))
			pte |= ARM_LPAE_PTE_AP_RDONLY;
		else if (data->iop.cfg.quirks & IO_PGTABLE_QUIRK_ARM_HD)
			pte |= ARM_LPAE_PTE_DBM;
		if (!(prot & IOMMU_PRIV))
			pte |= ARM_LPAE_PTE_AP_UNPRIV;
	} else {
		pte = ARM_LPAE_PTE_HAP_FAULT;
		if (prot & IOMMU_READ)
			pte |= ARM_LPAE_PTE_HAP_READ;
		if (prot & IOMMU_WRITE)
			pte |= ARM_LPAE_PTE_HAP_WRITE;
	}
	/*
	 * Note that this logic is structured to accommodate Mali LPAE
	 * having stage-1-like attributes but stage-2-like permissions.
	 */
	if (data->iop.fmt == ARM_64_LPAE_S2 ||
	    data->iop.fmt == ARM_32_LPAE_S2) {
		if (prot & IOMMU_MMIO)
			pte |= ARM_LPAE_PTE_MEMATTR_DEV;
		else if (prot & IOMMU_CACHE)
			pte |= ARM_LPAE_PTE_MEMATTR_OIWB;
		else
			pte |= ARM_LPAE_PTE_MEMATTR_NC;
	} else {
		if (prot & IOMMU_MMIO)
			pte |= (ARM_LPAE_MAIR_ATTR_IDX_DEV
				<< ARM_LPAE_PTE_ATTRINDX_SHIFT);
		else if (prot & IOMMU_CACHE)
			pte |= (ARM_LPAE_MAIR_ATTR_IDX_CACHE
				<< ARM_LPAE_PTE_ATTRINDX_SHIFT);
	}

	/*
	 * Also Mali has its own notions of shareability wherein its Inner
	 * domain covers the cores within the GPU, and its Outer domain is
	 * "outside the GPU" (i.e. either the Inner or System domain in CPU
	 * terms, depending on coherency).
	 */
	if (prot & IOMMU_CACHE && data->iop.fmt != ARM_MALI_LPAE)
		pte |= ARM_LPAE_PTE_SH_IS;
	else
		pte |= ARM_LPAE_PTE_SH_OS;

	if (prot & IOMMU_NOEXEC)
		pte |= ARM_LPAE_PTE_XN;

	if (data->iop.cfg.quirks & IO_PGTABLE_QUIRK_ARM_NS)
		pte |= ARM_LPAE_PTE_NS;

	if (data->iop.fmt != ARM_MALI_LPAE)
		pte |= ARM_LPAE_PTE_AF;

	return pte;
}

static int arm_panthor_lpae_map_pages(struct io_pgtable_ops *ops,
				      unsigned long iova, phys_addr_t paddr,
				      size_t pgsize, size_t pgcount,
				      int iommu_prot, gfp_t gfp, size_t *mapped)
{
	struct arm_lpae_io_pgtable *data = io_pgtable_ops_to_data(ops);
	struct io_pgtable_cfg *cfg = &data->iop.cfg;
	arm_lpae_iopte *ptep = data->pgd;
	int ret, lvl = data->start_level;
	arm_lpae_iopte prot;
	long iaext = (s64)iova >> cfg->ias;

	if (WARN_ON(!pgsize || (pgsize & cfg->pgsize_bitmap) != pgsize))
		return -EINVAL;

	if (cfg->quirks & IO_PGTABLE_QUIRK_ARM_TTBR1)
		iaext = ~iaext;
	if (WARN_ON(iaext || paddr >> cfg->oas))
		return -ERANGE;

	if (!(iommu_prot & (IOMMU_READ | IOMMU_WRITE)))
		return -EINVAL;

	prot = arm_lpae_prot_to_pte(data, iommu_prot);
	pr_debug("[MZH][PANTHOR_MAP] iova=%lx paddr=%pa pgsize=%zx pgcount=%zu iommu_prot=%x pte_prot=%llx\n",
		 iova, &paddr, pgsize, pgcount, iommu_prot, prot);
	ret = __arm_panthor_lpae_map(data, iova, paddr, pgsize, pgcount, prot,
				     lvl, ptep, gfp, mapped);
	/*
	 * Synchronise all PTE updates for the new mapping before there's
	 * a chance for anything to kick off a table walk for the new iova.
	 */
	wmb();

	return ret;
}

static int arm_lpae_map_pages(struct io_pgtable_ops *ops, unsigned long iova,
			      phys_addr_t paddr, size_t pgsize, size_t pgcount,
			      int iommu_prot, gfp_t gfp, size_t *mapped)
{
	struct arm_lpae_io_pgtable *data = io_pgtable_ops_to_data(ops);
	struct io_pgtable_cfg *cfg = &data->iop.cfg;
	arm_lpae_iopte *ptep = data->pgd;
	int ret, lvl = data->start_level;
	arm_lpae_iopte prot;
	long iaext = (s64)iova >> cfg->ias;

	if (WARN_ON(!pgsize || (pgsize & cfg->pgsize_bitmap) != pgsize))
		return -EINVAL;

	if (cfg->quirks & IO_PGTABLE_QUIRK_ARM_TTBR1)
		iaext = ~iaext;
	if (WARN_ON(iaext || paddr >> cfg->oas))
		return -ERANGE;

	if (!(iommu_prot & (IOMMU_READ | IOMMU_WRITE)))
		return -EINVAL;

	prot = arm_lpae_prot_to_pte(data, iommu_prot);
	ret = __arm_lpae_map(data, iova, paddr, pgsize, pgcount, prot, lvl,
			     ptep, gfp, mapped);
	/*
	 * Synchronise all PTE updates for the new mapping before there's
	 * a chance for anything to kick off a table walk for the new iova.
	 */
	wmb();

	return ret;
}

static void __arm_lpae_free_pgtable(struct arm_lpae_io_pgtable *data, int lvl,
				    arm_lpae_iopte *ptep)
{
	arm_lpae_iopte *start, *end;
	unsigned long table_size;

	if (lvl == data->start_level)
		table_size = ARM_LPAE_PGD_SIZE(data);
	else
		table_size = ARM_LPAE_GRANULE(data);

	start = ptep;

	/* Only leaf entries at the last level */
	if (lvl == ARM_LPAE_MAX_LEVELS - 1)
		end = ptep;
	else
		end = (void *)ptep + table_size;

	while (ptep != end) {
		arm_lpae_iopte pte = *ptep++;

		if (!pte || iopte_leaf(pte, lvl, data->iop.fmt))
			continue;

		__arm_lpae_free_pgtable(data, lvl + 1, iopte_deref(pte, data));
	}

	__arm_lpae_free_pages(start, table_size, &data->iop.cfg,
			      data->iop.cookie);
}

static void __arm_panthor_lpae_free_pgtable(struct arm_lpae_io_pgtable *data,
					    int lvl, arm_lpae_iopte *ptep)
{
	arm_lpae_iopte *start, *end;
	unsigned long table_size;
	phys_addr_t hpa = 0;
	int ret;

	if (lvl == data->start_level)
		table_size = ARM_LPAE_PGD_SIZE(data);
	else
		table_size = ARM_LPAE_GRANULE(data);

	start = ptep;
	if (lvl != data->start_level) {
		ret = panthor_gpa_to_hpa(data, __pa(start), &hpa);
		if (ret)
			pr_err("[MZH][PTFREE] failed to translate table %px ret=%d\n",
			       start, ret);
	}

	/* Only leaf entries at the last level */
	if (lvl == ARM_LPAE_MAX_LEVELS - 1)
		end = ptep;
	else
		end = (void *)ptep + table_size;

	while (ptep != end) {
		arm_lpae_iopte pte = *ptep++;

		if (!pte || iopte_leaf(pte, lvl, data->iop.fmt))
			continue;

		arm_lpae_iopte *child =
			panthor_table_from_pte(data, pte, lvl, 0, "free");
		if (!child)
			continue;

		__arm_panthor_lpae_free_pgtable(
			data, lvl + 1, child);
	}

	if (hpa)
		remove_mapping(data, hpa & ARM_LPAE_PTE_ADDR_MASK);
	__arm_lpae_free_pages(start, table_size, &data->iop.cfg,
			      data->iop.cookie);
}

static void arm_lpae_free_pgtable(struct io_pgtable *iop)
{
	struct arm_lpae_io_pgtable *data = io_pgtable_to_data(iop);

	__arm_lpae_free_pgtable(data, data->start_level, data->pgd);
	kfree(data);
}

static void arm_panthor_lpae_free_pgtable(struct io_pgtable *iop)
{
	struct arm_lpae_io_pgtable *data = io_pgtable_to_data(iop);
	struct panthor_passthrough_stats *stats = &data->panthor_stats;
	bool timing = panthor_pt_timing_active();

	__arm_panthor_lpae_free_pgtable(data, data->start_level, data->pgd);
	if (timing && (stats->gpa2hpa_batch_calls || stats->map_2m_attempts))
		pr_info("[MZH][PANTHOR_PT_STATS] gpa2hpa_batches=%llu entries=%llu hvc_calls=%llu hvc_entries=%llu cache_hits=%llu cache_misses=%llu 2m_attempts=%llu 2m_blocks=%llu 2m_fallback_tables=%llu 2m_hpa_unaligned=%llu 2m_hpa_discontig=%llu\n",
			stats->gpa2hpa_batch_calls, stats->gpa2hpa_entries,
			stats->gpa2hpa_hvc_calls, stats->gpa2hpa_hvc_entries,
			stats->gpa2hpa_cache_hits,
			stats->gpa2hpa_cache_misses, stats->map_2m_attempts,
			stats->map_2m_blocks, stats->map_2m_fallback_tables,
			stats->map_2m_hpa_unaligned,
			stats->map_2m_hpa_discontig);
	if (timing && stats->pt_timing_samples) {
		pr_info("[MZH][PANTHOR_PT_TIMING] samples=%llu gpa2hpa_total_ns=%llu gpa2hpa_max_ns=%llu gpa2hpa_lock_wait_ns=%llu gpa2hpa_scan_ns=%llu gpa2hpa_prep_ns=%llu gpa2hpa_hvc_ns=%llu gpa2hpa_hvc_max_ns=%llu gpa2hpa_result_ns=%llu install_calls=%llu install_total_ns=%llu install_max_ns=%llu install_translate_ns=%llu install_store_ns=%llu install_sync_ns=%llu\n",
			stats->pt_timing_samples,
			stats->gpa2hpa_total_ns, stats->gpa2hpa_max_ns,
			stats->gpa2hpa_lock_wait_ns, stats->gpa2hpa_scan_ns,
			stats->gpa2hpa_prep_ns, stats->gpa2hpa_hvc_ns,
			stats->gpa2hpa_hvc_max_ns,
			stats->gpa2hpa_result_ns, stats->install_table_calls,
			stats->install_table_total_ns,
			stats->install_table_max_ns,
			stats->install_table_translate_ns,
			stats->install_table_store_ns,
			stats->install_table_sync_ns);
		pr_info("[MZH][PANTHOR_PT_TIMING] map_2m_total_ns=%llu map_2m_max_ns=%llu map_2m_alloc_hpas_ns=%llu map_2m_translate_ns=%llu map_2m_check_ns=%llu map_2m_block_install_ns=%llu map_2m_fallback_alloc_ns=%llu map_2m_fallback_fill_ns=%llu map_2m_fallback_sync_ns=%llu map_2m_fallback_install_ns=%llu map_2m_free_hpas_ns=%llu\n",
			stats->map_2m_total_ns, stats->map_2m_max_ns,
			stats->map_2m_alloc_hpas_ns,
			stats->map_2m_translate_ns, stats->map_2m_check_ns,
			stats->map_2m_block_install_ns,
			stats->map_2m_fallback_alloc_ns,
			stats->map_2m_fallback_fill_ns,
			stats->map_2m_fallback_sync_ns,
			stats->map_2m_fallback_install_ns,
			stats->map_2m_free_hpas_ns);
		pr_info("[MZH][PANTHOR_PT_TIMING] init_pte_total_ns=%llu init_pte_max_ns=%llu init_pte_alloc_hpas_ns=%llu init_pte_translate_ns=%llu init_pte_fill_ns=%llu init_pte_sync_ns=%llu init_pte_free_hpas_ns=%llu\n",
			stats->init_pte_total_ns, stats->init_pte_max_ns,
			stats->init_pte_alloc_hpas_ns,
			stats->init_pte_translate_ns, stats->init_pte_fill_ns,
			stats->init_pte_sync_ns,
			stats->init_pte_free_hpas_ns);
	}
	panthor_gpa_hpa_cache_free(data);
	panthor_realm_free_gpa2hpa_page(data->panthor_gpa2hpa_page);
	kfree(data);
}

static size_t arm_lpae_split_blk_unmap(struct arm_lpae_io_pgtable *data,
				       struct iommu_iotlb_gather *gather,
				       unsigned long iova, size_t size,
				       arm_lpae_iopte blk_pte, int lvl,
				       arm_lpae_iopte *ptep, size_t pgcount)
{
	struct io_pgtable_cfg *cfg = &data->iop.cfg;
	arm_lpae_iopte pte, *tablep;
	phys_addr_t blk_paddr;
	size_t tablesz = ARM_LPAE_GRANULE(data);
	size_t split_sz = ARM_LPAE_BLOCK_SIZE(lvl, data);
	int ptes_per_table = ARM_LPAE_PTES_PER_TABLE(data);
	int i, unmap_idx_start = -1, num_entries = 0, max_entries;

	if (WARN_ON(lvl == ARM_LPAE_MAX_LEVELS))
		return 0;

	tablep = __arm_lpae_alloc_pages(tablesz, GFP_ATOMIC, cfg,
					data->iop.cookie);
	if (!tablep)
		return 0; /* Bytes unmapped */

	if (size == split_sz) {
		unmap_idx_start = ARM_LPAE_LVL_IDX(iova, lvl, data);
		max_entries = ptes_per_table - unmap_idx_start;
		num_entries = min_t(int, pgcount, max_entries);
	}

	blk_paddr = iopte_to_paddr(blk_pte, data);
	pte = iopte_prot(blk_pte);

	for (i = 0; i < ptes_per_table; i++, blk_paddr += split_sz) {
		/* Unmap! */
		if (i >= unmap_idx_start && i < (unmap_idx_start + num_entries))
			continue;

		__arm_lpae_init_pte(data, blk_paddr, pte, lvl, 1, &tablep[i]);
	}

	pte = arm_lpae_install_table(tablep, ptep, blk_pte, data);
	if (pte != blk_pte) {
		__arm_lpae_free_pages(tablep, tablesz, cfg, data->iop.cookie);
		/*
		 * We may race against someone unmapping another part of this
		 * block, but anything else is invalid. We can't misinterpret
		 * a page entry here since we're never at the last level.
		 */
		if (iopte_type(pte) != ARM_LPAE_PTE_TYPE_TABLE)
			return 0;

		tablep = iopte_deref(pte, data);
	} else if (unmap_idx_start >= 0) {
		if (gather)
			for (i = 0; i < num_entries; i++)
				io_pgtable_tlb_add_page(&data->iop, gather,
							iova + i * size, size);

		return num_entries * size;
	}

	return __arm_lpae_unmap(data, gather, iova, size, pgcount, lvl, tablep);
}

static size_t arm_panthor_lpae_split_blk_unmap(
	struct arm_lpae_io_pgtable *data, struct iommu_iotlb_gather *gather,
	unsigned long iova, size_t size, arm_lpae_iopte blk_pte, int lvl,
	arm_lpae_iopte *ptep, size_t pgcount)
{
	struct io_pgtable_cfg *cfg = &data->iop.cfg;
	arm_lpae_iopte pte, *tablep, old;
	phys_addr_t blk_paddr;
	size_t tablesz = ARM_LPAE_GRANULE(data);
	size_t split_sz = ARM_LPAE_BLOCK_SIZE(lvl, data);
	int ptes_per_table = ARM_LPAE_PTES_PER_TABLE(data);
	int i, unmap_idx_start = -1, num_entries = 0, max_entries;
	int ret;

	if (WARN_ON(lvl == ARM_LPAE_MAX_LEVELS))
		return 0;

	tablep = __arm_lpae_alloc_pages(tablesz, GFP_ATOMIC, cfg,
					data->iop.cookie);
	if (!tablep)
		return 0;

	if (size == split_sz) {
		unmap_idx_start = ARM_LPAE_LVL_IDX(iova, lvl, data);
		max_entries = ptes_per_table - unmap_idx_start;
		num_entries = min_t(int, pgcount, max_entries);
	}

	blk_paddr = iopte_to_paddr(blk_pte, data);
	pte = iopte_prot(blk_pte);

	for (i = 0; i < ptes_per_table; i++, blk_paddr += split_sz) {
		if (i >= unmap_idx_start && i < (unmap_idx_start + num_entries))
			continue;

		__arm_lpae_init_pte(data, blk_paddr, pte, lvl, 1, &tablep[i]);
	}

	ret = arm_panthor_lpae_install_table(tablep, ptep, blk_pte, data,
					     &old);
	if (ret) {
		__arm_lpae_free_pages(tablep, tablesz, cfg, data->iop.cookie);
		return 0;
	}

	if (old != blk_pte) {
		__arm_lpae_free_pages(tablep, tablesz, cfg, data->iop.cookie);
		if (iopte_type(old) != ARM_LPAE_PTE_TYPE_TABLE)
			return 0;

		tablep = panthor_table_from_pte(data, old, lvl, iova,
						"split-unmap");
		if (!tablep)
			return 0;
	} else if (unmap_idx_start >= 0) {
		if (gather)
			for (i = 0; i < num_entries; i++)
				io_pgtable_tlb_add_page(&data->iop, gather,
							iova + i * size, size);

		return num_entries * size;
	}

	return __arm_panthor_lpae_unmap(data, gather, iova, size, pgcount,
					lvl, tablep);
}

static size_t __arm_lpae_unmap(struct arm_lpae_io_pgtable *data,
			       struct iommu_iotlb_gather *gather,
			       unsigned long iova, size_t size, size_t pgcount,
			       int lvl, arm_lpae_iopte *ptep)
{
	arm_lpae_iopte pte;
	struct io_pgtable *iop = &data->iop;
	int i = 0, num_entries, max_entries, unmap_idx_start;

	/* Something went horribly wrong and we ran out of page table */
	if (WARN_ON(lvl == ARM_LPAE_MAX_LEVELS))
		return 0;

	unmap_idx_start = ARM_LPAE_LVL_IDX(iova, lvl, data);
	ptep += unmap_idx_start;
	pte = READ_ONCE(*ptep);
	if (WARN_ON(!pte))
		return 0;

	/* If the size matches this level, we're in the right place */
	if (size == ARM_LPAE_BLOCK_SIZE(lvl, data)) {
		max_entries = ARM_LPAE_PTES_PER_TABLE(data) - unmap_idx_start;
		num_entries = min_t(int, pgcount, max_entries);

		/* Find and handle non-leaf entries */
		for (i = 0; i < num_entries; i++) {
			pte = READ_ONCE(ptep[i]);
			if (WARN_ON(!pte))
				break;

			if (!iopte_leaf(pte, lvl, iop->fmt)) {
				__arm_lpae_clear_pte(&ptep[i], &iop->cfg, 1);

				/* Also flush any partial walks */
				io_pgtable_tlb_flush_walk(
					iop, iova + i * size, size,
					ARM_LPAE_GRANULE(data));
				__arm_lpae_free_pgtable(data, lvl + 1,
							iopte_deref(pte, data));
			}
		}

		/* Clear the remaining entries */
		__arm_lpae_clear_pte(ptep, &iop->cfg, i);

		if (gather && !iommu_iotlb_gather_queued(gather))
			for (int j = 0; j < i; j++)
				io_pgtable_tlb_add_page(iop, gather,
							iova + j * size, size);

		return i * size;
	} else if (iopte_leaf(pte, lvl, iop->fmt)) {
		/*
		 * Insert a table at the next level to map the old region,
		 * minus the part we want to unmap
		 */
		return arm_lpae_split_blk_unmap(data, gather, iova, size, pte,
						lvl + 1, ptep, pgcount);
	}

	/* Keep on walkin' */
	ptep = iopte_deref(pte, data);
	return __arm_lpae_unmap(data, gather, iova, size, pgcount, lvl + 1,
				ptep);
}

static size_t __arm_panthor_lpae_unmap(struct arm_lpae_io_pgtable *data,
				       struct iommu_iotlb_gather *gather,
				       unsigned long iova, size_t size,
				       size_t pgcount, int lvl,
				       arm_lpae_iopte *ptep)
{
	arm_lpae_iopte pte;
	struct io_pgtable *iop = &data->iop;
	int i = 0, num_entries, max_entries, unmap_idx_start;

	/* Something went horribly wrong and we ran out of page table */
	if (WARN_ON(lvl == ARM_LPAE_MAX_LEVELS))
		return 0;

	unmap_idx_start = ARM_LPAE_LVL_IDX(iova, lvl, data);
	ptep += unmap_idx_start;
	pte = READ_ONCE(*ptep);
	if (WARN_ON(!pte))
		return 0;

	/* If the size matches this level, we're in the right place */
	if (size == ARM_LPAE_BLOCK_SIZE(lvl, data)) {
		max_entries = ARM_LPAE_PTES_PER_TABLE(data) - unmap_idx_start;
		num_entries = min_t(int, pgcount, max_entries);

		/* Find and handle non-leaf entries */
		for (i = 0; i < num_entries; i++) {
			pte = READ_ONCE(ptep[i]);
			if (WARN_ON(!pte))
				break;

			if (!iopte_leaf(pte, lvl, iop->fmt)) {
				arm_lpae_iopte *child;

				__arm_lpae_clear_pte(&ptep[i], &iop->cfg, 1);

				/* Also flush any partial walks */
				io_pgtable_tlb_flush_walk(
					iop, iova + i * size, size,
					ARM_LPAE_GRANULE(data));
				child = panthor_table_from_pte(data, pte, lvl,
							       iova + i * size,
							       "unmap-free");
				if (!child)
					break;
				__arm_panthor_lpae_free_pgtable(
					data, lvl + 1, child);
			}
		}

		/* Clear the remaining entries */
		__arm_lpae_clear_pte(ptep, &iop->cfg, i);

		if (gather && !iommu_iotlb_gather_queued(gather))
			for (int j = 0; j < i; j++)
				io_pgtable_tlb_add_page(iop, gather,
							iova + j * size, size);

		return i * size;
	} else if (iopte_leaf(pte, lvl, iop->fmt)) {
		/*
		 * Insert a table at the next level to map the old region,
		 * minus the part we want to unmap
		 */
		return arm_panthor_lpae_split_blk_unmap(data, gather, iova,
							size, pte, lvl + 1,
							ptep, pgcount);
	}

	/* Keep on walkin' */
	ptep = panthor_table_from_pte(data, pte, lvl, iova, "unmap");
	if (!ptep)
		return 0;
	return __arm_panthor_lpae_unmap(data, gather, iova, size, pgcount,
					lvl + 1, ptep);
}

static size_t arm_lpae_unmap_pages(struct io_pgtable_ops *ops,
				   unsigned long iova, size_t pgsize,
				   size_t pgcount,
				   struct iommu_iotlb_gather *gather)
{
	struct arm_lpae_io_pgtable *data = io_pgtable_ops_to_data(ops);
	struct io_pgtable_cfg *cfg = &data->iop.cfg;
	arm_lpae_iopte *ptep = data->pgd;
	long iaext = (s64)iova >> cfg->ias;

	if (WARN_ON(!pgsize || (pgsize & cfg->pgsize_bitmap) != pgsize ||
		    !pgcount))
		return 0;

	if (cfg->quirks & IO_PGTABLE_QUIRK_ARM_TTBR1)
		iaext = ~iaext;
	if (WARN_ON(iaext))
		return 0;

	return __arm_lpae_unmap(data, gather, iova, pgsize, pgcount,
				data->start_level, ptep);
}

static size_t arm_panthor_lpae_unmap_pages(struct io_pgtable_ops *ops,
					   unsigned long iova, size_t pgsize,
					   size_t pgcount,
					   struct iommu_iotlb_gather *gather)
{
	struct arm_lpae_io_pgtable *data = io_pgtable_ops_to_data(ops);
	struct io_pgtable_cfg *cfg = &data->iop.cfg;
	arm_lpae_iopte *ptep = data->pgd;
	long iaext = (s64)iova >> cfg->ias;

	if (WARN_ON(!pgsize || (pgsize & cfg->pgsize_bitmap) != pgsize ||
		    !pgcount))
		return 0;

	if (cfg->quirks & IO_PGTABLE_QUIRK_ARM_TTBR1)
		iaext = ~iaext;
	if (WARN_ON(iaext))
		return 0;

	return __arm_panthor_lpae_unmap(data, gather, iova, pgsize, pgcount,
					data->start_level, ptep);
}

static phys_addr_t arm_lpae_iova_to_phys(struct io_pgtable_ops *ops,
					 unsigned long iova)
{
	struct arm_lpae_io_pgtable *data = io_pgtable_ops_to_data(ops);
	arm_lpae_iopte pte, *ptep = data->pgd;
	int lvl = data->start_level;

	do {
		/* Valid IOPTE pointer? */
		if (!ptep)
			return 0;

		/* Grab the IOPTE we're interested in */
		ptep += ARM_LPAE_LVL_IDX(iova, lvl, data);
		pte = READ_ONCE(*ptep);

		/* Valid entry? */
		if (!pte)
			return 0;

		/* Leaf entry? */
		if (iopte_leaf(pte, lvl, data->iop.fmt))
			goto found_translation;

		/* Take it to the next level */
		ptep = iopte_deref(pte, data);
	} while (++lvl < ARM_LPAE_MAX_LEVELS);

	/* Ran out of page tables to walk */
	return 0;

found_translation:
	iova &= (ARM_LPAE_BLOCK_SIZE(lvl, data) - 1);
	return iopte_to_paddr(pte, data) | iova;
}

static phys_addr_t arm_panthor_lpae_iova_to_phys(struct io_pgtable_ops *ops,
						 unsigned long iova)
{
	struct arm_lpae_io_pgtable *data = io_pgtable_ops_to_data(ops);
	arm_lpae_iopte pte, *ptep = data->pgd;
	int lvl = data->start_level;

	do {
		/* Valid IOPTE pointer? */
		if (!ptep)
			return 0;

		/* Grab the IOPTE we're interested in */
		ptep += ARM_LPAE_LVL_IDX(iova, lvl, data);
		pte = READ_ONCE(*ptep);

		/* Valid entry? */
		if (!pte)
			return 0;

		/* Leaf entry? */
		if (iopte_leaf(pte, lvl, data->iop.fmt))
			goto found_translation;

		/* Take it to the next level */
		ptep = panthor_table_from_pte(data, pte, lvl, iova,
					      "iova_to_phys");
	} while (++lvl < ARM_LPAE_MAX_LEVELS);

	/* Ran out of page tables to walk */
	return 0;

found_translation:
	iova &= (ARM_LPAE_BLOCK_SIZE(lvl, data) - 1);
	return iopte_to_paddr(pte, data) | iova;
}

struct io_pgtable_walk_data {
	struct iommu_dirty_bitmap *dirty;
	unsigned long flags;
	u64 addr;
	const u64 end;
};

static int __arm_lpae_iopte_walk_dirty(struct arm_lpae_io_pgtable *data,
				       struct io_pgtable_walk_data *walk_data,
				       arm_lpae_iopte *ptep, int lvl);

static int io_pgtable_visit_dirty(struct arm_lpae_io_pgtable *data,
				  struct io_pgtable_walk_data *walk_data,
				  arm_lpae_iopte *ptep, int lvl)
{
	struct io_pgtable *iop = &data->iop;
	arm_lpae_iopte pte = READ_ONCE(*ptep);

	if (iopte_leaf(pte, lvl, iop->fmt)) {
		size_t size = ARM_LPAE_BLOCK_SIZE(lvl, data);

		if (iopte_writeable_dirty(pte)) {
			iommu_dirty_bitmap_record(walk_data->dirty,
						  walk_data->addr, size);
			if (!(walk_data->flags & IOMMU_DIRTY_NO_CLEAR))
				iopte_set_writeable_clean(ptep);
		}
		walk_data->addr += size;
		return 0;
	}

	if (WARN_ON(!iopte_table(pte, lvl)))
		return -EINVAL;

	ptep = iopte_deref(pte, data);
	return __arm_lpae_iopte_walk_dirty(data, walk_data, ptep, lvl + 1);
}

static int __arm_lpae_iopte_walk_dirty(struct arm_lpae_io_pgtable *data,
				       struct io_pgtable_walk_data *walk_data,
				       arm_lpae_iopte *ptep, int lvl)
{
	u32 idx;
	int max_entries, ret;

	if (WARN_ON(lvl == ARM_LPAE_MAX_LEVELS))
		return -EINVAL;

	if (lvl == data->start_level)
		max_entries = ARM_LPAE_PGD_SIZE(data) / sizeof(arm_lpae_iopte);
	else
		max_entries = ARM_LPAE_PTES_PER_TABLE(data);

	for (idx = ARM_LPAE_LVL_IDX(walk_data->addr, lvl, data);
	     (idx < max_entries) && (walk_data->addr < walk_data->end); ++idx) {
		ret = io_pgtable_visit_dirty(data, walk_data, ptep + idx, lvl);
		if (ret)
			return ret;
	}

	return 0;
}

static int arm_lpae_read_and_clear_dirty(struct io_pgtable_ops *ops,
					 unsigned long iova, size_t size,
					 unsigned long flags,
					 struct iommu_dirty_bitmap *dirty)
{
	struct arm_lpae_io_pgtable *data = io_pgtable_ops_to_data(ops);
	struct io_pgtable_cfg *cfg = &data->iop.cfg;
	struct io_pgtable_walk_data walk_data = {
		.dirty = dirty,
		.flags = flags,
		.addr = iova,
		.end = iova + size,
	};
	arm_lpae_iopte *ptep = data->pgd;
	int lvl = data->start_level;

	if (WARN_ON(!size))
		return -EINVAL;
	if (WARN_ON((iova + size - 1) & ~(BIT(cfg->ias) - 1)))
		return -EINVAL;
	if (data->iop.fmt != ARM_64_LPAE_S1)
		return -EINVAL;

	return __arm_lpae_iopte_walk_dirty(data, &walk_data, ptep, lvl);
}

static void arm_lpae_restrict_pgsizes(struct io_pgtable_cfg *cfg)
{
	unsigned long granule, page_sizes;
	unsigned int max_addr_bits = 48;

	/*
	 * We need to restrict the supported page sizes to match the
	 * translation regime for a particular granule. Aim to match
	 * the CPU page size if possible, otherwise prefer smaller sizes.
	 * While we're at it, restrict the block sizes to match the
	 * chosen granule.
	 */
	if (cfg->pgsize_bitmap & PAGE_SIZE)
		granule = PAGE_SIZE;
	else if (cfg->pgsize_bitmap & ~PAGE_MASK)
		granule = 1UL << __fls(cfg->pgsize_bitmap & ~PAGE_MASK);
	else if (cfg->pgsize_bitmap & PAGE_MASK)
		granule = 1UL << __ffs(cfg->pgsize_bitmap & PAGE_MASK);
	else
		granule = 0;

	switch (granule) {
	case SZ_4K:
		page_sizes = (SZ_4K | SZ_2M | SZ_1G);
		break;
	case SZ_16K:
		page_sizes = (SZ_16K | SZ_32M);
		break;
	case SZ_64K:
		max_addr_bits = 52;
		page_sizes = (SZ_64K | SZ_512M);
		if (cfg->oas > 48)
			page_sizes |= 1ULL << 42; /* 4TB */
		break;
	default:
		page_sizes = 0;
	}

	cfg->pgsize_bitmap &= page_sizes;
	cfg->ias = min(cfg->ias, max_addr_bits);
	cfg->oas = min(cfg->oas, max_addr_bits);
}

static struct arm_lpae_io_pgtable *
arm_lpae_alloc_pgtable(struct io_pgtable_cfg *cfg)
{
	struct arm_lpae_io_pgtable *data;
	int levels, va_bits, pg_shift;

	arm_lpae_restrict_pgsizes(cfg);

	if (!(cfg->pgsize_bitmap & (SZ_4K | SZ_16K | SZ_64K)))
		return NULL;

	if (cfg->ias > ARM_LPAE_MAX_ADDR_BITS)
		return NULL;

	if (cfg->oas > ARM_LPAE_MAX_ADDR_BITS)
		return NULL;

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return NULL;

	pg_shift = __ffs(cfg->pgsize_bitmap);
	data->bits_per_level = pg_shift - ilog2(sizeof(arm_lpae_iopte));

	va_bits = cfg->ias - pg_shift;
	levels = DIV_ROUND_UP(va_bits, data->bits_per_level);
	data->start_level = ARM_LPAE_MAX_LEVELS - levels;

	/* Calculate the actual size of our pgd (without concatenation) */
	data->pgd_bits = va_bits - (data->bits_per_level * (levels - 1));

	data->iop.ops = (struct io_pgtable_ops){
		.map_pages = arm_lpae_map_pages,
		.unmap_pages = arm_lpae_unmap_pages,
		.iova_to_phys = arm_lpae_iova_to_phys,
		.read_and_clear_dirty = arm_lpae_read_and_clear_dirty,
	};

	return data;
}

static struct io_pgtable *
arm_64_lpae_alloc_pgtable_s1(struct io_pgtable_cfg *cfg, void *cookie)
{
	u64 reg;
	struct arm_lpae_io_pgtable *data;
	typeof(&cfg->arm_lpae_s1_cfg.tcr) tcr = &cfg->arm_lpae_s1_cfg.tcr;
	bool tg1;

	if (cfg->quirks &
	    ~(IO_PGTABLE_QUIRK_ARM_NS | IO_PGTABLE_QUIRK_ARM_TTBR1 |
	      IO_PGTABLE_QUIRK_ARM_OUTER_WBWA | IO_PGTABLE_QUIRK_ARM_HD))
		return NULL;

	data = arm_lpae_alloc_pgtable(cfg);
	if (!data)
		return NULL;

	/* TCR */
	if (cfg->coherent_walk) {
		tcr->sh = ARM_LPAE_TCR_SH_IS;
		tcr->irgn = ARM_LPAE_TCR_RGN_WBWA;
		tcr->orgn = ARM_LPAE_TCR_RGN_WBWA;
		if (cfg->quirks & IO_PGTABLE_QUIRK_ARM_OUTER_WBWA)
			goto out_free_data;
	} else {
		tcr->sh = ARM_LPAE_TCR_SH_OS;
		tcr->irgn = ARM_LPAE_TCR_RGN_NC;
		if (!(cfg->quirks & IO_PGTABLE_QUIRK_ARM_OUTER_WBWA))
			tcr->orgn = ARM_LPAE_TCR_RGN_NC;
		else
			tcr->orgn = ARM_LPAE_TCR_RGN_WBWA;
	}

	tg1 = cfg->quirks & IO_PGTABLE_QUIRK_ARM_TTBR1;
	switch (ARM_LPAE_GRANULE(data)) {
	case SZ_4K:
		tcr->tg = tg1 ? ARM_LPAE_TCR_TG1_4K : ARM_LPAE_TCR_TG0_4K;
		break;
	case SZ_16K:
		tcr->tg = tg1 ? ARM_LPAE_TCR_TG1_16K : ARM_LPAE_TCR_TG0_16K;
		break;
	case SZ_64K:
		tcr->tg = tg1 ? ARM_LPAE_TCR_TG1_64K : ARM_LPAE_TCR_TG0_64K;
		break;
	}

	switch (cfg->oas) {
	case 32:
		tcr->ips = ARM_LPAE_TCR_PS_32_BIT;
		break;
	case 36:
		tcr->ips = ARM_LPAE_TCR_PS_36_BIT;
		break;
	case 40:
		tcr->ips = ARM_LPAE_TCR_PS_40_BIT;
		break;
	case 42:
		tcr->ips = ARM_LPAE_TCR_PS_42_BIT;
		break;
	case 44:
		tcr->ips = ARM_LPAE_TCR_PS_44_BIT;
		break;
	case 48:
		tcr->ips = ARM_LPAE_TCR_PS_48_BIT;
		break;
	case 52:
		tcr->ips = ARM_LPAE_TCR_PS_52_BIT;
		break;
	default:
		goto out_free_data;
	}

	tcr->tsz = 64ULL - cfg->ias;

	/* MAIRs */
	reg = (ARM_LPAE_MAIR_ATTR_NC
	       << ARM_LPAE_MAIR_ATTR_SHIFT(ARM_LPAE_MAIR_ATTR_IDX_NC)) |
	      (ARM_LPAE_MAIR_ATTR_WBRWA
	       << ARM_LPAE_MAIR_ATTR_SHIFT(ARM_LPAE_MAIR_ATTR_IDX_CACHE)) |
	      (ARM_LPAE_MAIR_ATTR_DEVICE
	       << ARM_LPAE_MAIR_ATTR_SHIFT(ARM_LPAE_MAIR_ATTR_IDX_DEV)) |
	      (ARM_LPAE_MAIR_ATTR_INC_OWBRWA
	       << ARM_LPAE_MAIR_ATTR_SHIFT(ARM_LPAE_MAIR_ATTR_IDX_INC_OCACHE));

	cfg->arm_lpae_s1_cfg.mair = reg;

	/* Looking good; allocate a pgd */
	data->pgd = __arm_lpae_alloc_pages(ARM_LPAE_PGD_SIZE(data), GFP_KERNEL,
					   cfg, cookie);
	if (!data->pgd)
		goto out_free_data;

	/* Ensure the empty pgd is visible before any actual TTBR write */
	wmb();

	/* TTBR */
	cfg->arm_lpae_s1_cfg.ttbr = virt_to_phys(data->pgd);
	return &data->iop;

out_free_data:
	kfree(data);
	return NULL;
}

static struct io_pgtable *
arm_64_panthor_lpae_alloc_pgtable_s1(struct io_pgtable_cfg *cfg, void *cookie)
{
	u64 gpa_ttbr;
	phys_addr_t ttbr_hpa;
	int ret = 0;
	u64 reg;
	struct arm_lpae_io_pgtable *data;
	typeof(&cfg->arm_lpae_s1_cfg.tcr) tcr = &cfg->arm_lpae_s1_cfg.tcr;
	bool tg1;

	if (cfg->quirks &
	    ~(IO_PGTABLE_QUIRK_ARM_NS | IO_PGTABLE_QUIRK_ARM_TTBR1 |
	      IO_PGTABLE_QUIRK_ARM_OUTER_WBWA | IO_PGTABLE_QUIRK_ARM_HD))
		return NULL;

	data = arm_lpae_alloc_pgtable(cfg);
	if (!data)
		return NULL;
	data->iop.ops = (struct io_pgtable_ops){
		.map_pages = arm_panthor_lpae_map_pages,
		.unmap_pages = arm_panthor_lpae_unmap_pages,
		.iova_to_phys = arm_panthor_lpae_iova_to_phys,
		.read_and_clear_dirty = arm_lpae_read_and_clear_dirty,
	};
	/* TCR */
	if (cfg->coherent_walk) {
		tcr->sh = ARM_LPAE_TCR_SH_IS;
		tcr->irgn = ARM_LPAE_TCR_RGN_WBWA;
		tcr->orgn = ARM_LPAE_TCR_RGN_WBWA;
		if (cfg->quirks & IO_PGTABLE_QUIRK_ARM_OUTER_WBWA)
			goto out_free_data;
	} else {
		tcr->sh = ARM_LPAE_TCR_SH_OS;
		tcr->irgn = ARM_LPAE_TCR_RGN_NC;
		if (!(cfg->quirks & IO_PGTABLE_QUIRK_ARM_OUTER_WBWA))
			tcr->orgn = ARM_LPAE_TCR_RGN_NC;
		else
			tcr->orgn = ARM_LPAE_TCR_RGN_WBWA;
	}

	tg1 = cfg->quirks & IO_PGTABLE_QUIRK_ARM_TTBR1;
	switch (ARM_LPAE_GRANULE(data)) {
	case SZ_4K:
		tcr->tg = tg1 ? ARM_LPAE_TCR_TG1_4K : ARM_LPAE_TCR_TG0_4K;
		break;
	case SZ_16K:
		tcr->tg = tg1 ? ARM_LPAE_TCR_TG1_16K : ARM_LPAE_TCR_TG0_16K;
		break;
	case SZ_64K:
		tcr->tg = tg1 ? ARM_LPAE_TCR_TG1_64K : ARM_LPAE_TCR_TG0_64K;
		break;
	}

	switch (cfg->oas) {
	case 32:
		tcr->ips = ARM_LPAE_TCR_PS_32_BIT;
		break;
	case 36:
		tcr->ips = ARM_LPAE_TCR_PS_36_BIT;
		break;
	case 40:
		tcr->ips = ARM_LPAE_TCR_PS_40_BIT;
		break;
	case 42:
		tcr->ips = ARM_LPAE_TCR_PS_42_BIT;
		break;
	case 44:
		tcr->ips = ARM_LPAE_TCR_PS_44_BIT;
		break;
	case 48:
		tcr->ips = ARM_LPAE_TCR_PS_48_BIT;
		break;
	case 52:
		tcr->ips = ARM_LPAE_TCR_PS_52_BIT;
		break;
	default:
		goto out_free_data;
	}

	tcr->tsz = 64ULL - cfg->ias;

	/* MAIRs */
	reg = (ARM_LPAE_MAIR_ATTR_NC
	       << ARM_LPAE_MAIR_ATTR_SHIFT(ARM_LPAE_MAIR_ATTR_IDX_NC)) |
	      (ARM_LPAE_MAIR_ATTR_WBRWA
	       << ARM_LPAE_MAIR_ATTR_SHIFT(ARM_LPAE_MAIR_ATTR_IDX_CACHE)) |
	      (ARM_LPAE_MAIR_ATTR_DEVICE
	       << ARM_LPAE_MAIR_ATTR_SHIFT(ARM_LPAE_MAIR_ATTR_IDX_DEV)) |
	      (ARM_LPAE_MAIR_ATTR_INC_OWBRWA
	       << ARM_LPAE_MAIR_ATTR_SHIFT(ARM_LPAE_MAIR_ATTR_IDX_INC_OCACHE));

	cfg->arm_lpae_s1_cfg.mair = reg;
	hash_init(data->panthor_table_pte_map);
	hash_init(data->panthor_gpa_hpa_map);
	mutex_init(&data->panthor_gpa2hpa_lock);
	data->panthor_gpa2hpa_page = alloc_page(GFP_KERNEL);
	if (!data->panthor_gpa2hpa_page)
		goto out_free_data;
	ret = panthor_realm_share_gpa2hpa_page(data->panthor_gpa2hpa_page);
	if (ret) {
		data->panthor_gpa2hpa_page = NULL;
		goto out_free_data;
	}
	/* Looking good; allocate a pgd */
	data->pgd = __arm_lpae_alloc_pages(ARM_LPAE_PGD_SIZE(data), GFP_KERNEL,
					   cfg, cookie);
	if (!data->pgd)
		goto out_free_data;
	/* Ensure the empty pgd is visible before any actual TTBR write */
	wmb();

	/* TTBR */
	//cfg->arm_lpae_s1_cfg.ttbr = virt_to_phys(data->pgd);
	gpa_ttbr = virt_to_phys(data->pgd);
	ret = panthor_gpa_to_hpa(data, gpa_ttbr, &ttbr_hpa);
	if (ret) {
		goto out_free_data;
	}
	pr_debug("[MZH][arm_lpae_s1_cfg.ttbr]:%llx\n", ttbr_hpa);
	cfg->arm_lpae_s1_cfg.ttbr = ttbr_hpa;
	return &data->iop;

out_free_data:
	if (data->pgd)
		__arm_lpae_free_pages(data->pgd, ARM_LPAE_PGD_SIZE(data), cfg,
				      cookie);
	panthor_gpa_hpa_cache_free(data);
	panthor_realm_free_gpa2hpa_page(data->panthor_gpa2hpa_page);
	kfree(data);
	return NULL;
}
static struct io_pgtable *
arm_64_lpae_alloc_pgtable_s2(struct io_pgtable_cfg *cfg, void *cookie)
{
	u64 sl;
	struct arm_lpae_io_pgtable *data;
	typeof(&cfg->arm_lpae_s2_cfg.vtcr) vtcr = &cfg->arm_lpae_s2_cfg.vtcr;

	/* The NS quirk doesn't apply at stage 2 */
	if (cfg->quirks)
		return NULL;

	data = arm_lpae_alloc_pgtable(cfg);
	if (!data)
		return NULL;

	/*
	 * Concatenate PGDs at level 1 if possible in order to reduce
	 * the depth of the stage-2 walk.
	 */
	if (data->start_level == 0) {
		unsigned long pgd_pages;

		pgd_pages = ARM_LPAE_PGD_SIZE(data) / sizeof(arm_lpae_iopte);
		if (pgd_pages <= ARM_LPAE_S2_MAX_CONCAT_PAGES) {
			data->pgd_bits += data->bits_per_level;
			data->start_level++;
		}
	}

	/* VTCR */
	if (cfg->coherent_walk) {
		vtcr->sh = ARM_LPAE_TCR_SH_IS;
		vtcr->irgn = ARM_LPAE_TCR_RGN_WBWA;
		vtcr->orgn = ARM_LPAE_TCR_RGN_WBWA;
	} else {
		vtcr->sh = ARM_LPAE_TCR_SH_OS;
		vtcr->irgn = ARM_LPAE_TCR_RGN_NC;
		vtcr->orgn = ARM_LPAE_TCR_RGN_NC;
	}

	sl = data->start_level;

	switch (ARM_LPAE_GRANULE(data)) {
	case SZ_4K:
		vtcr->tg = ARM_LPAE_TCR_TG0_4K;
		sl++; /* SL0 format is different for 4K granule size */
		break;
	case SZ_16K:
		vtcr->tg = ARM_LPAE_TCR_TG0_16K;
		break;
	case SZ_64K:
		vtcr->tg = ARM_LPAE_TCR_TG0_64K;
		break;
	}

	switch (cfg->oas) {
	case 32:
		vtcr->ps = ARM_LPAE_TCR_PS_32_BIT;
		break;
	case 36:
		vtcr->ps = ARM_LPAE_TCR_PS_36_BIT;
		break;
	case 40:
		vtcr->ps = ARM_LPAE_TCR_PS_40_BIT;
		break;
	case 42:
		vtcr->ps = ARM_LPAE_TCR_PS_42_BIT;
		break;
	case 44:
		vtcr->ps = ARM_LPAE_TCR_PS_44_BIT;
		break;
	case 48:
		vtcr->ps = ARM_LPAE_TCR_PS_48_BIT;
		break;
	case 52:
		vtcr->ps = ARM_LPAE_TCR_PS_52_BIT;
		break;
	default:
		goto out_free_data;
	}

	vtcr->tsz = 64ULL - cfg->ias;
	vtcr->sl = ~sl & ARM_LPAE_VTCR_SL0_MASK;

	/* Allocate pgd pages */
	data->pgd = __arm_lpae_alloc_pages(ARM_LPAE_PGD_SIZE(data), GFP_KERNEL,
					   cfg, cookie);
	if (!data->pgd)
		goto out_free_data;

	/* Ensure the empty pgd is visible before any actual TTBR write */
	wmb();

	/* VTTBR */
	cfg->arm_lpae_s2_cfg.vttbr = virt_to_phys(data->pgd);
	return &data->iop;

out_free_data:
	kfree(data);
	return NULL;
}

static struct io_pgtable *
arm_32_lpae_alloc_pgtable_s1(struct io_pgtable_cfg *cfg, void *cookie)
{
	if (cfg->ias > 32 || cfg->oas > 40)
		return NULL;

	cfg->pgsize_bitmap &= (SZ_4K | SZ_2M | SZ_1G);
	return arm_64_lpae_alloc_pgtable_s1(cfg, cookie);
}

static struct io_pgtable *
arm_32_lpae_alloc_pgtable_s2(struct io_pgtable_cfg *cfg, void *cookie)
{
	if (cfg->ias > 40 || cfg->oas > 40)
		return NULL;

	cfg->pgsize_bitmap &= (SZ_4K | SZ_2M | SZ_1G);
	return arm_64_lpae_alloc_pgtable_s2(cfg, cookie);
}

static struct io_pgtable *
arm_mali_lpae_alloc_pgtable(struct io_pgtable_cfg *cfg, void *cookie)
{
	struct arm_lpae_io_pgtable *data;

	/* No quirks for Mali (hopefully) */
	if (cfg->quirks)
		return NULL;

	if (cfg->ias > 48 || cfg->oas > 40)
		return NULL;

	cfg->pgsize_bitmap &= (SZ_4K | SZ_2M | SZ_1G);

	data = arm_lpae_alloc_pgtable(cfg);
	if (!data)
		return NULL;

	/* Mali seems to need a full 4-level table regardless of IAS */
	if (data->start_level > 0) {
		data->start_level = 0;
		data->pgd_bits = 0;
	}
	/*
	 * MEMATTR: Mali has no actual notion of a non-cacheable type, so the
	 * best we can do is mimic the out-of-tree driver and hope that the
	 * "implementation-defined caching policy" is good enough. Similarly,
	 * we'll use it for the sake of a valid attribute for our 'device'
	 * index, although callers should never request that in practice.
	 */
	cfg->arm_mali_lpae_cfg.memattr =
		(ARM_MALI_LPAE_MEMATTR_IMP_DEF
		 << ARM_LPAE_MAIR_ATTR_SHIFT(ARM_LPAE_MAIR_ATTR_IDX_NC)) |
		(ARM_MALI_LPAE_MEMATTR_WRITE_ALLOC
		 << ARM_LPAE_MAIR_ATTR_SHIFT(ARM_LPAE_MAIR_ATTR_IDX_CACHE)) |
		(ARM_MALI_LPAE_MEMATTR_IMP_DEF
		 << ARM_LPAE_MAIR_ATTR_SHIFT(ARM_LPAE_MAIR_ATTR_IDX_DEV));

	data->pgd = __arm_lpae_alloc_pages(ARM_LPAE_PGD_SIZE(data), GFP_KERNEL,
					   cfg, cookie);
	if (!data->pgd)
		goto out_free_data;

	/* Ensure the empty pgd is visible before TRANSTAB can be written */
	wmb();

	cfg->arm_mali_lpae_cfg.transtab = virt_to_phys(data->pgd) |
					  ARM_MALI_LPAE_TTBR_READ_INNER |
					  ARM_MALI_LPAE_TTBR_ADRMODE_TABLE;
	if (cfg->coherent_walk)
		cfg->arm_mali_lpae_cfg.transtab |=
			ARM_MALI_LPAE_TTBR_SHARE_OUTER;

	return &data->iop;

out_free_data:
	kfree(data);
	return NULL;
}

struct io_pgtable_init_fns io_pgtable_arm_64_lpae_s1_init_fns = {
	.caps = IO_PGTABLE_CAP_CUSTOM_ALLOCATOR,
	.alloc = arm_64_lpae_alloc_pgtable_s1,
	.free = arm_lpae_free_pgtable,
};

struct io_pgtable_init_fns io_pgtable_arm_64_panthor_lpae_s1_init_fns = {
	.caps = IO_PGTABLE_CAP_CUSTOM_ALLOCATOR,
	.alloc = arm_64_panthor_lpae_alloc_pgtable_s1,
	.free = arm_panthor_lpae_free_pgtable,
};

struct io_pgtable_init_fns io_pgtable_arm_64_lpae_s2_init_fns = {
	.caps = IO_PGTABLE_CAP_CUSTOM_ALLOCATOR,
	.alloc = arm_64_lpae_alloc_pgtable_s2,
	.free = arm_lpae_free_pgtable,
};

struct io_pgtable_init_fns io_pgtable_arm_32_lpae_s1_init_fns = {
	.caps = IO_PGTABLE_CAP_CUSTOM_ALLOCATOR,
	.alloc = arm_32_lpae_alloc_pgtable_s1,
	.free = arm_lpae_free_pgtable,
};

struct io_pgtable_init_fns io_pgtable_arm_32_lpae_s2_init_fns = {
	.caps = IO_PGTABLE_CAP_CUSTOM_ALLOCATOR,
	.alloc = arm_32_lpae_alloc_pgtable_s2,
	.free = arm_lpae_free_pgtable,
};

struct io_pgtable_init_fns io_pgtable_arm_mali_lpae_init_fns = {
	.caps = IO_PGTABLE_CAP_CUSTOM_ALLOCATOR,
	.alloc = arm_mali_lpae_alloc_pgtable,
	.free = arm_lpae_free_pgtable,
};

#ifdef CONFIG_IOMMU_IO_PGTABLE_LPAE_SELFTEST

static struct io_pgtable_cfg *cfg_cookie __initdata;

static void __init dummy_tlb_flush_all(void *cookie)
{
	WARN_ON(cookie != cfg_cookie);
}

static void __init dummy_tlb_flush(unsigned long iova, size_t size,
				   size_t granule, void *cookie)
{
	WARN_ON(cookie != cfg_cookie);
	WARN_ON(!(size & cfg_cookie->pgsize_bitmap));
}

static void __init dummy_tlb_add_page(struct iommu_iotlb_gather *gather,
				      unsigned long iova, size_t granule,
				      void *cookie)
{
	dummy_tlb_flush(iova, granule, granule, cookie);
}

static const struct iommu_flush_ops dummy_tlb_ops __initconst = {
	.tlb_flush_all = dummy_tlb_flush_all,
	.tlb_flush_walk = dummy_tlb_flush,
	.tlb_add_page = dummy_tlb_add_page,
};

static void __init arm_lpae_dump_ops(struct io_pgtable_ops *ops)
{
	struct arm_lpae_io_pgtable *data = io_pgtable_ops_to_data(ops);
	struct io_pgtable_cfg *cfg = &data->iop.cfg;

	pr_err("cfg: pgsize_bitmap 0x%lx, ias %u-bit\n", cfg->pgsize_bitmap,
	       cfg->ias);
	pr_err("data: %d levels, 0x%zx pgd_size, %u pg_shift, %u bits_per_level, pgd @ %p\n",
	       ARM_LPAE_MAX_LEVELS - data->start_level, ARM_LPAE_PGD_SIZE(data),
	       ilog2(ARM_LPAE_GRANULE(data)), data->bits_per_level, data->pgd);
}

#define __FAIL(ops, i)                                                  \
	({                                                              \
		WARN(1, "selftest: test failed for fmt idx %d\n", (i)); \
		arm_lpae_dump_ops(ops);                                 \
		selftest_running = false;                               \
		-EFAULT;                                                \
	})

static int __init arm_lpae_run_tests(struct io_pgtable_cfg *cfg)
{
	static const enum io_pgtable_fmt fmts[] __initconst = {
		ARM_64_LPAE_S1,
		ARM_64_LPAE_S2,
	};

	int i, j;
	unsigned long iova;
	size_t size, mapped;
	struct io_pgtable_ops *ops;

	selftest_running = true;

	for (i = 0; i < ARRAY_SIZE(fmts); ++i) {
		cfg_cookie = cfg;
		ops = alloc_io_pgtable_ops(fmts[i], cfg, cfg);
		if (!ops) {
			pr_err("selftest: failed to allocate io pgtable ops\n");
			return -ENOMEM;
		}

		/*
		 * Initial sanity checks.
		 * Empty page tables shouldn't provide any translations.
		 */
		if (ops->iova_to_phys(ops, 42))
			return __FAIL(ops, i);

		if (ops->iova_to_phys(ops, SZ_1G + 42))
			return __FAIL(ops, i);

		if (ops->iova_to_phys(ops, SZ_2G + 42))
			return __FAIL(ops, i);

		/*
		 * Distinct mappings of different granule sizes.
		 */
		iova = 0;
		for_each_set_bit(j, &cfg->pgsize_bitmap, BITS_PER_LONG) {
			size = 1UL << j;

			if (ops->map_pages(ops, iova, iova, size, 1,
					   IOMMU_READ | IOMMU_WRITE |
						   IOMMU_NOEXEC | IOMMU_CACHE,
					   GFP_KERNEL, &mapped))
				return __FAIL(ops, i);

			/* Overlapping mappings */
			if (!ops->map_pages(ops, iova, iova + size, size, 1,
					    IOMMU_READ | IOMMU_NOEXEC,
					    GFP_KERNEL, &mapped))
				return __FAIL(ops, i);

			if (ops->iova_to_phys(ops, iova + 42) != (iova + 42))
				return __FAIL(ops, i);

			iova += SZ_1G;
		}

		/* Partial unmap */
		size = 1UL << __ffs(cfg->pgsize_bitmap);
		if (ops->unmap_pages(ops, SZ_1G + size, size, 1, NULL) != size)
			return __FAIL(ops, i);

		/* Remap of partial unmap */
		if (ops->map_pages(ops, SZ_1G + size, size, size, 1, IOMMU_READ,
				   GFP_KERNEL, &mapped))
			return __FAIL(ops, i);

		if (ops->iova_to_phys(ops, SZ_1G + size + 42) != (size + 42))
			return __FAIL(ops, i);

		/* Full unmap */
		iova = 0;
		for_each_set_bit(j, &cfg->pgsize_bitmap, BITS_PER_LONG) {
			size = 1UL << j;

			if (ops->unmap_pages(ops, iova, size, 1, NULL) != size)
				return __FAIL(ops, i);

			if (ops->iova_to_phys(ops, iova + 42))
				return __FAIL(ops, i);

			/* Remap full block */
			if (ops->map_pages(ops, iova, iova, size, 1,
					   IOMMU_WRITE, GFP_KERNEL, &mapped))
				return __FAIL(ops, i);

			if (ops->iova_to_phys(ops, iova + 42) != (iova + 42))
				return __FAIL(ops, i);

			iova += SZ_1G;
		}

		free_io_pgtable_ops(ops);
	}

	selftest_running = false;
	return 0;
}

static int __init arm_lpae_do_selftests(void)
{
	static const unsigned long pgsize[] __initconst = {
		SZ_4K | SZ_2M | SZ_1G,
		SZ_16K | SZ_32M,
		SZ_64K | SZ_512M,
	};

	static const unsigned int ias[] __initconst = {
		32, 36, 40, 42, 44, 48,
	};

	int i, j, pass = 0, fail = 0;
	struct device dev;
	struct io_pgtable_cfg cfg = {
		.tlb = &dummy_tlb_ops,
		.oas = 48,
		.coherent_walk = true,
		.iommu_dev = &dev,
	};

	/* __arm_lpae_alloc_pages() merely needs dev_to_node() to work */
	set_dev_node(&dev, NUMA_NO_NODE);

	for (i = 0; i < ARRAY_SIZE(pgsize); ++i) {
		for (j = 0; j < ARRAY_SIZE(ias); ++j) {
			cfg.pgsize_bitmap = pgsize[i];
			cfg.ias = ias[j];
			pr_info("selftest: pgsize_bitmap 0x%08lx, IAS %u\n",
				pgsize[i], ias[j]);
			if (arm_lpae_run_tests(&cfg))
				fail++;
			else
				pass++;
		}
	}

	pr_info("selftest: completed with %d PASS %d FAIL\n", pass, fail);
	return fail ? -EFAULT : 0;
}
subsys_initcall(arm_lpae_do_selftests);
#endif
