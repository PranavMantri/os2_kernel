/* 6118 */
#ifndef _KERNEL_SCHED_WFS_H
#define _KERNEL_SCHED_WFS_H

#include "sched.h"

#define WFS_PRIO_BASE    140    /* Start after normal priority range (100-139) */
#define WFS_DEFAULT_PRIO 140    /* Default WFS priority */
static inline struct task_struct *task_of_wfs(struct sched_wfs_entity *wfs_se)
{
    return container_of(wfs_se, struct task_struct, wfs);
}

static inline struct sched_wfs_entity *wfs_se_of(struct task_struct *p)
{
    return &p->wfs;
}

extern void init_wfs_rq(struct wfs_rq *wfs_rq);

#endif /* _KERNEL_SCHED_WFS_H */
/* 6118 */
