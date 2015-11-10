#include <linux/fs.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/time.h>
#include <linux/kernel_stat.h>
#include <linux/cputime.h>
#include <linux/pid_namespace.h>

#ifdef CONFIG_CGROUP_CPUACCT
extern bool task_in_nonroot_cpuacct(struct task_struct *);
extern struct kernel_cpustat *task_ca_kcpustat_ptr(struct task_struct*, int);
#else
bool task_in_nonroot_cpuacct(struct task_struct *tsk)
{
	return false;
}

struct kernel_cpustat *task_ca_kcpustat_ptr(struct task_struct*, int)
{
	return NULL;
}
#endif

static int uptime_proc_show(struct seq_file *m, void *v)
{
	struct timespec uptime;
	struct timespec idle;
	u64 idletime;
	struct task_struct *init_tsk;
	struct kernel_cpustat *kcpustat;
	u64 nsec;
	u32 rem;
	int i;

	idletime = 0;
	for_each_possible_cpu(i)
		idletime += (__force u64) kcpustat_cpu(i).cpustat[CPUTIME_IDLE];

	get_monotonic_boottime(&uptime);

	/* instance view in container */
	if (in_noninit_pid_ns(current) &&
		task_in_nonroot_cpuacct(current)) {
		for_each_possible_cpu(i) {
			kcpustat = task_ca_kcpustat_ptr(current, i);
			/*
		 	* Cause that CPUs set to this namespace may be changed,
		 	* the real idle for this namespace is complicated.
		 	*
		 	* Just count the global idletime after this namespace
		 	* starts. When namespace is idle, but in global
		 	* there still have tasks running, the idle won't be
		 	* calculated in.
		 	*/
			idletime -= kcpustat->cpustat[CPUTIME_IDLE_BASE];
		}
		init_tsk = task_active_pid_ns(current)->child_reaper;
		uptime = timespec_sub(uptime, ns_to_timespec(init_tsk->start_time));
	}

	nsec = cputime64_to_jiffies64(idletime) * TICK_NSEC;
	idle.tv_sec = div_u64_rem(nsec, NSEC_PER_SEC, &rem);
	idle.tv_nsec = rem;
	seq_printf(m, "%lu.%02lu %lu.%02lu\n",
			(unsigned long) uptime.tv_sec,
			(uptime.tv_nsec / (NSEC_PER_SEC / 100)),
			(unsigned long) idle.tv_sec,
			(idle.tv_nsec / (NSEC_PER_SEC / 100)));
	return 0;
}

static int uptime_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, uptime_proc_show, NULL);
}

static const struct file_operations uptime_proc_fops = {
	.open		= uptime_proc_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int __init proc_uptime_init(void)
{
	proc_create("uptime", 0, NULL, &uptime_proc_fops);
	return 0;
}
fs_initcall(proc_uptime_init);
