#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sched.h>
#include <string.h>
#include <errno.h>
#include <sys/syscall.h>

#define SCHED_WFS 8

int main() {
    struct sched_param param;
    int current_policy;
    
    printf("Simple WFS Scheduler Test\n");
    printf("=========================\n");
    
    // Show current scheduler
    current_policy = sched_getscheduler(0);
    printf("Current scheduler policy: %d\n", current_policy);
    
    // Test 1: Try glibc wrapper first
    printf("\nTest 1: Using glibc sched_setscheduler()...\n");
    param.sched_priority = 0;
    int result = sched_setscheduler(0, SCHED_WFS, &param);
    
    if (result == 0) {
        printf("SUCCESS: glibc accepted SCHED_WFS\n");
    } else {
        printf("FAILED: glibc rejected SCHED_WFS\n");
        printf("Error: %s (errno=%d)\n", strerror(errno), errno);
    }
    
    // Test 2: Direct syscall bypass
    printf("\nTest 2: Direct syscall (bypassing glibc)...\n");
    param.sched_priority = 0;
    result = syscall(SYS_sched_setscheduler, 0, SCHED_WFS, &param);
    
    if (result == 0) {
        printf("SUCCESS: Kernel accepted SCHED_WFS via direct syscall!\n");
        
        // Verify it was actually set
        current_policy = sched_getscheduler(0);
        printf("New scheduler policy: %d\n", current_policy);
        
        if (current_policy == SCHED_WFS) {
            printf("VERIFIED: Process is now using WFS scheduler\n");
        } else {
            printf("ERROR: Policy shows %d instead of %d\n", current_policy, SCHED_WFS);
        }
        
    } else {
        printf("FAILED: Kernel rejected SCHED_WFS\n");
        printf("Error: %s (errno=%d)\n", strerror(errno), errno);
    }
    
    return 0;
}
