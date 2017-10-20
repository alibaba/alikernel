#include <ltp/tst_test.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/mount.h>

int doing = 1;
#define MNTPOINT "ali_cpuacct_02_mnt"

struct testcases {
	int pid_time;
	int sw_time;
} testcases[] = {
	{60, 100},
	{70, 100},
	{80, 100},
	{90, 100},
	{100, 100},
	{110, 100},
	{120, 100},
	{130, 100},
	{140, 100},
};

void handler(int sig)
{
	doing = 0;
}

void do_test(struct testcases *tc)
{
	int i;
	int pid = 0;

	while (doing) {
		pid = vfork();

		if (pid == 0) {
			usleep(tc->pid_time);
			exit(0);
		} else {
			waitpid(pid, NULL, 0);

			for (i = 0; i < 10; i++)
				usleep(1);
		}
	}
}

int get_running(char *dir, int *nr)
{
	char file[256];
	int value = 0;
	snprintf(file, sizeof(file), "%s/cpuacct.proc_stat", dir);
	SAFE_FILE_LINES_SCANF(file, "nr_uninterrupible %d", &value);
	SAFE_FILE_LINES_SCANF(file, "nr_running %d", nr);
	return value;
}

void do_move(struct testcases *tc, int pid)
{
	int i, ret, nr;
	int cnt = 0;
	char src[128];
	char dst[128];
	char str[3][128];

	umount(MNTPOINT);
	rmdir(MNTPOINT);
	mkdir(MNTPOINT, 0775);
	SAFE_MOUNT("cpuacct", MNTPOINT, "cgroup",
		   MS_MGC_VAL, "cpuacct,cpu,cpuset");

	for (i = 0; i < 3; i++) {
		snprintf(str[i], 128, "%s/test%d", MNTPOINT, i);
		mkdir(str[i], 0755);
		snprintf(src, 128, "%s/cpuset.mems", MNTPOINT);
		snprintf(dst, 128, "%s/cpuset.mems", str[i]);
		SAFE_CP(src, dst);
		snprintf(src, 128, "%s/cpuset.cpus", MNTPOINT);
		snprintf(dst, 128, "%s/cpuset.cpus", str[i]);
		SAFE_CP(src, dst);
	}

	ret = 0;
	nr = 0;

	while (ret >= 0 && nr >= 0 && cnt < 1000) {
		cnt++;
		snprintf(dst, 128, "%s/tasks", str[cnt % 3]);
		SAFE_FILE_PRINTF(dst, "%d", pid);
		usleep(tc->sw_time);
		nr = 0;
		ret = get_running(str[(cnt - 1) % 3], &nr);

		if (ret < 0 || nr < 0)
			break;
		else if (ret > 0 || nr > 0) {
			usleep(tc->pid_time + tc->sw_time);
			ret = get_running(str[(cnt - 1) % 3], &nr);
		}
	}

	for (i = 0; i < 3; i++)
		rmdir(str[i]);

	umount(MNTPOINT);
	rmdir(MNTPOINT);

	if (ret || nr)
		tst_res(TFAIL,
			"failure, cnt=%d nr_uninterrupible=%d, nr_running=%d",
			cnt, ret, nr);
	else
		tst_res(TPASS, "successfully: cnt=%d", cnt);
}

static void test_run(unsigned int n)
{
	struct testcases *tc = &testcases[n];
	int pid = fork();

	if (pid == 0) {
		signal(2, handler);
		do_test(tc);
	} else {
		do_move(tc, pid);
		kill(pid, 2);
		waitpid(pid, NULL, 0);
	}
}


static struct tst_test test = {
	.tcnt  = ARRAY_SIZE(testcases),
	.test = test_run,
};
