/* 6118 */
#include <linux/mmu_context.h>
#include "sched.h"
#include "wfs.h"

/* Global state for periodic load balancing */
static struct {
    spinlock_t lock;
    u64 last_balance_time;
} wfs_balance_state = {
    .lock = __SPIN_LOCK_UNLOCKED(wfs_balance_state.lock),
    .last_balance_time = 0,
};

static const u64 WFS_BALANCE_INTERVAL_NS = 500000000ULL; /* 500ms in nanoseconds */

/* Find the CPU with minimum total weight - LOCKLESS VERSION */
static int find_min_weight_cpu(struct task_struct *p)
{
    int cpu, best_cpu = -1;
    u64 min_weight = ULLONG_MAX;
    const struct cpumask *allowed_mask = p->cpus_ptr;

    for_each_cpu(cpu, allowed_mask) {
        struct rq *cpu_rq = cpu_rq(cpu);
        struct wfs_rq *wfs_rq = &cpu_rq->wfs;
        u64 weight;

        /* Skip offline CPUs */
        if (!cpu_online(cpu))
            continue;

        /* Lockless read - may be slightly stale but that's OK for load balancing */
        weight = READ_ONCE(wfs_rq->cpu_total_weight);

        /* Find CPU with minimum weight, breaking ties with lowest CPU number */
        if (weight < min_weight) {
            min_weight = weight;
            best_cpu = cpu;
        }
    }

    /* Fallback to current CPU if no suitable CPU found */
    if (best_cpu == -1)
        best_cpu = smp_processor_id();

    return best_cpu;
}

/* Find CPU with maximum total weight */
static int find_max_weight_cpu(void)
{
    int cpu, best_cpu = -1;
    u64 max_weight = 0;

    for_each_online_cpu(cpu) {
        struct rq *cpu_rq = cpu_rq(cpu);
        struct wfs_rq *wfs_rq = &cpu_rq->wfs;
        u64 weight;

        /* Lockless read */
        weight = READ_ONCE(wfs_rq->cpu_total_weight);

        if (weight > max_weight) {
            max_weight = weight;
            best_cpu = cpu;
        }
    }

    return best_cpu;
}

/* Find CPU with minimum total weight (for balancing) */
static int find_min_weight_cpu_for_balance(void)
{
    int cpu, best_cpu = -1;
    u64 min_weight = ULLONG_MAX;

    for_each_online_cpu(cpu) {
        struct rq *cpu_rq = cpu_rq(cpu);
        struct wfs_rq *wfs_rq = &cpu_rq->wfs;
        u64 weight;

        /* Lockless read */
        weight = READ_ONCE(wfs_rq->cpu_total_weight);

        if (weight < min_weight) {
            min_weight = weight;
            best_cpu = cpu;
        }
    }

    return best_cpu;
}

/* Find eligible task to migrate from source CPU to destination CPU */
static struct task_struct *find_eligible_task_to_migrate(struct rq *src_rq, int dest_cpu, u64 src_weight, u64 dest_weight)
{
    struct wfs_rq *wfs_rq = &src_rq->wfs;
    struct rb_node *node;
    struct task_struct *current_task = src_rq->curr;

    /* Walk through RB-tree to find eligible task */
    for (node = rb_first_cached(&wfs_rq->tasks_timeline); node; node = rb_next(node)) {
        struct sched_wfs_entity *se = rb_entry(node, struct sched_wfs_entity, run_node);
        struct task_struct *task = task_of_wfs(se);

        /* Skip currently running task */
        if (task == current_task)
            continue;

        /* Check CPU affinity */
        if (!cpumask_test_cpu(dest_cpu, task->cpus_ptr))
            continue;

        /* Check if moving this task would reverse the imbalance */
        u64 task_weight = se->weight;
        u64 new_src_weight = src_weight - task_weight;
        u64 new_dest_weight = dest_weight + task_weight;

        /* Only migrate if it doesn't reverse the imbalance */
        if (new_src_weight >= new_dest_weight)
            return task;
    }

    return NULL;
}


static void add_task_to_cpu_weight(struct wfs_rq *wfs_rq, u64 weight)
{
    /* Called with rq->lock held by enqueue_task, so this is safe */
    WRITE_ONCE(wfs_rq->cpu_total_weight, wfs_rq->cpu_total_weight + weight);
}

static void remove_task_from_cpu_weight(struct wfs_rq *wfs_rq, u64 weight)
{
    /* Called with rq->lock held by dequeue_task, so this is safe */
    u64 current_weight = wfs_rq->cpu_total_weight;
    WRITE_ONCE(wfs_rq->cpu_total_weight, (current_weight >= weight) ? current_weight - weight : 0);
}

/* Update CPU virtual time based on execution time */
static void update_cpu_vtime(struct wfs_rq *wfs_rq, u64 delta_exec)
{
    if (wfs_rq->cpu_total_weight > 0) {
        u64 vtime_delta = (delta_exec * WFS_SCALE_FACTOR) / wfs_rq->cpu_total_weight;
        wfs_rq->cpu_vtime += vtime_delta;
        
     //   printk(KERN_DEBUG "WFS: CPU vtime updated by %llu, now %llu (weight=%llu)\n",
       //        vtime_delta, wfs_rq->cpu_vtime, wfs_rq->cpu_total_weight);
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
    
   // printk(KERN_DEBUG "WFS: Task placed with vruntime=%llu, VFT=%llu on CPU %d (CPU vtime=%llu)\n",
     //      se->vruntime, se->vft, cpu, get_cpu_vtime(wfs_rq));
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

    /* Only remove if actually in RB-tree */
    if (!RB_EMPTY_NODE(&se->run_node)) {
        remove_task_from_cpu_weight(wfs_rq, se->weight);

        rb_erase_cached(&se->run_node, &wfs_rq->tasks_timeline);
        RB_CLEAR_NODE(&se->run_node);

        wfs_rq->wfs_nr_running--;
        sub_nr_running(rq, 1);

        update_min_vruntime(wfs_rq);
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
            rb_erase_cached(&se->run_node, &wfs_rq->tasks_timeline);

            /* Re-insert at new position based on updated VFT */
            rb_add_cached(&se->run_node, &wfs_rq->tasks_timeline, wfs_entity_before);

            update_min_vruntime(wfs_rq);
        }
    }
}

static void set_next_task_wfs(struct rq *rq, struct task_struct *p, bool first)
{
    p->wfs.exec_start = rq_clock_task(rq);
}

void update_curr_wfs(struct rq *rq)
{
    struct wfs_rq *wfs_rq = &rq->wfs;
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

        /* Update CPU virtual time */
        update_cpu_vtime(wfs_rq, delta_exec);

        /* Update virtual runtime continuously */
        update_vruntime(se, delta_exec);

        se->exec_start = now;
    }
}

/* Fixed periodic load balancing implementation */


/* Improved periodic load balancing that can handle this_cpu being involved */
static void wfs_periodic_balance(int this_cpu)
{
    int max_cpu, min_cpu;
    struct rq *this_rq, *max_rq, *min_rq;
    struct task_struct *task_to_migrate;
    u64 max_weight, min_weight;

    this_rq = cpu_rq(this_cpu);

    /* Find CPUs with max and min weights */
    max_cpu = find_max_weight_cpu();
    min_cpu = find_min_weight_cpu_for_balance();

    if (max_cpu == -1 || min_cpu == -1 || max_cpu == min_cpu)
        return;

    max_rq = cpu_rq(max_cpu);
    min_rq = cpu_rq(min_cpu);

    /* Always drop our lock and use double_rq_lock for consistent locking */
    raw_spin_unlock(&this_rq->__lock);
    double_rq_lock(max_rq, min_rq);

    /* Get weights under lock */
    max_weight = max_rq->wfs.cpu_total_weight;
    min_weight = min_rq->wfs.cpu_total_weight;

    /* Only balance if there's significant imbalance */
    if (max_weight > min_weight) {
        task_to_migrate = find_eligible_task_to_migrate(max_rq, min_cpu, max_weight, min_weight);

        if (task_to_migrate && task_to_migrate->sched_class == &wfs_sched_class) {
            deactivate_task(max_rq, task_to_migrate, DEQUEUE_NOCLOCK);
            set_task_cpu(task_to_migrate, min_cpu);
            activate_task(min_rq, task_to_migrate, ENQUEUE_NOCLOCK);

            //printk(KERN_INFO "WFS: Migrated task PID %d from CPU %d (weight %llu) to CPU %d (weight %llu)\n",
            //       task_to_migrate->pid, max_cpu, max_weight, min_cpu, min_weight);
        }
    }

    /* Always unlock both and re-acquire our lock */
    double_rq_unlock(max_rq, min_rq);
    raw_spin_lock(&this_rq->__lock);
}

/* Check if it's time for periodic load balancing */
static void wfs_check_periodic_balance(struct rq *this_rq, u64 now)
{
    unsigned long flags;
    bool should_balance = false;
    int this_cpu = cpu_of(this_rq);

    /* Quick check without lock first */
    if (now - READ_ONCE(wfs_balance_state.last_balance_time) < WFS_BALANCE_INTERVAL_NS)
        return;

    /* Acquire lock and check again */
    spin_lock_irqsave(&wfs_balance_state.lock, flags);
    if (now - wfs_balance_state.last_balance_time >= WFS_BALANCE_INTERVAL_NS) {
        wfs_balance_state.last_balance_time = now;
        should_balance = true;
    }
    spin_unlock_irqrestore(&wfs_balance_state.lock, flags);

    if (should_balance) {
        wfs_periodic_balance(this_cpu);
    }
}
/* Updated task_tick_wfs function */
static void task_tick_wfs(struct rq *rq, struct task_struct *p, int queued)
{
    struct wfs_rq *wfs_rq = &rq->wfs;
    u64 now = rq_clock_task(rq);

    /* Update runtime stats first */
    update_curr_wfs(rq);
    
    /* Check for periodic load balancing - pass the rq and time */
    wfs_check_periodic_balance(rq, now);
    
    /*
     * WFS: each task runs for exactly 1 tick quantum
     * Always preempt after 1 tick if there are other tasks
     */
    if (wfs_rq->wfs_nr_running > 1) {
        /* Trigger a reschedule - put_prev_task will handle repositioning */
        resched_curr(rq);
    }
}

static void switched_to_wfs(struct rq *rq, struct task_struct *p)
{
    
    /* If this task should preempt current task */
    if (rq->curr != p && rq->curr->sched_class == &wfs_sched_class)
        resched_curr(rq);
}

static void switched_from_wfs(struct rq *rq, struct task_struct *p)
{

    /* Clean up when task leaves WFS */
    if (p->wfs.exec_start) {
        u64 delta_exec = rq_clock_task(rq) - p->wfs.exec_start;
        p->se.sum_exec_runtime += delta_exec;
        update_cpu_vtime(&rq->wfs, delta_exec);
        p->wfs.exec_start = 0;
    }
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
    wfs_rq->tasks_timeline = RB_ROOT_CACHED;
    wfs_rq->wfs_nr_running = 0;
    wfs_rq->min_vruntime = 0;
    wfs_rq->cpu_total_weight = 0;
    wfs_rq->cpu_vtime = 0;
   // printk(KERN_INFO "WFS: Runqueue initialized\n");
}

/* Enhanced SMP hooks for wfs scheduler */

static int select_task_rq_wfs(struct task_struct *p, int cpu, int flags)
{
    int best_cpu;
    
    /* Find CPU with minimum total weight */
    best_cpu = find_min_weight_cpu(p);
    
    //printk(KERN_DEBUG "WFS: select_task_rq for PID %d: chose CPU %d (was %d)\n",
      //     p->pid, best_cpu, cpu);
    
    return best_cpu;
}
/* Idle load balancing - pull tasks from heaviest CPU to idle CPU */

static int balance_wfs(struct rq *rq, struct task_struct *prev, struct rq_flags *rf)
{
    int this_cpu = cpu_of(rq);
    int max_cpu;
    struct rq *max_rq;
    struct task_struct *task_to_migrate;
    u64 max_weight, this_weight;

    /* If we have WFS tasks, no need to balance */
    if (rq->wfs.wfs_nr_running)
        return 0;

    this_weight = rq->wfs.cpu_total_weight;

    /* Find CPU with maximum weight */
    max_cpu = find_max_weight_cpu();
    
    if (max_cpu == -1 || max_cpu == this_cpu)
        return 0;

    max_rq = cpu_rq(max_cpu);

    /* Only pull if the max CPU has significantly more work than us */
    max_weight = READ_ONCE(max_rq->wfs.cpu_total_weight);
    if (max_weight <= this_weight)
        return 0;

    /* Use double_lock_balance - we already hold rq lock */
    double_lock_balance(rq, max_rq);

    /* Re-read weights under lock */
    max_weight = max_rq->wfs.cpu_total_weight;
    this_weight = rq->wfs.cpu_total_weight;

    /* Find eligible task to migrate from max_cpu to this_cpu */
    if (max_weight > this_weight) {
        task_to_migrate = find_eligible_task_to_migrate(max_rq, this_cpu, max_weight, this_weight);

        if (task_to_migrate && task_to_migrate->sched_class == &wfs_sched_class) {
            deactivate_task(max_rq, task_to_migrate, DEQUEUE_NOCLOCK);
            set_task_cpu(task_to_migrate, this_cpu);
            activate_task(rq, task_to_migrate, ENQUEUE_NOCLOCK);

            double_unlock_balance(rq, max_rq);
            return 1; /* Successfully pulled a task */
        }
    }

    double_unlock_balance(rq, max_rq);
    return 0; /* No task pulled */
}

static void migrate_task_rq_wfs(struct task_struct *p, int new_cpu)
{
    struct sched_wfs_entity *se = &p->wfs;

    /* Only update the assigned CPU tracking - don't manipulate weights here */
    se->assigned_cpu = new_cpu;

    /*
     * DON'T manipulate weights here - the normal enqueue/dequeue cycle
     * will handle weight accounting properly:
     * 1. dequeue_task_wfs() removes weight from old CPU
     * 2. enqueue_task_wfs() adds weight to new CPU
     */
}

static void rq_online_wfs(struct rq *rq)
{
}

static void rq_offline_wfs(struct rq *rq)
{
}

static void task_woken_wfs(struct rq *rq, struct task_struct *p)
{
    /* No-op - nothing to do after remote wakeup */
}


static bool yield_to_task_wfs(struct rq *rq, struct task_struct *p)
{
    /* Return false - don't handle yield_to */
    return false;
}

static void yield_task_wfs(struct rq *rq)
{
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
    .set_cpus_allowed = set_cpus_allowed_common,
#endif
};

/* 6118 */
