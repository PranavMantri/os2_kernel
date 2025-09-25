/*
 * WFS System Call Definitions
 * File: include/uapi/linux/wfs.h
 */

#ifndef _UAPI_LINUX_WFS_H
#define _UAPI_LINUX_WFS_H

#define MAX_CPUS 8

struct wfs_info {
    int num_cpus;
    int nr_running[MAX_CPUS];
    int total_weight[MAX_CPUS];
};

/* System call numbers - these should match your architecture's syscall table */
#define __NR_get_wfs_info 467
#define __NR_set_wfs_weight 468

#endif /* _UAPI_LINUX_WFS_H */
