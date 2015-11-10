#ifdef CONFIG_CGROUP_CPUACCT

extern void cpuacct_charge(struct task_struct *tsk, u64 cputime);
extern void cpuacct_account_field(struct task_struct *tsk, int index, u64 val);
extern unsigned long long get_nr_switches_from_tsk(struct task_struct *tsk);
extern int task_ca_increase_nr_switches(struct task_struct *tsk);
#else

static inline void cpuacct_charge(struct task_struct *tsk, u64 cputime)
{
}

static inline void
cpuacct_account_field(struct task_struct *tsk, int index, u64 val)
{
}

unsigned long long get_nr_switches_from_tsk(struct task_struct *tsk)
{
	return 0;
}

int task_ca_increase_nr_switches(struct task_struct *tsk)
{
	return 0;
}

#endif
