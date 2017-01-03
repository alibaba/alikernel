#ifndef _SCHED_CPUACCT_H
#define _SCHED_CPUACTT_H

#ifdef CONFIG_CGROUP_CPUACCT

extern void cpuacct_charge(struct task_struct *tsk, u64 cputime);
extern void cpuacct_account_field(struct task_struct *tsk, int index, u64 val);
extern void update_cpuacct_nr(struct task_struct *p, int cpu,
			int nr_uninter, int nr_running);
extern int cpuacct_cgroup_walk_tree(void *data);
extern unsigned long long get_nr_switches_from_tsk(struct task_struct *tsk);
extern int task_ca_increase_nr_switches(struct task_struct *tsk);
extern void update_cpuacct_running_from_tg(struct task_group *tg,
			int cpu, int inc);
#else

static inline void cpuacct_charge(struct task_struct *tsk, u64 cputime)
{
}

static inline void
cpuacct_account_field(struct task_struct *tsk, int index, u64 val)
{
}

static inline void
update_cpuacct_nr(struct task_struct *p, int cpu,
			int nr_uninter, int nr_running)
{
}

int cpuacct_cgroup_walk_tree(void *data)
{
	return 0;
}

unsigned long long get_nr_switches_from_tsk(struct task_struct *tsk)
{
	return 0;
}

int task_ca_increase_nr_switches(struct task_struct *tsk)
{
	return 0;
}
void update_cpuacct_running_from_tg(struct task_group *tg,
			int cpu, int inc)
{
}

#endif
#endif
