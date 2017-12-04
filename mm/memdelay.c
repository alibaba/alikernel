/*
 * Memory delay metric
 *
 * Copyright (c) 2017 Facebook, Johannes Weiner
 *
 * This code quantifies and reports to userspace the wall-time impact
 * of memory pressure on the system and memory-controlled cgroups.
 */

#include <linux/memcontrol.h>
#include <linux/memdelay.h>
#include <linux/seq_file.h>
#include <linux/proc_fs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/fs.h>

static DEFINE_PER_CPU(struct memdelay_domain_cpu, global_domain_cpus);

/* System-level keeping of memory delay statistics */
struct memdelay_domain memdelay_global_domain = {
	.mdcs = &global_domain_cpus,
};

static void domain_init(struct memdelay_domain *md)
{
	md->period_expires = jiffies + LOAD_FREQ;
}

/**
 * memdelay_init - initialize the memdelay subsystem
 *
 * This needs to run before the scheduler starts queuing and
 * scheduling tasks.
 */
void __init memdelay_init(void)
{
	domain_init(&memdelay_global_domain);
}

static void domain_move_clock(struct memdelay_domain *md)
{
	unsigned long expires = READ_ONCE(md->period_expires);
	unsigned long none, some, full;
	int missed_periods;
	unsigned long next;
	int i;

	if (time_before(jiffies, expires))
		return;

	missed_periods = 1 + (jiffies - expires) / LOAD_FREQ;
	next = expires + (missed_periods * LOAD_FREQ);

	if (cmpxchg(&md->period_expires, expires, next) != expires)
		return;

	none = xchg(&md->times[MDS_NONE], 0);
	some = xchg(&md->times[MDS_SOME], 0);
	full = xchg(&md->times[MDS_FULL], 0);

	for (i = 0; i < missed_periods; i++) {
		unsigned long pct;

		pct = some * 100 / max(none + some + full, 1UL);
		pct *= FIXED_1;
		CALC_LOAD(md->avg_some[0], EXP_1, pct);
		CALC_LOAD(md->avg_some[1], EXP_5, pct);
		CALC_LOAD(md->avg_some[2], EXP_15, pct);

		pct = full * 100 / max(none + some + full, 1UL);
		pct *= FIXED_1;
		CALC_LOAD(md->avg_full[0], EXP_1, pct);
		CALC_LOAD(md->avg_full[1], EXP_5, pct);
		CALC_LOAD(md->avg_full[2], EXP_15, pct);

		none = some = full = 0;
	}
}

static void domain_cpu_update(struct memdelay_domain *md, int cpu,
			      enum memdelay_task_state old,
			      enum memdelay_task_state new)
{
	enum memdelay_domain_state state;
	struct memdelay_domain_cpu *mdc;
	unsigned long delta;
	u64 now;

	mdc = per_cpu_ptr(md->mdcs, cpu);

	if (old) {
		WARN_ONCE(!mdc->tasks[old], "cpu=%d old=%d new=%d counter=%d\n",
			  cpu, old, new, mdc->tasks[old]);
		mdc->tasks[old] -= 1;
	}
	if (new)
		mdc->tasks[new] += 1;

	/*
	 * The domain is somewhat delayed when a number of tasks are
	 * delayed but there are still others running the workload.
	 *
	 * The domain is fully delayed when all non-idle tasks on the
	 * CPU are delayed, or when a delayed task is actively running
	 * and preventing productive tasks from making headway.
	 *
	 * The state times then add up over all CPUs in the domain: if
	 * the domain is fully blocked on one CPU and there is another
	 * one running the workload, the domain is considered fully
	 * blocked 50% of the time.
	 */
	if (mdc->tasks[MTS_DELAYED_ACTIVE] && !mdc->tasks[MTS_IOWAIT])
		state = MDS_FULL;
	else if (mdc->tasks[MTS_DELAYED])
		state = (mdc->tasks[MTS_RUNNABLE] || mdc->tasks[MTS_IOWAIT]) ?
			MDS_SOME : MDS_FULL;
	else
		state = MDS_NONE;

	if (mdc->state == state)
		return;

	now = cpu_clock(cpu);
	delta = (now - mdc->state_start) / NSEC_PER_USEC;

	domain_move_clock(md);
	md->times[mdc->state] += delta;

	mdc->state = state;
	mdc->state_start = now;
}

static struct memdelay_domain *memcg_domain(struct mem_cgroup *memcg)
{
#ifdef CONFIG_MEMCG
	if (!mem_cgroup_disabled())
		return memcg->memdelay_domain;
#endif
	return &memdelay_global_domain;
}

/**
 * memdelay_task_change - note a task changing its delay/work state
 * @task: the task changing state
 * @old: old task state
 * @new: new task state
 *
 * Updates the task's domain counters to reflect a change in the
 * task's delayed/working state.
 */
void memdelay_task_change(struct task_struct *task,
			  enum memdelay_task_state old,
			  enum memdelay_task_state new)
{
	int cpu = task_cpu(task);
	struct mem_cgroup *memcg;
	unsigned long delay = 0;

#ifdef CONFIG_DEBUG_VM
	WARN_ONCE(task->memdelay_state != old,
		  "cpu=%d task=%p state=%d (in_iowait=%d PF_MEMDELAYED=%d) old=%d new=%d\n",
		  cpu, task, task->memdelay_state, task->in_iowait,
		  !!(task->flags & PF_MEMDELAY), old, new);
	task->memdelay_state = new;
#endif

	/* Account when tasks are entering and leaving delays */
	if (old < MTS_DELAYED && new >= MTS_DELAYED) {
		task->memdelay_start = cpu_clock(cpu);
	} else if (old >= MTS_DELAYED && new < MTS_DELAYED) {
		delay = (cpu_clock(cpu) - task->memdelay_start) / NSEC_PER_USEC;
		task->memdelay_total += delay;
	}

	/* Account domain state changes */
	rcu_read_lock();
	memcg = mem_cgroup_from_task(task);
	do {
		struct memdelay_domain *md;

		md = memcg_domain(memcg);
		md->aggregate += delay;
		domain_cpu_update(md, cpu, old, new);
	} while (memcg && (memcg = parent_mem_cgroup(memcg)));
	rcu_read_unlock();
};

/**
 * memdelay_domain_alloc - allocate a cgroup memory delay domain
 */
struct memdelay_domain *memdelay_domain_alloc(void)
{
	struct memdelay_domain *md;

	md = kzalloc(sizeof(*md), GFP_KERNEL);
	if (!md)
		return NULL;
	md->mdcs = alloc_percpu(struct memdelay_domain_cpu);
	if (!md->mdcs) {
		kfree(md);
		return NULL;
	}
	domain_init(md);
	return md;
}

/**
 * memdelay_domain_free - free a cgroup memory delay domain
 */
void memdelay_domain_free(struct memdelay_domain *md)
{
	if (md) {
		free_percpu(md->mdcs);
		kfree(md);
	}
}

/**
 * memdelay_domain_show - format memory delay domain stats to a seq_file
 * @s: the seq_file
 * @md: the memory domain
 */
int memdelay_domain_show(struct seq_file *s, struct memdelay_domain *md)
{
	domain_move_clock(md);

	seq_printf(s, "%lu\n", md->aggregate);

	seq_printf(s, "%lu.%02lu %lu.%02lu %lu.%02lu\n",
		   LOAD_INT(md->avg_some[0]), LOAD_FRAC(md->avg_some[0]),
		   LOAD_INT(md->avg_some[1]), LOAD_FRAC(md->avg_some[1]),
		   LOAD_INT(md->avg_some[2]), LOAD_FRAC(md->avg_some[2]));

	seq_printf(s, "%lu.%02lu %lu.%02lu %lu.%02lu\n",
		   LOAD_INT(md->avg_full[0]), LOAD_FRAC(md->avg_full[0]),
		   LOAD_INT(md->avg_full[1]), LOAD_FRAC(md->avg_full[1]),
		   LOAD_INT(md->avg_full[2]), LOAD_FRAC(md->avg_full[2]));

#ifdef CONFIG_DEBUG_VM
	{
		int cpu;

		for_each_online_cpu(cpu) {
			struct memdelay_domain_cpu *mdc;

			mdc = per_cpu_ptr(md->mdcs, cpu);
			seq_printf(s, "%d %d %d %d\n",
				   mdc->tasks[MTS_IOWAIT],
				   mdc->tasks[MTS_RUNNABLE],
				   mdc->tasks[MTS_DELAYED],
				   mdc->tasks[MTS_DELAYED_ACTIVE]);
		}
	}
#endif

	return 0;
}

static int memdelay_show(struct seq_file *m, void *v)
{
	return memdelay_domain_show(m, &memdelay_global_domain);
}

static int memdelay_open(struct inode *inode, struct file *file)
{
	return single_open(file, memdelay_show, NULL);
}

static const struct file_operations memdelay_fops = {
	.open           = memdelay_open,
	.read           = seq_read,
	.llseek         = seq_lseek,
	.release        = single_release,
};

static int __init memdelay_proc_init(void)
{
	proc_create("memdelay", 0, NULL, &memdelay_fops);
	return 0;
}
module_init(memdelay_proc_init);
