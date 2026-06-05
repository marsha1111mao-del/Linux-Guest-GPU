// SPDX-License-Identifier: GPL-2.0 or MIT
/* Copyright 2018 Marty E. Plummer <hanetzer@startmail.com> */
/* Copyright 2019 Linaro, Ltd., Rob Herring <robh@kernel.org> */
/* Copyright 2019 Collabora ltd. */

#include <linux/list.h>
#include <linux/atomic.h>
#include <linux/kstrtox.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/pagemap.h>
#include <linux/panthor_vmshm.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/proxy_vmshm.h>

#include <drm/drm_auth.h>
#include <drm/drm_debugfs.h>
#include <drm/drm_drv.h>
#include <drm/drm_exec.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_syncobj.h>
#include <drm/drm_utils.h>
#include <drm/gpu_scheduler.h>
#include <drm/panthor_drm.h>

#include "panthor_device.h"
#include "panthor_fw.h"
#include "panthor_gem.h"
#include "panthor_gpu.h"
#include "panthor_heap.h"
#include "panthor_mmu.h"
#include "panthor_regs.h"
#include "panthor_sched.h"

struct panthor_job_irq_stats {
	u64 raw_entries;
	u64 raw_wake_thread;
	u64 raw_none;
	u64 raw_total_ns;
	u64 raw_max_ns;
	u64 raw_to_thread_samples;
	u64 raw_to_thread_total_ns;
	u64 raw_to_thread_max_ns;
	u64 thread_entries;
	u64 thread_handled;
	u64 thread_none;
	u64 thread_without_raw;
	u64 thread_total_ns;
	u64 thread_max_ns;
	u64 loop_iterations;
	u64 last_raw_end_ns;
	u32 status_or;
	bool thread_pending;
};

struct panthor_submit_stats {
	u64 group_submit_calls;
	u64 group_submit_jobs;
	u64 group_submit_total_ns;
	u64 group_submit_max_ns;
	u64 group_copy_jobs_ns;
	u64 group_init_ctx_ns;
	u64 group_create_jobs_ns;
	u64 group_collect_signals_ns;
	u64 group_prepare_resvs_ns;
	u64 group_arm_jobs_ns;
	u64 group_push_jobs_ns;
	u64 group_push_update_resvs_ns;
	u64 group_push_entity_ns;
	u64 group_push_fences_ns;
	u64 group_cleanup_ns;
	u64 vm_bind_async_calls;
	u64 vm_bind_sync_calls;
	u64 vm_bind_ops;
	u64 vm_bind_total_ns;
	u64 vm_bind_max_ns;
	u64 vm_bind_copy_ops_ns;
	u64 vm_bind_init_ctx_ns;
	u64 vm_bind_create_jobs_ns;
	u64 vm_bind_collect_signals_ns;
	u64 vm_bind_prepare_resvs_ns;
	u64 vm_bind_arm_jobs_ns;
	u64 vm_bind_push_jobs_ns;
	u64 vm_bind_push_update_resvs_ns;
	u64 vm_bind_push_entity_ns;
	u64 vm_bind_push_fences_ns;
	u64 vm_bind_sync_exec_ns;
	u64 vm_bind_cleanup_ns;
	u64 run_job_calls;
	u64 run_job_total_ns;
	u64 run_job_max_ns;
	u64 run_job_pm_resume_ns;
	u64 run_job_lock_wait_ns;
	u64 run_job_init_fence_ns;
	u64 run_job_ringbuf_write_ns;
	u64 run_job_inflight_add_ns;
	u64 run_job_iface_update_ns;
	u64 run_job_schedule_or_doorbell_ns;
	u64 run_job_last_fence_ns;
	u64 run_job_pm_put_ns;
	u64 run_job_errors;
	u64 run_job_zero_size_jobs;
};

static bool panthor_job_irq_stats_enabled;
static bool panthor_submit_stats_enabled;
static DEFINE_SPINLOCK(panthor_job_irq_stats_lock);
static DEFINE_SPINLOCK(panthor_submit_stats_lock);
static struct panthor_job_irq_stats panthor_job_irq_stats_data;
static struct panthor_submit_stats panthor_submit_stats_data;

enum panthor_submit_stats_kind {
	PANTHOR_SUBMIT_STATS_GROUP_SUBMIT,
	PANTHOR_SUBMIT_STATS_VM_BIND_ASYNC,
	PANTHOR_SUBMIT_STATS_VM_BIND_SYNC,
};

struct panthor_submit_stats_sample {
	enum panthor_submit_stats_kind kind;
	u64 start_ns;
	u64 copy_ns;
	u64 init_ctx_ns;
	u64 create_jobs_ns;
	u64 collect_signals_ns;
	u64 prepare_resvs_ns;
	u64 arm_jobs_ns;
	u64 push_jobs_ns;
	u64 push_update_resvs_ns;
	u64 push_entity_ns;
	u64 push_fences_ns;
	u64 sync_exec_ns;
	u64 cleanup_ns;
	u32 job_or_op_count;
};

bool panthor_submit_stats_is_enabled(void)
{
	return READ_ONCE(panthor_submit_stats_enabled);
}

static inline u64 panthor_submit_stats_now(bool enabled)
{
	if (!enabled)
		return 0;

	return ktime_get_ns();
}

static inline void panthor_submit_stats_accum(u64 *field, u64 start_ns)
{
	if (start_ns)
		*field += ktime_get_ns() - start_ns;
}

void panthor_submit_stats_record_run_job(
	const struct panthor_sched_run_job_stats *sample)
{
	struct panthor_submit_stats *stats = &panthor_submit_stats_data;
	unsigned long flags;

	if (!sample || !sample->calls || !panthor_submit_stats_is_enabled())
		return;

	spin_lock_irqsave(&panthor_submit_stats_lock, flags);
	stats->run_job_calls += sample->calls;
	stats->run_job_total_ns += sample->total_ns;
	if (sample->max_ns > stats->run_job_max_ns)
		stats->run_job_max_ns = sample->max_ns;
	stats->run_job_pm_resume_ns += sample->pm_resume_ns;
	stats->run_job_lock_wait_ns += sample->lock_wait_ns;
	stats->run_job_init_fence_ns += sample->init_fence_ns;
	stats->run_job_ringbuf_write_ns += sample->ringbuf_write_ns;
	stats->run_job_inflight_add_ns += sample->inflight_add_ns;
	stats->run_job_iface_update_ns += sample->iface_update_ns;
	stats->run_job_schedule_or_doorbell_ns +=
		sample->schedule_or_doorbell_ns;
	stats->run_job_last_fence_ns += sample->last_fence_ns;
	stats->run_job_pm_put_ns += sample->pm_put_ns;
	stats->run_job_errors += sample->errors;
	stats->run_job_zero_size_jobs += sample->zero_size_jobs;
	spin_unlock_irqrestore(&panthor_submit_stats_lock, flags);
}

static void
panthor_submit_stats_dump_snapshot(const struct panthor_submit_stats *stats)
{
	u64 group_avg_ns = 0, vm_bind_avg_ns = 0;
	u64 vm_bind_calls = stats->vm_bind_async_calls + stats->vm_bind_sync_calls;

	if (stats->group_submit_calls)
		group_avg_ns = div64_u64(stats->group_submit_total_ns,
					 stats->group_submit_calls);
	if (vm_bind_calls)
		vm_bind_avg_ns = div64_u64(stats->vm_bind_total_ns,
					   vm_bind_calls);

	pr_info("panthor: [MZH][PANTHOR_SUBMIT_STATS_GROUP] group_calls=%llu group_jobs=%llu group_avg_ns=%llu group_max_ns=%llu group_copy_jobs_ns=%llu group_init_ctx_ns=%llu group_create_jobs_ns=%llu group_collect_signals_ns=%llu group_prepare_resvs_ns=%llu group_arm_jobs_ns=%llu group_push_jobs_ns=%llu group_push_update_resvs_ns=%llu group_push_entity_ns=%llu group_push_fences_ns=%llu group_cleanup_ns=%llu\n",
		stats->group_submit_calls, stats->group_submit_jobs,
		group_avg_ns, stats->group_submit_max_ns,
		stats->group_copy_jobs_ns, stats->group_init_ctx_ns,
		stats->group_create_jobs_ns, stats->group_collect_signals_ns,
		stats->group_prepare_resvs_ns, stats->group_arm_jobs_ns,
		stats->group_push_jobs_ns,
		stats->group_push_update_resvs_ns,
		stats->group_push_entity_ns, stats->group_push_fences_ns,
		stats->group_cleanup_ns);
	pr_info("panthor: [MZH][PANTHOR_SUBMIT_STATS_VMBIND] vm_bind_async_calls=%llu vm_bind_sync_calls=%llu vm_bind_ops=%llu vm_bind_avg_ns=%llu vm_bind_max_ns=%llu vm_bind_copy_ops_ns=%llu vm_bind_init_ctx_ns=%llu vm_bind_create_jobs_ns=%llu vm_bind_collect_signals_ns=%llu vm_bind_prepare_resvs_ns=%llu vm_bind_arm_jobs_ns=%llu vm_bind_push_jobs_ns=%llu vm_bind_push_update_resvs_ns=%llu vm_bind_push_entity_ns=%llu vm_bind_push_fences_ns=%llu vm_bind_sync_exec_ns=%llu vm_bind_cleanup_ns=%llu\n",
		stats->vm_bind_async_calls, stats->vm_bind_sync_calls,
		stats->vm_bind_ops, vm_bind_avg_ns, stats->vm_bind_max_ns,
		stats->vm_bind_copy_ops_ns, stats->vm_bind_init_ctx_ns,
		stats->vm_bind_create_jobs_ns, stats->vm_bind_collect_signals_ns,
		stats->vm_bind_prepare_resvs_ns, stats->vm_bind_arm_jobs_ns,
		stats->vm_bind_push_jobs_ns,
		stats->vm_bind_push_update_resvs_ns,
		stats->vm_bind_push_entity_ns, stats->vm_bind_push_fences_ns,
		stats->vm_bind_sync_exec_ns, stats->vm_bind_cleanup_ns);
	pr_info("panthor: [MZH][PANTHOR_SUBMIT_STATS_RUN_JOB] run_job_calls=%llu run_job_total_ns=%llu run_job_max_ns=%llu run_job_pm_resume_ns=%llu run_job_lock_wait_ns=%llu run_job_init_fence_ns=%llu run_job_ringbuf_write_ns=%llu run_job_inflight_add_ns=%llu run_job_iface_update_ns=%llu run_job_schedule_or_doorbell_ns=%llu run_job_last_fence_ns=%llu run_job_pm_put_ns=%llu run_job_errors=%llu run_job_zero_size_jobs=%llu\n",
		stats->run_job_calls, stats->run_job_total_ns,
		stats->run_job_max_ns, stats->run_job_pm_resume_ns,
		stats->run_job_lock_wait_ns, stats->run_job_init_fence_ns,
		stats->run_job_ringbuf_write_ns,
		stats->run_job_inflight_add_ns,
		stats->run_job_iface_update_ns,
		stats->run_job_schedule_or_doorbell_ns,
		stats->run_job_last_fence_ns, stats->run_job_pm_put_ns,
		stats->run_job_errors, stats->run_job_zero_size_jobs);
}

static void panthor_submit_stats_dump_current_if_enabled(const char *reason)
{
	struct panthor_submit_stats snapshot;
	unsigned long flags;
	bool enabled;

	spin_lock_irqsave(&panthor_submit_stats_lock, flags);
	enabled = panthor_submit_stats_enabled;
	if (enabled)
		snapshot = panthor_submit_stats_data;
	spin_unlock_irqrestore(&panthor_submit_stats_lock, flags);

	if (!enabled)
		return;

	pr_info("panthor: [MZH][PANTHOR_SUBMIT_STATS_SNAPSHOT] reason=%s\n",
		reason ? reason : "unspecified");
	panthor_submit_stats_dump_snapshot(&snapshot);
}

static int panthor_submit_stats_set(const char *val,
				    const struct kernel_param *kp)
{
	struct panthor_submit_stats snapshot;
	bool enabled, dump = false;
	unsigned long flags;
	int ret;

	ret = kstrtobool(val, &enabled);
	if (ret)
		return ret;

	spin_lock_irqsave(&panthor_submit_stats_lock, flags);
	if (enabled && !panthor_submit_stats_enabled) {
		memset(&panthor_submit_stats_data, 0,
		       sizeof(panthor_submit_stats_data));
	} else if (!enabled && panthor_submit_stats_enabled) {
		snapshot = panthor_submit_stats_data;
		dump = true;
	}
	WRITE_ONCE(panthor_submit_stats_enabled, enabled);
	spin_unlock_irqrestore(&panthor_submit_stats_lock, flags);

	if (dump)
		panthor_submit_stats_dump_snapshot(&snapshot);

	return 0;
}

static const struct kernel_param_ops panthor_submit_stats_param_ops = {
	.set = panthor_submit_stats_set,
	.get = param_get_bool,
};

module_param_cb(submit_stats, &panthor_submit_stats_param_ops,
		&panthor_submit_stats_enabled, 0644);
MODULE_PARM_DESC(submit_stats,
		 "Collect low-overhead Panthor submit/vm-bind ioctl aggregate timing stats");

static void panthor_submit_stats_commit(struct panthor_submit_stats_sample *sample)
{
	struct panthor_submit_stats *stats = &panthor_submit_stats_data;
	unsigned long flags;
	u64 total_ns;

	if (!sample->start_ns || !READ_ONCE(panthor_submit_stats_enabled))
		return;

	total_ns = ktime_get_ns() - sample->start_ns;

	spin_lock_irqsave(&panthor_submit_stats_lock, flags);
	switch (sample->kind) {
	case PANTHOR_SUBMIT_STATS_GROUP_SUBMIT:
		stats->group_submit_calls++;
		stats->group_submit_jobs += sample->job_or_op_count;
		stats->group_submit_total_ns += total_ns;
		if (total_ns > stats->group_submit_max_ns)
			stats->group_submit_max_ns = total_ns;
		stats->group_copy_jobs_ns += sample->copy_ns;
		stats->group_init_ctx_ns += sample->init_ctx_ns;
		stats->group_create_jobs_ns += sample->create_jobs_ns;
		stats->group_collect_signals_ns += sample->collect_signals_ns;
		stats->group_prepare_resvs_ns += sample->prepare_resvs_ns;
		stats->group_arm_jobs_ns += sample->arm_jobs_ns;
		stats->group_push_jobs_ns += sample->push_jobs_ns;
		stats->group_push_update_resvs_ns +=
			sample->push_update_resvs_ns;
		stats->group_push_entity_ns += sample->push_entity_ns;
		stats->group_push_fences_ns += sample->push_fences_ns;
		stats->group_cleanup_ns += sample->cleanup_ns;
		break;
	case PANTHOR_SUBMIT_STATS_VM_BIND_ASYNC:
		stats->vm_bind_async_calls++;
		fallthrough;
	case PANTHOR_SUBMIT_STATS_VM_BIND_SYNC:
		if (sample->kind == PANTHOR_SUBMIT_STATS_VM_BIND_SYNC)
			stats->vm_bind_sync_calls++;
		stats->vm_bind_ops += sample->job_or_op_count;
		stats->vm_bind_total_ns += total_ns;
		if (total_ns > stats->vm_bind_max_ns)
			stats->vm_bind_max_ns = total_ns;
		stats->vm_bind_copy_ops_ns += sample->copy_ns;
		stats->vm_bind_init_ctx_ns += sample->init_ctx_ns;
		stats->vm_bind_create_jobs_ns += sample->create_jobs_ns;
		stats->vm_bind_collect_signals_ns += sample->collect_signals_ns;
		stats->vm_bind_prepare_resvs_ns += sample->prepare_resvs_ns;
		stats->vm_bind_arm_jobs_ns += sample->arm_jobs_ns;
		stats->vm_bind_push_jobs_ns += sample->push_jobs_ns;
		stats->vm_bind_push_update_resvs_ns +=
			sample->push_update_resvs_ns;
		stats->vm_bind_push_entity_ns += sample->push_entity_ns;
		stats->vm_bind_push_fences_ns += sample->push_fences_ns;
		stats->vm_bind_sync_exec_ns += sample->sync_exec_ns;
		stats->vm_bind_cleanup_ns += sample->cleanup_ns;
		break;
	}
	spin_unlock_irqrestore(&panthor_submit_stats_lock, flags);
}

static void
panthor_job_irq_stats_dump_snapshot(const struct panthor_job_irq_stats *stats)
{
	u64 raw_avg_ns = 0, raw_to_thread_avg_ns = 0, thread_avg_ns = 0;

	if (stats->raw_entries)
		raw_avg_ns = div64_u64(stats->raw_total_ns, stats->raw_entries);
	if (stats->raw_to_thread_samples)
		raw_to_thread_avg_ns = div64_u64(stats->raw_to_thread_total_ns,
						 stats->raw_to_thread_samples);
	if (stats->thread_entries)
		thread_avg_ns = div64_u64(stats->thread_total_ns,
					  stats->thread_entries);

	pr_info("panthor: [MZH][PANTHOR_JOB_IRQ_STATS] raw_entries=%llu raw_wake_thread=%llu raw_none=%llu raw_avg_ns=%llu raw_max_ns=%llu raw_to_thread_samples=%llu raw_to_thread_avg_ns=%llu raw_to_thread_max_ns=%llu thread_entries=%llu thread_handled=%llu thread_none=%llu thread_without_raw=%llu thread_avg_ns=%llu thread_max_ns=%llu loop_iterations=%llu status_or=%#x pending=%u\n",
		stats->raw_entries, stats->raw_wake_thread, stats->raw_none,
		raw_avg_ns, stats->raw_max_ns, stats->raw_to_thread_samples,
		raw_to_thread_avg_ns, stats->raw_to_thread_max_ns,
		stats->thread_entries, stats->thread_handled, stats->thread_none,
		stats->thread_without_raw, thread_avg_ns, stats->thread_max_ns,
		stats->loop_iterations, stats->status_or,
		stats->thread_pending ? 1 : 0);
}

static void panthor_job_irq_stats_dump_current_if_enabled(const char *reason)
{
	struct panthor_job_irq_stats snapshot;
	unsigned long flags;
	bool enabled;

	spin_lock_irqsave(&panthor_job_irq_stats_lock, flags);
	enabled = panthor_job_irq_stats_enabled;
	if (enabled)
		snapshot = panthor_job_irq_stats_data;
	spin_unlock_irqrestore(&panthor_job_irq_stats_lock, flags);

	if (!enabled)
		return;

	pr_info("panthor: [MZH][PANTHOR_JOB_IRQ_STATS_SNAPSHOT] reason=%s\n",
		reason ? reason : "unspecified");
	panthor_job_irq_stats_dump_snapshot(&snapshot);
}

static int panthor_job_irq_stats_set(const char *val,
				     const struct kernel_param *kp)
{
	struct panthor_job_irq_stats snapshot;
	bool enabled, dump = false;
	unsigned long flags;
	int ret;

	ret = kstrtobool(val, &enabled);
	if (ret)
		return ret;

	spin_lock_irqsave(&panthor_job_irq_stats_lock, flags);
	if (enabled && !panthor_job_irq_stats_enabled) {
		memset(&panthor_job_irq_stats_data, 0,
		       sizeof(panthor_job_irq_stats_data));
	} else if (!enabled && panthor_job_irq_stats_enabled) {
		snapshot = panthor_job_irq_stats_data;
		dump = true;
	}
	WRITE_ONCE(panthor_job_irq_stats_enabled, enabled);
	spin_unlock_irqrestore(&panthor_job_irq_stats_lock, flags);

	if (dump)
		panthor_job_irq_stats_dump_snapshot(&snapshot);

	return 0;
}

static const struct kernel_param_ops panthor_job_irq_stats_param_ops = {
	.set = panthor_job_irq_stats_set,
	.get = param_get_bool,
};

module_param_cb(job_irq_stats, &panthor_job_irq_stats_param_ops,
		&panthor_job_irq_stats_enabled, 0644);
MODULE_PARM_DESC(job_irq_stats,
		 "Collect low-overhead Panthor job IRQ timing stats");

u64 panthor_job_irq_stats_raw_begin(void)
{
	if (!READ_ONCE(panthor_job_irq_stats_enabled))
		return 0;

	return ktime_get_ns();
}

void panthor_job_irq_stats_raw_end(u64 start_ns, bool wake_thread)
{
	unsigned long flags;
	u64 now, delta;

	if (!start_ns || !READ_ONCE(panthor_job_irq_stats_enabled))
		return;

	now = ktime_get_ns();
	delta = now - start_ns;

	spin_lock_irqsave(&panthor_job_irq_stats_lock, flags);
	panthor_job_irq_stats_data.raw_entries++;
	panthor_job_irq_stats_data.raw_total_ns += delta;
	if (delta > panthor_job_irq_stats_data.raw_max_ns)
		panthor_job_irq_stats_data.raw_max_ns = delta;
	if (wake_thread) {
		panthor_job_irq_stats_data.raw_wake_thread++;
		panthor_job_irq_stats_data.last_raw_end_ns = now;
		panthor_job_irq_stats_data.thread_pending = true;
	} else {
		panthor_job_irq_stats_data.raw_none++;
	}
	spin_unlock_irqrestore(&panthor_job_irq_stats_lock, flags);
}

u64 panthor_job_irq_stats_thread_begin(void)
{
	unsigned long flags;
	u64 now, delta;

	if (!READ_ONCE(panthor_job_irq_stats_enabled))
		return 0;

	now = ktime_get_ns();

	spin_lock_irqsave(&panthor_job_irq_stats_lock, flags);
	panthor_job_irq_stats_data.thread_entries++;
	if (panthor_job_irq_stats_data.thread_pending) {
		delta = now - panthor_job_irq_stats_data.last_raw_end_ns;
		panthor_job_irq_stats_data.raw_to_thread_samples++;
		panthor_job_irq_stats_data.raw_to_thread_total_ns += delta;
		if (delta > panthor_job_irq_stats_data.raw_to_thread_max_ns)
			panthor_job_irq_stats_data.raw_to_thread_max_ns = delta;
		panthor_job_irq_stats_data.thread_pending = false;
	} else {
		panthor_job_irq_stats_data.thread_without_raw++;
	}
	spin_unlock_irqrestore(&panthor_job_irq_stats_lock, flags);

	return now;
}

void panthor_job_irq_stats_thread_loop(u32 status)
{
	unsigned long flags;

	if (!READ_ONCE(panthor_job_irq_stats_enabled))
		return;

	spin_lock_irqsave(&panthor_job_irq_stats_lock, flags);
	panthor_job_irq_stats_data.loop_iterations++;
	panthor_job_irq_stats_data.status_or |= status;
	spin_unlock_irqrestore(&panthor_job_irq_stats_lock, flags);
}

void panthor_job_irq_stats_thread_end(u64 start_ns, irqreturn_t ret)
{
	unsigned long flags;
	u64 now, delta;

	if (!start_ns || !READ_ONCE(panthor_job_irq_stats_enabled))
		return;

	now = ktime_get_ns();
	delta = now - start_ns;

	spin_lock_irqsave(&panthor_job_irq_stats_lock, flags);
	panthor_job_irq_stats_data.thread_total_ns += delta;
	if (delta > panthor_job_irq_stats_data.thread_max_ns)
		panthor_job_irq_stats_data.thread_max_ns = delta;
	if (ret == IRQ_HANDLED)
		panthor_job_irq_stats_data.thread_handled++;
	else
		panthor_job_irq_stats_data.thread_none++;
	spin_unlock_irqrestore(&panthor_job_irq_stats_lock, flags);
}

static DEFINE_MUTEX(panthor_vmshm_lock);
static atomic_t panthor_vmshm_active_sessions = ATOMIC_INIT(0);
static struct panthor_device *panthor_vmshm_ptdev;

struct panthor_vmshm_session {
	struct drm_file file;
	struct panthor_file *pfile;
	bool gem_idr_init;
	bool syncobj_idr_init;
};

#define PANTHOR_BO_FLAGS DRM_PANTHOR_BO_NO_MMAP

static void panthor_vmshm_release_gem_handles(struct panthor_vmshm_session *session)
{
	struct drm_gem_object *obj;
	int handle;

	for (;;) {
		handle = 1;

		spin_lock(&session->file.table_lock);
		obj = idr_get_next(&session->file.object_idr, &handle);
		spin_unlock(&session->file.table_lock);
		if (!obj)
			break;

		drm_gem_handle_delete(&session->file, handle);
	}
}

static int panthor_vmshm_release_syncobj_handle(int id, void *ptr, void *data)
{
	struct drm_syncobj *syncobj = ptr;

	drm_syncobj_put(syncobj);
	return 0;
}

/**
 * DOC: user <-> kernel object copy helpers.
 */

/**
 * panthor_set_uobj() - Copy kernel object to user object.
 * @usr_ptr: Users pointer.
 * @usr_size: Size of the user object.
 * @min_size: Minimum size for this object.
 * @kern_size: Size of the kernel object.
 * @in: Address of the kernel object to copy.
 *
 * Helper automating kernel -> user object copies.
 *
 * Don't use this function directly, use PANTHOR_UOBJ_SET() instead.
 *
 * Return: 0 on success, a negative error code otherwise.
 */
static int panthor_set_uobj(u64 usr_ptr, u32 usr_size, u32 min_size,
			    u32 kern_size, const void *in)
{
	/* User size shouldn't be smaller than the minimal object size. */
	if (usr_size < min_size)
		return -EINVAL;

	if (copy_to_user(u64_to_user_ptr(usr_ptr), in,
			 min_t(u32, usr_size, kern_size)))
		return -EFAULT;

	/* When the kernel object is smaller than the user object, we fill the gap with
	 * zeros.
	 */
	if (usr_size > kern_size &&
	    clear_user(u64_to_user_ptr(usr_ptr + kern_size),
		       usr_size - kern_size)) {
		return -EFAULT;
	}

	return 0;
}

/**
 * panthor_get_uobj_array() - Copy a user object array into a kernel accessible object array.
 * @in: The object array to copy.
 * @min_stride: Minimum array stride.
 * @obj_size: Kernel object size.
 *
 * Helper automating user -> kernel object copies.
 *
 * Don't use this function directly, use PANTHOR_UOBJ_GET_ARRAY() instead.
 *
 * Return: newly allocated object array or an ERR_PTR on error.
 */
static void *panthor_get_uobj_array(const struct drm_panthor_obj_array *in,
				    u32 min_stride, u32 obj_size)
{
	int ret = 0;
	void *out_alloc;

	if (!in->count)
		return NULL;

	/* User stride must be at least the minimum object size, otherwise it might
	 * lack useful information.
	 */
	if (in->stride < min_stride)
		return ERR_PTR(-EINVAL);

	out_alloc = kvmalloc_array(in->count, obj_size, GFP_KERNEL);
	if (!out_alloc)
		return ERR_PTR(-ENOMEM);

	if (obj_size == in->stride) {
		/* Fast path when user/kernel have the same uAPI header version. */
		if (copy_from_user(out_alloc, u64_to_user_ptr(in->array),
				   (unsigned long)obj_size * in->count))
			ret = -EFAULT;
	} else {
		void __user *in_ptr = u64_to_user_ptr(in->array);
		void *out_ptr = out_alloc;

		/* If the sizes differ, we need to copy elements one by one. */
		for (u32 i = 0; i < in->count; i++) {
			ret = copy_struct_from_user(out_ptr, obj_size, in_ptr,
						    in->stride);
			if (ret)
				break;

			out_ptr += obj_size;
			in_ptr += in->stride;
		}
	}

	if (ret) {
		kvfree(out_alloc);
		return ERR_PTR(ret);
	}

	return out_alloc;
}

/**
 * PANTHOR_UOBJ_MIN_SIZE_INTERNAL() - Get the minimum user object size
 * @_typename: Object type.
 * @_last_mandatory_field: Last mandatory field.
 *
 * Get the minimum user object size based on the last mandatory field name,
 * A.K.A, the name of the last field of the structure at the time this
 * structure was added to the uAPI.
 *
 * Don't use directly, use PANTHOR_UOBJ_DECL() instead.
 */
#define PANTHOR_UOBJ_MIN_SIZE_INTERNAL(_typename, _last_mandatory_field) \
	(offsetof(_typename, _last_mandatory_field) +                    \
	 sizeof(((_typename *)NULL)->_last_mandatory_field))

/**
 * PANTHOR_UOBJ_DECL() - Declare a new uAPI object whose subject to
 * evolutions.
 * @_typename: Object type.
 * @_last_mandatory_field: Last mandatory field.
 *
 * Should be used to extend the PANTHOR_UOBJ_MIN_SIZE() list.
 */
#define PANTHOR_UOBJ_DECL(_typename, _last_mandatory_field) \
_typename:                                                  \
	PANTHOR_UOBJ_MIN_SIZE_INTERNAL(_typename, _last_mandatory_field)

/**
 * PANTHOR_UOBJ_MIN_SIZE() - Get the minimum size of a given uAPI object
 * @_obj_name: Object to get the minimum size of.
 *
 * Don't use this macro directly, it's automatically called by
 * PANTHOR_UOBJ_{SET,GET_ARRAY}().
 */
#define PANTHOR_UOBJ_MIN_SIZE(_obj_name)                                       \
	_Generic(_obj_name,                                                    \
		PANTHOR_UOBJ_DECL(struct drm_panthor_gpu_info, tiler_present), \
		PANTHOR_UOBJ_DECL(struct drm_panthor_csif_info, pad),          \
		PANTHOR_UOBJ_DECL(struct drm_panthor_sync_op, timeline_value), \
		PANTHOR_UOBJ_DECL(struct drm_panthor_queue_submit, syncs),     \
		PANTHOR_UOBJ_DECL(struct drm_panthor_queue_create,             \
				  ringbuf_size),                               \
		PANTHOR_UOBJ_DECL(struct drm_panthor_vm_bind_op, syncs))

/**
 * PANTHOR_UOBJ_SET() - Copy a kernel object to a user object.
 * @_dest_usr_ptr: User pointer to copy to.
 * @_usr_size: Size of the user object.
 * @_src_obj: Kernel object to copy (not a pointer).
 *
 * Return: 0 on success, a negative error code otherwise.
 */
#define PANTHOR_UOBJ_SET(_dest_usr_ptr, _usr_size, _src_obj)                \
	panthor_set_uobj(_dest_usr_ptr, _usr_size,                          \
			 PANTHOR_UOBJ_MIN_SIZE(_src_obj), sizeof(_src_obj), \
			 &(_src_obj))

/**
 * PANTHOR_UOBJ_GET_ARRAY() - Copy a user object array to a kernel accessible
 * object array.
 * @_dest_array: Local variable that will hold the newly allocated kernel
 * object array.
 * @_uobj_array: The drm_panthor_obj_array object describing the user object
 * array.
 *
 * Return: 0 on success, a negative error code otherwise.
 */
#define PANTHOR_UOBJ_GET_ARRAY(_dest_array, _uobj_array)                      \
	({                                                                    \
		typeof(_dest_array) _tmp;                                     \
		_tmp = panthor_get_uobj_array(                                \
			_uobj_array, PANTHOR_UOBJ_MIN_SIZE((_dest_array)[0]), \
			sizeof((_dest_array)[0]));                            \
		if (!IS_ERR(_tmp))                                            \
			_dest_array = _tmp;                                   \
		PTR_ERR_OR_ZERO(_tmp);                                        \
	})

/**
 * struct panthor_sync_signal - Represent a synchronization object point to attach
 * our job fence to.
 *
 * This structure is here to keep track of fences that are currently bound to
 * a specific syncobj point.
 *
 * At the beginning of a job submission, the fence
 * is retrieved from the syncobj itself, and can be NULL if no fence was attached
 * to this point.
 *
 * At the end, it points to the fence of the last job that had a
 * %DRM_PANTHOR_SYNC_OP_SIGNAL on this syncobj.
 *
 * With jobs being submitted in batches, the fence might change several times during
 * the process, allowing one job to wait on a job that's part of the same submission
 * but appears earlier in the drm_panthor_group_submit::queue_submits array.
 */
struct panthor_sync_signal {
	/** @node: list_head to track signal ops within a submit operation */
	struct list_head node;

	/** @handle: The syncobj handle. */
	u32 handle;

	/**
	 * @point: The syncobj point.
	 *
	 * Zero for regular syncobjs, and non-zero for timeline syncobjs.
	 */
	u64 point;

	/**
	 * @syncobj: The sync object pointed by @handle.
	 */
	struct drm_syncobj *syncobj;

	/**
	 * @chain: Chain object used to link the new fence to an existing
	 * timeline syncobj.
	 *
	 * NULL for regular syncobj, non-NULL for timeline syncobjs.
	 */
	struct dma_fence_chain *chain;

	/**
	 * @fence: The fence to assign to the syncobj or syncobj-point.
	 */
	struct dma_fence *fence;
};

/**
 * struct panthor_job_ctx - Job context
 */
struct panthor_job_ctx {
	/** @job: The job that is about to be submitted to drm_sched. */
	struct drm_sched_job *job;

	/** @syncops: Array of sync operations. */
	struct drm_panthor_sync_op *syncops;

	/** @syncop_count: Number of sync operations. */
	u32 syncop_count;
};

/**
 * struct panthor_submit_ctx - Submission context
 *
 * Anything that's related to a submission (%DRM_IOCTL_PANTHOR_VM_BIND or
 * %DRM_IOCTL_PANTHOR_GROUP_SUBMIT) is kept here, so we can automate the
 * initialization and cleanup steps.
 */
struct panthor_submit_ctx {
	/** @file: DRM file this submission happens on. */
	struct drm_file *file;

	/**
	 * @signals: List of struct panthor_sync_signal.
	 *
	 * %DRM_PANTHOR_SYNC_OP_SIGNAL operations will be recorded here,
	 * and %DRM_PANTHOR_SYNC_OP_WAIT will first check if an entry
	 * matching the syncobj+point exists before calling
	 * drm_syncobj_find_fence(). This allows us to describe dependencies
	 * existing between jobs that are part of the same batch.
	 */
	struct list_head signals;

	/** @jobs: Array of jobs. */
	struct panthor_job_ctx *jobs;

	/** @job_count: Number of entries in the @jobs array. */
	u32 job_count;

	/** @exec: drm_exec context used to acquire and prepare resv objects. */
	struct drm_exec exec;

};

#define PANTHOR_SYNC_OP_FLAGS_MASK \
	(DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_MASK | DRM_PANTHOR_SYNC_OP_SIGNAL)

static bool sync_op_is_signal(const struct drm_panthor_sync_op *sync_op)
{
	return !!(sync_op->flags & DRM_PANTHOR_SYNC_OP_SIGNAL);
}

static bool sync_op_is_wait(const struct drm_panthor_sync_op *sync_op)
{
	/* Note that DRM_PANTHOR_SYNC_OP_WAIT == 0 */
	return !(sync_op->flags & DRM_PANTHOR_SYNC_OP_SIGNAL);
}

/**
 * panthor_check_sync_op() - Check drm_panthor_sync_op fields
 * @sync_op: The sync operation to check.
 *
 * Return: 0 on success, -EINVAL otherwise.
 */
static int panthor_check_sync_op(const struct drm_panthor_sync_op *sync_op)
{
	u8 handle_type;

	if (sync_op->flags & ~PANTHOR_SYNC_OP_FLAGS_MASK)
		return -EINVAL;

	handle_type = sync_op->flags & DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_MASK;
	if (handle_type != DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_SYNCOBJ &&
	    handle_type != DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_TIMELINE_SYNCOBJ)
		return -EINVAL;

	if (handle_type == DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_SYNCOBJ &&
	    sync_op->timeline_value != 0)
		return -EINVAL;

	return 0;
}

/**
 * panthor_sync_signal_free() - Release resources and free a panthor_sync_signal object
 * @sig_sync: Signal object to free.
 */
static void panthor_sync_signal_free(struct panthor_sync_signal *sig_sync)
{
	if (!sig_sync)
		return;

	drm_syncobj_put(sig_sync->syncobj);
	dma_fence_chain_free(sig_sync->chain);
	dma_fence_put(sig_sync->fence);
	kfree(sig_sync);
}

/**
 * panthor_submit_ctx_add_sync_signal() - Add a signal operation to a submit context
 * @ctx: Context to add the signal operation to.
 * @handle: Syncobj handle.
 * @point: Syncobj point.
 *
 * Return: 0 on success, otherwise negative error value.
 */
static int panthor_submit_ctx_add_sync_signal(struct panthor_submit_ctx *ctx,
					      u32 handle, u64 point)
{
	struct panthor_sync_signal *sig_sync;
	struct dma_fence *cur_fence;
	int ret;

	sig_sync = kzalloc(sizeof(*sig_sync), GFP_KERNEL);
	if (!sig_sync)
		return -ENOMEM;

	sig_sync->handle = handle;
	sig_sync->point = point;

	if (point > 0) {
		sig_sync->chain = dma_fence_chain_alloc();
		if (!sig_sync->chain) {
			ret = -ENOMEM;
			goto err_free_sig_sync;
		}
	}

	sig_sync->syncobj = drm_syncobj_find(ctx->file, handle);
	if (!sig_sync->syncobj) {
		ret = -EINVAL;
		goto err_free_sig_sync;
	}

	/* Retrieve the current fence attached to that point. It's
	 * perfectly fine to get a NULL fence here, it just means there's
	 * no fence attached to that point yet.
	 */
	if (!drm_syncobj_find_fence(ctx->file, handle, point, 0, &cur_fence))
		sig_sync->fence = cur_fence;

	list_add_tail(&sig_sync->node, &ctx->signals);

	return 0;

err_free_sig_sync:
	panthor_sync_signal_free(sig_sync);
	return ret;
}

/**
 * panthor_submit_ctx_search_sync_signal() - Search an existing signal operation in a
 * submit context.
 * @ctx: Context to search the signal operation in.
 * @handle: Syncobj handle.
 * @point: Syncobj point.
 *
 * Return: A valid panthor_sync_signal object if found, NULL otherwise.
 */
static struct panthor_sync_signal *
panthor_submit_ctx_search_sync_signal(struct panthor_submit_ctx *ctx,
				      u32 handle, u64 point)
{
	struct panthor_sync_signal *sig_sync;

	list_for_each_entry(sig_sync, &ctx->signals, node) {
		if (handle == sig_sync->handle && point == sig_sync->point)
			return sig_sync;
	}

	return NULL;
}

/**
 * panthor_submit_ctx_add_job() - Add a job to a submit context
 * @ctx: Context to search the signal operation in.
 * @idx: Index of the job in the context.
 * @job: Job to add.
 * @syncs: Sync operations provided by userspace.
 *
 * Return: 0 on success, a negative error code otherwise.
 */
static int panthor_submit_ctx_add_job(struct panthor_submit_ctx *ctx, u32 idx,
				      struct drm_sched_job *job,
				      const struct drm_panthor_obj_array *syncs)
{
	int ret;

	ctx->jobs[idx].job = job;

	ret = PANTHOR_UOBJ_GET_ARRAY(ctx->jobs[idx].syncops, syncs);
	if (ret)
		return ret;

	ctx->jobs[idx].syncop_count = syncs->count;
	return 0;
}

static int
panthor_submit_ctx_add_job_kernel_syncs(struct panthor_submit_ctx *ctx, u32 idx,
					struct drm_sched_job *job,
					const struct drm_panthor_sync_op *syncs,
					u32 sync_count)
{
	ctx->jobs[idx].job = job;

	if (sync_count) {
		if (!syncs)
			return -EINVAL;

		ctx->jobs[idx].syncops =
			kvmalloc_array(sync_count, sizeof(*syncs), GFP_KERNEL);
		if (!ctx->jobs[idx].syncops)
			return -ENOMEM;

		memcpy(ctx->jobs[idx].syncops, syncs,
		       sizeof(*syncs) * sync_count);
	}

	ctx->jobs[idx].syncop_count = sync_count;
	return 0;
}

/**
 * panthor_submit_ctx_get_sync_signal() - Search signal operation and add one if none was found.
 * @ctx: Context to search the signal operation in.
 * @handle: Syncobj handle.
 * @point: Syncobj point.
 *
 * Return: 0 on success, a negative error code otherwise.
 */
static int panthor_submit_ctx_get_sync_signal(struct panthor_submit_ctx *ctx,
					      u32 handle, u64 point)
{
	struct panthor_sync_signal *sig_sync;

	sig_sync = panthor_submit_ctx_search_sync_signal(ctx, handle, point);
	if (sig_sync)
		return 0;

	return panthor_submit_ctx_add_sync_signal(ctx, handle, point);
}

/**
 * panthor_submit_ctx_update_job_sync_signal_fences() - Update fences
 * on the signal operations specified by a job.
 * @ctx: Context to search the signal operation in.
 * @job_idx: Index of the job to operate on.
 *
 * Return: 0 on success, a negative error code otherwise.
 */
static int
panthor_submit_ctx_update_job_sync_signal_fences(struct panthor_submit_ctx *ctx,
						 u32 job_idx)
{
	struct panthor_device *ptdev = container_of(
		ctx->file->minor->dev, struct panthor_device, base);
	struct dma_fence *done_fence =
		&ctx->jobs[job_idx].job->s_fence->finished;
	const struct drm_panthor_sync_op *sync_ops = ctx->jobs[job_idx].syncops;
	u32 sync_op_count = ctx->jobs[job_idx].syncop_count;

	for (u32 i = 0; i < sync_op_count; i++) {
		struct dma_fence *old_fence;
		struct panthor_sync_signal *sig_sync;

		if (!sync_op_is_signal(&sync_ops[i]))
			continue;

		sig_sync = panthor_submit_ctx_search_sync_signal(
			ctx, sync_ops[i].handle, sync_ops[i].timeline_value);
		if (drm_WARN_ON(&ptdev->base, !sig_sync))
			return -EINVAL;

		old_fence = sig_sync->fence;
		sig_sync->fence = dma_fence_get(done_fence);
		dma_fence_put(old_fence);

		if (drm_WARN_ON(&ptdev->base, !sig_sync->fence))
			return -EINVAL;
	}

	return 0;
}

/**
 * panthor_submit_ctx_collect_job_signal_ops() - Iterate over all job signal operations
 * and add them to the context.
 * @ctx: Context to search the signal operation in.
 * @job_idx: Index of the job to operate on.
 *
 * Return: 0 on success, a negative error code otherwise.
 */
static int
panthor_submit_ctx_collect_job_signal_ops(struct panthor_submit_ctx *ctx,
					  u32 job_idx)
{
	const struct drm_panthor_sync_op *sync_ops = ctx->jobs[job_idx].syncops;
	u32 sync_op_count = ctx->jobs[job_idx].syncop_count;

	for (u32 i = 0; i < sync_op_count; i++) {
		int ret;

		if (!sync_op_is_signal(&sync_ops[i]))
			continue;

		ret = panthor_check_sync_op(&sync_ops[i]);
		if (ret)
			return ret;

		ret = panthor_submit_ctx_get_sync_signal(
			ctx, sync_ops[i].handle, sync_ops[i].timeline_value);
		if (ret)
			return ret;
	}

	return 0;
}

/**
 * panthor_submit_ctx_push_fences() - Iterate over the signal array, and for each entry, push
 * the currently assigned fence to the associated syncobj.
 * @ctx: Context to push fences on.
 *
 * This is the last step of a submission procedure, and is done once we know the submission
 * is effective and job fences are guaranteed to be signaled in finite time.
 */
static void panthor_submit_ctx_push_fences(struct panthor_submit_ctx *ctx)
{
	struct panthor_sync_signal *sig_sync;

	list_for_each_entry(sig_sync, &ctx->signals, node) {
		if (sig_sync->chain) {
			drm_syncobj_add_point(sig_sync->syncobj,
					      sig_sync->chain, sig_sync->fence,
					      sig_sync->point);
			sig_sync->chain = NULL;
		} else {
			drm_syncobj_replace_fence(sig_sync->syncobj,
						  sig_sync->fence);
		}
	}
}

/**
 * panthor_submit_ctx_add_sync_deps_to_job() - Add sync wait operations as
 * job dependencies.
 * @ctx: Submit context.
 * @job_idx: Index of the job to operate on.
 *
 * Return: 0 on success, a negative error code otherwise.
 */
static int
panthor_submit_ctx_add_sync_deps_to_job(struct panthor_submit_ctx *ctx,
					u32 job_idx)
{
	struct panthor_device *ptdev = container_of(
		ctx->file->minor->dev, struct panthor_device, base);
	const struct drm_panthor_sync_op *sync_ops = ctx->jobs[job_idx].syncops;
	struct drm_sched_job *job = ctx->jobs[job_idx].job;
	u32 sync_op_count = ctx->jobs[job_idx].syncop_count;
	int ret = 0;

	for (u32 i = 0; i < sync_op_count; i++) {
		struct panthor_sync_signal *sig_sync;
		struct dma_fence *fence;

		if (!sync_op_is_wait(&sync_ops[i]))
			continue;

		ret = panthor_check_sync_op(&sync_ops[i]);
		if (ret)
			return ret;

		sig_sync = panthor_submit_ctx_search_sync_signal(
			ctx, sync_ops[i].handle, sync_ops[i].timeline_value);
		if (sig_sync) {
			if (drm_WARN_ON(&ptdev->base, !sig_sync->fence))
				return -EINVAL;

			fence = dma_fence_get(sig_sync->fence);
		} else {
			ret = drm_syncobj_find_fence(ctx->file,
						     sync_ops[i].handle,
						     sync_ops[i].timeline_value,
						     0, &fence);
			if (ret)
				return ret;
		}

		ret = drm_sched_job_add_dependency(job, fence);
		if (ret)
			return ret;
	}

	return 0;
}

/**
 * panthor_submit_ctx_collect_jobs_signal_ops() - Collect all signal operations
 * and add them to the submit context.
 * @ctx: Submit context.
 *
 * Return: 0 on success, a negative error code otherwise.
 */
static int
panthor_submit_ctx_collect_jobs_signal_ops(struct panthor_submit_ctx *ctx)
{
	for (u32 i = 0; i < ctx->job_count; i++) {
		int ret;

		ret = panthor_submit_ctx_collect_job_signal_ops(ctx, i);
		if (ret)
			return ret;
	}

	return 0;
}

/**
 * panthor_submit_ctx_add_deps_and_arm_jobs() - Add jobs dependencies and arm jobs
 * @ctx: Submit context.
 *
 * Must be called after the resv preparation has been taken care of.
 *
 * Return: 0 on success, a negative error code otherwise.
 */
static int
panthor_submit_ctx_add_deps_and_arm_jobs(struct panthor_submit_ctx *ctx)
{
	for (u32 i = 0; i < ctx->job_count; i++) {
		int ret;

		ret = panthor_submit_ctx_add_sync_deps_to_job(ctx, i);
		if (ret)
			return ret;

		drm_sched_job_arm(ctx->jobs[i].job);

		ret = panthor_submit_ctx_update_job_sync_signal_fences(ctx, i);
		if (ret)
			return ret;
	}

	return 0;
}

/**
 * panthor_submit_ctx_push_jobs() - Push jobs to their scheduling entities.
 * @ctx: Submit context.
 * @upd_resvs: Callback used to update reservation objects that were previously
 * preapred.
 */
static void panthor_submit_ctx_push_jobs(
	struct panthor_submit_ctx *ctx,
	void (*upd_resvs)(struct drm_exec *, struct drm_sched_job *))
{
	for (u32 i = 0; i < ctx->job_count; i++) {
		upd_resvs(&ctx->exec, ctx->jobs[i].job);
		drm_sched_entity_push_job(ctx->jobs[i].job);

		/* Job is owned by the scheduler now. */
		ctx->jobs[i].job = NULL;
	}

	panthor_submit_ctx_push_fences(ctx);
}

static void panthor_submit_ctx_push_jobs_stats(
	struct panthor_submit_ctx *ctx,
	void (*upd_resvs)(struct drm_exec *, struct drm_sched_job *),
	struct panthor_submit_stats_sample *stats)
{
	bool stats_enabled = stats->start_ns;
	u64 stats_phase_start;

	for (u32 i = 0; i < ctx->job_count; i++) {
		stats_phase_start = panthor_submit_stats_now(stats_enabled);
		upd_resvs(&ctx->exec, ctx->jobs[i].job);
		panthor_submit_stats_accum(&stats->push_update_resvs_ns,
					   stats_phase_start);

		stats_phase_start = panthor_submit_stats_now(stats_enabled);
		drm_sched_entity_push_job(ctx->jobs[i].job);
		panthor_submit_stats_accum(&stats->push_entity_ns,
					   stats_phase_start);

		/* Job is owned by the scheduler now. */
		ctx->jobs[i].job = NULL;
	}

	stats_phase_start = panthor_submit_stats_now(stats_enabled);
	panthor_submit_ctx_push_fences(ctx);
	panthor_submit_stats_accum(&stats->push_fences_ns, stats_phase_start);
}

/**
 * panthor_submit_ctx_init() - Initializes a submission context
 * @ctx: Submit context to initialize.
 * @file: drm_file this submission happens on.
 * @job_count: Number of jobs that will be submitted.
 *
 * Return: 0 on success, a negative error code otherwise.
 */
static int panthor_submit_ctx_init(struct panthor_submit_ctx *ctx,
				   struct drm_file *file, u32 job_count)
{
	ctx->jobs = kvmalloc_array(job_count, sizeof(*ctx->jobs),
				   GFP_KERNEL | __GFP_ZERO);
	if (!ctx->jobs)
		return -ENOMEM;

	ctx->file = file;
	ctx->job_count = job_count;
	INIT_LIST_HEAD(&ctx->signals);
	drm_exec_init(&ctx->exec,
		      DRM_EXEC_INTERRUPTIBLE_WAIT | DRM_EXEC_IGNORE_DUPLICATES,
		      0);
	return 0;
}

/**
 * panthor_submit_ctx_cleanup() - Cleanup a submission context
 * @ctx: Submit context to cleanup.
 * @job_put: Job put callback.
 */
static void panthor_submit_ctx_cleanup(struct panthor_submit_ctx *ctx,
				       void (*job_put)(struct drm_sched_job *))
{
	struct panthor_sync_signal *sig_sync, *tmp;
	unsigned long i;

	drm_exec_fini(&ctx->exec);

	list_for_each_entry_safe(sig_sync, tmp, &ctx->signals, node)
		panthor_sync_signal_free(sig_sync);

	for (i = 0; i < ctx->job_count; i++) {
		job_put(ctx->jobs[i].job);
		kvfree(ctx->jobs[i].syncops);
	}

	kvfree(ctx->jobs);
}

static int panthor_ioctl_dev_query(struct drm_device *ddev, void *data,
				   struct drm_file *file)
{
	struct panthor_device *ptdev =
		container_of(ddev, struct panthor_device, base);
	struct drm_panthor_dev_query *args = data;

	if (!args->pointer) {
		switch (args->type) {
		case DRM_PANTHOR_DEV_QUERY_GPU_INFO:
			args->size = sizeof(ptdev->gpu_info);
			return 0;

		case DRM_PANTHOR_DEV_QUERY_CSIF_INFO:
			args->size = sizeof(ptdev->csif_info);
			return 0;

		default:
			return -EINVAL;
		}
	}

	switch (args->type) {
	case DRM_PANTHOR_DEV_QUERY_GPU_INFO:
		return PANTHOR_UOBJ_SET(args->pointer, args->size,
					ptdev->gpu_info);

	case DRM_PANTHOR_DEV_QUERY_CSIF_INFO:
		return PANTHOR_UOBJ_SET(args->pointer, args->size,
					ptdev->csif_info);

	default:
		return -EINVAL;
	}
}

static int
panthor_vmshm_fill_dev_query(struct panthor_device *ptdev,
			     const struct panthor_vmshm_dev_query_req *req,
			     struct panthor_vmshm_dev_query_rsp *rsp)
{
	const void *info;
	u32 info_size, min_size;

	if (req->flags & ~PANTHOR_VMSHM_DEV_QUERY_F_DATA)
		return -EINVAL;

	memset(rsp, 0, sizeof(*rsp));
	rsp->type = req->type;

	switch (req->type) {
	case DRM_PANTHOR_DEV_QUERY_GPU_INFO:
		info = &ptdev->gpu_info;
		info_size = sizeof(ptdev->gpu_info);
		min_size = PANTHOR_UOBJ_MIN_SIZE(ptdev->gpu_info);
		break;

	case DRM_PANTHOR_DEV_QUERY_CSIF_INFO:
		info = &ptdev->csif_info;
		info_size = sizeof(ptdev->csif_info);
		min_size = PANTHOR_UOBJ_MIN_SIZE(ptdev->csif_info);
		break;

	default:
		return -EINVAL;
	}

	rsp->size = info_size;
	if (!(req->flags & PANTHOR_VMSHM_DEV_QUERY_F_DATA))
		return 0;

	if (req->size < min_size)
		return -EINVAL;
	if (info_size > sizeof(rsp->data))
		return -EOVERFLOW;

	rsp->data_len = min(req->size, info_size);
	memcpy(rsp->data, info, rsp->data_len);
	return 0;
}

int panthor_vmshm_dev_query(const struct panthor_vmshm_dev_query_req *req,
			    struct panthor_vmshm_dev_query_rsp *rsp)
{
	struct panthor_device *ptdev;
	int cookie, ret;

	if (!req || !rsp)
		return -EINVAL;

	mutex_lock(&panthor_vmshm_lock);
	ptdev = panthor_vmshm_ptdev;
	if (ptdev)
		drm_dev_get(&ptdev->base);
	mutex_unlock(&panthor_vmshm_lock);

	if (!ptdev) {
		memset(rsp, 0, sizeof(*rsp));
		rsp->type = req->type;
		rsp->ret = -ENODEV;
		return 0;
	}

	if (!drm_dev_enter(&ptdev->base, &cookie)) {
		ret = -ENODEV;
		goto out_put;
	}

	ret = panthor_vmshm_fill_dev_query(ptdev, req, rsp);
	drm_dev_exit(cookie);

out_put:
	drm_dev_put(&ptdev->base);
	if (ret) {
		memset(rsp, 0, sizeof(*rsp));
		rsp->type = req->type;
		rsp->ret = ret;
	} else {
		rsp->ret = 0;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(panthor_vmshm_dev_query);

int panthor_vmshm_session_open(struct panthor_vmshm_session **session)
{
	struct panthor_vmshm_session *vmshm_session;
	struct panthor_device *ptdev;
	struct panthor_file *pfile;
	int ret;

	if (!session)
		return -EINVAL;

	*session = NULL;

	mutex_lock(&panthor_vmshm_lock);
	ptdev = panthor_vmshm_ptdev;
	if (ptdev)
		drm_dev_get(&ptdev->base);
	mutex_unlock(&panthor_vmshm_lock);

	if (!ptdev)
		return -ENODEV;

	if (!try_module_get(THIS_MODULE)) {
		ret = -EINVAL;
		goto err_put_dev;
	}

	vmshm_session = kzalloc(sizeof(*vmshm_session), GFP_KERNEL);
	if (!vmshm_session) {
		ret = -ENOMEM;
		goto err_put_module;
	}

	pfile = kzalloc(sizeof(*pfile), GFP_KERNEL);
	if (!pfile) {
		ret = -ENOMEM;
		goto err_free_session;
	}

	pfile->ptdev = ptdev;

	vmshm_session->file.minor = ptdev->base.render ? ptdev->base.render :
						    ptdev->base.primary;
	if (!vmshm_session->file.minor) {
		ret = -ENODEV;
		goto err_free_pfile;
	}

	idr_init_base(&vmshm_session->file.object_idr, 1);
	spin_lock_init(&vmshm_session->file.table_lock);
	vmshm_session->gem_idr_init = true;
	idr_init_base(&vmshm_session->file.syncobj_idr, 1);
	spin_lock_init(&vmshm_session->file.syncobj_table_lock);
	vmshm_session->syncobj_idr_init = true;

	ret = panthor_vm_pool_create(pfile);
	if (ret)
		goto err_syncobj_idr_destroy;

	ret = panthor_group_pool_create(pfile);
	if (ret)
		goto err_destroy_vm_pool;

	vmshm_session->pfile = pfile;
	vmshm_session->file.driver_priv = pfile;
	atomic_inc(&panthor_vmshm_active_sessions);
	*session = vmshm_session;
	return 0;

err_destroy_vm_pool:
	panthor_vm_pool_destroy(pfile);

err_syncobj_idr_destroy:
	idr_destroy(&vmshm_session->file.syncobj_idr);

	idr_destroy(&vmshm_session->file.object_idr);

err_free_pfile:
	kfree(pfile);

err_free_session:
	kfree(vmshm_session);

err_put_module:
	module_put(THIS_MODULE);

err_put_dev:
	drm_dev_put(&ptdev->base);
	return ret;
}
EXPORT_SYMBOL_GPL(panthor_vmshm_session_open);

void panthor_vmshm_session_close(struct panthor_vmshm_session *session)
{
	struct panthor_device *ptdev;
	struct panthor_file *pfile;
	bool last_session;

	if (!session)
		return;

	pfile = session->pfile;
	if (!pfile) {
		kfree(session);
		return;
	}

	ptdev = pfile->ptdev;
	panthor_group_pool_destroy(pfile);
	panthor_vm_pool_destroy(pfile);
	if (session->gem_idr_init) {
		panthor_vmshm_release_gem_handles(session);
		idr_destroy(&session->file.object_idr);
	}
	if (session->syncobj_idr_init) {
		idr_for_each(&session->file.syncobj_idr,
			     panthor_vmshm_release_syncobj_handle, session);
		idr_destroy(&session->file.syncobj_idr);
	}
	last_session = atomic_dec_and_test(&panthor_vmshm_active_sessions);
	if (last_session) {
		panthor_submit_stats_dump_current_if_enabled("vmshm-last-session-close");
		panthor_job_irq_stats_dump_current_if_enabled("vmshm-last-session-close");
	}
	kfree(pfile);
	kfree(session);
	drm_dev_put(&ptdev->base);
	module_put(THIS_MODULE);
}
EXPORT_SYMBOL_GPL(panthor_vmshm_session_close);

int panthor_vmshm_vm_create(struct panthor_vmshm_session *session,
			    struct drm_panthor_vm_create *args)
{
	struct panthor_file *pfile;
	int cookie, ret;

	if (!session || !session->pfile || !args)
		return -EINVAL;

	pfile = session->pfile;
	if (!drm_dev_enter(&pfile->ptdev->base, &cookie))
		return -ENODEV;

	ret = panthor_vm_pool_create_vm(pfile->ptdev, pfile->vms, args);
	if (ret >= 0) {
		args->id = ret;
		ret = 0;
	}

	drm_dev_exit(cookie);
	return ret;
}
EXPORT_SYMBOL_GPL(panthor_vmshm_vm_create);

int panthor_vmshm_vm_destroy(struct panthor_vmshm_session *session,
			     __u32 vm_id)
{
	struct panthor_file *pfile;
	int cookie, ret;

	if (!session || !session->pfile)
		return -EINVAL;

	pfile = session->pfile;
	if (!drm_dev_enter(&pfile->ptdev->base, &cookie))
		return -ENODEV;

	ret = panthor_vm_pool_destroy_vm(pfile->vms, vm_id);

	drm_dev_exit(cookie);
	return ret;
}
EXPORT_SYMBOL_GPL(panthor_vmshm_vm_destroy);

int panthor_vmshm_bo_create(struct panthor_vmshm_session *session,
			    struct drm_panthor_bo_create *args)
{
	struct panthor_file *pfile;
	struct panthor_vm *vm = NULL;
	u64 size;
	int cookie, ret;

	if (!session || !session->pfile || !args)
		return -EINVAL;

	pfile = session->pfile;
	if (!drm_dev_enter(&pfile->ptdev->base, &cookie))
		return -ENODEV;

	if (!args->size || args->pad || (args->flags & ~PANTHOR_BO_FLAGS)) {
		ret = -EINVAL;
		goto out_dev_exit;
	}

	size = args->size;

	if (args->exclusive_vm_id) {
		vm = panthor_vm_pool_get_vm(pfile->vms, args->exclusive_vm_id);
		if (!vm) {
			ret = -EINVAL;
			goto out_dev_exit;
		}
	}

	ret = panthor_gem_create_with_handle(&session->file, &pfile->ptdev->base,
					     vm, &size, args->flags,
					     &args->handle);
	if (!ret)
		args->size = size;

	panthor_vm_put(vm);

out_dev_exit:
	drm_dev_exit(cookie);
	return ret;
}
EXPORT_SYMBOL_GPL(panthor_vmshm_bo_create);

int panthor_vmshm_bo_create_from_payload(struct panthor_vmshm_session *session,
					 struct drm_panthor_bo_create *args,
					 struct proxy_vmshm_object *payload)
{
	struct panthor_file *pfile;
	struct panthor_vm *vm = NULL;
	u64 size, payload_size;
	int cookie, ret;

	if (!session || !session->pfile || !args || !payload)
		return -EINVAL;

	pfile = session->pfile;
	if (!drm_dev_enter(&pfile->ptdev->base, &cookie))
		return -ENODEV;

	if (!args->size || args->pad || (args->flags & ~PANTHOR_BO_FLAGS)) {
		ret = -EINVAL;
		goto out_dev_exit;
	}

	payload_size = proxy_vmshm_obj_size(payload);
	if (!payload_size || args->size > payload_size) {
		ret = -EINVAL;
		goto out_dev_exit;
	}

	size = args->size;

	if (args->exclusive_vm_id) {
		vm = panthor_vm_pool_get_vm(pfile->vms, args->exclusive_vm_id);
		if (!vm) {
			ret = -EINVAL;
			goto out_dev_exit;
		}
	}

	ret = panthor_gem_create_vmshm_with_handle(&session->file,
						   &pfile->ptdev->base,
						   vm, &size, args->flags,
						   &args->handle, payload);
	if (!ret) {
		args->size = size;
		pr_info_ratelimited("panthor: BO_CREATE vmshm-backed handle=%u size=0x%llx payload=0x%llx payload_size=0x%llx segments=%u\n",
				    args->handle, args->size,
				    proxy_vmshm_obj_handle(payload),
				    (unsigned long long)payload_size,
				    proxy_vmshm_obj_nr_segments(payload));
	}

	panthor_vm_put(vm);

out_dev_exit:
	drm_dev_exit(cookie);
	return ret;
}
EXPORT_SYMBOL_GPL(panthor_vmshm_bo_create_from_payload);

int panthor_vmshm_bo_destroy(struct panthor_vmshm_session *session,
			     __u32 bo_handle)
{
	if (!session || !session->pfile)
		return -EINVAL;

	return drm_gem_handle_delete(&session->file, bo_handle);
}
EXPORT_SYMBOL_GPL(panthor_vmshm_bo_destroy);

int panthor_vmshm_syncobj_create(struct panthor_vmshm_session *session,
				 __u32 flags, __u32 *handle)
{
	struct drm_syncobj *syncobj;
	int ret;

	if (!session || !session->pfile || !handle)
		return -EINVAL;
	if (flags & ~DRM_SYNCOBJ_CREATE_SIGNALED)
		return -EINVAL;

	ret = drm_syncobj_create(&syncobj, flags, NULL);
	if (ret)
		return ret;

	ret = drm_syncobj_get_handle(&session->file, syncobj, handle);
	drm_syncobj_put(syncobj);
	return ret;
}
EXPORT_SYMBOL_GPL(panthor_vmshm_syncobj_create);

int panthor_vmshm_syncobj_destroy(struct panthor_vmshm_session *session,
				  __u32 handle)
{
	struct drm_syncobj *syncobj;

	if (!session || !session->pfile || !handle)
		return -EINVAL;

	spin_lock(&session->file.syncobj_table_lock);
	syncobj = idr_remove(&session->file.syncobj_idr, handle);
	spin_unlock(&session->file.syncobj_table_lock);

	if (!syncobj)
		return -EINVAL;

	drm_syncobj_put(syncobj);
	return 0;
}
EXPORT_SYMBOL_GPL(panthor_vmshm_syncobj_destroy);

static s64 panthor_vmshm_rel_timeout_to_abs_ns(s64 rel_timeout_ns)
{
	s64 now_ns;

	if (rel_timeout_ns <= 0)
		return 0;

	now_ns = ktime_get_ns();
	if (rel_timeout_ns > S64_MAX - now_ns)
		return S64_MAX;

	return now_ns + rel_timeout_ns;
}

static u64 panthor_vmshm_rel_deadline_to_abs_ns(u64 rel_deadline_ns)
{
	u64 now_ns;

	if (!rel_deadline_ns)
		return 0;

	now_ns = ktime_get_ns();
	if (rel_deadline_ns > U64_MAX - now_ns)
		return U64_MAX;

	return now_ns + rel_deadline_ns;
}

int panthor_vmshm_syncobj_wait(struct panthor_vmshm_session *session,
			       const __u32 *handles, __u32 count_handles,
			       __s64 timeout_rel_nsec, __u32 flags,
			       __u64 deadline_rel_nsec, __u32 *first_signaled)
{
	s64 timeout_abs_nsec;
	u64 deadline_abs_nsec = 0;

	if (!session || !session->pfile ||
	    (count_handles && !handles) || !first_signaled)
		return -EINVAL;
	if (count_handles > PANTHOR_VMSHM_MAX_SYNCOBJ_WAIT_HANDLES)
		return -E2BIG;

	timeout_abs_nsec =
		panthor_vmshm_rel_timeout_to_abs_ns(timeout_rel_nsec);
	if (flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE)
		deadline_abs_nsec =
			panthor_vmshm_rel_deadline_to_abs_ns(deadline_rel_nsec);

	return drm_syncobj_wait_handles(&session->file, handles, count_handles,
					timeout_abs_nsec, flags,
					deadline_abs_nsec, first_signaled);
}
EXPORT_SYMBOL_GPL(panthor_vmshm_syncobj_wait);

int panthor_vmshm_syncobj_transfer(struct panthor_vmshm_session *session,
				   struct drm_syncobj_transfer *args)
{
	if (!session || !session->pfile || !args)
		return -EINVAL;

	return drm_syncobj_transfer(&session->file, args);
}
EXPORT_SYMBOL_GPL(panthor_vmshm_syncobj_transfer);

int panthor_vmshm_syncobj_timeline_wait(
	struct panthor_vmshm_session *session, const __u32 *handles,
	const __u64 *points, __u32 count_handles, __s64 timeout_rel_nsec,
	__u32 flags, __u64 deadline_rel_nsec, __u32 *first_signaled)
{
	s64 timeout_abs_nsec;
	u64 deadline_abs_nsec = 0;

	if (!session || !session->pfile || !first_signaled ||
	    (count_handles && (!handles || !points)))
		return -EINVAL;
	if (count_handles > PANTHOR_VMSHM_MAX_SYNCOBJ_WAIT_HANDLES)
		return -E2BIG;

	timeout_abs_nsec =
		panthor_vmshm_rel_timeout_to_abs_ns(timeout_rel_nsec);
	if (flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE)
		deadline_abs_nsec =
			panthor_vmshm_rel_deadline_to_abs_ns(deadline_rel_nsec);

	return drm_syncobj_timeline_wait_handles(
		&session->file, handles, points, count_handles,
		timeout_abs_nsec, flags, deadline_abs_nsec, first_signaled);
}
EXPORT_SYMBOL_GPL(panthor_vmshm_syncobj_timeline_wait);

int panthor_vmshm_syncobj_reset(struct panthor_vmshm_session *session,
				const __u32 *handles, __u32 count_handles)
{
	if (!session || !session->pfile || (count_handles && !handles))
		return -EINVAL;
	if (count_handles > PANTHOR_VMSHM_MAX_SYNCOBJ_WAIT_HANDLES)
		return -E2BIG;

	return drm_syncobj_reset_handles(&session->file, handles,
					 count_handles);
}
EXPORT_SYMBOL_GPL(panthor_vmshm_syncobj_reset);

int panthor_vmshm_syncobj_signal(struct panthor_vmshm_session *session,
				 const __u32 *handles, __u32 count_handles)
{
	if (!session || !session->pfile || (count_handles && !handles))
		return -EINVAL;
	if (count_handles > PANTHOR_VMSHM_MAX_SYNCOBJ_WAIT_HANDLES)
		return -E2BIG;

	return drm_syncobj_signal_handles(&session->file, handles,
					  count_handles);
}
EXPORT_SYMBOL_GPL(panthor_vmshm_syncobj_signal);

int panthor_vmshm_syncobj_timeline_signal(
	struct panthor_vmshm_session *session, const __u32 *handles,
	const __u64 *points, __u32 count_handles, __u32 flags)
{
	if (!session || !session->pfile ||
	    (count_handles && (!handles || !points)))
		return -EINVAL;
	if (count_handles > PANTHOR_VMSHM_MAX_SYNCOBJ_WAIT_HANDLES)
		return -E2BIG;

	return drm_syncobj_timeline_signal_handles(&session->file, handles,
						   points, count_handles,
						   flags);
}
EXPORT_SYMBOL_GPL(panthor_vmshm_syncobj_timeline_signal);

int panthor_vmshm_syncobj_query(struct panthor_vmshm_session *session,
				const __u32 *handles, __u64 *points,
				__u32 count_handles, __u32 flags)
{
	if (!session || !session->pfile ||
	    (count_handles && (!handles || !points)))
		return -EINVAL;
	if (count_handles > PANTHOR_VMSHM_MAX_SYNCOBJ_WAIT_HANDLES)
		return -E2BIG;

	return drm_syncobj_query_handles(&session->file, handles, points,
					 count_handles, flags);
}
EXPORT_SYMBOL_GPL(panthor_vmshm_syncobj_query);

static int
panthor_vmshm_vm_bind_async(struct panthor_vmshm_session *session,
			    struct panthor_vm *vm,
			    const struct drm_panthor_vm_bind_op *ops,
			    __u32 op_count,
			    const struct drm_panthor_sync_op *syncs,
			    const __u32 *sync_starts,
			    const __u32 *sync_counts, __u32 sync_count,
			    __u32 *failed_op)
{
	struct panthor_submit_ctx ctx;
	int ret;

	if (!op_count)
		return 0;
	if (!sync_starts || !sync_counts)
		return -EINVAL;

	for (u32 i = 0; i < op_count; i++) {
		__u32 sync_start = sync_starts[i];
		__u32 op_sync_count = sync_counts[i];
		__u32 op_type = ops[i].flags & DRM_PANTHOR_VM_BIND_OP_TYPE_MASK;

		if (op_sync_count &&
		    (!syncs || sync_start >= sync_count ||
		     op_sync_count > sync_count ||
		     sync_start > sync_count - op_sync_count))
			return -EINVAL;

		if (op_type == DRM_PANTHOR_VM_BIND_OP_TYPE_SYNC_ONLY &&
		    !op_sync_count)
			return -EINVAL;

		for (u32 j = 0; j < op_sync_count; j++) {
			ret = panthor_check_sync_op(&syncs[sync_start + j]);
			if (ret)
				return ret;
		}
	}

	ret = panthor_submit_ctx_init(&ctx, &session->file, op_count);
	if (ret)
		return ret;

	for (u32 i = 0; i < op_count; i++) {
		struct drm_sched_job *job;
		struct drm_panthor_vm_bind_op op = ops[i];
		const struct drm_panthor_sync_op *op_syncs = NULL;

		op.syncs.count = sync_counts[i];
		op.syncs.stride = sizeof(*op_syncs);
		op.syncs.array = 0;

		job = panthor_vm_bind_job_create(&session->file, vm, &op);
		if (IS_ERR(job)) {
			ret = PTR_ERR(job);
			if (failed_op)
				*failed_op = i;
			goto out_cleanup_submit_ctx;
		}

		if (sync_counts[i])
			op_syncs = &syncs[sync_starts[i]];

		ret = panthor_submit_ctx_add_job_kernel_syncs(
			&ctx, i, job, op_syncs, sync_counts[i]);
		if (ret) {
			if (failed_op)
				*failed_op = i;
			goto out_cleanup_submit_ctx;
		}
	}

	ret = panthor_submit_ctx_collect_jobs_signal_ops(&ctx);
	if (ret)
		goto out_cleanup_submit_ctx;

	drm_exec_until_all_locked(&ctx.exec)
	{
		for (u32 i = 0; i < ctx.job_count; i++) {
			ret = panthor_vm_bind_job_prepare_resvs(&ctx.exec,
							       ctx.jobs[i].job);
			drm_exec_retry_on_contention(&ctx.exec);
			if (ret)
				goto out_cleanup_submit_ctx;
		}
	}

	ret = panthor_submit_ctx_add_deps_and_arm_jobs(&ctx);
	if (ret)
		goto out_cleanup_submit_ctx;

	panthor_submit_ctx_push_jobs(&ctx, panthor_vm_bind_job_update_resvs);

out_cleanup_submit_ctx:
	panthor_submit_ctx_cleanup(&ctx, panthor_vm_bind_job_put);
	return ret;
}

int panthor_vmshm_vm_bind(struct panthor_vmshm_session *session, __u32 vm_id,
			  struct drm_panthor_vm_bind_op *ops, __u32 op_count,
			  const struct drm_panthor_sync_op *syncs,
			  const __u32 *sync_starts,
			  const __u32 *sync_counts, __u32 sync_count,
			  __u32 flags,
			  __u32 *failed_op)
{
	struct panthor_file *pfile;
	struct panthor_vm *vm;
	int cookie, ret = 0;
	u32 i;

	if (failed_op)
		*failed_op = PANTHOR_VMSHM_VM_BIND_FAILED_OP_NONE;

	if (!session || !session->pfile || (op_count && !ops))
		return -EINVAL;
	if (op_count > PANTHOR_VMSHM_MAX_VM_BIND_OPS)
		return -E2BIG;
	if (flags & ~DRM_PANTHOR_VM_BIND_ASYNC)
		return -EINVAL;
	if (sync_count > PANTHOR_VMSHM_MAX_VM_BIND_SYNCS)
		return -E2BIG;
	if (sync_count && (!syncs || !sync_starts || !sync_counts))
		return -EINVAL;

	pfile = session->pfile;
	if (!drm_dev_enter(&pfile->ptdev->base, &cookie))
		return -ENODEV;

	vm = panthor_vm_pool_get_vm(pfile->vms, vm_id);
	if (!vm) {
		ret = -EINVAL;
		goto out_dev_exit;
	}

	if (flags & DRM_PANTHOR_VM_BIND_ASYNC) {
		ret = panthor_vmshm_vm_bind_async(session, vm, ops,
						  op_count, syncs, sync_starts,
						  sync_counts, sync_count,
						  failed_op);
	} else {
		if (sync_count)
			ret = -EINVAL;

		for (i = 0; !ret && i < op_count; i++) {
			ret = panthor_vm_bind_exec_sync_op(&session->file, vm,
							   &ops[i]);
			if (ret) {
				if (failed_op)
					*failed_op = i;
				break;
			}
		}
	}

	panthor_vm_put(vm);

out_dev_exit:
	drm_dev_exit(cookie);
	return ret;
}
EXPORT_SYMBOL_GPL(panthor_vmshm_vm_bind);

int panthor_vmshm_vm_get_state(struct panthor_vmshm_session *session,
				       __u32 vm_id, __u32 *state)
{
	struct panthor_file *pfile;
	struct panthor_vm *vm;
	int cookie, ret = 0;

	if (!session || !session->pfile || !state)
		return -EINVAL;

	pfile = session->pfile;
	if (!drm_dev_enter(&pfile->ptdev->base, &cookie))
		return -ENODEV;

	vm = panthor_vm_pool_get_vm(pfile->vms, vm_id);
	if (!vm) {
		ret = -EINVAL;
		goto out_dev_exit;
	}

	if (panthor_vm_is_unusable(vm))
		*state = DRM_PANTHOR_VM_STATE_UNUSABLE;
	else
		*state = DRM_PANTHOR_VM_STATE_USABLE;

	panthor_vm_put(vm);

out_dev_exit:
	drm_dev_exit(cookie);
	return ret;
}
EXPORT_SYMBOL_GPL(panthor_vmshm_vm_get_state);

int panthor_vmshm_group_create(struct panthor_vmshm_session *session,
			       struct drm_panthor_group_create *args,
			       const struct drm_panthor_queue_create *queues)
{
	struct panthor_file *pfile;
	int cookie, ret;

	if (!session || !session->pfile || !args || !queues)
		return -EINVAL;
	if (!args->queues.count ||
	    args->queues.count > PANTHOR_VMSHM_MAX_GROUP_QUEUES)
		return -EINVAL;
	if (args->priority > PANTHOR_GROUP_PRIORITY_MEDIUM)
		return -EACCES;

	pfile = session->pfile;
	if (!drm_dev_enter(&pfile->ptdev->base, &cookie))
		return -ENODEV;

	ret = panthor_group_create(pfile, args, queues);
	if (ret >= 0) {
		args->group_handle = ret;
		ret = 0;
	}

	drm_dev_exit(cookie);
	return ret;
}
EXPORT_SYMBOL_GPL(panthor_vmshm_group_create);

int panthor_vmshm_group_destroy(struct panthor_vmshm_session *session,
				__u32 group_handle)
{
	struct panthor_file *pfile;
	int cookie, ret;

	if (!session || !session->pfile || !group_handle)
		return -EINVAL;

	pfile = session->pfile;
	if (!drm_dev_enter(&pfile->ptdev->base, &cookie))
		return -ENODEV;

	ret = panthor_group_destroy(pfile, group_handle);
	drm_dev_exit(cookie);
	return ret;
}
EXPORT_SYMBOL_GPL(panthor_vmshm_group_destroy);

int panthor_vmshm_group_get_state(struct panthor_vmshm_session *session,
				  struct drm_panthor_group_get_state *args)
{
	struct panthor_file *pfile;
	int cookie, ret;

	if (!session || !session->pfile || !args)
		return -EINVAL;

	pfile = session->pfile;
	if (!drm_dev_enter(&pfile->ptdev->base, &cookie))
		return -ENODEV;

	ret = panthor_group_get_state(pfile, args);
	drm_dev_exit(cookie);
	return ret;
}
EXPORT_SYMBOL_GPL(panthor_vmshm_group_get_state);

int panthor_vmshm_group_submit(struct panthor_vmshm_session *session,
			       __u32 group_handle,
			       const struct drm_panthor_queue_submit *jobs,
			       const struct drm_panthor_sync_op *syncs,
			       const __u32 *sync_starts,
			       const __u32 *sync_counts,
			       __u32 job_count)
{
	struct panthor_file *pfile;
	struct panthor_submit_ctx ctx;
	int ret = 0, cookie;
	u32 i;

	if (!session || !session->pfile || !group_handle || !jobs ||
	    !sync_starts || !sync_counts)
		return -EINVAL;
	if (!job_count || job_count > PANTHOR_VMSHM_MAX_GROUP_SUBMITS)
		return -EINVAL;

	for (i = 0; i < job_count; i++) {
		u32 sync_start = sync_starts[i];
		u32 sync_count = sync_counts[i];

		if (sync_count &&
		    (!syncs ||
		     sync_start >= PANTHOR_VMSHM_MAX_GROUP_SUBMIT_SYNCS ||
		     sync_count > PANTHOR_VMSHM_MAX_GROUP_SUBMIT_SYNCS ||
		     sync_start > PANTHOR_VMSHM_MAX_GROUP_SUBMIT_SYNCS - sync_count))
			return -EINVAL;

		for (u32 j = 0; j < sync_count; j++) {
			ret = panthor_check_sync_op(&syncs[sync_start + j]);
			if (ret)
				return ret;
		}
	}

	pfile = session->pfile;
	if (!drm_dev_enter(&pfile->ptdev->base, &cookie))
		return -ENODEV;

	ret = panthor_submit_ctx_init(&ctx, &session->file, job_count);
	if (ret)
		goto out_dev_exit;

	for (i = 0; i < job_count; i++) {
		struct drm_sched_job *job;
		const struct drm_panthor_sync_op *job_syncs = NULL;

		job = panthor_job_create(pfile, group_handle, &jobs[i]);
		if (IS_ERR(job)) {
			ret = PTR_ERR(job);
			goto out_cleanup_submit_ctx;
		}

		if (sync_counts[i])
			job_syncs = &syncs[sync_starts[i]];

		ret = panthor_submit_ctx_add_job_kernel_syncs(
			&ctx, i, job, job_syncs, sync_counts[i]);
		if (ret)
			goto out_cleanup_submit_ctx;
	}

	ret = panthor_submit_ctx_collect_jobs_signal_ops(&ctx);
	if (ret)
		goto out_cleanup_submit_ctx;

	if (job_count > 0) {
		struct panthor_vm *vm = panthor_job_vm(ctx.jobs[0].job);

		drm_exec_until_all_locked(&ctx.exec)
		{
			ret = panthor_vm_prepare_mapped_bos_resvs(&ctx.exec, vm,
								  job_count);
		}
		if (ret)
			goto out_cleanup_submit_ctx;
	}

	ret = panthor_submit_ctx_add_deps_and_arm_jobs(&ctx);
	if (ret)
		goto out_cleanup_submit_ctx;

	panthor_submit_ctx_push_jobs(&ctx, panthor_job_update_resvs);

out_cleanup_submit_ctx:
	panthor_submit_ctx_cleanup(&ctx, panthor_job_put);

out_dev_exit:
	drm_dev_exit(cookie);
	return ret;
}
EXPORT_SYMBOL_GPL(panthor_vmshm_group_submit);

int panthor_vmshm_tiler_heap_create(struct panthor_vmshm_session *session,
				    struct drm_panthor_tiler_heap_create *args)
{
	struct panthor_file *pfile;
	struct panthor_heap_pool *pool;
	struct panthor_vm *vm;
	int cookie, ret;

	if (!session || !session->pfile || !args)
		return -EINVAL;

	pfile = session->pfile;
	if (!drm_dev_enter(&pfile->ptdev->base, &cookie))
		return -ENODEV;

	vm = panthor_vm_pool_get_vm(pfile->vms, args->vm_id);
	if (!vm) {
		ret = -EINVAL;
		goto out_dev_exit;
	}

	pool = panthor_vm_get_heap_pool(vm, true);
	if (IS_ERR(pool)) {
		ret = PTR_ERR(pool);
		goto out_put_vm;
	}

	ret = panthor_heap_create(pool, args->initial_chunk_count,
				  args->chunk_size, args->max_chunks,
				  args->target_in_flight,
				  &args->tiler_heap_ctx_gpu_va,
				  &args->first_heap_chunk_gpu_va);
	if (ret >= 0) {
		args->handle = (args->vm_id << 16) | ret;
		ret = 0;
	}

	panthor_heap_pool_put(pool);

out_put_vm:
	panthor_vm_put(vm);

out_dev_exit:
	drm_dev_exit(cookie);
	return ret;
}
EXPORT_SYMBOL_GPL(panthor_vmshm_tiler_heap_create);

int panthor_vmshm_tiler_heap_destroy(struct panthor_vmshm_session *session,
				     __u32 heap_handle)
{
	struct panthor_file *pfile;
	struct panthor_heap_pool *pool;
	struct panthor_vm *vm;
	int cookie, ret;

	if (!session || !session->pfile || !heap_handle)
		return -EINVAL;

	pfile = session->pfile;
	if (!drm_dev_enter(&pfile->ptdev->base, &cookie))
		return -ENODEV;

	vm = panthor_vm_pool_get_vm(pfile->vms, heap_handle >> 16);
	if (!vm) {
		ret = -EINVAL;
		goto out_dev_exit;
	}

	pool = panthor_vm_get_heap_pool(vm, false);
	if (IS_ERR(pool)) {
		ret = PTR_ERR(pool);
		goto out_put_vm;
	}

	ret = panthor_heap_destroy(pool, heap_handle & GENMASK(15, 0));
	panthor_heap_pool_put(pool);

out_put_vm:
	panthor_vm_put(vm);

out_dev_exit:
	drm_dev_exit(cookie);
	return ret;
}
EXPORT_SYMBOL_GPL(panthor_vmshm_tiler_heap_destroy);

#define PANTHOR_VM_CREATE_FLAGS 0

static int panthor_ioctl_vm_create(struct drm_device *ddev, void *data,
				   struct drm_file *file)
{
	struct panthor_device *ptdev =
		container_of(ddev, struct panthor_device, base);
	struct panthor_file *pfile = file->driver_priv;
	struct drm_panthor_vm_create *args = data;
	int cookie, ret;

	if (!drm_dev_enter(ddev, &cookie))
		return -ENODEV;

	ret = panthor_vm_pool_create_vm(ptdev, pfile->vms, args);
	if (ret >= 0) {
		args->id = ret;
		ret = 0;
	}

	drm_dev_exit(cookie);
	return ret;
}

static int panthor_ioctl_vm_destroy(struct drm_device *ddev, void *data,
				    struct drm_file *file)
{
	struct panthor_file *pfile = file->driver_priv;
	struct drm_panthor_vm_destroy *args = data;

	if (args->pad)
		return -EINVAL;

	return panthor_vm_pool_destroy_vm(pfile->vms, args->id);
}

static int panthor_ioctl_bo_create(struct drm_device *ddev, void *data,
				   struct drm_file *file)
{
	struct panthor_file *pfile = file->driver_priv;
	struct drm_panthor_bo_create *args = data;
	struct panthor_vm *vm = NULL;
	u64 size;
	int cookie, ret;

	if (!drm_dev_enter(ddev, &cookie))
		return -ENODEV;

	if (!args->size || args->pad || (args->flags & ~PANTHOR_BO_FLAGS)) {
		ret = -EINVAL;
		goto out_dev_exit;
	}

	size = args->size;

	if (args->exclusive_vm_id) {
		vm = panthor_vm_pool_get_vm(pfile->vms, args->exclusive_vm_id);
		if (!vm) {
			ret = -EINVAL;
			goto out_dev_exit;
		}
	}

	ret = panthor_gem_create_with_handle(file, ddev, vm, &size,
					     args->flags, &args->handle);
	if (!ret)
		args->size = size;

	panthor_vm_put(vm);

out_dev_exit:
	drm_dev_exit(cookie);
	return ret;
}

static int panthor_ioctl_bo_mmap_offset(struct drm_device *ddev, void *data,
					struct drm_file *file)
{
	struct drm_panthor_bo_mmap_offset *args = data;
	struct drm_gem_object *obj;
	int ret;

	if (args->pad)
		return -EINVAL;

	obj = drm_gem_object_lookup(file, args->handle);
	if (!obj)
		return -ENOENT;

	ret = drm_gem_create_mmap_offset(obj);
	if (ret)
		goto out;

	args->offset = drm_vma_node_offset_addr(&obj->vma_node);

out:
	drm_gem_object_put(obj);
	return ret;
}

static int panthor_ioctl_group_submit(struct drm_device *ddev, void *data,
				      struct drm_file *file)
{
	struct panthor_file *pfile = file->driver_priv;
	struct drm_panthor_group_submit *args = data;
	struct drm_panthor_queue_submit *jobs_args;
	struct panthor_submit_ctx ctx;
	struct panthor_submit_stats_sample stats = {
		.kind = PANTHOR_SUBMIT_STATS_GROUP_SUBMIT,
		.job_or_op_count = args->queue_submits.count,
	};
	bool stats_enabled = panthor_submit_stats_is_enabled();
	u64 stats_phase_start;
	int ret = 0, cookie;

	if (args->pad)
		return -EINVAL;

	if (!drm_dev_enter(ddev, &cookie))
		return -ENODEV;

	stats.start_ns = panthor_submit_stats_now(stats_enabled);
	stats_phase_start = panthor_submit_stats_now(stats_enabled);
	ret = PANTHOR_UOBJ_GET_ARRAY(jobs_args, &args->queue_submits);
	panthor_submit_stats_accum(&stats.copy_ns, stats_phase_start);
	if (ret)
		goto out_dev_exit;

	stats_phase_start = panthor_submit_stats_now(stats_enabled);
	ret = panthor_submit_ctx_init(&ctx, file, args->queue_submits.count);
	panthor_submit_stats_accum(&stats.init_ctx_ns, stats_phase_start);
	if (ret)
		goto out_free_jobs_args;

	/* Create jobs and attach sync operations */
	stats_phase_start = panthor_submit_stats_now(stats_enabled);
	for (u32 i = 0; i < args->queue_submits.count; i++) {
		const struct drm_panthor_queue_submit *qsubmit = &jobs_args[i];
		struct drm_sched_job *job;

		job = panthor_job_create(pfile, args->group_handle, qsubmit);
		if (IS_ERR(job)) {
			ret = PTR_ERR(job);
			goto out_cleanup_submit_ctx;
		}

		ret = panthor_submit_ctx_add_job(&ctx, i, job, &qsubmit->syncs);
		if (ret)
			goto out_cleanup_submit_ctx;
	}
	panthor_submit_stats_accum(&stats.create_jobs_ns, stats_phase_start);

	/*
	 * Collect signal operations on all jobs, such that each job can pick
	 * from it for its dependencies and update the fence to signal when the
	 * job is submitted.
	 */
	stats_phase_start = panthor_submit_stats_now(stats_enabled);
	ret = panthor_submit_ctx_collect_jobs_signal_ops(&ctx);
	panthor_submit_stats_accum(&stats.collect_signals_ns, stats_phase_start);
	if (ret)
		goto out_cleanup_submit_ctx;

	/*
	 * We acquire/prepare revs on all jobs before proceeding with the
	 * dependency registration.
	 *
	 * This is solving two problems:
	 * 1. drm_sched_job_arm() and drm_sched_entity_push_job() must be
	 *    protected by a lock to make sure no concurrent access to the same
	 *    entity get interleaved, which would mess up with the fence seqno
	 *    ordering. Luckily, one of the resv being acquired is the VM resv,
	 *    and a scheduling entity is only bound to a single VM. As soon as
	 *    we acquire the VM resv, we should be safe.
	 * 2. Jobs might depend on fences that were issued by previous jobs in
	 *    the same batch, so we can't add dependencies on all jobs before
	 *    arming previous jobs and registering the fence to the signal
	 *    array, otherwise we might miss dependencies, or point to an
	 *    outdated fence.
	 */
	if (args->queue_submits.count > 0) {
		/* All jobs target the same group, so they also point to the same VM. */
		struct panthor_vm *vm = panthor_job_vm(ctx.jobs[0].job);

		stats_phase_start = panthor_submit_stats_now(stats_enabled);
		drm_exec_until_all_locked(&ctx.exec)
		{
			ret = panthor_vm_prepare_mapped_bos_resvs(
				&ctx.exec, vm, args->queue_submits.count);
		}
		panthor_submit_stats_accum(&stats.prepare_resvs_ns,
					   stats_phase_start);

		if (ret)
			goto out_cleanup_submit_ctx;
	}

	/*
	 * Now that resvs are locked/prepared, we can iterate over each job to
	 * add the dependencies, arm the job fence, register the job fence to
	 * the signal array.
	 */
	stats_phase_start = panthor_submit_stats_now(stats_enabled);
	ret = panthor_submit_ctx_add_deps_and_arm_jobs(&ctx);
	panthor_submit_stats_accum(&stats.arm_jobs_ns, stats_phase_start);
	if (ret)
		goto out_cleanup_submit_ctx;

	/* Nothing can fail after that point, so we can make our job fences
	 * visible to the outside world. Push jobs and set the job fences to
	 * the resv slots we reserved.  This also pushes the fences to the
	 * syncobjs that are part of the signal array.
	 */
	if (stats_enabled) {
		stats_phase_start = panthor_submit_stats_now(stats_enabled);
		panthor_submit_ctx_push_jobs_stats(&ctx, panthor_job_update_resvs,
						   &stats);
		panthor_submit_stats_accum(&stats.push_jobs_ns,
					   stats_phase_start);
	} else {
		panthor_submit_ctx_push_jobs(&ctx, panthor_job_update_resvs);
	}

out_cleanup_submit_ctx:
	stats_phase_start = panthor_submit_stats_now(stats_enabled);
	panthor_submit_ctx_cleanup(&ctx, panthor_job_put);
	panthor_submit_stats_accum(&stats.cleanup_ns, stats_phase_start);

out_free_jobs_args:
	stats_phase_start = panthor_submit_stats_now(stats_enabled);
	kvfree(jobs_args);
	panthor_submit_stats_accum(&stats.cleanup_ns, stats_phase_start);

out_dev_exit:
	panthor_submit_stats_commit(&stats);
	drm_dev_exit(cookie);
	return ret;
}

static int panthor_ioctl_group_destroy(struct drm_device *ddev, void *data,
				       struct drm_file *file)
{
	struct panthor_file *pfile = file->driver_priv;
	struct drm_panthor_group_destroy *args = data;

	if (args->pad)
		return -EINVAL;

	return panthor_group_destroy(pfile, args->group_handle);
}

static int group_priority_permit(struct drm_file *file, u8 priority)
{
	/* Ensure that priority is valid */
	if (priority > PANTHOR_GROUP_PRIORITY_HIGH)
		return -EINVAL;

	/* Medium priority and below are always allowed */
	if (priority <= PANTHOR_GROUP_PRIORITY_MEDIUM)
		return 0;

	/* Higher priorities require CAP_SYS_NICE or DRM_MASTER */
	if (capable(CAP_SYS_NICE) || drm_is_current_master(file))
		return 0;

	return -EACCES;
}

static int panthor_ioctl_group_create(struct drm_device *ddev, void *data,
				      struct drm_file *file)
{
	struct panthor_file *pfile = file->driver_priv;
	struct drm_panthor_group_create *args = data;
	struct drm_panthor_queue_create *queue_args;
	int ret;

	if (!args->queues.count)
		return -EINVAL;

	ret = PANTHOR_UOBJ_GET_ARRAY(queue_args, &args->queues);
	if (ret)
		return ret;

	ret = group_priority_permit(file, args->priority);
	if (ret)
		return ret;

	ret = panthor_group_create(pfile, args, queue_args);
	if (ret >= 0) {
		args->group_handle = ret;
		ret = 0;
	}

	kvfree(queue_args);
	return ret;
}

static int panthor_ioctl_group_get_state(struct drm_device *ddev, void *data,
					 struct drm_file *file)
{
	struct panthor_file *pfile = file->driver_priv;
	struct drm_panthor_group_get_state *args = data;

	return panthor_group_get_state(pfile, args);
}

static int panthor_ioctl_tiler_heap_create(struct drm_device *ddev, void *data,
					   struct drm_file *file)
{
	struct panthor_file *pfile = file->driver_priv;
	struct drm_panthor_tiler_heap_create *args = data;
	struct panthor_heap_pool *pool;
	struct panthor_vm *vm;
	int ret;

	vm = panthor_vm_pool_get_vm(pfile->vms, args->vm_id);
	if (!vm)
		return -EINVAL;

	pool = panthor_vm_get_heap_pool(vm, true);
	if (IS_ERR(pool)) {
		ret = PTR_ERR(pool);
		goto out_put_vm;
	}

	ret = panthor_heap_create(pool, args->initial_chunk_count,
				  args->chunk_size, args->max_chunks,
				  args->target_in_flight,
				  &args->tiler_heap_ctx_gpu_va,
				  &args->first_heap_chunk_gpu_va);
	if (ret < 0)
		goto out_put_heap_pool;

	/* Heap pools are per-VM. We combine the VM and HEAP id to make
	 * a unique heap handle.
	 */
	args->handle = (args->vm_id << 16) | ret;
	ret = 0;

out_put_heap_pool:
	panthor_heap_pool_put(pool);

out_put_vm:
	panthor_vm_put(vm);
	return ret;
}

static int panthor_ioctl_tiler_heap_destroy(struct drm_device *ddev, void *data,
					    struct drm_file *file)
{
	struct panthor_file *pfile = file->driver_priv;
	struct drm_panthor_tiler_heap_destroy *args = data;
	struct panthor_heap_pool *pool;
	struct panthor_vm *vm;
	int ret;

	if (args->pad)
		return -EINVAL;

	vm = panthor_vm_pool_get_vm(pfile->vms, args->handle >> 16);
	if (!vm)
		return -EINVAL;

	pool = panthor_vm_get_heap_pool(vm, false);
	if (IS_ERR(pool)) {
		ret = PTR_ERR(pool);
		goto out_put_vm;
	}

	ret = panthor_heap_destroy(pool, args->handle & GENMASK(15, 0));
	panthor_heap_pool_put(pool);

out_put_vm:
	panthor_vm_put(vm);
	return ret;
}

static int panthor_ioctl_vm_bind_async(struct drm_device *ddev,
				       struct drm_panthor_vm_bind *args,
				       struct drm_file *file)
{
	struct panthor_file *pfile = file->driver_priv;
	struct drm_panthor_vm_bind_op *jobs_args;
	struct panthor_submit_ctx ctx;
	struct panthor_submit_stats_sample stats = {
		.kind = PANTHOR_SUBMIT_STATS_VM_BIND_ASYNC,
		.job_or_op_count = args->ops.count,
	};
	struct panthor_vm *vm;
	bool stats_enabled = panthor_submit_stats_is_enabled();
	u64 stats_phase_start;
	int ret = 0;

	stats.start_ns = panthor_submit_stats_now(stats_enabled);
	vm = panthor_vm_pool_get_vm(pfile->vms, args->vm_id);
	if (!vm)
		return -EINVAL;

	stats_phase_start = panthor_submit_stats_now(stats_enabled);
	ret = PANTHOR_UOBJ_GET_ARRAY(jobs_args, &args->ops);
	panthor_submit_stats_accum(&stats.copy_ns, stats_phase_start);
	if (ret)
		goto out_put_vm;

	stats_phase_start = panthor_submit_stats_now(stats_enabled);
	ret = panthor_submit_ctx_init(&ctx, file, args->ops.count);
	panthor_submit_stats_accum(&stats.init_ctx_ns, stats_phase_start);
	if (ret)
		goto out_free_jobs_args;

	stats_phase_start = panthor_submit_stats_now(stats_enabled);
	for (u32 i = 0; i < args->ops.count; i++) {
		struct drm_panthor_vm_bind_op *op = &jobs_args[i];
		struct drm_sched_job *job;

		job = panthor_vm_bind_job_create(file, vm, op);
		if (IS_ERR(job)) {
			ret = PTR_ERR(job);
			goto out_cleanup_submit_ctx;
		}

		ret = panthor_submit_ctx_add_job(&ctx, i, job, &op->syncs);
		if (ret)
			goto out_cleanup_submit_ctx;
	}
	panthor_submit_stats_accum(&stats.create_jobs_ns, stats_phase_start);

	stats_phase_start = panthor_submit_stats_now(stats_enabled);
	ret = panthor_submit_ctx_collect_jobs_signal_ops(&ctx);
	panthor_submit_stats_accum(&stats.collect_signals_ns, stats_phase_start);
	if (ret)
		goto out_cleanup_submit_ctx;

	/* Prepare reservation objects for each VM_BIND job. */
	stats_phase_start = panthor_submit_stats_now(stats_enabled);
	drm_exec_until_all_locked(&ctx.exec)
	{
		for (u32 i = 0; i < ctx.job_count; i++) {
			ret = panthor_vm_bind_job_prepare_resvs(
				&ctx.exec, ctx.jobs[i].job);
			drm_exec_retry_on_contention(&ctx.exec);
			if (ret)
				goto out_cleanup_submit_ctx;
		}
	}
	panthor_submit_stats_accum(&stats.prepare_resvs_ns, stats_phase_start);

	stats_phase_start = panthor_submit_stats_now(stats_enabled);
	ret = panthor_submit_ctx_add_deps_and_arm_jobs(&ctx);
	panthor_submit_stats_accum(&stats.arm_jobs_ns, stats_phase_start);
	if (ret)
		goto out_cleanup_submit_ctx;

	/* Nothing can fail after that point. */
	if (stats_enabled) {
		stats_phase_start = panthor_submit_stats_now(stats_enabled);
		panthor_submit_ctx_push_jobs_stats(
			&ctx, panthor_vm_bind_job_update_resvs, &stats);
		panthor_submit_stats_accum(&stats.push_jobs_ns,
					   stats_phase_start);
	} else {
		panthor_submit_ctx_push_jobs(&ctx,
					     panthor_vm_bind_job_update_resvs);
	}

out_cleanup_submit_ctx:
	stats_phase_start = panthor_submit_stats_now(stats_enabled);
	panthor_submit_ctx_cleanup(&ctx, panthor_vm_bind_job_put);
	panthor_submit_stats_accum(&stats.cleanup_ns, stats_phase_start);

out_free_jobs_args:
	stats_phase_start = panthor_submit_stats_now(stats_enabled);
	kvfree(jobs_args);
	panthor_submit_stats_accum(&stats.cleanup_ns, stats_phase_start);

out_put_vm:
	panthor_vm_put(vm);
	panthor_submit_stats_commit(&stats);
	return ret;
}

static int panthor_ioctl_vm_bind_sync(struct drm_device *ddev,
				      struct drm_panthor_vm_bind *args,
				      struct drm_file *file)
{
	struct panthor_file *pfile = file->driver_priv;
	struct drm_panthor_vm_bind_op *jobs_args;
	struct panthor_submit_stats_sample stats = {
		.kind = PANTHOR_SUBMIT_STATS_VM_BIND_SYNC,
		.job_or_op_count = args->ops.count,
	};
	struct panthor_vm *vm;
	bool stats_enabled = panthor_submit_stats_is_enabled();
	u64 stats_phase_start;
	int ret;

	stats.start_ns = panthor_submit_stats_now(stats_enabled);
	vm = panthor_vm_pool_get_vm(pfile->vms, args->vm_id);
	if (!vm)
		return -EINVAL;

	stats_phase_start = panthor_submit_stats_now(stats_enabled);
	ret = PANTHOR_UOBJ_GET_ARRAY(jobs_args, &args->ops);
	panthor_submit_stats_accum(&stats.copy_ns, stats_phase_start);
	if (ret)
		goto out_put_vm;

	stats_phase_start = panthor_submit_stats_now(stats_enabled);
	for (u32 i = 0; i < args->ops.count; i++) {
		ret = panthor_vm_bind_exec_sync_op(file, vm, &jobs_args[i]);
		if (ret) {
			/* Update ops.count so the user knows where things failed. */
			args->ops.count = i;
			break;
		}
	}
	panthor_submit_stats_accum(&stats.sync_exec_ns, stats_phase_start);

	stats_phase_start = panthor_submit_stats_now(stats_enabled);
	kvfree(jobs_args);
	panthor_submit_stats_accum(&stats.cleanup_ns, stats_phase_start);

out_put_vm:
	panthor_vm_put(vm);
	panthor_submit_stats_commit(&stats);
	return ret;
}

#define PANTHOR_VM_BIND_FLAGS DRM_PANTHOR_VM_BIND_ASYNC

static int panthor_ioctl_vm_bind(struct drm_device *ddev, void *data,
				 struct drm_file *file)
{
	struct drm_panthor_vm_bind *args = data;
	int cookie, ret;

	if (!drm_dev_enter(ddev, &cookie))
		return -ENODEV;

	if (args->flags & DRM_PANTHOR_VM_BIND_ASYNC)
		ret = panthor_ioctl_vm_bind_async(ddev, args, file);
	else
		ret = panthor_ioctl_vm_bind_sync(ddev, args, file);

	drm_dev_exit(cookie);
	return ret;
}

static int panthor_ioctl_vm_get_state(struct drm_device *ddev, void *data,
				      struct drm_file *file)
{
	struct panthor_file *pfile = file->driver_priv;
	struct drm_panthor_vm_get_state *args = data;
	struct panthor_vm *vm;

	vm = panthor_vm_pool_get_vm(pfile->vms, args->vm_id);
	if (!vm)
		return -EINVAL;

	if (panthor_vm_is_unusable(vm))
		args->state = DRM_PANTHOR_VM_STATE_UNUSABLE;
	else
		args->state = DRM_PANTHOR_VM_STATE_USABLE;

	panthor_vm_put(vm);
	return 0;
}

static int panthor_open(struct drm_device *ddev, struct drm_file *file)
{
	struct panthor_device *ptdev =
		container_of(ddev, struct panthor_device, base);
	struct panthor_file *pfile;
	int ret;

	if (!try_module_get(THIS_MODULE))
		return -EINVAL;

	pfile = kzalloc(sizeof(*pfile), GFP_KERNEL);
	if (!pfile) {
		ret = -ENOMEM;
		goto err_put_mod;
	}

	pfile->ptdev = ptdev;

	ret = panthor_vm_pool_create(pfile);
	if (ret)
		goto err_free_file;

	ret = panthor_group_pool_create(pfile);
	if (ret)
		goto err_destroy_vm_pool;

	file->driver_priv = pfile;
	return 0;

err_destroy_vm_pool:
	panthor_vm_pool_destroy(pfile);

err_free_file:
	kfree(pfile);

err_put_mod:
	module_put(THIS_MODULE);
	return ret;
}

static void panthor_postclose(struct drm_device *ddev, struct drm_file *file)
{
	struct panthor_file *pfile = file->driver_priv;

	panthor_group_pool_destroy(pfile);
	panthor_vm_pool_destroy(pfile);

	kfree(pfile);
	module_put(THIS_MODULE);
}

static const struct drm_ioctl_desc panthor_drm_driver_ioctls[] = {
#define PANTHOR_IOCTL(n, func, flags) \
	DRM_IOCTL_DEF_DRV(PANTHOR_##n, panthor_ioctl_##func, flags)

	PANTHOR_IOCTL(DEV_QUERY, dev_query, DRM_RENDER_ALLOW),
	PANTHOR_IOCTL(VM_CREATE, vm_create, DRM_RENDER_ALLOW),
	PANTHOR_IOCTL(VM_DESTROY, vm_destroy, DRM_RENDER_ALLOW),
	PANTHOR_IOCTL(VM_BIND, vm_bind, DRM_RENDER_ALLOW),
	PANTHOR_IOCTL(VM_GET_STATE, vm_get_state, DRM_RENDER_ALLOW),
	PANTHOR_IOCTL(BO_CREATE, bo_create, DRM_RENDER_ALLOW),
	PANTHOR_IOCTL(BO_MMAP_OFFSET, bo_mmap_offset, DRM_RENDER_ALLOW),
	PANTHOR_IOCTL(GROUP_CREATE, group_create, DRM_RENDER_ALLOW),
	PANTHOR_IOCTL(GROUP_DESTROY, group_destroy, DRM_RENDER_ALLOW),
	PANTHOR_IOCTL(GROUP_GET_STATE, group_get_state, DRM_RENDER_ALLOW),
	PANTHOR_IOCTL(TILER_HEAP_CREATE, tiler_heap_create, DRM_RENDER_ALLOW),
	PANTHOR_IOCTL(TILER_HEAP_DESTROY, tiler_heap_destroy, DRM_RENDER_ALLOW),
	PANTHOR_IOCTL(GROUP_SUBMIT, group_submit, DRM_RENDER_ALLOW),
};

static int panthor_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct drm_file *file = filp->private_data;
	struct panthor_file *pfile = file->driver_priv;
	struct panthor_device *ptdev = pfile->ptdev;
	u64 offset = (u64)vma->vm_pgoff << PAGE_SHIFT;
	int ret, cookie;

	if (!drm_dev_enter(file->minor->dev, &cookie))
		return -ENODEV;

#ifdef CONFIG_ARM64
	/*
	 * With 32-bit systems being limited by the 32-bit representation of
	 * mmap2's pgoffset field, we need to make the MMIO offset arch
	 * specific. This converts a user MMIO offset into something the kernel
	 * driver understands.
	 */
	if (test_tsk_thread_flag(current, TIF_32BIT) &&
	    offset >= DRM_PANTHOR_USER_MMIO_OFFSET_32BIT) {
		offset += DRM_PANTHOR_USER_MMIO_OFFSET_64BIT -
			  DRM_PANTHOR_USER_MMIO_OFFSET_32BIT;
		vma->vm_pgoff = offset >> PAGE_SHIFT;
	}
#endif

	if (offset >= DRM_PANTHOR_USER_MMIO_OFFSET)
		ret = panthor_device_mmap_io(ptdev, vma);
	else
		ret = drm_gem_mmap(filp, vma);

	drm_dev_exit(cookie);
	return ret;
}

static const struct file_operations panthor_drm_driver_fops = {
	.open = drm_open,
	.release = drm_release,
	.unlocked_ioctl = drm_ioctl,
	.compat_ioctl = drm_compat_ioctl,
	.poll = drm_poll,
	.read = drm_read,
	.llseek = noop_llseek,
	.mmap = panthor_mmap,
	.fop_flags = FOP_UNSIGNED_OFFSET,
};

#ifdef CONFIG_DEBUG_FS
static void panthor_debugfs_init(struct drm_minor *minor)
{
	panthor_mmu_debugfs_init(minor);
}
#endif

/*
 * PanCSF driver version:
 * - 1.0 - initial interface
 */
static const struct drm_driver panthor_drm_driver = {
	.driver_features = DRIVER_RENDER | DRIVER_GEM | DRIVER_SYNCOBJ |
			   DRIVER_SYNCOBJ_TIMELINE | DRIVER_GEM_GPUVA,
	.open = panthor_open,
	.postclose = panthor_postclose,
	.ioctls = panthor_drm_driver_ioctls,
	.num_ioctls = ARRAY_SIZE(panthor_drm_driver_ioctls),
	.fops = &panthor_drm_driver_fops,
	.name = "panthor",
	.desc = "Panthor DRM driver",
	.date = "20230801",
	.major = 1,
	.minor = 0,

	.gem_create_object = panthor_gem_create_object,
	.gem_prime_import_sg_table = drm_gem_shmem_prime_import_sg_table,
#ifdef CONFIG_DEBUG_FS
	.debugfs_init = panthor_debugfs_init,
#endif
};

static int panthor_probe(struct platform_device *pdev)
{
	pr_debug("[MZH]panthor_probe\n");
	struct panthor_device *ptdev;
	int ret;

	ptdev = devm_drm_dev_alloc(&pdev->dev, &panthor_drm_driver,
				   struct panthor_device, base);
	if (IS_ERR(ptdev))
		return -ENOMEM;

	platform_set_drvdata(pdev, ptdev);

	ret = panthor_device_init(ptdev);
	if (ret)
		return ret;

	mutex_lock(&panthor_vmshm_lock);
	panthor_vmshm_ptdev = ptdev;
	mutex_unlock(&panthor_vmshm_lock);
	return 0;
}

static void panthor_remove(struct platform_device *pdev)
{
	struct panthor_device *ptdev = platform_get_drvdata(pdev);

	mutex_lock(&panthor_vmshm_lock);
	if (panthor_vmshm_ptdev == ptdev)
		panthor_vmshm_ptdev = NULL;
	mutex_unlock(&panthor_vmshm_lock);

	panthor_device_unplug(ptdev);
}

static const struct of_device_id dt_match[] = {
	{ .compatible = "rockchip,rk3588-mali" },
	{ .compatible = "arm,mali-valhall-csf" },
	{}
};
MODULE_DEVICE_TABLE(of, dt_match);

static DEFINE_RUNTIME_DEV_PM_OPS(panthor_pm_ops, panthor_device_suspend,
				 panthor_device_resume, NULL);

static struct platform_driver panthor_driver = {
	.probe = panthor_probe,
	.remove_new = panthor_remove,
	.driver = {
		.name = "panthor",
		.pm = pm_ptr(&panthor_pm_ops),
		.of_match_table = dt_match,
	},
};

/*
 * Workqueue used to cleanup stuff.
 *
 * We create a dedicated workqueue so we can drain on unplug and
 * make sure all resources are freed before the module is unloaded.
 */
struct workqueue_struct *panthor_cleanup_wq;

static int __init panthor_init(void)
{
	int ret;
	pr_debug("[MZH]panthor_init\n");
	ret = panthor_mmu_pt_cache_init();
	if (ret)
		return ret;

	panthor_cleanup_wq = alloc_workqueue("panthor-cleanup", WQ_UNBOUND, 0);
	if (!panthor_cleanup_wq) {
		pr_err("panthor: Failed to allocate the workqueues");
		ret = -ENOMEM;
		goto err_mmu_pt_cache_fini;
	}

	ret = platform_driver_register(&panthor_driver);
	if (ret)
		goto err_destroy_cleanup_wq;

	return 0;

err_destroy_cleanup_wq:
	destroy_workqueue(panthor_cleanup_wq);

err_mmu_pt_cache_fini:
	panthor_mmu_pt_cache_fini();
	return ret;
}
module_init(panthor_init);

static void __exit panthor_exit(void)
{
	platform_driver_unregister(&panthor_driver);
	destroy_workqueue(panthor_cleanup_wq);
	panthor_mmu_pt_cache_fini();
}
module_exit(panthor_exit);

MODULE_AUTHOR("Panthor Project Developers");
MODULE_DESCRIPTION("Panthor DRM Driver");
MODULE_LICENSE("Dual MIT/GPL");
