/* 6118 */
#include <linux/mmu_context.h>
#include "sched.h"
#include "wfs.h"

/* No separate per-CPU structure needed - using wfs_rq instead */

/* Find the CPU with minimum total weight */
static int find_min_weight_cpu(struct task_struct *p)
{
    int cpu, best_cpu = -1;
    u64 min_weight = ULLONG_MAX;
    const struct cpumask *allowed_mask = p->cpus_ptr;
    
    for_each_cpu(cpu, allowed_mask) {
        struct rq *cpu_rq = cpu_rq(cpu);
        struct wfs_rq *wfs_rq = &cpu_rq->wfs;
        
        /* Skip offline CPUs */
        if (!cpu_online(cpu))
            continue;
            
        /* Find CPU with minimum weight, breaking ties with lowest CPU number */
        if (wfs_rq->cpu_total_weight < min_weight) {
            min_weight = wfs_rq->cpu_total_weight;
            best_cpu = cpu;
        }
    }
    
    /* Fallback to current CPU if no suitable CPU found */
    if (best_cpu == -1)
        best_cpu = smp_processor_id();
        
    return best_cpu;
}

/* Add task weight to CPU's total weight */
static void add_task_to_cpu_weight(struct wfs_rq *wfs_rq, u64 weight)
{
    wfs_rq->cpu_total_weight += weight;
    
    printk(KERN_DEBUG "WFS: Added weight %u to CPU, total weight now %llu\n",
           weight, wfs_rq->cpu_total_weight);
}

/* Remove task weight from CPU's total weight */
static void remove_task_from_cpu_weight(struct wfs_rq *wfs_rq, u64 weight)
{
    if (wfs_rq->cpu_total_weight >= weight) {
        wfs_rq->cpu_total_weight -= weight;
    } else {
        printk(KERN_WARNING "WFS: CPU weight underflow, resetting to 0\n");
        wfs_rq->cpu_total_weight = 0;
    }
    
    printk(KERN_DEBUG "WFS: Removed weight %u from CPU, total weight now %llu\n",
           weight, wfs_rq->cpu_total_weight);
}

/* Update CPU virtual time based on execution time */
static void update_cpu_vtime(struct wfs_rq *wfs_rq, u64 delta_exec)
{
    if (wfs_rq->cpu_total_weight > 0) {
        u64 vtime_delta = (delta_exec * WFS_SCALE_FACTOR) / wfs_rq->cpu_total_weight;
        wfs_rq->cpu_vtime += vtime_delta;
        
        printk(KERN_DEBUG "WFS: CPU vtime updated by %llu, now %llu (weight=%llu)\n",
               vtime_delta, wfs_rq->cpu_vtime, wfs_rq->cpu_total_weight);
    }
}

/* Get current CPU virtual time */
static u64 get_cpu_vtime(struct wfs_rq *wfs_rq)
{
    return wfs_rq->cpu_vtime;
}

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
static void place_entity(struct wfs_rq *wfs_rq, struct sched_wfs_entity *se, int cpu)
{
    /* Initialize weight if not set */
    if (!se->weight) {
        se->weight = WFS_DEFAULT_WEIGHT;
        se->inv_weight = WFS_SCALE_FACTOR / se->weight;
    }
    
    /* Set task's virtual time to current CPU virtual time */
    se->vruntime = get_cpu_vtime(wfs_rq);
    
    /* If this is greater than min_vruntime, use min_vruntime to prevent starvation */
    if (se->vruntime < wfs_rq->min_vruntime) {
        se->vruntime = wfs_rq->min_vruntime;
    }
    
    /* Calculate initial VFT */
    u64 tick_ns = TICK_NSEC;
    u64 scaled_tick = calc_delta_fair(tick_ns, se);
    se->vft = se->vruntime + scaled_tick;
    
    /* Record which CPU this task is assigned to */
    se->assigned_cpu = cpu;
    
    printk(KERN_DEBUG "WFS: Task placed with vruntime=%llu, VFT=%llu on CPU %d (CPU vtime=%llu)\n",
           se->vruntime, se->vft, cpu, get_cpu_vtime(wfs_rq));
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
    int cpu = cpu_of(rq);

    /* Only add if not already on RB-tree */
    if (RB_EMPTY_NODE(&se->run_node)) {
        /* Initialize virtual time for new or waking tasks */
        if (!(flags & ENQUEUE_WAKEUP) || se->vruntime == 0) {
            place_entity(wfs_rq, se, cpu);
        } else {
            /* Waking task - recalculate VFT based on current vruntime */
            u64 tick_ns = TICK_NSEC;
            u64 scaled_tick = calc_delta_fair(tick_ns, se);
            se->vft = se->vruntime + scaled_tick;
        }

        /* Add task weight to CPU's total weight */
        add_task_to_cpu_weight(wfs_rq, se->weight);

        /* Insert into RB-tree ordered by VFT */
        rb_add_cached(&se->run_node, &wfs_rq->tasks_timeline, wfs_entity_before);

        wfs_rq->wfs_nr_running++;
        add_nr_running(rq, 1);

        update_min_vruntime(wfs_rq);

        // TODO: clean up the if/else for printing later
        //printk(KERN_INFO "WFS: PID %d ENQUEUED on CPU %d (flags=%d), vruntime=%llu, VFT=%llu, weight=%u, runqueue now has %u tasks\n",
        //       p->pid, cpu, flags, se->vruntime, se->vft, se->weight, wfs_rq->wfs_nr_running);
    } else {
        //printk(KERN_WARNING "WFS: PID %d already on runqueue, skipping enqueue\n", p->pid);
    }
}

static bool dequeue_task_wfs(struct rq *rq, struct task_struct *p, int flags)
{
    struct wfs_rq *wfs_rq = &rq->wfs;
    struct sched_wfs_entity *se = &p->wfs;
    int cpu = cpu_of(rq);

    /* Only remove if actually in RB-tree */
    if (!RB_EMPTY_NODE(&se->run_node)) {
        /* Remove task weight from CPU's total weight */
        remove_task_from_cpu_weight(wfs_rq, se->weight);

        rb_erase_cached(&se->run_node, &wfs_rq->tasks_timeline);
        RB_CLEAR_NODE(&se->run_node);

        wfs_rq->wfs_nr_running--;
        sub_nr_running(rq, 1);

        update_min_vruntime(wfs_rq);

        // TODO: Clean up the if/else
        //printk(KERN_INFO "WFS: PID %d DEQUEUED from CPU %d (flags=%d), weight=%u, runqueue now has %u tasks\n",
        //       p->pid, cpu, flags, se->weight, wfs_rq->wfs_nr_running);
    } else {
        //printk(KERN_WARNING "WFS: PID %d not on runqueue, skipping dequeue\n", p->pid);
    }
    return true;
}

static struct task_struct *pick_task_wfs(struct rq *rq)
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
        //printk(KERN_WARNING "WFS: RB-tree empty but wfs_nr_running=%u, fixing\n",
        //       wfs_rq->wfs_nr_running);
        wfs_rq->wfs_nr_running = 0;
        return NULL;
    }
    
    wfs_se = rb_entry(leftmost, struct sched_wfs_entity, run_node);
    next_task = task_of_wfs(wfs_se);

    // printk(KERN_DEBUG "WFS: PICKED next task PID %d (prev was PID %d), %u tasks in queue\n",
    //      next_task->pid, prev ? prev->pid : -1, wfs_rq->wfs_nr_running);

    return next_task;
}

static void put_prev_task_wfs(struct rq *rq, struct task_struct *p, struct task_struct *next)
{
    struct wfs_rq *wfs_rq = &rq->wfs;
    struct sched_wfs_entity *se = &p->wfs;
    u64 now = rq_clock_task(rq);
    int cpu = cpu_of(rq);
    
    printk(KERN_DEBUG "WFS: PUT_PREV task PID %d on CPU %d (next is PID %d)\n",
           p->pid, cpu, next ? next->pid : -1);
    
    /* Update execution time and virtual runtime */
    if (se->exec_start) {
        u64 delta_exec = now - se->exec_start;
        
        /* Update total runtime */
        p->se.sum_exec_runtime += delta_exec;
        se->sum_exec_runtime += delta_exec;
        
        /* Update CPU virtual time */
        update_cpu_vtime(wfs_rq, delta_exec);
        
        /* Update virtual runtime and recalculate VFT */
        update_vruntime(se, delta_exec);
        
        se->exec_start = 0;
        
        /* If task is still runnable, we need to reposition it in RB-tree */
        if (next != p && !RB_EMPTY_NODE(&se->run_node)) {
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
    int cpu = cpu_of(rq);
    
    p->wfs.exec_start = rq_clock_task(rq);
    
    //printk(KERN_DEBUG "WFS: SET_NEXT task PID %d (first=%d), %u tasks in queue\n", 
    //       p->pid, first, wfs_rq->wfs_nr_running);
}

static void update_curr_wfs(struct rq *rq)
{
    struct wfs_rq *wfs_rq = &rq->wfs;
    struct task_struct *curr = rq->curr;
    struct sched_wfs_entity *se;
    u64 now = rq_clock_task(rq);
    int cpu = cpu_of(rq);

    if (curr->sched_class != &wfs_sched_class)
        return;

    se = &curr->wfs;

    if (se->exec_start) {
        u64 delta_exec = now - se->exec_start;

        /* Update total runtime */
        curr->se.sum_exec_runtime += delta_exec;
        se->sum_exec_runtime += delta_exec;

        /* Update CPU virtual time */
        update_cpu_vtime(wfs_rq, delta_exec);

        /* Update virtual runtime continuously */
        update_vruntime(se, delta_exec);

        se->exec_start = now;
    }
}

static void task_tick_wfs(struct rq *rq, struct task_struct *p, int queued)
{
    struct wfs_rq *wfs_rq = &rq->wfs;
    struct sched_wfs_entity *se = &p->wfs;
    int cpu = cpu_of(rq);

    //printk(KERN_DEBUG "WFS: TASK_TICK PID %d on CPU %d (queued=%d), VFT=%llu, %u tasks in queue\n",
    //       p->pid, cpu, queued, se->vft, wfs_rq->wfs_nr_running);

    /* Update runtime stats first */
    update_curr_wfs(rq);

    /*
     * WFS: each task runs for exactly 1 tick quantum
     * Always preempt after 1 tick if there are other tasks
     */
    if (wfs_rq->wfs_nr_running > 1) {
        //printk(KERN_DEBUG "WFS: Multiple tasks (%u) - preempting PID %d after 1 tick, VFT=%llu\n",
        //       wfs_rq->wfs_nr_running, p->pid, se->vft);

        /* Trigger a reschedule - put_prev_task will handle repositioning */
        resched_curr(rq);

        //printk(KERN_DEBUG "WFS: Task PID %d preempted, will be repositioned based on updated VFT\n", p->pid);
    }
}

static void switched_to_wfs(struct rq *rq, struct task_struct *p)
{
    struct wfs_rq *wfs_rq = &rq->wfs;
    int cpu = cpu_of(rq);
    
    //printk(KERN_INFO "WFS: Task PID %d SWITCHED_TO WFS class, %u tasks in queue\n", 
    //       p->pid, wfs_rq->wfs_nr_running);
    
    /* If this task should preempt current task */
    if (rq->curr != p && rq->curr->sched_class == &wfs_sched_class)
        resched_curr(rq);
}

static void switched_from_wfs(struct rq *rq, struct task_struct *p)
{
    struct wfs_rq *wfs_rq = &rq->wfs;
    int cpu = cpu_of(rq);
    
    //printk(KERN_INFO "WFS: Task PID %d SWITCHED_FROM WFS class, %u tasks in queue\n", 
    //       p->pid, wfs_rq->wfs_nr_running);

    
    /* Clean up when task leaves WFS */
    if (p->wfs.exec_start) {
        u64 delta_exec = rq_clock_task(rq) - p->wfs.exec_start;
        p->se.sum_exec_runtime += delta_exec;
        update_cpu_vtime(&rq->wfs, delta_exec);
        p->wfs.exec_start = 0;
    }
}

static void check_preempt_curr_wfs(struct rq *rq, struct task_struct *p, int flags)
{
    /* For now, WFS is non-preemptive except for round-robin in task_tick */
    /* Could add preemption logic here if needed */
    //printk(KERN_DEBUG "WFS: check_preempt_curr called for PID %d (flags=%d)\n", 
    //       p->pid, flags);
}

static void wakeup_preempt_wfs(struct rq *rq, struct task_struct *p, int flags)
{
    /* 
     * Called when a task wakes up to determine if it should preempt current task.
     * For WFS round-robin, we don't do immediate preemption on wakeup.
     * Tasks will be scheduled in round-robin order via task_tick.
     */
    // printk(KERN_DEBUG "WFS: wakeup_preempt called for PID %d (flags=%d)\n", 
    //        p->pid, flags);
}

void init_wfs_rq(struct wfs_rq *wfs_rq)
{
    /* Remove INIT_LIST_HEAD since we're not using the list anymore */
    wfs_rq->tasks_timeline = RB_ROOT_CACHED;
    wfs_rq->wfs_nr_running = 0;
    wfs_rq->min_vruntime = 0;
    wfs_rq->cpu_total_weight = 0;
    wfs_rq->cpu_vtime = 0;
    printk(KERN_INFO "WFS: Runqueue initialized\n");
}

/* Enhanced SMP hooks for wfs scheduler */

static int select_task_rq_wfs(struct task_struct *p, int cpu, int flags)
{
    int best_cpu;
    
    /* Find CPU with minimum total weight */
    best_cpu = find_min_weight_cpu(p);
    
    printk(KERN_DEBUG "WFS: select_task_rq for PID %d: chose CPU %d (was %d)\n",
           p->pid, best_cpu, cpu);
    
    return best_cpu;
}

static int balance_wfs(struct rq *rq, struct task_struct *prev, struct rq_flags *rf)
{
    /* No-op balancing for now - load balancing happens at task placement */
    return 0;
}

static void migrate_task_rq_wfs(struct task_struct *p, int new_cpu)
{
    struct sched_wfs_entity *se = &p->wfs;
    int old_cpu = se->assigned_cpu;
    
    if (old_cpu != new_cpu && se->weight > 0) {
        /* Remove weight from old CPU and add to new CPU */
        if (old_cpu >= 0 && old_cpu < nr_cpu_ids) {
            struct rq *old_rq = cpu_rq(old_cpu);
            remove_task_from_cpu_weight(&old_rq->wfs, se->weight);
        }
        
        struct rq *new_rq = cpu_rq(new_cpu);
        add_task_to_cpu_weight(&new_rq->wfs, se->weight);
        se->assigned_cpu = new_cpu;
        
        printk(KERN_DEBUG "WFS: Task PID %d migrated from CPU %d to CPU %d, weight=%u\n",
               p->pid, old_cpu, new_cpu, se->weight);
    }
}

static void rq_online_wfs(struct rq *rq)
{
    int cpu = cpu_of(rq);
    printk(KERN_INFO "WFS: CPU %d came online\n", cpu);
}

static void rq_offline_wfs(struct rq *rq)
{
    int cpu = cpu_of(rq);
    struct wfs_rq *wfs_rq = &rq->wfs;
    
    printk(KERN_INFO "WFS: CPU %d going offline, had total weight %llu\n",
           cpu, wfs_rq->cpu_total_weight);
}

static void task_woken_wfs(struct rq *rq, struct task_struct *p)
{
    /* No-op - nothing to do after remote wakeup */
}

static void set_cpus_allowed_wfs(struct task_struct *p, struct affinity_context *ctx)
{
    /* Could implement logic to rebalance if affinity changes */
    printk(KERN_DEBUG "WFS: CPU affinity changed for PID %d\n", p->pid);
}

static bool yield_to_task_wfs(struct rq *rq, struct task_struct *p)
{
    /* Return false - don't handle yield_to */
    return false;
}

static void yield_task_wfs(struct rq *rq)
{
    /* Remove the old list-based yield logic since we don't have run_list anymore */
    struct task_struct *curr = rq->curr;
    struct wfs_rq *wfs_rq = &rq->wfs;
    
    /* For WFS, yielding just triggers a reschedule */
    if (wfs_rq->wfs_nr_running > 1) {
        resched_curr(rq);
    }
}

static void prio_changed_wfs(struct rq *rq, struct task_struct *p, int oldprio)
{
    /* No-op - WFS doesn't use priority levels */
}

const struct sched_class wfs_sched_class __section("__wfs_sched_class") = {
    .enqueue_task = enqueue_task_wfs,
    .dequeue_task = dequeue_task_wfs,
    .pick_task = pick_task_wfs,
    .put_prev_task = put_prev_task_wfs,
    .set_next_task = set_next_task_wfs,
    .task_tick = task_tick_wfs,
    .switched_to = switched_to_wfs,
    .switched_from = switched_from_wfs,
    .wakeup_preempt = wakeup_preempt_wfs,
    .update_curr = update_curr_wfs,
    .yield_to_task = yield_to_task_wfs,
    .yield_task = yield_task_wfs,
    .prio_changed = prio_changed_wfs,
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
