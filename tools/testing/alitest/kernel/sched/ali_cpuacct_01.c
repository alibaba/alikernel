#define _GNU_SOURCE
#include <sched.h>
#include <ltp/tst_test.h>
#include <linux/sched.h>
#include <sys/mount.h>
#include <syscall.h>
#include <pthread.h>
#include <stdlib.h>

#define MNTPOINT "ali_cpuacct_01_mnt"
int doing = 1;

static void *pid_test(void *args)
{
	int *pid = (int *)args;
	*pid = syscall(__NR_gettid);

	while (doing)
		usleep(10);

	return (void *)NULL;
}

int testcases[] = {
	0x03020100, /*1. A1 A2 B1 B2*/
	0x03010200, /*2. A1 B1 A2 B2*/
	0x02010300, /*3. A1 B1 B2 A2*/
	0x03000201, /*4. B1 A1 A2 B2*/
	0x02000301, /*5. B1 A1 B2 A2*/
	0x01000302, /*6. B1 B2 A1 A2*/
	0x03020100, /*1. A1 A2 B1 B2*/
	0x03010200, /*2. A1 B1 A2 B2*/
	0x02010300, /*3. A1 B1 B2 A2*/
	0x03000201, /*4. B1 A1 A2 B2*/
	0x02000301, /*5. B1 A1 B2 A2*/
	0x01000302, /*6. B1 B2 A1 A2*/
};

static void test_run(unsigned int n)
{
	int i;
	int pid = 0;
	int mask = 0;
	int value = 0;
	int ret = 0;
	pthread_t tid;

	doing = 1;
	mask = 1;
	sched_setaffinity(getpid(), sizeof(int), (cpu_set_t *)&mask);
	pthread_create(&tid, NULL, pid_test, &pid);

	while (pid == 0)
		usleep(1);

	mask = 2;
	sched_setaffinity(pid, sizeof(int), (cpu_set_t *)&mask);
	value = testcases[n];
	tst_res(TINFO, "testcase: %08x", value);

	for (i = 0; i < 17; i++) {
		SAFE_FILE_PRINTF("/proc/sys/mod_cpuacct_01/ret", "0");
		SAFE_FILE_PRINTF("/proc/sys/mod_cpuacct_01/in2", "%d", pid);
		value = testcases[n];
		SAFE_FILE_PRINTF("/proc/sys/mod_cpuacct_01/in1", "%d", value);
		SAFE_FILE_PRINTF(MNTPOINT "/test1/tasks", "%d", pid);

		ret = 0;

		while (ret < 4) {
			SAFE_FILE_SCANF("/proc/sys/mod_cpuacct_01/ret",
					"%d", &ret);
			usleep(10);
		}

		SAFE_FILE_PRINTF("/proc/sys/mod_cpuacct_01/in1", "0");
		SAFE_FILE_PRINTF(MNTPOINT "/tasks", "%d", pid);
		SAFE_FILE_LINES_SCANF(MNTPOINT "/test1/cpuacct.proc_stat",
				      "nr_running %d", &value);

		if (value < 0 || value > 5)
			break;
	}

	doing = 0;
	pthread_join(tid, NULL);
	usleep(10);
	SAFE_FILE_LINES_SCANF(MNTPOINT "/test1/cpuacct.proc_stat",
			      "nr_running %d", &value);

	if (ret != 4 || value)
		tst_res(TFAIL, "cpuacct failure, ret=%d, value=%d", ret, value);
	else
		tst_res(TPASS, "cpuacct successfully");
}

static void test_begin(void)
{
	char buf[32];
	umount(MNTPOINT);
	rmdir(MNTPOINT);
	mkdir(MNTPOINT, 0775);
	SAFE_MOUNT("cpuacct", MNTPOINT, "cgroup", MS_MGC_VAL,
		   "cpuacct,cpu,cpuset");
	mkdir(MNTPOINT "/test1", 0777);
	SAFE_FILE_SCANF(MNTPOINT "/cpuset.mems", "%s", buf);
	SAFE_FILE_PRINTF(MNTPOINT "/test1/cpuset.mems", buf);
	SAFE_FILE_SCANF(MNTPOINT "/cpuset.cpus", "%s", buf);
	SAFE_FILE_PRINTF(MNTPOINT "/test1/cpuset.cpus", "1-2");
	system("rmmod mod_cpuacct_01 2>/dev/null; insmod ./mod_cpuacct_01.ko");
}

static void test_end(void)
{
	system("rmmod mod_cpuacct_01");
	rmdir(MNTPOINT "/test1");
	umount(MNTPOINT);
	rmdir(MNTPOINT);
}

static void do_test(unsigned int n)
{
	test_begin();
	test_run(n);
	test_end();
}

static struct tst_test test = {
	.tcnt = ARRAY_SIZE(testcases),
	.test = do_test,
};

