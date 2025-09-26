/*
 * WFQ System Calls Implementation
 * File: kernel/sched/wfq_syscalls.c
 */

#include <linux/syscalls.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <linux/capability.h>
#include <linux/errno.h>
#include <linux/mmu_context.h>  /* Add this for task_cpu_possible */
#include <linux/cpumask.h>
#include "sched.h"  /* Include scheduler internals */
#include "wfq.h"
#define MAX_CPUS 8

struct wfq_info {
    int num_cpus;
    int nr_running[MAX_CPUS];
    int total_weight[MAX_CPUS];
};

/*
 * get_wfq_info - Get WFQ scheduler information
 * @wfq_info: user space pointer to wfq_info structure
 *
 * Returns: number of CPUs on success, negative error code on failure
 */
SYSCALL_DEFINE1(get_wfq_info, struct wfq_info __user *, wfq_info)
{
    struct wfq_info info;
    struct rq *rq;
    int cpu;
    int online_cpus;
    struct rq_flags rf;  /* Use struct rq_flags instead of unsigned long */
    
    if (!wfq_info)
        return -EINVAL;
    
    /* Initialize the structure */
    memset(&info, 0, sizeof(info));
    
    /* Get number of online CPUs */
    online_cpus = num_online_cpus();
    info.num_cpus = (online_cpus > MAX_CPUS) ? MAX_CPUS : online_cpus;
    
    /* Collect data from each CPU's WFQ runqueue with proper locking */
    for_each_online_cpu(cpu) {
        if (cpu >= MAX_CPUS)
            break;
            
        /* Get the runqueue for this CPU */
        rq = cpu_rq(cpu);
        
        /* Lock the runqueue - using struct rq_flags */
        rq_lock_irqsave(rq, &rf);
        
        /* Read WFQ data while holding the lock */
        info.nr_running[cpu] = rq->wfq.wfq_nr_running;
        
        /* Safe conversion of u64 to int */
        if (rq->wfq.cpu_total_weight > INT_MAX)
            info.total_weight[cpu] = INT_MAX;
        else
            info.total_weight[cpu] = (int)rq->wfq.cpu_total_weight;
        
        /* Release the lock */
        rq_unlock_irqrestore(rq, &rf);
    }
    
    /* Copy to user space */
    if (copy_to_user(wfq_info, &info, sizeof(info)))
        return -EFAULT;
    
    return info.num_cpus;
}

/*
 * set_wfq_weight - Set WFQ weight for current process
 * @weight: new weight value
 *
 * Returns: 0 on success, negative error code on failure
 */
SYSCALL_DEFINE1(set_wfq_weight, int, weight)
{
    struct task_struct *p = current;
    struct sched_wfq_entity *se = &p->wfq;
    struct rq *rq;
    struct rq_flags rf;

    /* Validate weight range */
    if (weight < 1)
        return -EINVAL;

    /* Check permissions - only root can increase weight beyond default */
    if (weight > WFQ_DEFAULT_WEIGHT && !capable(CAP_SYS_NICE))
        return -EPERM;

    /* Only allow for WFQ tasks */
    if (p->policy != SCHED_WFQ)
        return -EINVAL;

    /* Get the runqueue and lock it */
    rq = task_rq_lock(p, &rf);

    /* Update current task's virtual time using OLD weight before changing it */
    update_curr_wfq(rq);

    /* If task is queued, update CPU weight accounting */
    if (task_on_rq_queued(p) && !RB_EMPTY_NODE(&se->run_node)) {
        struct wfq_rq *wfq_rq = &rq->wfq;

        /* Remove old weight from CPU total */
        wfq_rq->cpu_total_weight -= se->weight;

        /* Update the task's weight */
        se->weight = weight;
        se->inv_weight = WFQ_SCALE_FACTOR / weight;

        /* Add new weight to CPU total */
        wfq_rq->cpu_total_weight += se->weight;
    } else {
        /* Task not queued, just update weight */
        se->weight = weight;
        se->inv_weight = WFQ_SCALE_FACTOR / weight;
    }

    task_rq_unlock(rq, p, &rf);

    return 0;
}

