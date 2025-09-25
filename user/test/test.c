#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <getopt.h>
#include <sched.h>

#define __NR_get_wfq_info 467
#define __NR_set_wfq_weight 468
#define MAX_CPUS 8

struct wfq_info {
    int num_cpus;
    int nr_running[MAX_CPUS];
    int total_weight[MAX_CPUS];
};

static inline int get_wfq_info(struct wfq_info *info) {
    return syscall(__NR_get_wfq_info, info);
}

static inline int set_wfq_weight(int weight) {
    return syscall(__NR_set_wfq_weight, weight);
}

void busy_work(void) {
    volatile unsigned long long x = 0;
    while (1) {
        x += 1;
        if (x % 1000000000ULL == 0) {
            asm volatile("" ::: "memory"); // prevent compiler optimizations
        }
    }
}

int main(int argc, char *argv[]) {
    int num_procs = 4;   // default
    int timeout = 20;    // default seconds

    int opt;
    while ((opt = getopt(argc, argv, "n:t:")) != -1) {
        switch (opt) {
        case 'n':
            num_procs = atoi(optarg);
            break;
        case 't':
            timeout = atoi(optarg);
            break;
        default:
            fprintf(stderr, "Usage: %s [-n num_procs] [-t seconds]\n", argv[0]);
            exit(EXIT_FAILURE);
        }
    }

    pid_t *pids = calloc(num_procs, sizeof(pid_t));
    if (!pids) {
        perror("calloc");
        exit(1);
    }

    printf("Starting %d test processes for %d seconds...\n", num_procs, timeout);

    for (int i = 0; i < num_procs; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            // Child process: assign a weight (heavier every 3rd process)
            int weight = (i % 3 == 0) ? 20 : 10;
            if (set_wfq_weight(weight) < 0) {
                fprintf(stderr, "Child %d: set_wfq_weight(%d) failed: %s\n",
                        getpid(), weight, strerror(errno));
            } else {
                printf("Child %d: weight set to %d\n", getpid(), weight);
            }
            busy_work();
            exit(0);
        } else if (pid > 0) {
            pids[i] = pid;
        } else {
            perror("fork");
            exit(1);
        }
    }

    // Parent: sample scheduler state once per second
    for (int t = 0; t < timeout; t++) {
        struct wfq_info info;
        if (get_wfq_info(&info) < 0) {
            perror("get_wfq_info");
            break;
        }
        printf("[t=%d] CPU distribution:\n", t);
        for (int c = 0; c < info.num_cpus; c++) {
            printf("  CPU %d: nr_running=%d total_weight=%d\n",
                   c, info.nr_running[c], info.total_weight[c]);
        }
        sleep(1);
    }

    // Cleanup children
    for (int i = 0; i < num_procs; i++) {
        kill(pids[i], SIGKILL);
        waitpid(pids[i], NULL, 0);
        printf("Child %d: killed\n", pids[i]);
    }

    free(pids);
    return 0;
}

