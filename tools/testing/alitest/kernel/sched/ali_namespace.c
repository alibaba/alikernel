#include <ltp/tst_test.h>
#include <linux/sched.h>
#include <sys/mount.h>
#include <stdio.h>
#include <sys/wait.h>
#include <errno.h>
#include <syscall.h>

#define MNTPOINT "ali_cgroup_01_mnt"
#define MNTPOINT1 "ali_cgroup_01_mnt1"
#define STACK_SIZE 65536
static char container_stack[STACK_SIZE];
static int mount_flag;
static int fildes[2];

static struct host_data {
	unsigned long btime_host;
	unsigned long uptime_host;
};

static int test_btime(unsigned long bh)
{
	unsigned long btime_docker;

	SAFE_FILE_LINES_SCANF("/proc/stat","btime %ld", &btime_docker);
	if (btime_docker > bh){
		tst_res(TPASS, "btime test");
	} else {
		tst_res(TFAIL, "btime test  host[%ld], cgroup[%ld]",bh,btime_docker);
	}
	
	return 0;
}

static int test_uptime(unsigned long uh)
{
        unsigned long uptime_docker;

        SAFE_FILE_SCANF("/proc/uptime","%ld", &uptime_docker);
        if (uptime_docker < uh){
                tst_res(TPASS, "uptime test");
        } else {
                tst_res(TFAIL, "uptime test  host[%ld], cgroup[%ld]",uh,uptime_docker);
        }
        
        return 0;
}

static int test_ctxt(void)
{
	int ctxt;
	
	SAFE_FILE_LINES_SCANF("/proc/stat","ctxt %ld", &ctxt);
	if (ctxt > 6){
		tst_res(TFAIL, "ctxt %d",ctxt);
	} else {
		tst_res(TPASS, "ctxt test");
	}

	return 0;	
}

static int cpu_online_test(void)
{
	int online;
	
	SAFE_FILE_SCANF("/sys/devices/system/cpu/online","%ld", &online);
	
	if(online != 1){
		tst_res(TFAIL, "cpu online %d",online);
	} else {
		tst_res(TPASS, "cpu online test");
	}	

	return 0;
}
static void touch_memory(char *p, int size)
{
        int i;
        int pagesize = getpagesize();

        for (i = 0; i < size; i += pagesize)
                p[i] = 0xef;
}


static int test_memsize(void)
{
	int memsize = 4096000;
	int memusage;
	char *p;

	p = mmap(NULL, memsize, PROT_WRITE | PROT_READ,
		MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
	if (p == 0){
		tst_res(TFAIL, "mmap(anonymous) failed");
		return 0;
	}
	touch_memory(p, memsize);
	SAFE_FILE_SCANF(MNTPOINT "/docker1/memory.usage_in_bytes", "%d", &memusage);
	if (memusage - memsize > 1000000)
		tst_res(TFAIL, "memusage %d, but real memsize is about 4096000",memusage);	
	else
		tst_res(TPASS, "test memsize");
	munmap(p,memsize);

}

static int test_meminfo(void)
{
	long total,value1,value2;
	
	SAFE_FILE_LINES_SCANF("/proc/meminfo","MemTotal: %ld", &total);
	if (total != 6400){
		tst_res(TFAIL, "MemTotal: %ld should be 6400",total);
	}
	SAFE_FILE_LINES_SCANF("/proc/meminfo","MemFree: %ld", &value1);
	SAFE_FILE_LINES_SCANF("/proc/meminfo","MemAvailable: %ld", &value2);
	if(value1 != value2){
		tst_res(TFAIL, "MemFree:%ld and MemAvailable:%ld not same",value1,value2);
	}
	SAFE_FILE_LINES_SCANF("/proc/meminfo","Cached: %ld", &value1);
	if(value1 > total){
		tst_res(TFAIL, "cached:%ld",value1);
	}
	SAFE_FILE_LINES_SCANF("/proc/meminfo","Active: %ld", &value1);
	if(value1 > total){
		tst_res(TFAIL, "Active:%ld",value1);
	}
	SAFE_FILE_LINES_SCANF("/proc/meminfo","Inactive: %ld", &value1);
	if(value1 > total){
		tst_res(TFAIL, "Inactive:%ld",value1);
	}
		tst_res(TPASS, "meminfo pass");
	
}

char *const child_args[] = {
	"/bin/bash",
	NULL
};


static int container_main(void *arg)
{
	int ret, i;
	struct host_data *hd = (struct host_data *)arg;

	SAFE_READ(1, fildes[0], &ret, sizeof(int));
	test_memsize();
	test_btime(hd->btime_host);
	test_uptime(hd->uptime_host);
	cpu_online_test();
	test_meminfo();
	test_ctxt();

	return 0;
}

static void run_test(void)
{
	int ret = 0;
	int flags = CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | SIGCHLD;
	struct host_data hostdata;

	SAFE_FILE_LINES_SCANF("/proc/stat","btime %ld", &hostdata.btime_host);
	SAFE_FILE_SCANF("/proc/uptime","%ld", &hostdata.uptime_host);
	int pid = ltp_clone(flags, container_main, &hostdata, STACK_SIZE,
			    container_stack);
	SAFE_FILE_PRINTF(MNTPOINT "/docker1/tasks", "%d", pid);
	SAFE_FILE_PRINTF(MNTPOINT "/docker1/memory.limit_in_bytes", "6553600");
	SAFE_FILE_PRINTF(MNTPOINT1 "/docker1/cpuset.cpus", "%d", 1);
	SAFE_FILE_PRINTF(MNTPOINT1 "/docker1/cpuset.mems", "%d", 0);
	SAFE_FILE_PRINTF(MNTPOINT1 "/docker1/tasks", "%d", pid);
	SAFE_WRITE(1, fildes[1], &ret, sizeof(int));
	waitpid(pid, &ret, 0);
}

static void do_test(unsigned int n)
{
	run_test();
}


static void setup(void)
{
	umount(MNTPOINT);
	rmdir(MNTPOINT);
	mkdir(MNTPOINT, 0775);
	umount(MNTPOINT1);
	rmdir(MNTPOINT1);
	mkdir(MNTPOINT1, 0775);
	SAFE_MOUNT("memory", MNTPOINT, "cgroup", MS_MGC_VAL, "memory");
	SAFE_MOUNT("cpuacct", MNTPOINT1, "cgroup", MS_MGC_VAL, "cpu,cpuacct,cpuset");
	mount_flag = 1;
	mkdir(MNTPOINT "/docker1", 0777);
	mkdir(MNTPOINT1 "/docker1", 0777);
	SAFE_PIPE(fildes);
}

static void cleanup(void)
{
	rmdir(MNTPOINT "/docker1");
	rmdir(MNTPOINT1 "/docker1");

	if (mount_flag){
		tst_umount(MNTPOINT);
		tst_umount(MNTPOINT1);
	}
	mount_flag = 0;
	rmdir(MNTPOINT);
	rmdir(MNTPOINT1);
	close(fildes[0]);
	close(fildes[1]);
}

static struct tst_test test = {
	.tcnt = 1,
	.test = do_test,
	.setup = setup,
	.cleanup = cleanup,
};

