// SPDX-License-Identifier: GPL-2.0-only
/* Privileged access to the RMM's Realm-wide endpoint/event service. */
#include <linux/arm-smccc.h>
#include <linux/capability.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/irqdomain.h>
#include <linux/of.h>
#include <dt-bindings/interrupt-controller/arm-gic.h>
#include <linux/dma-map-ops.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/sizes.h>
#include <linux/miscdevice.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/uaccess.h>
#include <linux/realm_xshm.h>
#include <linux/client_vmshm.h>
#include <linux/proxy_vmshm.h>
#include <linux/vmshm_comm.h>
#include <asm/rsi.h>
#include <asm/cacheflush.h>
#include <asm/set_memory.h>

/* Bounded service storage for this Realm lifetime. Partially converted pools
 * remain pinned; RMM revocation and GPU fences are separate obligations.
 */
#define REALM_XSHM_MAX_POOLS 8
static DEFINE_MUTEX(pool_lock);
static DEFINE_SPINLOCK(event_lock);
static bool event_irq_ready;
struct realm_xshm_pool_state {
	struct page *pages;
	size_t size;
	bool target;
	bool ready;
	u64 grant;
	/* Queue pages stay WB; GPU payload pages have one WC kernel alias. */
	void *payload;
	struct platform_device *transport;
	u64 transport_grant;
	u64 channel;
	void (*receive)(void *);
	void *receive_data;
	bool provider;
	bool payload_bound;
	u32 client;
};
static struct realm_xshm_pool_state pools[REALM_XSHM_MAX_POOLS];

/* A zero selector is only unambiguous with exactly one allocated pool. */
static struct realm_xshm_pool_state *realm_xshm_find_pool(u64 ipa)
{
	struct realm_xshm_pool_state *found = NULL;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(pools); i++) {
		if (!pools[i].pages)
			continue;
		if (ipa) {
			if (page_to_phys(pools[i].pages) == ipa)
				return &pools[i];
		} else {
			if (found)
				return ERR_PTR(-EINVAL);
			found = &pools[i];
		}
	}
	return found ?: ERR_PTR(-ENXIO);
}

static void *realm_xshm_pool_address(struct realm_xshm_pool_state *pool, u64 offset)
{
	if (offset >= SZ_1M)
		return pool->payload + offset - SZ_1M;
	return page_address(pool->pages) + offset;
}

static int realm_xshm_prepare_payload(struct realm_xshm_pool_state *pool)
{
	unsigned long start;
	size_t size;
	int ret;

	if (pool->size <= SZ_1M)
		return 0;
	/* Realm linear maps use page entries. The initial WB zeroing was
	 * already cleaned to PoC before any target DATA pages were replaced.
	 * Remove the WB alias before installing WC, matching the GPU BO mmap
	 * attributes without making private pages shared with the Host.
	 */
	if (!can_set_direct_map())
		return -EOPNOTSUPP;
	start = (unsigned long)page_address(pool->pages) + SZ_1M;
	size = pool->size - SZ_1M;
	ret = set_memory_valid(start, size >> PAGE_SHIFT, 0);
	if (ret)
		return ret;
	pool->payload = dma_common_contiguous_remap(pool->pages + SZ_1M / PAGE_SIZE,
				 size, pgprot_writecombine(PAGE_KERNEL),
				 __builtin_return_address(0));
	/* A failed remap leaves this bounded allocation pinned and unusable;
	 * it must never return to the allocator with an invalid linear map.
	 */
	return pool->payload ? 0 : -ENOMEM;
}

static void xshm_call(u64 command, u64 id, struct arm_smccc_1_2_regs *output)
{
	struct arm_smccc_1_2_regs input = {};

	input.a0 = SMC_RSI_OPENCCA_XSHM;
	input.a1 = command;
	input.a2 = id;
	arm_smccc_1_2_smc(&input, output);
}

static void realm_xshm_notify(u64 channel)
{
	struct arm_smccc_1_2_regs output;

	/* Queue publication precedes this coalesced event. A failed hint must
	 * never cause replay of a published request.
	 */
	xshm_call(REALM_XSHM_NOTIFY, channel, &output);
	if (output.a0)
		pr_warn_ratelimited("realm-xshm: notify channel=%llu status=%lu\n",
				    channel, output.a0);
}


/* IRQ handling never takes pool_lock: transport removal holds it while
 * unsubscribing and cancelling RX work. Pools themselves persist for the
 * Realm lifetime; event_lock protects channel/callback publication.
 */
static int realm_xshm_subscribe(u64 channel, void (*receive)(void *), void *data)
{
	struct arm_smccc_1_2_regs output;
	unsigned long flags;
	unsigned int i;
	int ret = -ENOENT;

	if (!event_irq_ready)
		return -EOPNOTSUPP;
	xshm_call(REALM_XSHM_IRQ_BIND, 0, &output);
	if (output.a0 || output.a1 != REALM_XSHM_IRQ_INTID)
		return -EIO;
	spin_lock_irqsave(&event_lock, flags);
	for (i = 0; i < ARRAY_SIZE(pools); i++) {
		if (pools[i].channel != channel)
			continue;
		if (pools[i].receive) {
			ret = -EBUSY;
			break;
		}
		pools[i].receive_data = data;
		pools[i].receive = receive;
		ret = 0;
		break;
	}
	spin_unlock_irqrestore(&event_lock, flags);
	return ret;
}

static void realm_xshm_unsubscribe(u64 channel)
{
	unsigned long flags;
	unsigned int i;

	spin_lock_irqsave(&event_lock, flags);
	for (i = 0; i < ARRAY_SIZE(pools); i++) {
		if (pools[i].channel == channel) {
			pools[i].receive = NULL;
			pools[i].receive_data = NULL;
			break;
		}
	}
	spin_unlock_irqrestore(&event_lock, flags);
}

static int realm_xshm_arm(u64 channel)
{
	struct arm_smccc_1_2_regs output;

	/* RX always performs an acquire queue recheck after this call. */
	xshm_call(REALM_XSHM_ACK_ARM, channel, &output);
	if (output.a0)
		return -ESTALE;
	return !!(output.a3 & 2U);
}

static irqreturn_t realm_xshm_irq(int irq, void *data)
{
	unsigned long flags;
	unsigned int i;

	/* The RMM-private LR makes this a wake hint, not a message or access
	 * authorization. Wake each subscribed queue: its worker snapshots and
	 * validates messages, then ACK_ARMs and rechecks before sleeping. No
	 * QUERY SMC or sleeping callback belongs in the interrupt path.
	 * The bounded callbacks only schedule RX work. event_lock serializes
	 * unsubscribe, which precedes transport work cancellation/removal.
	 */
	spin_lock_irqsave(&event_lock, flags);
	for (i = 0; i < ARRAY_SIZE(pools); i++) {
		if (pools[i].channel && pools[i].receive)
			pools[i].receive(pools[i].receive_data);
	}
	spin_unlock_irqrestore(&event_lock, flags);
	return IRQ_HANDLED;
}

static int __init realm_xshm_irq_init(void)
{
	struct arm_smccc_1_2_regs output;
	struct irq_fwspec spec = { .param_count = 3,
		.param = { GIC_SPI, REALM_XSHM_IRQ_INTID - 32, IRQ_TYPE_EDGE_RISING } };
	struct device_node *node;
	unsigned int irq;
	int ret;

	xshm_call(REALM_XSHM_REGISTER, 0, &output);
	if (output.a0 || !(output.a3 & REALM_XSHM_CAP_EVENT_IRQ))
		return 0;
	if (!(output.a3 & REALM_XSHM_CAP_ACK_ARM))
		return -EOPNOTSUPP;
	/* DT identifies the existing GIC only; the trusted RMM ABI fixes the
	 * interrupt number and decides which Realm can raise it.
	 */
	node = of_find_compatible_node(NULL, NULL, "arm,gic-v3");
	if (!node)
		return -ENODEV;
	spec.fwnode = of_node_to_fwnode(node);
	irq = irq_create_fwspec_mapping(&spec);
	of_node_put(node);
	if (!irq)
		return -EINVAL;
	ret = request_irq(irq, realm_xshm_irq, 0, "realm-xshm", NULL);
	if (ret)
		goto err_mapping;
	xshm_call(REALM_XSHM_IRQ_BIND, 0, &output);
	if (output.a0 || output.a1 != REALM_XSHM_IRQ_INTID) {
		ret = -EIO;
		free_irq(irq, NULL);
		goto err_mapping;
	}
	event_irq_ready = true;
	pr_info("realm-xshm: RMM event IRQ enabled intid=%u irq=%u\n",
		REALM_XSHM_IRQ_INTID, irq);
	return 0;
err_mapping:
	irq_dispose_mapping(irq);
	return ret;
}

/* Called under pool_lock. Sharing policy and peer identifiers come from RMM,
 * not from userspace, device tree, VMID parameters or an untrusted header.
 */
static int realm_xshm_bind(struct realm_xshm_pool_state *pool, u64 grant, bool payload)
{
	struct arm_smccc_1_2_regs registration, identity, status;
	struct realm_xshm_transport data = {
		.notify = realm_xshm_notify, .subscribe = realm_xshm_subscribe,
		.unsubscribe = realm_xshm_unsubscribe, .arm = realm_xshm_arm,
	};
	struct resource resource = { .flags = IORESOURCE_MEM };
	struct platform_device_info info = {
		.id = PLATFORM_DEVID_AUTO, .res = &resource, .num_res = 1,
		.data = &data, .size_data = sizeof(data),
	};
	struct platform_device *pdev;
	unsigned long flags;
	bool provider;
	int ret;

	if (pool->transport)
		return -EALREADY;
	if (!grant || pool->size < SZ_1M)
		return -EINVAL;
	xshm_call(REALM_XSHM_REGISTER, 0, &registration);
	if (registration.a0 || !(registration.a3 & REALM_XSHM_CAP_MEMORY_IDENTITY))
		return -EOPNOTSUPP;
	xshm_call(REALM_XSHM_MEM_DESCRIBE, grant, &identity);
	if (identity.a0 || identity.a4 != page_to_phys(pool->pages) ||
	    !identity.a1 || !identity.a2 || !identity.a3 || identity.a3 > U32_MAX)
		return -EPERM;
	provider = identity.a2 == registration.a2;
	if ((provider && pool->target) || (!provider && !pool->target) ||
	    (!provider && (identity.a3 != registration.a2 || grant != pool->grant)))
		return -EPERM;
	if ((provider && !IS_ENABLED(CONFIG_PROXY_VMSHM_COMM)) ||
	    (!provider && !IS_ENABLED(CONFIG_CLIENT_VMSHM_COMM)))
		return -EOPNOTSUPP;
	if (payload && (pool->size <= SZ_1M + SZ_2M || !pool->payload ||
		(provider && !IS_ENABLED(CONFIG_PROXY_VMSHM_MANAGER)) ||
		(!provider && !IS_ENABLED(CONFIG_CLIENT_VMSHM_MANAGER))))
		return -EOPNOTSUPP;
	xshm_call(REALM_XSHM_MEM_STATUS, grant, &status);
	if (status.a0 || status.a2 != REALM_XSHM_MEMORY_ACTIVE ||
	    status.a4 != pool->size / PAGE_SIZE)
		return -ESTALE;
	data.channel = identity.a1;
	data.client_vmid = identity.a3;
	resource.start = identity.a4;
	resource.end = resource.start + SZ_1M - 1;
	if (payload) {
		phys_addr_t gpa = resource.start + SZ_1M;
		size_t size = pool->size - SZ_1M;

		if (provider)
			ret = proxy_vmshm_manager_register_domain(data.client_vmid,
						pool->payload, gpa, size);
		else
			ret = client_vmshm_manager_bind(pool->payload, gpa, size,
						data.client_vmid);
		if (ret)
			return ret;
	}
	info.name = provider ? "proxy_comm_vmshm" : "client_comm_vmshm";
	spin_lock_irqsave(&event_lock, flags);
	pool->channel = data.channel;
	spin_unlock_irqrestore(&event_lock, flags);
	pdev = platform_device_register_full(&info);
	if (IS_ERR(pdev)) {
		ret = PTR_ERR(pdev);
		goto err_payload;
	}
	if (!pdev->dev.driver) {
		platform_device_unregister(pdev);
		ret = -ENODEV;
		goto err_payload;
	}
	pool->transport = pdev;
	pool->transport_grant = grant;
	pool->provider = provider;
	pool->payload_bound = payload;
	pool->client = data.client_vmid;
	pr_info("realm-xshm: bound grant=%llu channel=%llu client_endpoint=%u role=%s\n",
		grant, data.channel, data.client_vmid, provider ? "proxy" : "client");
	return 0;
err_payload:
	spin_lock_irqsave(&event_lock, flags);
	pool->channel = 0;
	spin_unlock_irqrestore(&event_lock, flags);
	if (payload) {
		if (provider)
			WARN_ON(proxy_vmshm_manager_unregister_domain(data.client_vmid,
							       pool->payload));
		else
			client_vmshm_manager_unbind(pool->payload);
	}
	return ret;
}

static int realm_xshm_unbind(struct realm_xshm_pool_state *pool, u64 grant)
{
	unsigned long flags;
	int ret;

	if (!pool->transport || grant != pool->transport_grant)
		return -ENOENT;
	ret = pool->provider ? proxy_comm_vmshm_quiesce(&pool->transport->dev) :
		client_comm_vmshm_quiesce(&pool->transport->dev);
	if (ret)
		return ret;
	if (pool->payload_bound) {
		if (pool->provider) {
			ret = proxy_vmshm_manager_unregister_domain(pool->client,
							   pool->payload);
			/* Keep the queue quiesced until all unpublished/pinned
			 * objects have drained; never revive or release its pages.
			 */
			if (ret)
				return ret;
		} else {
			client_vmshm_manager_unbind(pool->payload);
		}
	}
	platform_device_unregister(pool->transport);
	pool->transport = NULL;
	pool->transport_grant = 0;
	spin_lock_irqsave(&event_lock, flags);
	pool->channel = 0;
	spin_unlock_irqrestore(&event_lock, flags);
	pool->payload_bound = false;
	pool->client = 0;
	return 0;
}

static long realm_xshm_pool_ioctl(unsigned long arg)
{
	struct realm_xshm_pool request;
	struct realm_xshm_pool_state *pool;
	unsigned int i;
	phys_addr_t base;
	void *address;
	long ret = 0;

	if (copy_from_user(&request, (void __user *)arg, sizeof(request)))
		return -EFAULT;
	if (request.operation > REALM_XSHM_POOL_BIND_GPU)
		return -EINVAL;
	mutex_lock(&pool_lock);
	if (request.operation <= REALM_XSHM_POOL_TARGET) {
		for (i = 0; i < ARRAY_SIZE(pools); i++)
			if (!pools[i].pages)
				break;
		if (i == ARRAY_SIZE(pools)) {
			ret = -ENOSPC;
			goto out;
		}
		pool = &pools[i];
		if (!request.size || request.size > SZ_512M ||
		    !PAGE_ALIGNED(request.size)) {
			ret = -EINVAL;
			goto out;
		}
		pool->pages = dma_alloc_from_contiguous(NULL,
				request.size >> PAGE_SHIFT, 0, false);
		if (!pool->pages) {
			ret = -ENOMEM;
			goto out;
		}
		pool->size = request.size;
		pool->target = request.operation == REALM_XSHM_POOL_TARGET;
		address = page_address(pool->pages);
		base = page_to_phys(pool->pages);
		memset(address, 0, pool->size);
		/* Flush before the old target PA can be released/reused by Host. */
		dcache_clean_inval_poc((unsigned long)address,
				      (unsigned long)address + pool->size);
		if (pool->target) {
			phys_addr_t ipa;

			ret = rsi_set_memory_range_shared(base, base + pool->size);
			if (ret)
				goto out;
			/* A 4 KiB RAM request forces a protected L3 table in each
			 * 2 MiB block. It does not fault in destination DATA pages.
			 */
			ipa = base;
			while (ipa < base + pool->size) {
				ret = rsi_set_memory_range_protected(ipa, ipa + PAGE_SIZE);
				if (ret)
					goto out;
				ipa = ALIGN(ipa + PAGE_SIZE, SZ_2M);
			}
			ret = rsi_set_memory_range_protected(base, base + pool->size);
			if (ret)
				goto out;
		}
		ret = realm_xshm_prepare_payload(pool);
		if (ret)
			goto out;
		pool->ready = true;
	} else {
		u64 selector = request.ipa;

		if (!selector && (request.operation == REALM_XSHM_POOL_BIND ||
				  request.operation == REALM_XSHM_POOL_BIND_GPU)) {
			struct arm_smccc_1_2_regs identity;

			xshm_call(REALM_XSHM_MEM_DESCRIBE, request.grant, &identity);
			/* Preserve ENXIO for callers without any prepared pool. */
			if (!identity.a0)
				selector = identity.a4;
		} else if (!selector && request.operation == REALM_XSHM_POOL_UNBIND) {
			for (i = 0; i < ARRAY_SIZE(pools); i++)
				if (pools[i].transport &&
				    pools[i].transport_grant == request.grant) {
					selector = page_to_phys(pools[i].pages);
					break;
				}
		}
		pool = realm_xshm_find_pool(selector);
		if (IS_ERR(pool)) {
			ret = PTR_ERR(pool);
			goto out;
		}
		if (!pool->ready) {
			ret = -ENXIO;
			goto out;
		}
	}
	if (request.operation == REALM_XSHM_POOL_BIND ||
	    request.operation == REALM_XSHM_POOL_BIND_GPU) {
		ret = realm_xshm_bind(pool, request.grant,
				       request.operation == REALM_XSHM_POOL_BIND_GPU);
		if (ret)
			goto out;
	} else if (request.operation == REALM_XSHM_POOL_UNBIND) {
		ret = realm_xshm_unbind(pool, request.grant);
		if (ret)
			goto out;
	}
	request.ipa = page_to_phys(pool->pages);
	request.size = pool->size;
	if (request.operation == REALM_XSHM_POOL_INFO ||
	    request.operation == REALM_XSHM_POOL_BIND ||
	    request.operation == REALM_XSHM_POOL_BIND_GPU ||
	    request.operation == REALM_XSHM_POOL_UNBIND)
		request.grant = pool->transport_grant;
	if (request.operation == REALM_XSHM_POOL_READ ||
	    request.operation == REALM_XSHM_POOL_WRITE) {
		if (pool->transport) {
			ret = -EBUSY;
			goto out;
		}
		if (request.offset & (sizeof(u64) - 1) ||
		    request.offset > pool->size - sizeof(u64)) {
			ret = -EINVAL;
			goto out;
		}
		if (pool->target) {
			struct arm_smccc_1_2_regs input = {}, output;

			input.a0 = SMC_RSI_OPENCCA_XSHM;
			input.a1 = REALM_XSHM_MEM_STATUS;
			input.a2 = request.grant;
			arm_smccc_1_2_smc(&input, &output);
			/* ACTIVE is the only state allowing borrower CPU access.
			 * The trusted provider must coordinate revoke with users.
			 */
			if (request.grant != pool->grant || output.a0 ||
			    output.a2 != REALM_XSHM_MEMORY_ACTIVE ||
			    output.a4 != pool->size / PAGE_SIZE) {
				ret = -ESTALE;
				goto out;
			}
		}
		address = realm_xshm_pool_address(pool, request.offset);
		if (request.operation == REALM_XSHM_POOL_WRITE)
			WRITE_ONCE(*(u64 *)address, request.value);
		else
			request.value = READ_ONCE(*(u64 *)address);
	}
	if (copy_to_user((void __user *)arg, &request, sizeof(request)))
		ret = -EFAULT;
out:
	mutex_unlock(&pool_lock);
	return ret;
}

static long realm_xshm_ioctl(struct file *file, unsigned int cmd,
			     unsigned long arg)
{
	struct arm_smccc_1_2_regs input = {}, output;
	struct realm_xshm_call call;
	struct realm_xshm_pool_state *pool = NULL;
	unsigned int i;

	/* An endpoint represents this Realm, not the calling process. */
	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (cmd == REALM_XSHM_POOL)
		return realm_xshm_pool_ioctl(arg);
	if (cmd != REALM_XSHM_CALL)
		return -ENOTTY;
	if (copy_from_user(&call, (void __user *)arg, sizeof(call)))
		return -EFAULT;
	if (call.command > REALM_XSHM_ACK_ARM ||
	    (call.command != REALM_XSHM_MEM_OFFER && call.size))
		return -EINVAL;
	input.a0 = SMC_RSI_OPENCCA_XSHM;
	input.a1 = call.command;
	input.a2 = call.id;
	input.a3 = call.sequence;
	input.a4 = call.size;
	mutex_lock(&pool_lock);
	for (i = 0; i < ARRAY_SIZE(pools); i++) {
		struct realm_xshm_pool_state *bound = &pools[i];

		if (!bound->transport)
			continue;
		if (call.command == REALM_XSHM_UNREGISTER ||
		    (call.command == REALM_XSHM_CLOSE && call.id == bound->channel) ||
		    (call.command == REALM_XSHM_MEM_ATTACH &&
		     call.sequence == page_to_phys(bound->pages)) ||
		    ((call.command == REALM_XSHM_MEM_DETACH ||
		      call.command == REALM_XSHM_MEM_REVOKE) &&
		     call.id == bound->transport_grant)) {
			mutex_unlock(&pool_lock);
			return -EBUSY;
		}
	}
	if (call.command == REALM_XSHM_MEM_ATTACH) {
		pool = realm_xshm_find_pool(call.sequence);
		if (IS_ERR(pool)) {
			/* No local storage: let RMM reject unauthorized raw calls. */
			for (i = 0; i < ARRAY_SIZE(pools); i++)
				if (pools[i].pages)
					break;
			if (i != ARRAY_SIZE(pools)) {
				mutex_unlock(&pool_lock);
				return -EINVAL;
			}
			pool = NULL;
		} else if (!pool->target || !pool->ready) {
			mutex_unlock(&pool_lock);
			return -EINVAL;
		}
	}
	arm_smccc_1_2_smc(&input, &output);
	if (call.command == REALM_XSHM_MEM_ATTACH && pool &&
	    (output.a0 == 0 || output.a0 == 3) && output.a1 == call.id &&
	    output.a2 >= 3 && output.a2 <= REALM_XSHM_MEMORY_ACTIVE)
		pool->grant = call.id;
	mutex_unlock(&pool_lock);
	if (output.a0 == (unsigned long)SMCCC_RET_NOT_SUPPORTED)
		return -EOPNOTSUPP;
	call.status = output.a0;
	call.result[0] = output.a1;
	call.result[1] = output.a2;
	call.result[2] = output.a3;
	call.result[3] = output.a4;
	if (copy_to_user((void __user *)arg, &call, sizeof(call)))
		return -EFAULT;
	return 0;
}

static const struct file_operations realm_xshm_fops = {
	.owner = THIS_MODULE,
	.open = nonseekable_open,
	.unlocked_ioctl = realm_xshm_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
};

static struct miscdevice realm_xshm_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "realm-xshm",
	.fops = &realm_xshm_fops,
	.mode = 0600,
};

static int __init realm_xshm_init(void)
{
	int ret;

	if (!is_realm_world())
		return -ENODEV;
	ret = realm_xshm_irq_init();
	if (ret)
		return ret;
	return misc_register(&realm_xshm_device);
}

module_init(realm_xshm_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("OpenCCA Realm endpoint and event service frontend");
