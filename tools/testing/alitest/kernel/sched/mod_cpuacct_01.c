#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/sysctl.h>
#include <linux/delay.h>
#include <linux/sched.h>

unsigned long (*orig_find_task_by_vpid)(pid_t vnr);
struct gcss {
	int in1;
	int in2;
	int ret;
	u32 loop;
	unsigned long tsk;
} gcss;

static int do_handler(int *c, int index)
{
	int cnt = 0;
	u8 *arr = (u8 *)c;

	if (*c == 0) {
		gcss.loop = 0;
		return 0;
	}

	if (++gcss.loop > 1000000) {
		gcss.in1 = 0;
		gcss.ret = 5;
		pr_info("timeout.2");
		return 0;
	}

	if (index < 2 || (index == 3 && gcss.ret == 3)) {
		/*
		pr_info("timeout: %d arr: %d,%d,%d,%d",
		index, arr[0], arr[1], arr[2], arr[3]);
		*/
		while (1) {
			cnt++;

			if (arr[index] == gcss.ret)
				break;

			udelay(10);

			if (cnt > 10000) {
				pr_info("timeout: %d check: %d <> %d",
					index, arr[index], gcss.ret);
				cnt = 0;
				gcss.in1 = 0;
				gcss.ret = 5;
				break;
			}
		}
	} else {
		cnt++;
	}

	if (cnt && arr[index] == gcss.ret) {
		/*pr_info("done: %d => step: %d", index, arr[index]);*/
		gcss.ret++;
	}

	return 0;
}

static int can_attach_handler(struct kretprobe_instance *ri,
			      struct pt_regs *regs)
{
	return do_handler(&gcss.in1, 0);
}

static int attach_handler(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	return do_handler(&gcss.in1, 1);
}

static int check_pid(unsigned long tsk)
{
	if (gcss.in2 == 0)
		return 1;

	if (gcss.tsk == 0) {
		rcu_read_lock();
		gcss.tsk = orig_find_task_by_vpid(
				   gcss.in2);
		rcu_read_unlock();
	}

	if (tsk != gcss.tsk)
		return 1;

	return 0;
}

static int enqueue_task_fair_handler(struct kretprobe_instance *ri,
				     struct pt_regs *regs)
{
	if (check_pid(regs->si))
		return 0;

	return do_handler(&gcss.in1, 2);
}

static int dequeue_task_fair_handler(struct kretprobe_instance *ri,
				     struct pt_regs *regs)
{
	if (check_pid(regs->si))
		return 0;

	return do_handler(&gcss.in1, 3);
}

static struct kretprobe rp1 = {
	.entry_handler  = attach_handler,
	.kp.symbol_name = "cpuacct_css_attach"
};

static struct kretprobe rp2 = {
	.entry_handler  = can_attach_handler,
	.kp.symbol_name = "cpuacct_css_can_attach"
};

static struct kretprobe rp3 = {
	.entry_handler  = enqueue_task_fair_handler,
	.kp.symbol_name = "enqueue_task_fair"
};

static struct kretprobe rp4 = {
	.entry_handler  = dequeue_task_fair_handler,
	.kp.symbol_name = "dequeue_task_fair"
};

static struct kretprobe *rps[] = {&rp1, &rp2, &rp3, &rp4};

static struct ctl_table cpuacct_table[] = {
	{
		.procname       = "in1",
		.data           = &gcss.in1,
		.maxlen         = sizeof(int),
		.mode           = 0666,
		.proc_handler   = proc_dointvec,
	},
	{
		.procname       = "in2",
		.data           = &gcss.in2,
		.maxlen         = sizeof(int),
		.mode           = 0666,
		.proc_handler   = proc_dointvec,
	},
	{
		.procname       = "ret",
		.data           = &gcss.ret,
		.maxlen         = sizeof(int),
		.mode           = 0666,
		.proc_handler   = proc_dointvec,
	},
	{ }
};

static struct ctl_table cpuacct_root_table[] = {
	{
		.procname       = "mod_cpuacct_01",
		.maxlen         = 0,
		.mode           = 0555,
		.child          = cpuacct_table,
	},
	{ }
};

static struct ctl_table_header *cpuacct_sysctl_header;

#define LOOPUP_SYM(n) {							\
		if (!(orig_##n = (void *)kallsyms_lookup_name(#n))) {	\
			pr_err("Cannot find symbol %s\n", #n);		\
			return -EINVAL;					\
		}							\
	}

static int __init kprobe_init(void)
{
	int ret;

	LOOPUP_SYM(find_task_by_vpid);
	cpuacct_sysctl_header = register_sysctl_table(cpuacct_root_table);
	ret = register_kretprobes(rps, sizeof(rps) / sizeof(*rps));

	if (ret < 0) {
		pr_info("register_kprobe failed, returned %d\n", ret);
		return ret;
	}

	return 0;
}

static void __exit kprobe_exit(void)
{
	unregister_kretprobes(rps, sizeof(rps) / sizeof(*rps));

	if (cpuacct_sysctl_header)
		unregister_sysctl_table(cpuacct_sysctl_header);
}

module_init(kprobe_init)
module_exit(kprobe_exit)
MODULE_LICENSE("GPL");

