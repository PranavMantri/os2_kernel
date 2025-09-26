/*
 * Interactive WFQ test
 * Run: gcc -o test_wfq_interactive test_wfq_interactive.c
 * Usage: sudo ./test_wfq_interactive
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <errno.h>
#include <string.h>
#include <sys/wait.h>

#define MAX_CPUS 8
#define __NR_get_wfq_info 467
#define __NR_set_wfq_weight 468

struct wfq_info {
    int num_cpus;
    int nr_running[MAX_CPUS];
    int total_weight[MAX_CPUS];
};

/* Wrapper functions for syscalls */
int get_wfq_info(struct wfq_info *info) {
    return syscall(__NR_get_wfq_info, info);
}

int set_wfq_weight(int weight) {
    return syscall(__NR_set_wfq_weight, weight);
}

int main() {
    struct wfq_info info;
    int ret, i;
    pid_t child;

    printf("Spawning a child process with high WFQ weight...\n");

    child = fork();
    if (child < 0) {
        perror("fork failed");
        exit(1);
    }

    if (child == 0) {
        // Child process: set high weight and spin forever
        ret = set_wfq_weight(50);
        if (ret < 0) {
            printf("Child: set_wfq_weight(50) failed: %s\n", strerror(errno));
            exit(1);
        } else {
            printf("Child: set_wfq_weight(50) succeeded, entering busy loop.\n");
        }

        // Keep the process alive and consuming CPU
        while (1) {
            // do some dummy work so scheduler notices us
            for (volatile int j = 0; j < 1000000; j++);
        }
    } else {
        // Parent process: interactive loop
        printf("Parent: Press 'g' + Enter to call get_wfq_info, 'q' + Enter to quit.\n");

        char buf[16];
        while (1) {
            if (!fgets(buf, sizeof(buf), stdin))
                break;

            if (buf[0] == 'g') {
                ret = get_wfq_info(&info);
                if (ret < 0) {
                    printf("get_wfq_info failed: %s\n", strerror(errno));
                } else {
                    printf("\n=== WFQ Info ===\n");
                    printf("num_cpus: %d\n", info.num_cpus);
                    for (i = 0; i < info.num_cpus && i < MAX_CPUS; i++) {
                        printf("CPU %d: nr_running=%d, total_weight=%d\n",
                               i, info.nr_running[i], info.total_weight[i]);
                    }
                    printf("================\n\n");
                }
            } else if (buf[0] == 'q') {
                printf("Exiting, killing child %d...\n", child);
                kill(child, SIGKILL);
                waitpid(child, NULL, 0);
                break;
            }
        }
    }

    return 0;
}

