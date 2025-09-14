/* 6118 */
#include <linux/mmu_context.h>
#include "sched.h"
#include "wfs.h"

static void enqueue_task_wfs(struct rq *rq, struct task_struct *p, int flags)
{
    struct wfs_rq *wfs_rq = &rq->wfs;
    
    list_add_tail(&p->wfs.run_list, &wfs_rq->queue);
    wfs_rq->wfs_nr_running++;
    p->wfs.exec_start = rq_clock_task(rq);
    
    printk_once(KERN_INFO "WFS: First task enqueued\n");
}

static bool dequeue_task_wfs(struct rq *rq, struct task_struct *p, int flags)
{
    struct wfs_rq *wfs_rq = &rq->wfs;
    
    list_del(&p->wfs.run_list);
    wfs_rq->wfs_nr_running--;
    
    return true;  /* Return true indicating task was dequeued */
}

static struct task_struct *pick_next_task_wfs(struct rq *rq, struct task_struct *prev)
{
    struct wfs_rq *wfs_rq = &rq->wfs;
    struct sched_wfs_entity *wfs_se;
    
    if (list_empty(&wfs_rq->queue))
        return NULL;
        
    wfs_se = list_first_entry(&wfs_rq->queue, struct sched_wfs_entity, run_list);
    return task_of_wfs(wfs_se);
}

static void put_prev_task_wfs(struct rq *rq, struct task_struct *p, struct task_struct *next)
{
    struct wfs_rq *wfs_rq = &rq->wfs;
    
    /* Move task to end of queue for round-robin */
    list_move_tail(&p->wfs.run_list, &wfs_rq->queue);
}

static void set_next_task_wfs(struct rq *rq, struct task_struct *p, bool first)
{
    p->wfs.exec_start = rq_clock_task(rq);
}

static void task_tick_wfs(struct rq *rq, struct task_struct *p, int queued)
{
    /* Basic time slice handling - placeholder */
}

static void switched_to_wfs(struct rq *rq, struct task_struct *p)
{
    printk_ratelimited(KERN_INFO "WFS: Task %d switched to WFS class\n", p->pid);
}

void init_wfs_rq(struct wfs_rq *wfs_rq)
{
    INIT_LIST_HEAD(&wfs_rq->queue);
    wfs_rq->wfs_nr_running = 0;
}

const struct sched_class wfs_sched_class __section("__wfs_sched_class") = {
    .enqueue_task = enqueue_task_wfs,
    .dequeue_task = dequeue_task_wfs,
    .pick_next_task = pick_next_task_wfs,
    .put_prev_task = put_prev_task_wfs,
    .set_next_task = set_next_task_wfs,
    .task_tick = task_tick_wfs,
    .switched_to = switched_to_wfs,
};
/* 6118 */
