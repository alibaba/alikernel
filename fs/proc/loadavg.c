#include <linux/fs.h>
#include <linux/init.h>
#include <linux/pid_namespace.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/seqlock.h>
#include <linux/time.h>
#include <linux/cgroup.h>
#include <linux/cpuset.h>

#ifdef CONFIG_CGROUP_CPUACCT
extern unsigned long task_ca_running(struct task_struct *, int);
extern bool task_in_nonroot_cpuacct(struct task_struct *);
extern void get_avenrun_from_tsk(struct task_struct *,
			unsigned long *, unsigned long, int);
#else
bool task_in_nonroot_cpuacct(struct task_struct *) { return false; }
unsigned long task_ca_running(struct task_struct *, int) { return 0; }
void get_avenrun_from_tsk(struct task_struct *, unsigned long *,
			unsigned long, int) {}
#endif

static int loadavg_proc_show(struct seq_file *m, void *v)
{
	unsigned long avnrun[3], nr_runnable = 0;
	struct cpumask cpus_allowed;
	int i;

	rcu_read_lock();
	if (task_in_nonroot_cpuacct(current) &&
		in_noninit_pid_ns(current)) {

		get_avenrun_from_tsk(current, avnrun, FIXED_1/200, 0);

		cpumask_copy(&cpus_allowed, cpu_possible_mask);
		if (task_css(current, cpuset_cgrp_id))
			memset(&cpus_allowed, 0, sizeof(cpus_allowed));
		get_tsk_cpu_allowed(current, &cpus_allowed);

		for_each_cpu_and(i, cpu_possible_mask, &cpus_allowed)
			nr_runnable += task_ca_running(current, i);
	} else {
		get_avenrun(avnrun, FIXED_1/200, 0);
		nr_runnable = nr_running();
	}
	rcu_read_unlock();

	seq_printf(m, "%lu.%02lu %lu.%02lu %lu.%02lu %ld/%d %d\n",
		LOAD_INT(avnrun[0]), LOAD_FRAC(avnrun[0]),
		LOAD_INT(avnrun[1]), LOAD_FRAC(avnrun[1]),
		LOAD_INT(avnrun[2]), LOAD_FRAC(avnrun[2]),
		nr_runnable, nr_threads,
		task_active_pid_ns(current)->last_pid);
	return 0;
}

static int loadavg_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, loadavg_proc_show, NULL);
}

static const struct file_operations loadavg_proc_fops = {
	.open		= loadavg_proc_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int __init proc_loadavg_init(void)
{
	proc_create("loadavg", 0, NULL, &loadavg_proc_fops);
	return 0;
}
fs_initcall(proc_loadavg_init);
