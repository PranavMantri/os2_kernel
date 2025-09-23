/*
 * Test program for WFS system calls
 * File: test_wfs_syscalls.c
 * 
 * Compile with: gcc -o test_wfs_syscalls test_wfs_syscalls.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <errno.h>
#include <string.h>

#define MAX_CPUS 8
#define __NR_get_wfs_info 467
#define __NR_set_wfs_weight 468

struct wfs_info {
    int num_cpus;
    int nr_running[MAX_CPUS];
    int total_weight[MAX_CPUS];
};

/* Wrapper functions for our system calls */
int get_wfs_info(struct wfs_info *info) {
    return syscall(__NR_get_wfs_info, info);
}

int set_wfs_weight(int weight) {
    return syscall(__NR_set_wfs_weight, weight);
}

int main() {
    struct wfs_info info;
    int ret, i;
    
    printf("Testing WFS system calls...\n\n");
    
    /* Test get_wfs_info */
    printf("=== Testing get_wfs_info ===\n");
    ret = get_wfs_info(&info);
    if (ret < 0) {
        printf("get_wfs_info failed: %s\n", strerror(errno));
    } else {
        printf("get_wfs_info succeeded, returned %d CPUs\n", ret);
        printf("num_cpus: %d\n", info.num_cpus);
        for (i = 0; i < info.num_cpus && i < MAX_CPUS; i++) {
            printf("CPU %d: nr_running=%d, total_weight=%d\n", 
                   i, info.nr_running[i], info.total_weight[i]);
        }
    }
    
    printf("\n=== Testing set_wfs_weight ===\n");
    
    /* Test set_wfs_weight with valid weight */
    printf("Setting weight to 5...\n");
    ret = set_wfs_weight(5);
    if (ret < 0) {
        printf("set_wfs_weight(5) failed: %s\n", strerror(errno));
    } else {
        printf("set_wfs_weight(5) succeeded\n");
    }
    
    /* Test set_wfs_weight with invalid weight */
    printf("Setting weight to 0 (should fail)...\n");
    ret = set_wfs_weight(0);
    if (ret < 0) {
        printf("set_wfs_weight(0) failed as expected: %s\n", strerror(errno));
    } else {
        printf("set_wfs_weight(0) unexpectedly succeeded\n");
    }
    
    /* Test set_wfs_weight with high weight (might fail if not root) */
    printf("Setting weight to 15 (might fail if not root)...\n");
    ret = set_wfs_weight(15);
    if (ret < 0) {
        printf("set_wfs_weight(15) failed: %s\n", strerror(errno));
    } else {
        printf("set_wfs_weight(15) succeeded\n");
    }
    
    printf("\nTest completed.\n");
    return 0;
}
