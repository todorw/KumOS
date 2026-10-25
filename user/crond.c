#include "kumos_libc.h"

#define CRON_MAX   8
#define CRON_CMD   64

typedef struct {
    uint32_t interval_sec;
    uint32_t last_run;
    char     cmd[CRON_CMD];
    int      active;
} cron_job_t;

static cron_job_t jobs[CRON_MAX];
static int njobs = 0;

static void cron_add(uint32_t interval, const char *cmd) {
    if (njobs >= CRON_MAX) return;
    jobs[njobs].interval_sec = interval;
    jobs[njobs].last_run = uptime();
    strncpy(jobs[njobs].cmd, cmd, CRON_CMD-1);
    jobs[njobs].active = 1;
    njobs++;
}

static void run_cmd(const char *cmd) {
    char upper[64]; int i=0;
    while(cmd[i]&&i<60){char c=cmd[i];if(c>='a'&&c<='z')c-=32;upper[i++]=c;}
    upper[i]=0;
    if (!strchr(upper,'.')) strcat(upper,".ELF");
    int pid = sys_fork();
    if (pid == 0) {
        char *argv[2] = { (char *)cmd, 0 };
        execve(upper, argv);
        exit(127);
    }
    if (pid > 0) waitpid(pid);
}

int main(void) {
    printf("\n  crond - KumOS cron daemon\n");
    printf("  PID: %d\n\n", getpid());

    cron_add(10, "sysinfo");
    printf("  Job 1: sysinfo every 10s\n\n");
    printf("  Running... (type Ctrl+C or kill %d to stop)\n\n", getpid());

    for (;;) {
        uint32_t now = uptime();
        for (int i=0;i<njobs;i++) {
            if (!jobs[i].active) continue;
            if (now - jobs[i].last_run >= jobs[i].interval_sec) {
                printf("[crond] Running: %s\n", jobs[i].cmd);
                run_cmd(jobs[i].cmd);
                jobs[i].last_run = now;
            }
        }
        sleep(1000);
    }
}
