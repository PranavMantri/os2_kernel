/* 6118 */
#include <linux/mmu_context.h>
#include "sched.h"
#include "wfs.h"

static void enqueue_task_wfs(struct rq *rq, struct task_struct *p, int flags)
{
    struct wfs_rq *wfs_rq = &rq->wfs;
    
    /* Only add if not already on queue */
    if (list_empty(&p->wfs.run_list)) {
        list_add_tail(&p->wfs.run_list, &wfs_rq->queue);
        wfs_rq->wfs_nr_running++;
        
        printk(KERN_INFO "WFS: PID %d ENQUEUED (flags=%d), runqueue now has %u tasks\n", 
               p->pid, flags, wfs_rq->wfs_nr_running);
    } else {
        printk(KERN_WARNING "WFS: PID %d already on runqueue, skipping enqueue\n", p->pid);
    }
}

static bool dequeue_task_wfs(struct rq *rq, struct task_struct *p, int flags)
{
    struct wfs_rq *wfs_rq = &rq->wfs;
    
    /* Only remove if actually on queue */
    if (!list_empty(&p->wfs.run_list)) {
        list_del_init(&p->wfs.run_list);
        wfs_rq->wfs_nr_running--;
        
        printk(KERN_INFO "WFS: PID %d DEQUEUED (flags=%d), runqueue now has %u tasks\n", 
               p->pid, flags, wfs_rq->wfs_nr_running);
    } else {
        printk(KERN_WARNING "WFS: PID %d not on runqueue, skipping dequeue\n", p->pid);
    }
    
    return true;
}

static struct task_struct *pick_next_task_wfs(struct rq *rq, struct task_struct *prev)
{
    struct wfs_rq *wfs_rq = &rq->wfs;
    struct sched_wfs_entity *wfs_se;
    struct task_struct *next_task;
    
    if (list_empty(&wfs_rq->queue)) {
        printk(KERN_DEBUG "WFS: pick_next_task - queue empty, returning NULL\n");
        return NULL;
    }
        
    wfs_se = list_first_entry(&wfs_rq->queue, struct sched_wfs_entity, run_list);
    next_task = task_of_wfs(wfs_se);
    
    printk(KERN_INFO "WFS: PICKED next task PID %d (prev was PID %d), %u tasks in queue\n", 
           next_task->pid, prev ? prev->pid : -1, wfs_rq->wfs_nr_running);
    
    return next_task;
}

static void put_prev_task_wfs(struct rq *rq, struct task_struct *p, struct task_struct *next)
{
    printk(KERN_INFO "WFS: PUT_PREV task PID %d (next is PID %d)\n", 
           p->pid, next ? next->pid : -1);
    /*
     * Just clean up - don't do round-robin logic here.
     * Round-robin requeuing happens in task_tick.
     */
}

static void set_next_task_wfs(struct rq *rq, struct task_struct *p, bool first)
{
    struct wfs_rq *wfs_rq = &rq->wfs;
    
    p->wfs.exec_start = rq_clock_task(rq);
    
    printk(KERN_INFO "WFS: SET_NEXT task PID %d (first=%d), %u tasks in queue\n", 
           p->pid, first, wfs_rq->wfs_nr_running);
}

static void task_tick_wfs(struct rq *rq, struct task_struct *p, int queued)
{
    struct wfs_rq *wfs_rq = &rq->wfs;
    
    printk(KERN_INFO "WFS: TASK_TICK PID %d (queued=%d), %u tasks in queue\n", 
           p->pid, queued, wfs_rq->wfs_nr_running);
    
    /* 
     * Simple round-robin: if there are other WFS tasks waiting,
     * requeue current task and trigger reschedule after each tick
     */
    if (wfs_rq->wfs_nr_running > 1) {
        printk(KERN_INFO "WFS: Multiple tasks (%u) - requeuing PID %d and rescheduling\n", 
               wfs_rq->wfs_nr_running, p->pid);
        
        /* Move current task to end of queue */
        list_move_tail(&p->wfs.run_list, &wfs_rq->queue);
        
        /* Trigger a reschedule to pick next task */
        resched_curr(rq);
        
        printk(KERN_INFO "WFS: Task PID %d requeued and reschedule triggered\n", p->pid);
    } else {
        printk(KERN_INFO "WFS: Only 1 task (%u) - no preemption needed for PID %d\n", 
               wfs_rq->wfs_nr_running, p->pid);
    }
}

static void switched_to_wfs(struct rq *rq, struct task_struct *p)
{
    struct wfs_rq *wfs_rq = &rq->wfs;
    
    printk(KERN_INFO "WFS: Task PID %d SWITCHED_TO WFS class, %u tasks in queue\n", 
           p->pid, wfs_rq->wfs_nr_running);
}

static void switched_from_wfs(struct rq *rq, struct task_struct *p)
{
    struct wfs_rq *wfs_rq = &rq->wfs;
    
    printk(KERN_INFO "WFS: Task PID %d SWITCHED_FROM WFS class, %u tasks in queue\n", 
           p->pid, wfs_rq->wfs_nr_running);
    /* Clean up when task leaves WFS */
}

void init_wfs_rq(struct wfs_rq *wfs_rq)
{
    INIT_LIST_HEAD(&wfs_rq->queue);
    wfs_rq->wfs_nr_running = 0;
    
    printk(KERN_INFO "WFS: Runqueue initialized\n");
}

const struct sched_class wfs_sched_class __section("__wfs_sched_class") = {
    .enqueue_task = enqueue_task_wfs,
    .dequeue_task = dequeue_task_wfs,
    .pick_next_task = pick_next_task_wfs,
    .put_prev_task = put_prev_task_wfs,
    .set_next_task = set_next_task_wfs,
    .task_tick = task_tick_wfs,
    .switched_to = switched_to_wfs,
    .switched_from = switched_from_wfs,
};
/* 6118 */