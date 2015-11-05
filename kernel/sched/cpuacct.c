#include <linux/cgroup.h>
#include <linux/slab.h>
#include <linux/percpu.h>
#include <linux/spinlock.h>
#include <linux/cpumask.h>
#include <linux/seq_file.h>
#include <linux/rcupdate.h>
#include <linux/kernel_stat.h>
#include <linux/err.h>
#include <linux/cpuset.h>
#include <linux/pid_namespace.h>

#include "sched.h"

/*
 * CPU accounting code for task groups.
 *
 * Based on the work by Paul Menage (menage@google.com) and Balbir Singh
 * (balbir@in.ibm.com).
 */

/* Time spent by the tasks of the cpu accounting group executing in ... */
enum cpuacct_stat_index {
	CPUACCT_STAT_USER,	/* ... user mode */
	CPUACCT_STAT_SYSTEM,	/* ... kernel mode */

	CPUACCT_STAT_NSTATS,
};

static const char * const cpuacct_stat_desc[] = {
	[CPUACCT_STAT_USER] = "user",
	[CPUACCT_STAT_SYSTEM] = "system",
};

struct cpuacct_usage {
	u64	usages[CPUACCT_STAT_NSTATS];
};

/* track cpu usage of a group of tasks and its child groups */
struct cpuacct {
	struct cgroup_subsys_state css;
	/* cpuusage holds pointer to a u64-type object on every cpu */
	struct cpuacct_usage __percpu *cpuusage;
	struct kernel_cpustat __percpu *cpustat;
	unsigned long *nr_uninterruptible;
	unsigned long *nr_running;
	unsigned long avenrun[3];
	u64 *nr_switches;
};

static inline struct cpuacct *css_ca(struct cgroup_subsys_state *css)
{
	return css ? container_of(css, struct cpuacct, css) : NULL;
}

/* return cpu accounting group corresponding to this container */
static inline struct cpuacct *cgroup_ca(struct cgroup *cgrp)
{
	return container_of(global_cgroup_css(cgrp, cpuacct_cgrp_id),
			    struct cpuacct, css);
}

struct kernel_cpustat *cgroup_ca_kcpustat_ptr(struct cgroup *cgrp, int cpu)
{
	struct cpuacct *ca;

	ca = cgroup_ca(cgrp);
	return per_cpu_ptr(ca->cpustat, cpu);
}

/* return cpu accounting group to which this task belongs */
static inline struct cpuacct *task_ca(struct task_struct *tsk)
{
	return css_ca(task_css(tsk, cpuacct_cgrp_id));
}

static inline struct cpuacct *parent_ca(struct cpuacct *ca)
{
	return css_ca(ca->css.parent);
}

static DEFINE_PER_CPU(struct cpuacct_usage, root_cpuacct_cpuusage);
static DEFINE_PER_CPU(unsigned long, root_cpuacct_nr_unintr);
static DEFINE_PER_CPU(unsigned long, root_cpuacct_nr_running);
static struct cpuacct root_cpuacct = {
	.cpustat	= &kernel_cpustat,
	.cpuusage	= &root_cpuacct_cpuusage,
	.nr_uninterruptible = &root_cpuacct_nr_unintr,
	.nr_running                     = &root_cpuacct_nr_running,
};

void get_cgroup_avenrun(struct cpuacct *ca, unsigned long *loads,
			unsigned long offset, int shift)
{
	loads[0] = (ca->avenrun[0] + offset) << shift;
	loads[1] = (ca->avenrun[1] + offset) << shift;
	loads[2] = (ca->avenrun[2] + offset) << shift;
}

void get_avenrun_from_tsk(struct task_struct *tsk, unsigned long *loads,
			unsigned long offset, int shift)
{
	struct cpuacct *ca = task_ca(tsk);

	get_cgroup_avenrun(ca, loads, offset, shift);
}


unsigned long long get_nr_switches_from_tsk(struct task_struct *tsk)
{
	int i;
	unsigned long long sum = 0;
	struct cpuacct *ca = task_ca(tsk);
	for_each_possible_cpu(i)
		sum += *per_cpu_ptr(ca->nr_switches, i);

	return sum;
}

int task_ca_increase_nr_switches(struct task_struct *tsk)
{
	struct cpuacct *ca = NULL;
	if (!tsk)
		return -1;

	ca = task_ca(tsk);
	(*this_cpu_ptr(ca->nr_switches))++;

	return 0;
}


struct kernel_cpustat *task_ca_kcpustat_ptr(struct task_struct *tsk, int cpu)
{
	struct cpuacct *ca;

	ca = task_ca(tsk);

	return per_cpu_ptr(ca->cpustat, cpu);
}

unsigned long task_ca_running(struct task_struct *tsk, int cpu)
{
	struct cpuacct *ca;
	unsigned long nr_running = 0, *nrptr = NULL;

	ca = task_ca(tsk);
	nrptr = per_cpu_ptr(ca->nr_running, cpu);
	nr_running = *nrptr;

	return nr_running;
}

bool task_in_nonroot_cpuacct(struct task_struct *tsk)
{
	struct cpuacct *ca = NULL;

	ca = task_ca(tsk);
	if (ca && (ca != &root_cpuacct))
		return true;

	return false;
}


/* create a new cpu accounting group */
static struct cgroup_subsys_state *
cpuacct_css_alloc(struct cgroup_subsys_state *parent_css)
{
	struct cpuacct *ca;
	int i;
	struct kernel_cpustat *kcpustat;

	if (!parent_css)
		return &root_cpuacct.css;

	ca = kzalloc(sizeof(*ca), GFP_KERNEL);
	if (!ca)
		goto out;

	ca->nr_switches = alloc_percpu(u64);
	if (!ca->nr_switches)
		goto out_free_nr_switches;

	ca->cpuusage = alloc_percpu(struct cpuacct_usage);
	if (!ca->cpuusage)
		goto out_free_ca;

	ca->cpustat = alloc_percpu(struct kernel_cpustat);
	if (!ca->cpustat)
		goto out_free_cpuusage;

	ca->nr_uninterruptible = alloc_percpu(unsigned long);
	if (!ca->nr_uninterruptible)
		goto out_free_uninter;

	ca->nr_running = alloc_percpu(unsigned long);
	if (!ca->nr_running)
		goto out_free_running;

	for_each_possible_cpu(i) {
		kcpustat = per_cpu_ptr(ca->cpustat, i);
		kcpustat->cpustat[CPUTIME_IDLE_BASE] =
			kcpustat_cpu(i).cpustat[CPUTIME_IDLE];
		kcpustat->cpustat[CPUTIME_IOWAIT_BASE] =
			kcpustat_cpu(i).cpustat[CPUTIME_IOWAIT];
		kcpustat->cpustat[CPUTIME_STEAL_BASE] =
			kcpustat_cpu(i).cpustat[CPUTIME_USER]
			+ kcpustat_cpu(i).cpustat[CPUTIME_NICE]
			+ kcpustat_cpu(i).cpustat[CPUTIME_SYSTEM]
			+ kcpustat_cpu(i).cpustat[CPUTIME_IRQ]
			+ kcpustat_cpu(i).cpustat[CPUTIME_SOFTIRQ]
			+ kcpustat_cpu(i).cpustat[CPUTIME_GUEST];
	}

	avenrun[0] = avenrun[1] = avenrun[2] = 0;
	return &ca->css;

out_free_running:
	free_percpu(ca->nr_uninterruptible);
out_free_uninter:
	free_percpu(ca->cpustat);
out_free_cpuusage:
	free_percpu(ca->cpuusage);
out_free_nr_switches:
	free_percpu(ca->nr_switches);
out_free_ca:
	kfree(ca);
out:
	return ERR_PTR(-ENOMEM);
}

void update_cpuacct_uninterruptible(struct cpuacct *ca, int cpu, int inc)
{
	unsigned long *nr_uninterruptible =
		per_cpu_ptr(ca->nr_uninterruptible, cpu);

	if (inc == 1)
		(*nr_uninterruptible)++;
	else if (inc == -1)
		(*nr_uninterruptible)--;
}

void update_cpuacct_running(struct cpuacct *ca, int cpu, int inc)
{
	unsigned long *nr_running = per_cpu_ptr(ca->nr_running, cpu);

	if (inc == 1)
		(*nr_running)++;
	else if (inc == -1)
		(*nr_running)--;
}

void update_cpuacct_nr(struct task_struct *p, int cpu,
			int nr_uninter, int nr_running)
{
	struct cpuacct *ca = NULL;

	rcu_read_lock();
	ca = task_ca(p);
	if (unlikely(p->old_ca))
		ca = p->old_ca;

	if (ca && (ca != &root_cpuacct)) {
		update_cpuacct_uninterruptible(ca, cpu, nr_uninter);
		update_cpuacct_running(ca, cpu, nr_running);
	}
	rcu_read_unlock();
}

/* destroy an existing cpu accounting group */
static void cpuacct_css_free(struct cgroup_subsys_state *css)
{
	struct cpuacct *ca = css_ca(css);

	free_percpu(ca->nr_running);
	free_percpu(ca->nr_uninterruptible);
	free_percpu(ca->cpustat);
	free_percpu(ca->cpuusage);
	free_percpu(ca->nr_switches);
	kfree(ca);
}

static void
cpuacct_css_exit(struct task_struct *tsk)
{
	struct rq *rq;
	struct rq_flags flags;
	int cpu = task_cpu(tsk);
	struct cpuacct *ca = task_ca(tsk);

	if (ca && (ca != &root_cpuacct)) {
		rq = task_rq_lock(tsk, &flags);
		if (tsk->se.on_rq)
			update_cpuacct_running(ca, cpu, -1);
		else if (task_contributes_to_load(tsk))
			update_cpuacct_uninterruptible(ca, cpu, -1);
		tsk->old_ca = &root_cpuacct;
		task_rq_unlock(rq, tsk, &flags);
	}

}

static int
cpuacct_css_can_attach(struct cgroup_taskset *tset)
{
	struct rq *rq;
	struct cgroup_subsys_state *dst_css;
	struct task_struct *tsk;
	struct rq_flags flags;

	cgroup_taskset_for_each(tsk, dst_css, tset) {
		rq = task_rq_lock(tsk, &flags);
		tsk->old_ca = task_ca(tsk);
		task_rq_unlock(rq, tsk, &flags);
	}

	return 0;
}

static void
cpuacct_css_cancel_attach(struct cgroup_taskset *tset)
{
	struct rq *rq;
	struct cgroup_subsys_state *dst_css;
	struct task_struct *tsk;
	struct rq_flags flags;

	cgroup_taskset_for_each(tsk, dst_css, tset) {
		rq = task_rq_lock(tsk, &flags);
		tsk->old_ca = NULL;
		task_rq_unlock(rq, tsk, &flags);
	}

}

static void
cpuacct_css_attach(struct cgroup_taskset *tset)
{
	struct rq *rq;
	struct cgroup_subsys_state *dst_css;
	struct task_struct *tsk;
	struct rq_flags flags;
	struct cpuacct *src_ca, *dst_ca;
	int cpu;

	cgroup_taskset_for_each(tsk, dst_css, tset) {
again:
		/* wait for TASK_WAKEUP completion */
		while (unlikely(tsk->state == TASK_WAKING))
			cpu_relax();
		rq = task_rq_lock(tsk, &flags);
		if (unlikely(tsk->state == TASK_WAKING)) {
			task_rq_unlock(rq, tsk, &flags);
			goto again;
		}

		tsk->old_ca = NULL;
		src_ca = task_ca(tsk);
		dst_ca = css_ca(dst_css);

		/* a task not on queue must have been deactived */
		if (dst_ca && dst_ca != &root_cpuacct) {
			if (tsk->se.on_rq)
				update_cpuacct_running(dst_ca, cpu, 1);
			else if (task_contributes_to_load(tsk))
				update_cpuacct_uninterruptible(dst_ca, cpu, 1);
		}

		if (src_ca && src_ca != &root_cpuacct) {
			if (tsk->se.on_rq)
				update_cpuacct_running(src_ca, cpu, -1);
			else if (task_contributes_to_load(tsk))
				update_cpuacct_uninterruptible(src_ca, cpu, -1);
		}
		task_rq_unlock(rq, tsk, &flags);
	}

}


static u64 cpuacct_cpuusage_read(struct cpuacct *ca, int cpu,
				 enum cpuacct_stat_index index)
{
	struct cpuacct_usage *cpuusage = per_cpu_ptr(ca->cpuusage, cpu);
	u64 data;

	/*
	 * We allow index == CPUACCT_STAT_NSTATS here to read
	 * the sum of suages.
	 */
	BUG_ON(index > CPUACCT_STAT_NSTATS);

#ifndef CONFIG_64BIT
	/*
	 * Take rq->lock to make 64-bit read safe on 32-bit platforms.
	 */
	raw_spin_lock_irq(&cpu_rq(cpu)->lock);
#endif

	if (index == CPUACCT_STAT_NSTATS) {
		int i = 0;

		data = 0;
		for (i = 0; i < CPUACCT_STAT_NSTATS; i++)
			data += cpuusage->usages[i];
	} else {
		data = cpuusage->usages[index];
	}

#ifndef CONFIG_64BIT
	raw_spin_unlock_irq(&cpu_rq(cpu)->lock);
#endif

	return data;
}

static void cpuacct_cpuusage_write(struct cpuacct *ca, int cpu, u64 val)
{
	struct cpuacct_usage *cpuusage = per_cpu_ptr(ca->cpuusage, cpu);
	int i;

#ifndef CONFIG_64BIT
	/*
	 * Take rq->lock to make 64-bit write safe on 32-bit platforms.
	 */
	raw_spin_lock_irq(&cpu_rq(cpu)->lock);
#endif

	for (i = 0; i < CPUACCT_STAT_NSTATS; i++)
		cpuusage->usages[i] = val;

#ifndef CONFIG_64BIT
	raw_spin_unlock_irq(&cpu_rq(cpu)->lock);
#endif
}

/* return total cpu usage (in nanoseconds) of a group */
static u64 __cpuusage_read(struct cgroup_subsys_state *css,
			   enum cpuacct_stat_index index)
{
	struct cpuacct *ca = css_ca(css);
	u64 totalcpuusage = 0;
	int i;

	for_each_possible_cpu(i)
		totalcpuusage += cpuacct_cpuusage_read(ca, i, index);

	return totalcpuusage;
}

static u64 cpuusage_user_read(struct cgroup_subsys_state *css,
			      struct cftype *cft)
{
	return __cpuusage_read(css, CPUACCT_STAT_USER);
}

static u64 cpuusage_sys_read(struct cgroup_subsys_state *css,
			     struct cftype *cft)
{
	return __cpuusage_read(css, CPUACCT_STAT_SYSTEM);
}

static u64 cpuusage_read(struct cgroup_subsys_state *css, struct cftype *cft)
{
	return __cpuusage_read(css, CPUACCT_STAT_NSTATS);
}

static int cpuusage_write(struct cgroup_subsys_state *css, struct cftype *cft,
			  u64 val)
{
	struct cpuacct *ca = css_ca(css);
	int cpu;

	/*
	 * Only allow '0' here to do a reset.
	 */
	if (val)
		return -EINVAL;

	for_each_possible_cpu(cpu)
		cpuacct_cpuusage_write(ca, cpu, 0);

	return 0;
}

static int __cpuacct_percpu_seq_show(struct seq_file *m,
				     enum cpuacct_stat_index index)
{
	struct cpuacct *ca = css_ca(seq_css(m));
	u64 percpu;
	int i;

	for_each_possible_cpu(i) {
		percpu = cpuacct_cpuusage_read(ca, i, index);
		seq_printf(m, "%llu ", (unsigned long long) percpu);
	}
	seq_printf(m, "\n");
	return 0;
}

static int cpuacct_percpu_user_seq_show(struct seq_file *m, void *V)
{
	return __cpuacct_percpu_seq_show(m, CPUACCT_STAT_USER);
}

static int cpuacct_percpu_sys_seq_show(struct seq_file *m, void *V)
{
	return __cpuacct_percpu_seq_show(m, CPUACCT_STAT_SYSTEM);
}

static int cpuacct_percpu_seq_show(struct seq_file *m, void *V)
{
	return __cpuacct_percpu_seq_show(m, CPUACCT_STAT_NSTATS);
}

static int cpuacct_all_seq_show(struct seq_file *m, void *V)
{
	struct cpuacct *ca = css_ca(seq_css(m));
	int index;
	int cpu;

	seq_puts(m, "cpu");
	for (index = 0; index < CPUACCT_STAT_NSTATS; index++)
		seq_printf(m, " %s", cpuacct_stat_desc[index]);
	seq_puts(m, "\n");

	for_each_possible_cpu(cpu) {
		struct cpuacct_usage *cpuusage = per_cpu_ptr(ca->cpuusage, cpu);

		seq_printf(m, "%d", cpu);

		for (index = 0; index < CPUACCT_STAT_NSTATS; index++) {
#ifndef CONFIG_64BIT
			/*
			 * Take rq->lock to make 64-bit read safe on 32-bit
			 * platforms.
			 */
			raw_spin_lock_irq(&cpu_rq(cpu)->lock);
#endif

			seq_printf(m, " %llu", cpuusage->usages[index]);

#ifndef CONFIG_64BIT
			raw_spin_unlock_irq(&cpu_rq(cpu)->lock);
#endif
		}
		seq_puts(m, "\n");
	}
	return 0;
}

#ifndef arch_idle_time
#define arch_idle_time(cpu) 0
#endif

static int cpuacct_stats_show(struct seq_file *sf, void *v)
{
	struct cpuacct *ca = css_ca(seq_css(sf));
	s64 val[CPUACCT_STAT_NSTATS];
	int cpu;
	int stat;

	memset(val, 0, sizeof(val));
	for_each_possible_cpu(cpu) {
		u64 *cpustat = per_cpu_ptr(ca->cpustat, cpu)->cpustat;

		val[CPUACCT_STAT_USER]   += cpustat[CPUTIME_USER];
		val[CPUACCT_STAT_USER]   += cpustat[CPUTIME_NICE];
		val[CPUACCT_STAT_SYSTEM] += cpustat[CPUTIME_SYSTEM];
		val[CPUACCT_STAT_SYSTEM] += cpustat[CPUTIME_IRQ];
		val[CPUACCT_STAT_SYSTEM] += cpustat[CPUTIME_SOFTIRQ];
	}

	for (stat = 0; stat < CPUACCT_STAT_NSTATS; stat++) {
		seq_printf(sf, "%s %lld\n",
			   cpuacct_stat_desc[stat],
			   cputime64_to_clock_t(val[stat]));
	}

	return 0;
}

static int cpuacct_stats_proc_show(struct seq_file *sf, void *v)
{
	struct cpuacct *ca = css_ca(seq_css(sf));
	struct cgroup *cgrp;
	u64 user, nice, system, idle, iowait, irq, softirq, steal, guest, load;
	u64 nr_switches = 0;
	int cpu, i;
	struct kernel_cpustat *kcpustat;
	cpumask_var_t cpus_allowed;
	unsigned long avnrun[3];

	cgrp = seq_css(sf)->cgroup;
	user = nice = system = idle = iowait =
		irq = softirq = steal = guest = load = 0;

	for_each_online_cpu(cpu) {
		kcpustat = per_cpu_ptr(ca->cpustat, cpu);
		user += kcpustat->cpustat[CPUTIME_USER];
		nice += kcpustat->cpustat[CPUTIME_NICE];
		system += kcpustat->cpustat[CPUTIME_SYSTEM];
		irq += kcpustat->cpustat[CPUTIME_IRQ];
		softirq += kcpustat->cpustat[CPUTIME_SOFTIRQ];
		guest += kcpustat->cpustat[CPUTIME_GUEST];
	}

	if (global_cgroup_css(cgrp, cpuset_cgrp_id)) {
		cpus_allowed = get_cs_cpu_allowed(cgrp);
		for_each_cpu_and(cpu, cpu_online_mask, cpus_allowed) {
			kcpustat = per_cpu_ptr(ca->cpustat, cpu);
			idle += kcpustat_cpu(cpu).cpustat[CPUTIME_IDLE];
			idle += arch_idle_time(cpu);
			idle -= kcpustat->cpustat[CPUTIME_IDLE_BASE];
			iowait += kcpustat_cpu(cpu).cpustat[CPUTIME_IOWAIT];
			iowait -= kcpustat->cpustat[CPUTIME_IOWAIT_BASE];
			steal = kcpustat_cpu(cpu).cpustat[CPUTIME_USER]
				- kcpustat->cpustat[CPUTIME_USER]
				+ kcpustat_cpu(cpu).cpustat[CPUTIME_NICE]
				- kcpustat->cpustat[CPUTIME_NICE]
				+ kcpustat_cpu(cpu).cpustat[CPUTIME_SYSTEM]
				- kcpustat->cpustat[CPUTIME_SYSTEM]
				+ kcpustat_cpu(cpu).cpustat[CPUTIME_IRQ]
				- kcpustat->cpustat[CPUTIME_IRQ]
				+ kcpustat_cpu(cpu).cpustat[CPUTIME_SOFTIRQ]
				- kcpustat->cpustat[CPUTIME_SOFTIRQ]
				+ kcpustat_cpu(cpu).cpustat[CPUTIME_GUEST]
				- kcpustat->cpustat[CPUTIME_GUEST]
				-  kcpustat->cpustat[CPUTIME_STEAL_BASE];
		}
	} else {
		for_each_online_cpu(cpu) {
			kcpustat = per_cpu_ptr(ca->cpustat, cpu);
			idle += kcpustat_cpu(cpu).cpustat[CPUTIME_IDLE];
			idle += arch_idle_time(cpu);
			idle -= kcpustat->cpustat[CPUTIME_IDLE_BASE];
			iowait += kcpustat_cpu(cpu).cpustat[CPUTIME_IOWAIT];
			iowait -= kcpustat->cpustat[CPUTIME_IOWAIT_BASE];
			steal = kcpustat_cpu(cpu).cpustat[CPUTIME_USER]
			- kcpustat->cpustat[CPUTIME_USER]
			+ kcpustat_cpu(cpu).cpustat[CPUTIME_NICE]
			- kcpustat->cpustat[CPUTIME_NICE]
			+ kcpustat_cpu(cpu).cpustat[CPUTIME_SYSTEM]
			- kcpustat->cpustat[CPUTIME_SYSTEM]
			+ kcpustat_cpu(cpu).cpustat[CPUTIME_IRQ]
			- kcpustat->cpustat[CPUTIME_IRQ]
			+ kcpustat_cpu(cpu).cpustat[CPUTIME_SOFTIRQ]
			- kcpustat->cpustat[CPUTIME_SOFTIRQ]
			+ kcpustat_cpu(cpu).cpustat[CPUTIME_GUEST]
			- kcpustat->cpustat[CPUTIME_GUEST]
			- kcpustat->cpustat[CPUTIME_STEAL_BASE];
		}
	}
	if (ca != &root_cpuacct) {
		for_each_possible_cpu(i)
			nr_switches += *per_cpu_ptr(ca->nr_switches, i);
		get_cgroup_avenrun(ca, avnrun, FIXED_1/200, 0);
	} else {
		for_each_possible_cpu(i)
			nr_switches += cpu_rq(i)->nr_switches;
		get_avenrun(avnrun, FIXED_1/200, 0);
	}

	seq_printf(sf, "user %lld\n", cputime64_to_clock_t(user));
	seq_printf(sf, "nice %lld\n", cputime64_to_clock_t(nice));
	seq_printf(sf, "system %lld\n", cputime64_to_clock_t(system));
	seq_printf(sf, "idle %lld\n", cputime64_to_clock_t(idle));
	seq_printf(sf, "iowait %lld\n", cputime64_to_clock_t(iowait));
	seq_printf(sf, "irq %lld\n", cputime64_to_clock_t(irq));
	seq_printf(sf, "softirq %lld\n", cputime64_to_clock_t(softirq));
	seq_printf(sf, "steal %lld\n", cputime64_to_clock_t(steal));
	seq_printf(sf, "guest %lld\n", cputime64_to_clock_t(guest));
	load = LOAD_INT(avnrun[0]) * 100 + LOAD_FRAC(avnrun[0]);
	seq_printf(sf, "load average(1min) %lld\n", load);
	load = LOAD_INT(avnrun[1]) * 100 + LOAD_FRAC(avnrun[1]);
	seq_printf(sf, "load average(5min) %lld\n", load);
	load = LOAD_INT(avnrun[2]) * 100 + LOAD_FRAC(avnrun[2]);
	seq_printf(sf, "load average(15min) %lld\n", load);
	seq_printf(sf, "nr_switches %lld\n", (u64)nr_switches);

	return 0;
}

static struct cftype files[] = {
	{
		.name = "usage",
		.read_u64 = cpuusage_read,
		.write_u64 = cpuusage_write,
	},
	{
		.name = "usage_user",
		.read_u64 = cpuusage_user_read,
	},
	{
		.name = "usage_sys",
		.read_u64 = cpuusage_sys_read,
	},
	{
		.name = "usage_percpu",
		.seq_show = cpuacct_percpu_seq_show,
	},
	{
		.name = "usage_percpu_user",
		.seq_show = cpuacct_percpu_user_seq_show,
	},
	{
		.name = "usage_percpu_sys",
		.seq_show = cpuacct_percpu_sys_seq_show,
	},
	{
		.name = "usage_all",
		.seq_show = cpuacct_all_seq_show,
	},
	{
		.name = "stat",
		.seq_show = cpuacct_stats_show,
	},
	{
		.name = "proc_stat",
		.seq_show = cpuacct_stats_proc_show,
	},
	{ }	/* terminate */
};

/*
 * charge this task's execution time to its accounting group.
 *
 * called with rq->lock held.
 */
void cpuacct_charge(struct task_struct *tsk, u64 cputime)
{
	struct cpuacct *ca;
	int index = CPUACCT_STAT_SYSTEM;
	struct pt_regs *regs = task_pt_regs(tsk);

	if (regs && user_mode(regs))
		index = CPUACCT_STAT_USER;

	rcu_read_lock();

	for (ca = task_ca(tsk); ca; ca = parent_ca(ca))
		this_cpu_ptr(ca->cpuusage)->usages[index] += cputime;

	rcu_read_unlock();
}

/*
 * Add user/system time to cpuacct.
 *
 * Note: it's the caller that updates the account of the root cgroup.
 */
void cpuacct_account_field(struct task_struct *tsk, int index, u64 val)
{
	struct cpuacct *ca;

	rcu_read_lock();
	for (ca = task_ca(tsk); ca != &root_cpuacct; ca = parent_ca(ca))
		this_cpu_ptr(ca->cpustat)->cpustat[index] += val;
	rcu_read_unlock();
}

struct cgroup_subsys cpuacct_cgrp_subsys = {
	.css_alloc	= cpuacct_css_alloc,
	.css_free	= cpuacct_css_free,
	.can_attach     = cpuacct_css_can_attach,
	.cancel_attach  = cpuacct_css_cancel_attach,
	.attach         = cpuacct_css_attach,
	.exit           = cpuacct_css_exit,
	.legacy_cftypes	= files,
	.early_init	= true,
};

extern  unsigned long calc_load(unsigned long load, unsigned long exp, unsigned long active);

static int cpuacct_cgroup_calc_load(struct cpuacct *acct, void *data)
{
	long active = 0;
	cpumask_var_t cpus_allowed;
	struct cgroup *cgrp = acct->css.cgroup;
	int cpu;
	unsigned long *nrptr;

	if (acct != &root_cpuacct &&
		global_cgroup_css(cgrp, cpuset_cgrp_id)) {
		cpus_allowed = get_cs_cpu_allowed(cgrp);
		for_each_cpu_and(cpu, cpu_online_mask, cpus_allowed) {
			nrptr = per_cpu_ptr(acct->nr_uninterruptible, cpu);
			active += *nrptr;
			nrptr = per_cpu_ptr(acct->nr_running, cpu);
			WARN_ON_ONCE(*nrptr > cpu_rq(cpu)->nr_running);
			active += *nrptr;
		}
		active = active > 0 ? active * FIXED_1 : 0;
		acct->avenrun[0] = calc_load(acct->avenrun[0], EXP_1, active);
		acct->avenrun[1] = calc_load(acct->avenrun[1], EXP_5, active);
		acct->avenrun[2] = calc_load(acct->avenrun[2], EXP_15, active);
	} else {
		acct->avenrun[0] = avenrun[0];
		acct->avenrun[1] = avenrun[1];
		acct->avenrun[2] = avenrun[2];
	}

	return 0;
}

int cpuacct_cgroup_walk_tree(void *data)
{
	struct cpuacct *root = &root_cpuacct;
	int found, ret, nextid;
	struct cgroup_subsys_state *css;
	struct cpuacct *acct;

	nextid = 1;
	rcu_read_lock();
	css_for_each_descendant_pre(css, &root->css) {
		ret = 0;
		acct = NULL;
		if (css && css_tryget(css))
			acct = container_of(css, struct cpuacct, css);
		rcu_read_unlock();
		if (acct) {
			ret = cpuacct_cgroup_calc_load(acct, data);
			css_put(&acct->css);
		}
		nextid = found + 1;
		rcu_read_lock();
		if (ret || !css)
			break;
	}
	rcu_read_unlock();

	return ret;
}

bool in_instance_and_hiding(unsigned int cpu, struct task_struct *task,
			unsigned int *index, bool *in_instance, unsigned int *total)
{
	struct cpumask cpus_allowed;
	int i, id = 0;

	if (!in_noninit_pid_ns(task))
		return false;

       if (!task_in_nonroot_cpuacct(task))
		return false;

	*in_instance = true;
	cpumask_copy(&cpus_allowed, cpu_possible_mask);
	if (task_css(task, cpuset_cgrp_id)) {
		memset(&cpus_allowed, 0, sizeof(cpus_allowed));
		get_tsk_cpu_allowed(task, &cpus_allowed);
	}

	*total = cpumask_weight(&cpus_allowed);
	if (cpumask_test_cpu(cpu, &cpus_allowed)) {
		for_each_possible_cpu(i) {
			if (i == cpu)
				break;
			if (cpumask_test_cpu(i, &cpus_allowed))
				id++;
		}
		*index = id;
		return false;
	} else
		return true;
}

