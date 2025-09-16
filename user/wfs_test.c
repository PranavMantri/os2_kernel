#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sched.h>
#include <string.h>
#include <errno.h>
#include <sys/wait.h>
#include <time.h>

#define SCHED_WFS 8

// Define missing scheduler constants if not available
#ifndef SCHED_BATCH
#define SCHED_BATCH 3
#endif
#ifndef SCHED_IDLE
#define SCHED_IDLE 5
#endif
#ifndef SCHED_DEADLINE
#define SCHED_DEADLINE 6
#endif

// Function to print current scheduler policy
const char* get_sched_policy_name(int policy) {
    switch(policy) {
        case SCHED_OTHER: return "SCHED_OTHER";
        case SCHED_FIFO: return "SCHED_FIFO"; 
        case SCHED_RR: return "SCHED_RR";
        case SCHED_BATCH: return "SCHED_BATCH";
        case SCHED_IDLE: return "SCHED_IDLE";
        case SCHED_DEADLINE: return "SCHED_DEADLINE";
        case SCHED_WFS: return "SCHED_WFS";
        default: return "UNKNOWN";
    }
}

void print_current_scheduler() {
    int policy = sched_getscheduler(0);
    struct sched_param param;
    
    if (sched_getparam(0, &param) == 0) {
        printf("PID %d: Policy=%s, Priority=%d\n", 
               getpid(), get_sched_policy_name(policy), param.sched_priority);
    } else {
        perror("sched_getparam failed");
    }
}

void cpu_intensive_work(const char* task_name, int seconds) {
    time_t start_time = time(NULL);
    time_t current_time;
    unsigned long counter = 0;
    
    printf("[%s] Starting CPU-intensive work for %d seconds\n", task_name, seconds);
    print_current_scheduler();
    
    // CPU-intensive loop
    while ((current_time = time(NULL)) - start_time < seconds) {
        // Simulate work
        for (int i = 0; i < 1000000; i++) {
            counter += i * 2;
        }
        
        // Print progress every second
        if ((current_time - start_time) % 1 == 0 && counter % 100000000 == 0) {
            printf("[%s] Working... %ld seconds elapsed, counter=%lu\n", 
                   task_name, current_time - start_time, counter);
        }
    }
    
    printf("[%s] Completed! Final counter: %lu\n", task_name, counter);
}

int test_wfs_scheduler() {
    struct sched_param param;
    int ret;
    
    printf("=== Testing WFS Scheduler ===\n");
    
    // Test 1: Set current process to WFS
    printf("\n1. Setting current process to WFS scheduler\n");
    param.sched_priority = 0;  // WFS should only accept priority 0
    
    ret = sched_setscheduler(0, SCHED_WFS, &param);
    if (ret == 0) {
        printf("✓ Successfully set WFS scheduler\n");
        print_current_scheduler();
    } else {
        printf("✗ Failed to set WFS scheduler: %s\n", strerror(errno));
        return 1;
    }
    
    // Test 2: Try invalid priority (should fail)
    printf("\n2. Testing invalid priority (should fail)\n");
    param.sched_priority = 1;
    ret = sched_setscheduler(0, SCHED_WFS, &param);
    if (ret != 0) {
        printf("✓ Correctly rejected invalid priority: %s\n", strerror(errno));
    } else {
        printf("✗ Incorrectly accepted invalid priority\n");
    }
    
    // Reset to valid priority
    param.sched_priority = 0;
    sched_setscheduler(0, SCHED_WFS, &param);
    
    return 0;
}

int test_round_robin_behavior() {
    pid_t pids[3];
    struct sched_param param;
    int status;
    
    printf("\n=== Testing Round-Robin Behavior ===\n");
    printf("Creating 3 WFS tasks to test round-robin scheduling\n");
    
    param.sched_priority = 0;
    
    // Create 3 child processes
    for (int i = 0; i < 3; i++) {
        pids[i] = fork();
        
        if (pids[i] == 0) {
            // Child process
            char task_name[32];
            snprintf(task_name, sizeof(task_name), "WFS-Task-%d", i+1);
            
            // Set to WFS scheduler
            if (sched_setscheduler(0, SCHED_WFS, &param) != 0) {
                printf("Child %d failed to set WFS scheduler: %s\n", i+1, strerror(errno));
                exit(1);
            }
            
            // Run CPU-intensive work
            cpu_intensive_work(task_name, 10);
            exit(0);
        } else if (pids[i] < 0) {
            perror("fork failed");
            return 1;
        }
    }
    
    // Parent: wait for all children
    printf("Parent waiting for all WFS tasks to complete...\n");
    for (int i = 0; i < 3; i++) {
        waitpid(pids[i], &status, 0);
        printf("WFS-Task-%d completed with status %d\n", i+1, WEXITSTATUS(status));
    }
    
    return 0;
}

int test_scheduler_priority() {
    pid_t pid_normal, pid_wfs;
    struct sched_param param;
    int status;
    
    printf("\n=== Testing Scheduler Priority (WFS vs NORMAL) ===\n");
    
    // Create NORMAL priority task
    pid_normal = fork();
    if (pid_normal == 0) {
        // Keep default SCHED_OTHER
        cpu_intensive_work("NORMAL-Task", 8);
        exit(0);
    }
    
    sleep(1); // Let normal task start first
    
    // Create WFS priority task  
    pid_wfs = fork();
    if (pid_wfs == 0) {
        param.sched_priority = 0;
        if (sched_setscheduler(0, SCHED_WFS, &param) != 0) {
            printf("Failed to set WFS scheduler: %s\n", strerror(errno));
            exit(1);
        }
        cpu_intensive_work("WFS-Task", 8);
        exit(0);
    }
    
    // Wait for both
    waitpid(pid_normal, &status, 0);
    printf("NORMAL task completed\n");
    
    waitpid(pid_wfs, &status, 0);
    printf("WFS task completed\n");
    
    return 0;
}

void print_usage(const char* progname) {
    printf("Usage: %s [test_type]\n", progname);
    printf("test_type:\n");
    printf("  basic    - Basic WFS scheduler functionality test\n");
    printf("  rr       - Round-robin behavior test\n");
    printf("  priority - Priority comparison test\n");
    printf("  all      - Run all tests (default)\n");
}

int main(int argc, char* argv[]) {
    const char* test_type = "all";
    
    if (argc > 1) {
        test_type = argv[1];
    }
    
    printf("WFS Scheduler Test Program\n");
    printf("==========================\n");
    
    // Check if we're running as root (recommended for scheduler changes)
    if (geteuid() != 0) {
        printf("Warning: Not running as root. Some scheduler operations may fail.\n");
        printf("Consider running with: sudo %s\n\n", argv[0]);
    }
    
    int ret = 0;
    
    /*if (strcmp(test_type, "basic") == 0 || strcmp(test_type, "all") == 0) {
        ret |= test_wfs_scheduler();
    }*/
    
    if (strcmp(test_type, "rr") == 0 || strcmp(test_type, "all") == 0) {
        ret |= test_round_robin_behavior();
    }
    
    if (strcmp(test_type, "priority") == 0 || strcmp(test_type, "all") == 0) {
        ret |= test_scheduler_priority();
    }
    
    if (strcmp(test_type, "basic") != 0 && strcmp(test_type, "rr") != 0 && 
        strcmp(test_type, "priority") != 0 && strcmp(test_type, "all") != 0) {
        print_usage(argv[0]);
        return 1;
    }
    
    printf("\n=== Test Summary ===\n");
    if (ret == 0) {
        printf("✓ All tests passed!\n");
    } else {
        printf("✗ Some tests failed. Check output above.\n");
    }
    
    return ret;
}
