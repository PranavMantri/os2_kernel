/* 6118 */
#ifndef _KERNEL_SCHED_WFS_H
#define _KERNEL_SCHED_WFS_H

#include "sched.h"

#define WFS_SCALE_SHIFT    20
#define WFS_SCALE_FACTOR   (1ULL << WFS_SCALE_SHIFT)
#define WFS_DEFAULT_WEIGHT 10

static inline struct task_struct *task_of_wfs(struct sched_wfs_entity *wfs_se)
{
    return container_of(wfs_se, struct task_struct, wfs);
}

static inline struct sched_wfs_entity *wfs_se_of(struct task_struct *p)
{
    return &p->wfs;
}

// Comparison function for RB-tree ordering by VFT
static bool wfs_entity_before(struct rb_node *a, const struct rb_node *b)
{
    struct sched_wfs_entity *se_a = rb_entry(a, struct sched_wfs_entity, run_node);
    struct sched_wfs_entity *se_b = rb_entry(b, struct sched_wfs_entity, run_node);

    return se_a->vft < se_b->vft;
}

static inline u64 calc_delta_fair(u64 delta, struct sched_wfs_entity *se)
{
    return (delta * WFS_SCALE_FACTOR) / se->weight;
}

extern void init_wfs_rq(struct wfs_rq *wfs_rq);

#endif /* _KERNEL_SCHED_WFS_H */
/* 6118 */
