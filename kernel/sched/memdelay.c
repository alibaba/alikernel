/*
 * Memory delay metric
 *
 * Copyright (c) 2017 Facebook, Johannes Weiner
 *
 * This code quantifies and reports to userspace the wall-time impact
 * of memory pressure on the system and memory-controlled cgroups.
 */

#include <linux/memdelay.h>
#include <linux/cgroup.h>
#include <linux/sched.h>

#include "sched.h"

/**
 * memdelay_enter - mark the beginning of a memory delay section
 * @flags: flags to handle nested memdelay sections
 * @isdirect: direct delay or backgroud delay in app perspecitive
 *
 * Marks the calling task as being delayed due to a lack of memory,
 * such as waiting for a workingset refault or performing reclaim.
 */
void memdelay_enter(unsigned long *flags, bool isdirect)
{
	struct rq *rq;

	*flags = current->flags & PF_MEMDELAY;
	if (*flags)
		return;
	/*
	 * PF_MEMDELAY & accounting needs to be atomic wrt changes to
	 * the task's scheduling state and its domain association.
	 * Otherwise we could race with CPU or cgroup migration and
	 * misaccount.
	 */
	local_irq_disable();
	rq = this_rq();
	raw_spin_lock(&rq->lock);

	current->flags |= PF_MEMDELAY;
	current->memdelay_isdirect = isdirect;
	memdelay_task_change(current, MTS_RUNNABLE, MTS_DELAYED_ACTIVE);

	raw_spin_unlock(&rq->lock);
	local_irq_enable();
}

/**
 * memdelay_leave - mark the end of a memory delay section
 * @flags: flags to handle nested memdelay sections
 *
 * Marks the calling task as no longer delayed due to memory.
 */
void memdelay_leave(unsigned long *flags)
{
	struct rq *rq;

	if (*flags)
		return;
	/*
	 * PF_MEMDELAY & accounting needs to be atomic wrt changes to
	 * the task's scheduling state and its domain association.
	 * Otherwise we could race with CPU or cgroup migration and
	 * misaccount.
	 */
	local_irq_disable();
	rq = this_rq();
	raw_spin_lock(&rq->lock);

	current->flags &= ~PF_MEMDELAY;
	memdelay_task_change(current, MTS_DELAYED_ACTIVE, MTS_RUNNABLE);

	raw_spin_unlock(&rq->lock);
	local_irq_enable();
}

#ifdef CONFIG_CGROUPS
/**
 * cgroup_move_task - move task to a different cgroup
 * @task: the task
 * @to: the target css_set
 *
 * Move task to a new cgroup and safely migrate its associated
 * delayed/working state between the different domains.
 *
 * This function acquires the task's rq lock to lock out concurrent
 * changes to the task's scheduling state and - in case the task is
 * running - concurrent changes to its delay state.
 */
void cgroup_move_task(struct task_struct *task, struct css_set *to)
{
	struct rq_flags rf;
	struct rq *rq;
	int state;

	rq = task_rq_lock(task, &rf);

	if (task->flags & PF_MEMDELAY)
		state = MTS_DELAYED + task_current(rq, task);
	else if (task_on_rq_queued(task))
		state = MTS_RUNNABLE;
	else if (task->in_iowait)
		state = MTS_IOWAIT;
	else
		state = MTS_NONE;

	/*
	 * Lame to do this here, but the scheduler cannot be locked
	 * from the outside, so we move cgroups from inside sched/.
	 */
	memdelay_task_change(task, state, MTS_NONE);
	rcu_assign_pointer(task->cgroups, to);
	memdelay_task_change(task, MTS_NONE, state);

	task_rq_unlock(rq, task, &rf);
}
#endif /* CONFIG_CGROUPS */
