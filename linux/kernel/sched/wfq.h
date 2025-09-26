/* 6118 */
#ifndef _KERNEL_SCHED_WFQ_H
#define _KERNEL_SCHED_WFQ_H

#include "sched.h"

#define WFQ_SCALE_SHIFT    20
#define WFQ_SCALE_FACTOR   (1ULL << WFQ_SCALE_SHIFT)
#define WFQ_DEFAULT_WEIGHT 10

static inline struct task_struct *task_of_wfq(struct sched_wfq_entity *wfq_se)
{
    return container_of(wfq_se, struct task_struct, wfq);
}

static inline struct sched_wfq_entity *wfq_se_of(struct task_struct *p)
{
    return &p->wfq;
}

// Comparison function for RB-tree ordering by VFT
static inline bool wfq_entity_before(struct rb_node *a, const struct rb_node *b)
{
    struct sched_wfq_entity *se_a = rb_entry(a, struct sched_wfq_entity, run_node);
    struct sched_wfq_entity *se_b = rb_entry(b, struct sched_wfq_entity, run_node);

    return se_a->vft < se_b->vft;
}

static inline u64 calc_delta_fair(u64 delta, struct sched_wfq_entity *se)
{
    return (delta * WFQ_SCALE_FACTOR) / se->weight;
}
extern void update_curr_wfq(struct rq *rq);
extern void init_wfq_rq(struct wfq_rq *wfq_rq);

#endif /* _KERNEL_SCHED_WFQ_H */
/* 6118 */
