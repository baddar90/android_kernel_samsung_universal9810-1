/*
 * Services for Exynos Mobile Scheduler
 *
 * Copyright (C) 2018 Samsung Electronics Co., Ltd
 * Park Bumgyu <bumgyu.park@samsung.com>
 */

#include <linux/kobject.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/ems_service.h>
#include <trace/events/ems.h>

#include "../sched.h"
#include "../tune.h"
#include "ems.h"

/**********************************************************************
 *                        Kernel Prefer Perf                          *
 **********************************************************************/
struct plist_head kpp_list[STUNE_GROUP_COUNT];

static bool kpp_en;

int kpp_status(int grp_idx)
{
	if (unlikely(!kpp_en))
		return 0;

	if (grp_idx >= STUNE_GROUP_COUNT)
		return -EINVAL;

	if (plist_head_empty(&kpp_list[grp_idx]))
		return 0;

	return plist_last(&kpp_list[grp_idx])->prio;
}

static DEFINE_SPINLOCK(kpp_lock);

void kpp_request(int grp_idx, struct kpp *req, int value)
{
	unsigned long flags;

	if (unlikely(!kpp_en))
		return;

	if (grp_idx >= STUNE_GROUP_COUNT)
		return;

	if (req->node.prio == value)
		return;

	spin_lock_irqsave(&kpp_lock, flags);

	/*
	 * If the request already added to the list updates the value, remove
	 * the request from the list and add it again.
	 */
	if (req->active)
		plist_del(&req->node, &kpp_list[req->grp_idx]);
	else
		req->active = 1;

	plist_node_init(&req->node, value);
	plist_add(&req->node, &kpp_list[grp_idx]);
	req->grp_idx = grp_idx;

	spin_unlock_irqrestore(&kpp_lock, flags);
}

static void __init init_kpp(void)
{
	int i;

	for (i = 0; i < STUNE_GROUP_COUNT; i++)
		plist_head_init(&kpp_list[i]);

	kpp_en = 1;
}

struct prefer_perf {
	int			boost;
	unsigned int		threshold;
	unsigned int		coregroup_count;
	struct cpumask		*prefer_cpus;
};

static struct prefer_perf *prefer_perf_services;
static int prefer_perf_service_count;

static struct prefer_perf *find_prefer_perf(int boost)
{
	int i;

	for (i = 0; i < prefer_perf_service_count; i++)
		if (prefer_perf_services[i].boost == boost)
			return &prefer_perf_services[i];

	return NULL;
}

int prefer_perf_on_big(struct task_struct *p)
{
	struct prefer_perf *pp;
	int boost;

	if (!prefer_perf_services)
		return 0;

	boost = schedtune_prefer_perf(p);
	if (boost <= 0)
		return 0;

	pp = find_prefer_perf(boost);
	if (!pp)
		return 0;

	return task_util_est(p) > pp->threshold;
}

static int
select_prefer_cpu(struct task_struct *p, int coregroup_count, struct cpumask *prefer_cpus)
{
	struct cpumask mask;
	int coregroup, cpu;
	unsigned long max_spare_cap = 0;
	int best_perf_cstate = INT_MAX;
	int best_perf_cpu = -1;
	int backup_cpu = -1;

	rcu_read_lock();

	for (coregroup = 0; coregroup < coregroup_count; coregroup++) {
		cpumask_and(&mask, &prefer_cpus[coregroup], cpu_active_mask);
		if (cpumask_empty(&mask))
			continue;

		for_each_cpu_and(cpu, &p->cpus_allowed, &mask) {
			unsigned long capacity_orig;
			unsigned long wake_util;

			if (idle_cpu(cpu)) {
				int idle_idx = idle_get_state_idx(cpu_rq(cpu));

				/* find shallowest idle state cpu */
				if (idle_idx >= best_perf_cstate)
					continue;

				/* Keep track of best idle CPU */
				best_perf_cstate = idle_idx;
				best_perf_cpu = cpu;
				continue;
			}

			capacity_orig = capacity_orig_of(cpu);
			wake_util = cpu_util_wake(cpu, p);
			if ((capacity_orig - wake_util) < max_spare_cap)
				continue;

			max_spare_cap = capacity_orig - wake_util;
			backup_cpu = cpu;
		}

		if (cpu_selected(best_perf_cpu))
			break;
	}

	rcu_read_unlock();

	if (best_perf_cpu == -1)
		return backup_cpu;

	return best_perf_cpu;
}

int select_service_cpu(struct task_struct *p)
{
	struct prefer_perf *pp;
	int boost, service_cpu;
	unsigned long util;
	char state[30];

	if (!prefer_perf_services)
		return -1;

	boost = schedtune_prefer_perf(p);
	if (boost <= 0)
		return -1;

	pp = find_prefer_perf(boost);
	if (!pp)
		return -1;

	util = task_util_est(p);
	if (util <= pp->threshold) {
		service_cpu = select_prefer_cpu(p, 1, pp->prefer_cpus);
		strcpy(state, "light task");
		goto out;
	}

	service_cpu = select_prefer_cpu(p, pp->coregroup_count, pp->prefer_cpus);
	strcpy(state, "heavy task");

out:
	trace_ems_prefer_perf_service(p, util, service_cpu, state);
	return service_cpu;
}

static ssize_t show_kpp(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	int i, ret = 0;

	/* shows the prefer_perf value of all schedtune groups */
	for (i = 0; i < STUNE_GROUP_COUNT; i++)
		ret += snprintf(buf + ret, 10, "%d ", kpp_status(i));

	ret += snprintf(buf + ret, 10, "\n");

	return ret;
}

static struct kobj_attribute kpp_attr =
__ATTR(kernel_prefer_perf, 0444, show_kpp, NULL);

static void __init build_prefer_cpus(void)
{
	struct device_node *dn, *child;
	int index = 0;

	dn = of_find_node_by_name(NULL, "ems");
	dn = of_find_node_by_name(dn, "prefer-perf-service");
	prefer_perf_service_count = of_get_child_count(dn);

	prefer_perf_services = kcalloc(prefer_perf_service_count,
				sizeof(struct prefer_perf), GFP_KERNEL);
	if (!prefer_perf_services)
		return;

	for_each_child_of_node(dn, child) {
		const char *mask[NR_CPUS];
		int i, proplen;

		if (index >= prefer_perf_service_count)
			return;

		of_property_read_u32(child, "boost",
					&prefer_perf_services[index].boost);

		of_property_read_u32(child, "light-task-threshold",
					&prefer_perf_services[index].threshold);

		proplen = of_property_count_strings(child, "prefer-cpus");
		if (proplen < 0)
			goto next;

		prefer_perf_services[index].coregroup_count = proplen;

		of_property_read_string_array(child, "prefer-cpus", mask, proplen);
		prefer_perf_services[index].prefer_cpus = kcalloc(proplen,
						sizeof(struct cpumask), GFP_KERNEL);

		for (i = 0; i < proplen; i++)
			cpulist_parse(mask[i], &prefer_perf_services[index].prefer_cpus[i]);

next:
		index++;
	}
}

static int __init init_service(void)
{
	int ret;

	init_kpp();

	build_prefer_cpus();

	ret = sysfs_create_file(ems_kobj, &kpp_attr.attr);
	if (ret)
		pr_err("%s: faile to create sysfs file\n", __func__);

	return 0;
}
late_initcall(init_service);
