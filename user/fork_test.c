#define _GNU_SOURCE
#include <sched.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

#define WFS_SCHED 8   // Custom scheduling policy

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <command> [args...]\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    struct sched_param param;
    param.sched_priority = 0;  // Custom scheduler may ignore this

    // Set scheduler of current process
    if (sched_setscheduler(0, WFS_SCHED, &param) == -1) {
        fprintf(stderr, "sched_setscheduler failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(EXIT_FAILURE);
    }

    if (pid == 0) {
        // Child process
        int policy = sched_getscheduler(0);
        if (policy == -1) {
            fprintf(stderr, "sched_getscheduler failed: %s\n", strerror(errno));
        } else {
            printf("[child %d] scheduler policy = %d\n", getpid(), policy);
        }

        // Replace process image with provided command
        execvp(argv[1], &argv[1]);

        // If execvp returns, it's an error
        fprintf(stderr, "execvp failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    } else {
        // Parent waits for child
	printf("Child PID: %d\n", pid);
        int status;
        if (waitpid(pid, &status, 0) == -1) {
            perror("waitpid");
            exit(EXIT_FAILURE);
        }
        if (WIFEXITED(status)) {
            printf("Child exited with status %d\n", WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            printf("Child killed by signal %d\n", WTERMSIG(status));
        }
    }

    return 0;
}
