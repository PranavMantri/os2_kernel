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
#include <math.h>
#include <time.h>

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

    srand(time(NULL));
    printf("Starting %d test processes for %d seconds...\n", num_procs, timeout);

    for (int i = 0; i < num_procs; i++) {
        int weight = (rand() % 50) + 1;
        pid_t pid = fork();
        if (pid == 0) {
            // Child process: assign a weight (heavier every 3rd process)
            if (set_wfq_weight(weight) < 0) {
                fprintf(stderr, "Child %d: set_wfq_weight(%d) failed: %s\n",
                        getpid(), weight, strerror(errno));
            }
	    fprintf(stderr, "Child %d: set_wfq_weight(%d)\n",getpid(), weight);
            busy_work();
            exit(0);
        } else if (pid > 0) {
            pids[i] = pid;
        } else {
            perror("fork");
            exit(1);
        }
    }

    // Store samples for statistics
    int samples = 0;
    double cpu_weight_sums[MAX_CPUS] = {0};
    double cpu_weight_sq_sums[MAX_CPUS] = {0};
    double imbalance_sum = 0.0;

    for (int t = 0; t < timeout; t++) {
        struct wfq_info info;
        if (get_wfq_info(&info) < 0) {
            perror("get_wfq_info");
            break;
        }

        int min_w = info.total_weight[0];
        int max_w = info.total_weight[0];

        for (int c = 0; c < info.num_cpus; c++) {
            int w = info.total_weight[c];
            cpu_weight_sums[c] += w;
            cpu_weight_sq_sums[c] += (double)w * (double)w;
            if (w < min_w) min_w = w;
            if (w > max_w) max_w = w;
        }

        imbalance_sum += (max_w - min_w);
        samples++;
        sleep(1);
    }

    printf("Killing children...\n");

    // Cleanup children
    for (int i = 0; i < num_procs; i++) {
        kill(pids[i], SIGKILL);
        waitpid(pids[i], NULL, 0);
    }

    // Print summary
    if (samples > 0) {
        printf("\n=== WFQ Scheduler Balance Analysis ===\n");
        printf("Collected %d samples over %d seconds\n\n", samples, timeout);

        printf("CPU   AvgWeight   StdevWeight\n");
        printf("------------------------------\n");
        for (int c = 0; c < MAX_CPUS; c++) {
            if (cpu_weight_sums[c] > 0) {
                double mean = cpu_weight_sums[c] / samples;
                double mean_sq = cpu_weight_sq_sums[c] / samples;
                double variance = mean_sq - (mean * mean);
                if (variance < 0) variance = 0; // numerical safety
                double stdev = sqrt(variance);
                printf("%2d    %.2f       %.2f\n", c, mean, stdev);
            }
        }

        printf("\nAverage per-sample imbalance (max-min): %.2f\n",
               imbalance_sum / samples);
    }

    free(pids);
    return 0;
}

