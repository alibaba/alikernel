#ifndef _LINUX_MEMDELAY_H
#define _LINUX_MEMDELAY_H

#include <linux/spinlock_types.h>
#include <linux/sched.h>

struct seq_file;
struct css_set;
struct rq;

#define LOAD_INT(x) ((x) >> FSHIFT)
#define LOAD_FRAC(x) LOAD_INT(((x) & (FIXED_1-1)) * 100)

/*
 * Task productivity states tracked by the scheduler
 */
enum memdelay_task_state {
	MTS_NONE,		/* Idle/unqueued/untracked */
	MTS_IOWAIT,		/* Waiting for IO, not memory delayed */
	MTS_RUNNABLE,		/* On the runqueue, not memory delayed */
	MTS_DELAYED,		/* Memory delayed, not running */
	MTS_DELAYED_ACTIVE,	/* Memory delayed, actively running */
	NR_MEMDELAY_TASK_STATES,
};

/*
 * System/cgroup delay state tracked by the VM, composed of the
 * productivity states of all tasks inside the domain.
 */
enum memdelay_domain_state {
	MDS_NONE,		/* No delayed tasks */
	MDS_SOME,		/* Delayed tasks, working tasks */
	MDS_FULL,		/* Delayed tasks, no working tasks */
	NR_MEMDELAY_DOMAIN_STATES,
};

struct memdelay_domain_cpu {
	/* Task states of the domain on this CPU */
	int tasks[NR_MEMDELAY_TASK_STATES];

	/* Delay state of the domain on this CPU */
	enum memdelay_domain_state state;

	/* Aggregate delayed time of all domain tasks on this cpu*/
	unsigned long aggregate_direct;
	unsigned long aggregate_background;

	/* Time of last state change */
	u64 state_start;
};

struct memdelay_domain {
	/* Per-CPU delay states in the domain */
	struct memdelay_domain_cpu __percpu *mdcs;

	/* Cumulative state times from all CPUs */
	unsigned long times[NR_MEMDELAY_DOMAIN_STATES];

	/* Decaying state time averages over 1m, 5m, 15m */
	unsigned long period_expires;
	unsigned long avg_full[3];
	unsigned long avg_some[3];
};

/* mm/memdelay.c */
extern struct memdelay_domain memdelay_global_domain;

void memdelay_task_change(struct task_struct *task,
			  enum memdelay_task_state old,
			  enum memdelay_task_state new);
struct memdelay_domain *memdelay_domain_alloc(void);
void memdelay_domain_free(struct memdelay_domain *md);
int memdelay_domain_show(struct seq_file *s, struct memdelay_domain *md);

#ifdef CONFIG_MEM_DELAY
/* mm/memdelay.c */
void memdelay_init(void);

/* kernel/sched/memdelay.c */
void memdelay_enter(unsigned long *flags, bool isdirect);
void memdelay_leave(unsigned long *flags);

void memdelay_enqueue_task(struct task_struct *p, int flags);
void memdelay_dequeue_task(struct task_struct *p, int flags);
void memdelay_try_to_wake_up(struct task_struct *p);

/**
 * memdelay_schedule - note a context switch
 * @prev: task scheduling out
 * @next: task scheduling in
 *
 * A task switch doesn't affect the balance between delayed and
 * productive tasks, but we have to update whether the delay is
 * actively using the CPU or not.
 */
static inline void memdelay_schedule(struct task_struct *prev,
				     struct task_struct *next)
{
	if (unlikely(prev->memdelay_slowpath))
		memdelay_task_change(prev, MTS_DELAYED_ACTIVE, MTS_DELAYED);

	if (unlikely(next->memdelay_slowpath))
		memdelay_task_change(next, MTS_DELAYED, MTS_DELAYED_ACTIVE);
}

/**
 * memdelay_wakeup - note a task waking up
 * @task: the task
 *
 * Notes an idle task becoming productive. Delayed tasks remain
 * delayed even when they become runnable.
 */
static inline void memdelay_wakeup(struct task_struct *task)
{
	if (unlikely(task->memdelay_slowpath))
		return;

	if (unlikely(task->in_iowait))
		memdelay_task_change(task, MTS_IOWAIT, MTS_RUNNABLE);
	else
		memdelay_task_change(task, MTS_NONE, MTS_RUNNABLE);
}

/**
 * memdelay_wakeup - note a task going to sleep
 * @task: the task
 *
 * Notes a working tasks becoming unproductive. Delayed tasks remain
 * delayed.
 */
static inline void memdelay_sleep(struct task_struct *task)
{
	if (task->memdelay_slowpath)
		return;

	if (task->in_iowait)
		memdelay_task_change(task, MTS_RUNNABLE, MTS_IOWAIT);
	else
		memdelay_task_change(task, MTS_RUNNABLE, MTS_NONE);
}

/**
 * memdelay_del_add - track task movement between runqueues
 * @task: the task
 * @runnable: a runnable task is moved if %true, unqueued otherwise
 * @add: task is being added if %true, removed otherwise
 *
 * Update the memdelay domain per-cpu states as tasks are being moved
 * around the runqueues.
 */
static inline void memdelay_del_add(struct task_struct *task,
				    bool runnable, bool add)
{
	int state;

	if (task->memdelay_slowpath)
		state = MTS_DELAYED;
	else if (runnable)
		state = MTS_RUNNABLE;
	else if (task->in_iowait)
		state = MTS_IOWAIT;
	else
		return; /* already MTS_NONE */

	if (add)
		memdelay_task_change(task, MTS_NONE, state);
	else
		memdelay_task_change(task, state, MTS_NONE);
}

static inline void memdelay_del_runnable(struct task_struct *task)
{
	memdelay_del_add(task, true, false);
}

static inline void memdelay_add_runnable(struct task_struct *task)
{
	memdelay_del_add(task, true, true);
}

static inline void memdelay_del_sleeping(struct task_struct *task)
{
	memdelay_del_add(task, false, false);
}

static inline void memdelay_add_sleeping(struct task_struct *task)
{
	memdelay_del_add(task, false, true);
}

#ifdef CONFIG_CGROUPS
void cgroup_move_task(struct task_struct *task,
			struct css_set *from,
			struct css_set *to);
#endif

#else /* CONFIG_MEM_DELAY */
static inline void memdelay_enqueue_task(struct task_struct *p, int flags)
{
}

static inline void memdelay_dequeue_task(struct task_struct *p, int flags)
{
}

static inline void memdelay_try_to_wake_up(struct task_struct *p)
{
}

static inline void memdelay_enter(unsigned long *flags, bool isdirect)
{
}

static inline void memdelay_leave(unsigned long *flags)
{
}

static inline void memdelay_init(void)
{
}

static inline void memdelay_schedule(struct task_struct *prev,
				     struct task_struct *next)
{
}

static inline void memdelay_wakeup(struct task_struct *task)
{
}

static inline void memdelay_sleep(struct task_struct *task)
{
}

static inline void memdelay_del_runnable(struct task_struct *task)
{
}

static inline void memdelay_add_runnable(struct task_struct *task)
{
}

static inline void memdelay_del_sleeping(struct task_struct *task)
{
}

static inline void memdelay_add_sleeping(struct task_struct *task)
{
}

#ifdef CONFIG_CGROUPS
static inline void cgroup_move_task(struct task_struct *task,
				struct css_set *from,
				struct css_set *to)
{
	rcu_assign_pointer(task->cgroups, to);
}
#endif

#endif /* CONFIG_MEM_DELAY */

#endif /* _LINUX_MEMDELAY_H */
