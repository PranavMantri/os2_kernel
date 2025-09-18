/* 6118 */
#include <linux/mmu_context.h>
#include "sched.h"
#include "wfs.h"

/* Update virtual runtime and VFT for a task */
static void update_vruntime(struct sched_wfs_entity *se, u64 delta_exec)
{
    u64 vdelta = calc_delta_fair(delta_exec, se);
    se->vruntime += vdelta;
    
    /* Calculate VFT = vruntime + (1 tick scaled) / weight */
    u64 tick_ns = TICK_NSEC; /* Duration of 1 tick in nanoseconds */
    u64 scaled_tick = calc_delta_fair(tick_ns, se);
    se->vft = se->vruntime + scaled_tick;
}


/* Initialize virtual time for new tasks */
static void place_entity(struct wfs_rq *wfs_rq, struct sched_wfs_entity *se)
{
    /* New tasks start at current min_vruntime to prevent starvation */
    se->vruntime = wfs_rq->min_vruntime;
    
    /* Initialize weight if not set */
    if (!se->weight) {
        se->weight = WFS_DEFAULT_WEIGHT;
        se->inv_weight = WFS_SCALE_FACTOR / se->weight;
    }
    
    /* Calculate initial VFT */
    u64 tick_ns = TICK_NSEC;
    u64 scaled_tick = calc_delta_fair(tick_ns, se);
    se->vft = se->vruntime + scaled_tick;
}

/* Update min_vruntime for the runqueue */
static void update_min_vruntime(struct wfs_rq *wfs_rq)
{
    struct rb_node *leftmost = rb_first_cached(&wfs_rq->tasks_timeline);
    u64 vruntime = wfs_rq->min_vruntime;

    if (leftmost) {
        struct sched_wfs_entity *se = rb_entry(leftmost, struct sched_wfs_entity, run_node);
        vruntime = se->vruntime;
    }

    /* min_vruntime should never go backwards */
    wfs_rq->min_vruntime = max(wfs_rq->min_vruntime, vruntime);
}

/* Main scheduler functions */

static void enqueue_task_wfs(struct rq *rq, struct task_struct *p, int flags)
{
    struct wfs_rq *wfs_rq = &rq->wfs;
    struct sched_wfs_entity *se = &p->wfs;

    /* Only add if not already on RB-tree */
    if (RB_EMPTY_NODE(&se->run_node)) {
        /* Initialize virtual time for new or waking tasks */
        if (!(flags & ENQUEUE_WAKEUP) || se->vruntime == 0) {
            place_entity(wfs_rq, se);
        } else {
            /* Waking task - recalculate VFT based on current vruntime */
            u64 tick_ns = TICK_NSEC;
            u64 scaled_tick = calc_delta_fair(tick_ns, se);
            se->vft = se->vruntime + scaled_tick;
        }

        /* Insert into RB-tree ordered by VFT */
        rb_add_cached(&se->run_node, &wfs_rq->tasks_timeline, wfs_entity_before);

        wfs_rq->wfs_nr_running++;

        /* Also add to list for compatibility */
        if (list_empty(&se->run_list)) {
            list_add_tail(&se->run_list, &wfs_rq->queue);
        }

        update_min_vruntime(wfs_rq);

        printk(KERN_INFO "WFS: PID %d ENQUEUED (flags=%d), vruntime=%llu, VFT=%llu, runqueue now has %u tasks\n",
               p->pid, flags, se->vruntime, se->vft, wfs_rq->wfs_nr_running);
    } else {
        printk(KERN_WARNING "WFS: PID %d already on runqueue, skipping enqueue\n", p->pid);
    }
}

static bool dequeue_task_wfs(struct rq *rq, struct task_struct *p, int flags)
{
    struct wfs_rq *wfs_rq = &rq->wfs;
    struct sched_wfs_entity *se = &p->wfs;

    /* Only remove if actually in RB-tree */
    if (!RB_EMPTY_NODE(&se->run_node)) {
        rb_erase_cached(&se->run_node, &wfs_rq->tasks_timeline);
        RB_CLEAR_NODE(&se->run_node);

        wfs_rq->wfs_nr_running--;

        /* Also remove from list */
        if (!list_empty(&se->run_list)) {
            list_del_init(&se->run_list);
        }

        update_min_vruntime(wfs_rq);

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
    struct rb_node *leftmost;
    
    /* Check if we have any WFS tasks */
    if (!wfs_rq->wfs_nr_running) {
        return NULL;
    }
    
    /* Get the leftmost node (minimum VFT) from RB-tree */
    leftmost = rb_first_cached(&wfs_rq->tasks_timeline);
    if (!leftmost) {
        /* This shouldn't happen if wfs_nr_running > 0, but be safe */
        printk(KERN_WARNING "WFS: RB-tree empty but wfs_nr_running=%u, fixing\n",
               wfs_rq->wfs_nr_running);
        wfs_rq->wfs_nr_running = 0;
        return NULL;
    }
    
    wfs_se = rb_entry(leftmost, struct sched_wfs_entity, run_node);
    next_task = task_of_wfs(wfs_se);
    
    printk(KERN_DEBUG "WFS: PICKED next task PID %d (prev was PID %d), VFT=%llu, %u tasks in queue\n",
           next_task->pid, prev ? prev->pid : -1, wfs_se->vft, wfs_rq->wfs_nr_running);
    
    return next_task;
}

static void put_prev_task_wfs(struct rq *rq, struct task_struct *p, struct task_struct *next)
{
    struct sched_wfs_entity *se = &p->wfs;
    u64 now = rq_clock_task(rq);
    
    printk(KERN_DEBUG "WFS: PUT_PREV task PID %d (next is PID %d)\n",
           p->pid, next ? next->pid : -1);
    
    /* Update execution time and virtual runtime */
    if (se->exec_start) {
        u64 delta_exec = now - se->exec_start;
        
        /* Update total runtime */
        p->se.sum_exec_runtime += delta_exec;
        se->sum_exec_runtime += delta_exec;
        
        /* Update virtual runtime and recalculate VFT */
        update_vruntime(se, delta_exec);
        
        se->exec_start = 0;
        
        /* If task is still runnable, we need to reposition it in RB-tree */
        if (next != p && !RB_EMPTY_NODE(&se->run_node)) {
            struct wfs_rq *wfs_rq = &rq->wfs;
            
            /* Remove from current position */
            rb_erase_cached(&se->run_node, &wfs_rq->tasks_timeline);
            
            /* Re-insert at new position based on updated VFT */
            rb_add_cached(&se->run_node, &wfs_rq->tasks_timeline, wfs_entity_before);
            
            update_min_vruntime(wfs_rq);
            
            printk(KERN_DEBUG "WFS: Task PID %d repositioned in RB-tree, new VFT=%llu\n",
                   p->pid, se->vft);
        }
    }
}

static void set_next_task_wfs(struct rq *rq, struct task_struct *p, bool first)
{
    struct wfs_rq *wfs_rq = &rq->wfs;
    
    p->wfs.exec_start = rq_clock_task(rq);
    
    printk(KERN_DEBUG "WFS: SET_NEXT task PID %d (first=%d), %u tasks in queue\n", 
           p->pid, first, wfs_rq->wfs_nr_running);
}

static void update_curr_wfs(struct rq *rq)
{
    struct task_struct *curr = rq->curr;
    struct sched_wfs_entity *se;
    u64 now = rq_clock_task(rq);

    if (curr->sched_class != &wfs_sched_class)
        return;

    se = &curr->wfs;

    if (se->exec_start) {
        u64 delta_exec = now - se->exec_start;

        /* Update total runtime */
        curr->se.sum_exec_runtime += delta_exec;
        se->sum_exec_runtime += delta_exec;

        /* Update virtual runtime continuously */
        update_vruntime(se, delta_exec);

        se->exec_start = now;
    }
}

static void task_tick_wfs(struct rq *rq, struct task_struct *p, int queued)
{
    struct wfs_rq *wfs_rq = &rq->wfs;
    struct sched_wfs_entity *se = &p->wfs;

    printk(KERN_DEBUG "WFS: TASK_TICK PID %d (queued=%d), VFT=%llu, %u tasks in queue\n",
           p->pid, queued, se->vft, wfs_rq->wfs_nr_running);

    /* Update runtime stats first */
    update_curr_wfs(rq);

    /*
     * WFS: each task runs for exactly 1 tick quantum
     * Always preempt after 1 tick if there are other tasks
     */
    if (wfs_rq->wfs_nr_running > 1) {
        printk(KERN_DEBUG "WFS: Multiple tasks (%u) - preempting PID %d after 1 tick, VFT=%llu\n",
               wfs_rq->wfs_nr_running, p->pid, se->vft);

        /* Trigger a reschedule - put_prev_task will handle repositioning */
        resched_curr(rq);

        printk(KERN_DEBUG "WFS: Task PID %d preempted, will be repositioned based on updated VFT\n", p->pid);
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


void init_wfs_rq(struct wfs_rq *wfs_rq)
{
    INIT_LIST_HEAD(&wfs_rq->queue);
    wfs_rq->tasks_timeline = RB_ROOT_CACHED;
    wfs_rq->wfs_nr_running = 0;
    wfs_rq->min_vruntime = 0;
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
