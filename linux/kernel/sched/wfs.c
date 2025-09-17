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

    /* Check if we have any WFS tasks */
    if (!wfs_rq->wfs_nr_running) {
        return NULL;
    }

    if (list_empty(&wfs_rq->queue)) {
        /* This shouldn't happen if wfs_nr_running > 0, but be safe */
        printk(KERN_WARNING "WFS: Queue empty but wfs_nr_running=%u, fixing\n",
               wfs_rq->wfs_nr_running);
        wfs_rq->wfs_nr_running = 0;
        return NULL;
    }

    wfs_se = list_first_entry(&wfs_rq->queue, struct sched_wfs_entity, run_list);
    next_task = task_of_wfs(wfs_se);

    printk(KERN_DEBUG "WFS: PICKED next task PID %d (prev was PID %d), %u tasks in queue\n",
           next_task->pid, prev ? prev->pid : -1, wfs_rq->wfs_nr_running);

    return next_task;
}

static void put_prev_task_wfs(struct rq *rq, struct task_struct *p, struct task_struct *next)
{
    printk(KERN_DEBUG "WFS: PUT_PREV task PID %d (next is PID %d)\n", 
           p->pid, next ? next->pid : -1);
    
    /* Update execution time tracking */
    if (p->wfs.exec_start) {
        u64 delta_exec = rq_clock_task(rq) - p->wfs.exec_start;
        p->se.sum_exec_runtime += delta_exec;
        p->wfs.exec_start = 0;
    }
}

static void set_next_task_wfs(struct rq *rq, struct task_struct *p, bool first)
{
    struct wfs_rq *wfs_rq = &rq->wfs;
    
    p->wfs.exec_start = rq_clock_task(rq);
    
    printk(KERN_DEBUG "WFS: SET_NEXT task PID %d (first=%d), %u tasks in queue\n", 
           p->pid, first, wfs_rq->wfs_nr_running);
}

static void task_tick_wfs(struct rq *rq, struct task_struct *p, int queued)
{
    struct wfs_rq *wfs_rq = &rq->wfs;
    
    printk(KERN_DEBUG "WFS: TASK_TICK PID %d (queued=%d), %u tasks in queue\n", 
           p->pid, queued, wfs_rq->wfs_nr_running);
    
    /* Update runtime stats */
    if (p->wfs.exec_start) {
        u64 delta_exec = rq_clock_task(rq) - p->wfs.exec_start;
        p->se.sum_exec_runtime += delta_exec;
        p->wfs.exec_start = rq_clock_task(rq);
    }
    
    /* 
     * Round-robin: each task runs for exactly 1 tick
     * If there are other WFS tasks waiting, preempt after every tick
     */
    if (wfs_rq->wfs_nr_running > 1) {
        printk(KERN_DEBUG "WFS: Multiple tasks (%u) - preempting PID %d after 1 tick\n", 
               wfs_rq->wfs_nr_running, p->pid);
        
        /* Move current task to end of queue */
        list_move_tail(&p->wfs.run_list, &wfs_rq->queue);
        
        /* Trigger a reschedule to pick next task */
        resched_curr(rq);
        
        printk(KERN_DEBUG "WFS: Task PID %d preempted and moved to end of queue\n", p->pid);
    } else {
        printk(KERN_DEBUG "WFS: Only 1 task (%u) - no preemption needed for PID %d\n", 
               wfs_rq->wfs_nr_running, p->pid);
    }
}

static void switched_to_wfs(struct rq *rq, struct task_struct *p)
{
    struct wfs_rq *wfs_rq = &rq->wfs;
    
    printk(KERN_INFO "WFS: Task PID %d SWITCHED_TO WFS class, %u tasks in queue\n", 
           p->pid, wfs_rq->wfs_nr_running);
    
    /* If this task should preempt current task */
    if (rq->curr != p && rq->curr->sched_class == &wfs_sched_class)
        resched_curr(rq);
}

static void switched_from_wfs(struct rq *rq, struct task_struct *p)
{
    struct wfs_rq *wfs_rq = &rq->wfs;
    
    printk(KERN_INFO "WFS: Task PID %d SWITCHED_FROM WFS class, %u tasks in queue\n", 
           p->pid, wfs_rq->wfs_nr_running);
    
    /* Clean up when task leaves WFS */
    if (p->wfs.exec_start) {
        u64 delta_exec = rq_clock_task(rq) - p->wfs.exec_start;
        p->se.sum_exec_runtime += delta_exec;
        p->wfs.exec_start = 0;
    }
}

static void check_preempt_curr_wfs(struct rq *rq, struct task_struct *p, int flags)
{
    /* For now, WFS is non-preemptive except for round-robin in task_tick */
    /* Could add preemption logic here if needed */
    printk(KERN_DEBUG "WFS: check_preempt_curr called for PID %d (flags=%d)\n", 
           p->pid, flags);
}

static void wakeup_preempt_wfs(struct rq *rq, struct task_struct *p, int flags)
{
    /* 
     * Called when a task wakes up to determine if it should preempt current task.
     * For WFS round-robin, we don't do immediate preemption on wakeup.
     * Tasks will be scheduled in round-robin order via task_tick.
     */
    printk(KERN_DEBUG "WFS: wakeup_preempt called for PID %d (flags=%d)\n", 
           p->pid, flags);
}

static void update_curr_wfs(struct rq *rq)
{
    struct task_struct *curr = rq->curr;
    
    if (curr->sched_class != &wfs_sched_class)
        return;
        
    if (curr->wfs.exec_start) {
        u64 delta_exec = rq_clock_task(rq) - curr->wfs.exec_start;
        curr->se.sum_exec_runtime += delta_exec;
        curr->wfs.exec_start = rq_clock_task(rq);
    }
}

void init_wfs_rq(struct wfs_rq *wfs_rq)
{
    INIT_LIST_HEAD(&wfs_rq->queue);
    wfs_rq->wfs_nr_running = 0;
    
    printk(KERN_INFO "WFS: Runqueue initialized\n");
}

/* Most minimal SMP hooks for wfs scheduler */

static int select_task_rq_wfs(struct task_struct *p, int cpu, int flags)
{
    /* Naive: just return current CPU */
    return smp_processor_id();
}

static int balance_wfs(struct rq *rq, struct task_struct *prev, struct rq_flags *rf)
{
    /* No-op balancing - just return 0 (no tasks pulled) */
    return 0;
}

static void migrate_task_rq_wfs(struct task_struct *p, int new_cpu)
{
    /* No-op - nothing to do when task migrates */
}

static void rq_online_wfs(struct rq *rq)
{
    /* No-op - nothing to do when CPU comes online */
}

static void rq_offline_wfs(struct rq *rq)
{
    /* No-op - nothing to do when CPU goes offline */
}

static void task_woken_wfs(struct rq *rq, struct task_struct *p)
{
    /* No-op - nothing to do after remote wakeup */
}

static void set_cpus_allowed_wfs(struct task_struct *p, struct affinity_context *ctx)
{
    /* No-op - don't handle CPU affinity changes */
}

static bool yield_to_task_wfs(struct rq *rq, struct task_struct *p)
{
    /* Return false - don't handle yield_to */
    return false;
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
    .wakeup_preempt = wakeup_preempt_wfs,
    .update_curr = update_curr_wfs,
    .yield_to_task = yield_to_task_wfs,

#ifdef CONFIG_SMP
    .balance = balance_wfs,
    .select_task_rq = select_task_rq_wfs,
    .migrate_task_rq = migrate_task_rq_wfs,
    .rq_online = rq_online_wfs,
    .rq_offline = rq_offline_wfs,
    .task_woken = task_woken_wfs,
    .set_cpus_allowed = set_cpus_allowed_wfs,
#endif
};
/* 6118 */
