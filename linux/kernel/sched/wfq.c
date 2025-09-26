/* 6118 */
#include <linux/mmu_context.h>
#include "sched.h"
#include "wfq.h"

/* Global state for periodic load balancing */
static struct {
    spinlock_t lock;
    u64 last_balance_time;
} wfq_balance_state = {
    .lock = __SPIN_LOCK_UNLOCKED(wfq_balance_state.lock),
    .last_balance_time = 0,
};

static const u64 WFQ_BALANCE_INTERVAL_NS = 500000000ULL; /* 500ms in nanoseconds */

/* Find the CPU with minimum total weight - LOCKLESS VERSION */
static int find_min_weight_cpu(struct task_struct *p)
{
    int cpu, best_cpu = -1;
    u64 min_weight = ULLONG_MAX;
    const struct cpumask *allowed_mask = p->cpus_ptr;

    for_each_cpu(cpu, allowed_mask) {
        struct rq *cpu_rq = cpu_rq(cpu);
        struct wfq_rq *wfq_rq = &cpu_rq->wfq;
        u64 weight;

        /* Skip offline CPUs */
        if (!cpu_online(cpu))
            continue;

        /* Lockless read - may be slightly stale but that's OK for load balancing */
        weight = READ_ONCE(wfq_rq->cpu_total_weight);

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
        struct wfq_rq *wfq_rq = &cpu_rq->wfq;
        u64 weight;

        /* Lockless read */
        weight = READ_ONCE(wfq_rq->cpu_total_weight);

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
        struct wfq_rq *wfq_rq = &cpu_rq->wfq;
        u64 weight;

        /* Lockless read */
        weight = READ_ONCE(wfq_rq->cpu_total_weight);

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
    struct wfq_rq *wfq_rq = &src_rq->wfq;
    struct rb_node *node;
    struct task_struct *current_task = src_rq->curr;

    /* Walk through RB-tree to find eligible task */
    for (node = rb_first_cached(&wfq_rq->tasks_timeline); node; node = rb_next(node)) {
        struct sched_wfq_entity *se = rb_entry(node, struct sched_wfq_entity, run_node);
        struct task_struct *task = task_of_wfq(se);

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


static void add_task_to_cpu_weight(struct wfq_rq *wfq_rq, u64 weight)
{
    /* Called with rq->lock held by enqueue_task, so this is safe */
    WRITE_ONCE(wfq_rq->cpu_total_weight, wfq_rq->cpu_total_weight + weight);
}

static void remove_task_from_cpu_weight(struct wfq_rq *wfq_rq, u64 weight)
{
    /* Called with rq->lock held by dequeue_task, so this is safe */
    u64 current_weight = wfq_rq->cpu_total_weight;
    WRITE_ONCE(wfq_rq->cpu_total_weight, (current_weight >= weight) ? current_weight - weight : 0);
}

/* Update CPU virtual time based on execution time */
static void update_cpu_vtime(struct wfq_rq *wfq_rq, u64 delta_exec)
{
    if (wfq_rq->cpu_total_weight > 0) {
        u64 vtime_delta = (delta_exec * WFQ_SCALE_FACTOR) / wfq_rq->cpu_total_weight;
        wfq_rq->cpu_vtime += vtime_delta;
        
     //   printk(KERN_DEBUG "WFQ: CPU vtime updated by %llu, now %llu (weight=%llu)\n",
       //        vtime_delta, wfq_rq->cpu_vtime, wfq_rq->cpu_total_weight);
    }
}

/* Get current CPU virtual time */
static u64 get_cpu_vtime(struct wfq_rq *wfq_rq)
{
    return wfq_rq->cpu_vtime;
}

/* Update virtual runtime and VFT for a task */
static void update_vruntime(struct sched_wfq_entity *se, u64 delta_exec)
{
    u64 vdelta = calc_delta_fair(delta_exec, se);
    se->vruntime += vdelta;
    
    /* Calculate VFT = vruntime + (1 tick scaled) / weight */
    u64 tick_ns = TICK_NSEC; /* Duration of 1 tick in nanoseconds */
    u64 scaled_tick = calc_delta_fair(tick_ns, se);
    se->vft = se->vruntime + scaled_tick;
}

/* Initialize virtual time for new tasks */
static void place_entity(struct wfq_rq *wfq_rq, struct sched_wfq_entity *se, int cpu)
{
    /* Initialize weight if not set */
    if (!se->weight) {
        se->weight = WFQ_DEFAULT_WEIGHT;
        se->inv_weight = WFQ_SCALE_FACTOR / se->weight;
    }
    
    /* Set task's virtual time to current CPU virtual time */
    se->vruntime = get_cpu_vtime(wfq_rq);
    
    /* If this is greater than min_vruntime, use min_vruntime to prevent starvation */
    if (se->vruntime < wfq_rq->min_vruntime) {
        se->vruntime = wfq_rq->min_vruntime;
    }
    
    /* Calculate initial VFT */
    u64 tick_ns = TICK_NSEC;
    u64 scaled_tick = calc_delta_fair(tick_ns, se);
    se->vft = se->vruntime + scaled_tick;
    
    /* Record which CPU this task is assigned to */
    se->assigned_cpu = cpu;
    
   // printk(KERN_DEBUG "WFQ: Task placed with vruntime=%llu, VFT=%llu on CPU %d (CPU vtime=%llu)\n",
     //      se->vruntime, se->vft, cpu, get_cpu_vtime(wfq_rq));
}

/* Update min_vruntime for the runqueue */
static void update_min_vruntime(struct wfq_rq *wfq_rq)
{
    struct rb_node *leftmost = rb_first_cached(&wfq_rq->tasks_timeline);
    u64 vruntime = wfq_rq->min_vruntime;

    if (leftmost) {
        struct sched_wfq_entity *se = rb_entry(leftmost, struct sched_wfq_entity, run_node);
        vruntime = se->vruntime;
    }

    /* min_vruntime should never go backwards */
    wfq_rq->min_vruntime = max(wfq_rq->min_vruntime, vruntime);
}

/* Main scheduler functions */

static void enqueue_task_wfq(struct rq *rq, struct task_struct *p, int flags)
{
    struct wfq_rq *wfq_rq = &rq->wfq;
    struct sched_wfq_entity *se = &p->wfq;
    int cpu = cpu_of(rq);

    /* Only add if not already on RB-tree */
    if (RB_EMPTY_NODE(&se->run_node)) {
        /* Initialize virtual time for new or waking tasks */
        if (!(flags & ENQUEUE_WAKEUP) || se->vruntime == 0) {
            place_entity(wfq_rq, se, cpu);
        } else {
            /* Waking task - recalculate VFT based on current vruntime */
            u64 tick_ns = TICK_NSEC;
            u64 scaled_tick = calc_delta_fair(tick_ns, se);
            se->vft = se->vruntime + scaled_tick;
        }

        /* Add task weight to CPU's total weight */
        add_task_to_cpu_weight(wfq_rq, se->weight);

        /* Insert into RB-tree ordered by VFT */
        rb_add_cached(&se->run_node, &wfq_rq->tasks_timeline, wfq_entity_before);

        wfq_rq->wfq_nr_running++;
        add_nr_running(rq, 1);

        update_min_vruntime(wfq_rq);

        // TODO: clean up the if/else for printing later
        //printk(KERN_INFO "WFQ: PID %d ENQUEUED on CPU %d (flags=%d), vruntime=%llu, VFT=%llu, weight=%u, runqueue now has %u tasks\n",
        //       p->pid, cpu, flags, se->vruntime, se->vft, se->weight, wfq_rq->wfq_nr_running);
    } else {
        //printk(KERN_WARNING "WFQ: PID %d already on runqueue, skipping enqueue\n", p->pid);
    }
}

static bool dequeue_task_wfq(struct rq *rq, struct task_struct *p, int flags)
{
    struct wfq_rq *wfq_rq = &rq->wfq;
    struct sched_wfq_entity *se = &p->wfq;

    /* Only remove if actually in RB-tree */
    if (!RB_EMPTY_NODE(&se->run_node)) {
        remove_task_from_cpu_weight(wfq_rq, se->weight);

        rb_erase_cached(&se->run_node, &wfq_rq->tasks_timeline);
        RB_CLEAR_NODE(&se->run_node);

        wfq_rq->wfq_nr_running--;
        sub_nr_running(rq, 1);

        update_min_vruntime(wfq_rq);
    }
    return true;
}

static struct task_struct *pick_task_wfq(struct rq *rq)
{
    struct wfq_rq *wfq_rq = &rq->wfq;
    struct sched_wfq_entity *wfq_se;
    struct task_struct *next_task;
    struct rb_node *leftmost;
    
    /* Check if we have any WFQ tasks */
    if (!wfq_rq->wfq_nr_running) {
        return NULL;
    }
    
    /* Get the leftmost node (minimum VFT) from RB-tree */
    leftmost = rb_first_cached(&wfq_rq->tasks_timeline);
    if (!leftmost) {
        /* This shouldn't happen if wfq_nr_running > 0, but be safe */
        //printk(KERN_WARNING "WFQ: RB-tree empty but wfq_nr_running=%u, fixing\n",
        //       wfq_rq->wfq_nr_running);
        wfq_rq->wfq_nr_running = 0;
        return NULL;
    }
    
    wfq_se = rb_entry(leftmost, struct sched_wfq_entity, run_node);
    next_task = task_of_wfq(wfq_se);

    // printk(KERN_DEBUG "WFQ: PICKED next task PID %d (prev was PID %d), %u tasks in queue\n",
    //      next_task->pid, prev ? prev->pid : -1, wfq_rq->wfq_nr_running);

    return next_task;
}

static void put_prev_task_wfq(struct rq *rq, struct task_struct *p, struct task_struct *next)
{
    struct wfq_rq *wfq_rq = &rq->wfq;
    struct sched_wfq_entity *se = &p->wfq;
    u64 now = rq_clock_task(rq);

    /* Update execution time and virtual runtime */
    if (se->exec_start) {
        u64 delta_exec = now - se->exec_start;

        /* Update total runtime */
        p->se.sum_exec_runtime += delta_exec;
        se->sum_exec_runtime += delta_exec;

        /* Update CPU virtual time */
        update_cpu_vtime(wfq_rq, delta_exec);

        /* Update virtual runtime and recalculate VFT */
        update_vruntime(se, delta_exec);

        se->exec_start = 0;

        /* If task is still runnable, we need to reposition it in RB-tree */
        if (next != p && !RB_EMPTY_NODE(&se->run_node)) {
            rb_erase_cached(&se->run_node, &wfq_rq->tasks_timeline);

            /* Re-insert at new position based on updated VFT */
            rb_add_cached(&se->run_node, &wfq_rq->tasks_timeline, wfq_entity_before);

            update_min_vruntime(wfq_rq);
        }
    }
}

static void set_next_task_wfq(struct rq *rq, struct task_struct *p, bool first)
{
    p->wfq.exec_start = rq_clock_task(rq);
}

void update_curr_wfq(struct rq *rq)
{
    struct wfq_rq *wfq_rq = &rq->wfq;
    struct task_struct *curr = rq->curr;
    struct sched_wfq_entity *se;
    u64 now = rq_clock_task(rq);

    if (curr->sched_class != &wfq_sched_class)
        return;

    se = &curr->wfq;

    if (se->exec_start) {
        u64 delta_exec = now - se->exec_start;

        /* Update total runtime */
        curr->se.sum_exec_runtime += delta_exec;
        se->sum_exec_runtime += delta_exec;

        /* Update CPU virtual time */
        update_cpu_vtime(wfq_rq, delta_exec);

        /* Update virtual runtime continuously */
        update_vruntime(se, delta_exec);

        se->exec_start = now;
    }
}

/* Fixed periodic load balancing implementation */


/* Improved periodic load balancing that can handle this_cpu being involved */
static void wfq_periodic_balance(int this_cpu)
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
    max_weight = max_rq->wfq.cpu_total_weight;
    min_weight = min_rq->wfq.cpu_total_weight;

    /* Only balance if there's significant imbalance */
    if (max_weight > min_weight) {
        task_to_migrate = find_eligible_task_to_migrate(max_rq, min_cpu, max_weight, min_weight);

        if (task_to_migrate && task_to_migrate->sched_class == &wfq_sched_class) {
            deactivate_task(max_rq, task_to_migrate, DEQUEUE_NOCLOCK);
            set_task_cpu(task_to_migrate, min_cpu);
            activate_task(min_rq, task_to_migrate, ENQUEUE_NOCLOCK);

            //printk(KERN_INFO "WFQ: Migrated task PID %d from CPU %d (weight %llu) to CPU %d (weight %llu)\n",
            //       task_to_migrate->pid, max_cpu, max_weight, min_cpu, min_weight);
        }
    }

    /* Always unlock both and re-acquire our lock */
    double_rq_unlock(max_rq, min_rq);
    raw_spin_lock(&this_rq->__lock);
}

/* Check if it's time for periodic load balancing */
static void wfq_check_periodic_balance(struct rq *this_rq, u64 now)
{
    unsigned long flags;
    bool should_balance = false;
    int this_cpu = cpu_of(this_rq);

    /* Quick check without lock first */
    if (now - READ_ONCE(wfq_balance_state.last_balance_time) < WFQ_BALANCE_INTERVAL_NS)
        return;

    /* Acquire lock and check again */
    spin_lock_irqsave(&wfq_balance_state.lock, flags);
    if (now - wfq_balance_state.last_balance_time >= WFQ_BALANCE_INTERVAL_NS) {
        wfq_balance_state.last_balance_time = now;
        should_balance = true;
    }
    spin_unlock_irqrestore(&wfq_balance_state.lock, flags);

    if (should_balance) {
        wfq_periodic_balance(this_cpu);
    }
}
/* Updated task_tick_wfq function */
static void task_tick_wfq(struct rq *rq, struct task_struct *p, int queued)
{

    struct wfq_rq *wfq_rq = &rq->wfq;
    u64 now = rq_clock_task(rq);

    /* Update runtime stats first */
    update_curr_wfq(rq);
    
    /* Check for periodic load balancing - pass the rq and time */
    wfq_check_periodic_balance(rq, now);
    
    /*
     * WFQ: each task runs for exactly 1 tick quantum
     * Always preempt after 1 tick if there are other tasks
     */
    if (wfq_rq->wfq_nr_running > 1) {
        /* Trigger a reschedule - put_prev_task will handle repositioning */
        resched_curr(rq);
    }
}

static void switched_to_wfq(struct rq *rq, struct task_struct *p)
{
    
    /* If this task should preempt current task */
    if (rq->curr != p && rq->curr->sched_class == &wfq_sched_class)
        resched_curr(rq);
}

static void switched_from_wfq(struct rq *rq, struct task_struct *p)
{

    /* Clean up when task leaves WFQ */
    if (p->wfq.exec_start) {
        u64 delta_exec = rq_clock_task(rq) - p->wfq.exec_start;
        p->se.sum_exec_runtime += delta_exec;
        update_cpu_vtime(&rq->wfq, delta_exec);
        p->wfq.exec_start = 0;
    }
}


static void wakeup_preempt_wfq(struct rq *rq, struct task_struct *p, int flags)
{
    /* 
     * Called when a task wakes up to determine if it should preempt current task.
     * For WFQ round-robin, we don't do immediate preemption on wakeup.
     * Tasks will be scheduled in round-robin order via task_tick.
     */
    // printk(KERN_DEBUG "WFQ: wakeup_preempt called for PID %d (flags=%d)\n", 
    //        p->pid, flags);
}

void init_wfq_rq(struct wfq_rq *wfq_rq)
{
    wfq_rq->tasks_timeline = RB_ROOT_CACHED;
    wfq_rq->wfq_nr_running = 0;
    wfq_rq->min_vruntime = 0;
    wfq_rq->cpu_total_weight = 0;
    wfq_rq->cpu_vtime = 0;
   // printk(KERN_INFO "WFQ: Runqueue initialized\n");
}

/* Enhanced SMP hooks for wfq scheduler */

static int select_task_rq_wfq(struct task_struct *p, int cpu, int flags)
{
    int best_cpu;
    
    /* Find CPU with minimum total weight */
    best_cpu = find_min_weight_cpu(p);
    
    //printk(KERN_DEBUG "WFQ: select_task_rq for PID %d: chose CPU %d (was %d)\n",
      //     p->pid, best_cpu, cpu);
    
    return best_cpu;
}
/* Idle load balancing - pull tasks from heaviest CPU to idle CPU */

static int balance_wfq(struct rq *rq, struct task_struct *prev, struct rq_flags *rf)
{
    int this_cpu = cpu_of(rq);
    int max_cpu;
    struct rq *max_rq;
    struct task_struct *task_to_migrate;
    u64 max_weight, this_weight;

    /* If we have WFQ tasks, no need to balance */
    if (rq->wfq.wfq_nr_running)
        return 0;

    this_weight = rq->wfq.cpu_total_weight;

    /* Find CPU with maximum weight */
    max_cpu = find_max_weight_cpu();
    
    if (max_cpu == -1 || max_cpu == this_cpu)
        return 0;

    max_rq = cpu_rq(max_cpu);

    /* Only pull if the max CPU has significantly more work than us */
    max_weight = READ_ONCE(max_rq->wfq.cpu_total_weight);
    if (max_weight <= this_weight)
        return 0;

    /* Use double_lock_balance - we already hold rq lock */
    double_lock_balance(rq, max_rq);

    /* Re-read weights under lock */
    max_weight = max_rq->wfq.cpu_total_weight;
    this_weight = rq->wfq.cpu_total_weight;

    /* Find eligible task to migrate from max_cpu to this_cpu */
    if (max_weight > this_weight) {
        task_to_migrate = find_eligible_task_to_migrate(max_rq, this_cpu, max_weight, this_weight);

        if (task_to_migrate && task_to_migrate->sched_class == &wfq_sched_class) {
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

static void migrate_task_rq_wfq(struct task_struct *p, int new_cpu)
{
    struct sched_wfq_entity *se = &p->wfq;

    /* Only update the assigned CPU tracking - don't manipulate weights here */
    se->assigned_cpu = new_cpu;

    /*
     * DON'T manipulate weights here - the normal enqueue/dequeue cycle
     * will handle weight accounting properly:
     * 1. dequeue_task_wfq() removes weight from old CPU
     * 2. enqueue_task_wfq() adds weight to new CPU
     */
}

static void rq_online_wfq(struct rq *rq)
{
}

static void rq_offline_wfq(struct rq *rq)
{
}

static void task_woken_wfq(struct rq *rq, struct task_struct *p)
{
    /* No-op - nothing to do after remote wakeup */
}


static bool yield_to_task_wfq(struct rq *rq, struct task_struct *p)
{
    /* Return false - don't handle yield_to */
    return false;
}

static void yield_task_wfq(struct rq *rq)
{
    struct wfq_rq *wfq_rq = &rq->wfq;

    /* For WFQ, yielding just triggers a reschedule */
    if (wfq_rq->wfq_nr_running > 1) {
        resched_curr(rq);
    }
}


static void prio_changed_wfq(struct rq *rq, struct task_struct *p, int oldprio)
{
    /* No-op - WFQ doesn't use priority levels */
}


const struct sched_class wfq_sched_class __section("__wfq_sched_class") = {
    .enqueue_task = enqueue_task_wfq,
    .dequeue_task = dequeue_task_wfq,
    .pick_task = pick_task_wfq,
    .put_prev_task = put_prev_task_wfq,
    .set_next_task = set_next_task_wfq,
    .task_tick = task_tick_wfq,
    .switched_to = switched_to_wfq,
    .switched_from = switched_from_wfq,
    .wakeup_preempt = wakeup_preempt_wfq,
    .update_curr = update_curr_wfq,
    .yield_to_task = yield_to_task_wfq,
    .yield_task = yield_task_wfq,
    .prio_changed = prio_changed_wfq,
#ifdef CONFIG_SMP
    .balance = balance_wfq,
    .select_task_rq = select_task_rq_wfq,
    .migrate_task_rq = migrate_task_rq_wfq,
    .rq_online = rq_online_wfq,
    .rq_offline = rq_offline_wfq,
    .task_woken = task_woken_wfq,
    .set_cpus_allowed = set_cpus_allowed_common,
#endif
};

/* 6118 */
