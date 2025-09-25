/*
 * WFS System Calls Implementation
 * File: kernel/sched/wfs_syscalls.c
 */

#include <linux/syscalls.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <linux/capability.h>
#include <linux/errno.h>
#include <linux/mmu_context.h>  /* Add this for task_cpu_possible */
#include <linux/cpumask.h>
#include "sched.h"  /* Include scheduler internals */
#include "wfs.h"
#define MAX_CPUS 8

struct wfs_info {
    int num_cpus;
    int nr_running[MAX_CPUS];
    int total_weight[MAX_CPUS];
};

/*
 * get_wfs_info - Get WFS scheduler information
 * @wfs_info: user space pointer to wfs_info structure
 *
 * Returns: number of CPUs on success, negative error code on failure
 */
SYSCALL_DEFINE1(get_wfs_info, struct wfs_info __user *, wfs_info)
{
    struct wfs_info info;
    struct rq *rq;
    int cpu;
    int online_cpus;
    struct rq_flags rf;  /* Use struct rq_flags instead of unsigned long */
    
    if (!wfs_info)
        return -EINVAL;
    
    /* Initialize the structure */
    memset(&info, 0, sizeof(info));
    
    /* Get number of online CPUs */
    online_cpus = num_online_cpus();
    info.num_cpus = (online_cpus > MAX_CPUS) ? MAX_CPUS : online_cpus;
    
    /* Collect data from each CPU's WFS runqueue with proper locking */
    for_each_online_cpu(cpu) {
        if (cpu >= MAX_CPUS)
            break;
            
        /* Get the runqueue for this CPU */
        rq = cpu_rq(cpu);
        
        /* Lock the runqueue - using struct rq_flags */
        rq_lock_irqsave(rq, &rf);
        
        /* Read WFS data while holding the lock */
        info.nr_running[cpu] = rq->wfs.wfs_nr_running;
        
        /* Safe conversion of u64 to int */
        if (rq->wfs.cpu_total_weight > INT_MAX)
            info.total_weight[cpu] = INT_MAX;
        else
            info.total_weight[cpu] = (int)rq->wfs.cpu_total_weight;
        
        /* Release the lock */
        rq_unlock_irqrestore(rq, &rf);
    }
    
    /* Copy to user space */
    if (copy_to_user(wfs_info, &info, sizeof(info)))
        return -EFAULT;
    
    return info.num_cpus;
}

/*
 * set_wfs_weight - Set WFS weight for current process
 * @weight: new weight value
 *
 * Returns: 0 on success, negative error code on failure
 */
SYSCALL_DEFINE1(set_wfs_weight, int, weight)
{
    struct task_struct *p = current;
    struct sched_wfs_entity *se = &p->wfs;
    struct rq *rq;
    struct rq_flags rf;

    /* Validate weight range */
    if (weight < 1)
        return -EINVAL;

    /* Check permissions - only root can increase weight beyond default */
    if (weight > WFS_DEFAULT_WEIGHT && !capable(CAP_SYS_NICE))
        return -EPERM;

    /* Only allow for WFS tasks */
    if (p->policy != SCHED_WFS)
        return -EINVAL;

    /* Get the runqueue and lock it */
    rq = task_rq_lock(p, &rf);

    /* If task is queued, update CPU weight accounting */
    if (task_on_rq_queued(p) && !RB_EMPTY_NODE(&se->run_node)) {
        struct wfs_rq *wfs_rq = &rq->wfs;
        
        /* Remove old weight from CPU total */
        wfs_rq->cpu_total_weight -= se->weight;
        
        /* Update the task's weight */
        se->weight = weight;
        se->inv_weight = WFS_SCALE_FACTOR / weight;
        
        /* Add new weight to CPU total */
        wfs_rq->cpu_total_weight += se->weight;
    } else {
        /* Task not queued, just update weight */
        se->weight = weight;
        se->inv_weight = WFS_SCALE_FACTOR / weight;
    }
    update_curr_wfs(rq);

    task_rq_unlock(rq, p, &rf);

    return 0;
}
