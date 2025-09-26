#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sched.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <errno.h>
#include <signal.h>
#include <getopt.h>
#include <math.h>
#include <time.h>
#include <fcntl.h>

#define __NR_get_wfq_info 467
#define __NR_set_wfq_weight 468
#define SCHED_WFS 8
#define MAX_CPUS 8

pid_t *pids;
int num_procs;

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

void busy_cpu(void) {
    volatile unsigned long long x = 0;
    while (1) {
        x += 1;
        if (x % 1000000000ULL == 0) {
            asm volatile("" ::: "memory"); // prevent compiler optimizations
        }
    }
}

void busy_io(void) {
    char buf[4096];
    memset(buf, 'A', sizeof(buf));
    while (1) {
        int fd = open("/tmp/busyio.tmp", O_RDWR | O_CREAT, 0644);
        if (fd < 0) {
            perror("open");
            continue;
        }
        ssize_t ret = write(fd, buf, sizeof(buf));
	(void)ret;	
        lseek(fd, 0, SEEK_SET);
        ret = read(fd, buf, sizeof(buf));
	(void)ret;	
        close(fd);
    }
}

int spawn_children(int num_children, int use_rand, int use_cpu) {

    printf("Starting %d test processes...\n", num_children);

    for (int i = 0; i < num_children; i++) {
        int weight = use_rand ? ((rand() % 10) + 1) : 5;

        pid_t pid = fork();
	
        if (pid == 0) {
	    
            if (set_wfq_weight(weight) < 0) {
                fprintf(stderr, "Child %d: set_wfq_weight(%d) failed: %s\n",
                        getpid(), weight, strerror(errno));
            }
            if (use_cpu) {
                busy_cpu();
            } else {
                busy_io();
            }
            exit(0);
        } else if (pid > 0) {
            pids[i] = pid;
        } else {
            perror("fork");
            exit(1);
        }
    }

    return 0;
}


int monitor_and_print_stats(int timeout, int verbose) {

    // Print header
    if (!verbose) {
        printf("\nTime  L2_Distance\n");
        printf("------------------\n");
    }

    for (int t = 0; t < timeout; t++) {
        struct wfq_info info;
        if (get_wfq_info(&info) < 0) {
            perror("get_wfq_info");
            break;
        }

        int total = 0;
        for (int c = 0; c < info.num_cpus; c++) {
            total += info.total_weight[c];
        }
        double ideal = (info.num_cpus > 0) ? ((double) total / info.num_cpus) : 0.0;

        double dist_sq = 0.0;
        for (int c = 0; c < info.num_cpus; c++) {
            double diff = (double)info.total_weight[c] - ideal;
            dist_sq += diff * diff;
        }
        double l2 = sqrt(dist_sq);

        if (verbose) {
            printf("Time %d sec:\n", t);
            printf("  L2 distance: %.2f\n", l2);
            printf("  CPU weights: ");
            for (int c = 0; c < info.num_cpus; c++) {
                printf("%d ", info.total_weight[c]);
            }
	    printf("\n");
            fflush(stdout);
        } else {
            printf("%4d  %.2f\n", t, l2);
            fflush(stdout);
        }

        sleep(1);
    }

    printf("Killing children...\n");

    for (int i = 0; i < num_procs; i++) {
        kill(pids[i], SIGKILL);
        waitpid(pids[i], NULL, 0);
    }

    printf("Done killing...\n");

    return 0;
}

int main(int argc, char *argv[]) {
    num_procs = 16;   // default
    int timeout   = 10;   // default seconds
    int use_rand  = 0;    // default no rand
    int verbose   = 1;    // default verbose
    int test   = 2;    // default both

    int opt;
    while ((opt = getopt(argc, argv, "n:t:rvci")) != -1) {
        switch (opt) {
        case 'n':
            num_procs = atoi(optarg);
            break;
        case 't':
            timeout = atoi(optarg);
            break;
        case 'r':
            use_rand = 1;
            break;
        case 'q':
            verbose = 0;
            break;
        case 'c':
            test = 1;
            break;
        case 'i':
            test = 0;
            break;
        default:
            fprintf(stderr, "Usage: %s [-n num_procs] [-t seconds] [-r] [-v] [-c|-i]\n", argv[0]);
            exit(EXIT_FAILURE);
        }
    }

    // Elevate parent to RT scheduling

    pids = calloc(num_procs, sizeof(pid_t));
    if (!pids) {
        perror("calloc");
        exit(1);
    }

    srand(time(NULL));

    if (test == 2) {
	    printf("Executing CPU stress test\n");
	    spawn_children(num_procs, use_rand, 1);
	    monitor_and_print_stats(timeout, verbose);
	    printf("Executing IO stress test\n");
	    spawn_children(num_procs, use_rand, 0);
	    monitor_and_print_stats(timeout, verbose);
    } else if (test == 1) {
	    printf("Executing CPU stress test\n");
	    spawn_children(num_procs, use_rand, 1);
	    monitor_and_print_stats(timeout, verbose);
    } else {
	    printf("Executing IO stress test\n");
	    spawn_children(num_procs, use_rand, 0);
	    monitor_and_print_stats(timeout, verbose);
    }


    free(pids);
    return 0;
}

