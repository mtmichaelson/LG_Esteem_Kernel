/* drivers/misc/lowmemorykiller.c
 *
 * The lowmemorykiller driver lets user-space specify a set of memory thresholds
 * where processes with a range of oom_adj values will get killed. Specify the
 * minimum oom_adj values in /sys/module/lowmemorykiller/parameters/adj and the
 * number of free pages in /sys/module/lowmemorykiller/parameters/minfree. Both
 * files take a comma separated list of numbers in ascending order.
 *
 * For example, write "0,8" to /sys/module/lowmemorykiller/parameters/adj and
 * "1024,4096" to /sys/module/lowmemorykiller/parameters/minfree to kill processes
 * with a oom_adj value of 8 or higher when the free memory drops below 4096 pages
 * and kill processes with a oom_adj value of 0 or higher when the free memory
 * drops below 1024 pages.
 *
 * The driver considers memory used for caches to be free, but if a large
 * percentage of the cached memory is locked this can be very inaccurate
 * and processes may not get killed until the normal oom killer is triggered.
 *
 * Copyright (C) 2007-2008 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/oom.h>
#include <linux/sched.h>
#include <linux/notifier.h>

static uint32_t lowmem_debug_level = 2;
static int lowmem_adj[6] = {
	0,
	1,
	6,
	12,
};
static int lowmem_adj_size = 4;
static size_t lowmem_minfree[6] = {
	3 * 512,	/* 6MB */
	2 * 1024,	/* 8MB */
	4 * 1024,	/* 16MB */
	16 * 1024,	/* 64MB */
};
static int lowmem_minfree_size = 4;

static struct task_struct *lowmem_deathpending;
static DEFINE_SPINLOCK(lowmem_deathpending_lock);

#define lowmem_print(level, x...)			\
	do {						\
		if (lowmem_debug_level >= (level))	\
			printk(x);			\
	} while (0)

static int
task_notify_func(struct notifier_block *self, unsigned long val, void *data);

static struct notifier_block task_nb = {
	.notifier_call	= task_notify_func,
};


static void task_free_fn(struct work_struct *work)
{
	unsigned long flags;

	task_free_unregister(&task_nb);
	spin_lock_irqsave(&lowmem_deathpending_lock, flags);
	lowmem_deathpending = NULL;
	spin_unlock_irqrestore(&lowmem_deathpending_lock, flags);
}
static DECLARE_WORK(task_free_work, task_free_fn);

static int
task_notify_func(struct notifier_block *self, unsigned long val, void *data)
{
	struct task_struct *task = data;

	if (task == lowmem_deathpending) {
		schedule_work(&task_free_work);
	}
	return NOTIFY_OK;
}

#ifdef CONFIG_LGE_ANDROID_LOW_MEMORY_KILLER_PATCH
#define K(pages) ((pages) << (PAGE_SHIFT - 10))
extern unsigned long global_reclaimable_pages(void);
extern unsigned long zone_reclaimable_pages(struct zone *zone);
extern int min_free_kbytes;

unsigned long get_total_present_pages(void)
{
	int nid, zone_type;
	unsigned long total_size = 0;
	struct zone *z;
	
	for_each_online_node(nid) {
		for (zone_type = 0; zone_type < MAX_NR_ZONES; zone_type++) {
			z = &NODE_DATA(nid)->node_zones[zone_type];
			if (populated_zone(z)) {
				total_size += z->present_pages;
			}
		}
	}

	return total_size;
}

int check_oom_boundary(struct zone *zone, int array_size)
{
	int i;
	int zone_free = zone_page_state(zone, NR_FREE_PAGES);
	int zone_file = zone_page_state(zone, NR_FILE_PAGES) - zone_page_state(zone, NR_SHMEM);
	//int zone_reclaimable = zone_reclaimable_pages(zone);
	unsigned long zone_present = zone->present_pages;
	unsigned long total_present =  get_total_present_pages();
	//int min_free_page = min_free_kbytes>> (PAGE_SHIFT - 10);
	int oom_adj = OOM_ADJUST_MAX + 1;

	int proportional_min_free = 0;


	for (i = 0; i < array_size; i++) {
		// calculate proportioanl minfree to selecet specific value for eache zone
		proportional_min_free = (int)(((unsigned long)lowmem_minfree[i]*zone_present)/total_present);
		if ((zone_free < proportional_min_free) && (zone_file < proportional_min_free)){
			// select one belower than the proper adj since it would have already been selected in normal cases.
			if(i > 1)
				oom_adj = lowmem_adj[i-1];
			else
				oom_adj = lowmem_adj[0];
			break;
		}
	}

	lowmem_print(3, "oom_adj : %d, prop_min:%lukB, free:%lukB, file_pages - shmem :%lukB\n", oom_adj, K(proportional_min_free), K(zone_free), K(zone_file));

	return oom_adj;
}
#endif

static int lowmem_shrink(struct shrinker *s, int nr_to_scan, gfp_t gfp_mask)
{
	struct task_struct *p;
	struct task_struct *selected = NULL;
	int rem = 0;
	int tasksize;
	int i;
	int min_adj = OOM_ADJUST_MAX + 1;
	int selected_tasksize = 0;
	int selected_oom_adj;
	int array_size = ARRAY_SIZE(lowmem_adj);
	int other_free = global_page_state(NR_FREE_PAGES);
#ifdef CONFIG_LGE_ANDROID_LOW_MEMORY_KILLER_PATCH
	struct zone *zone;
	int zone_reclaimable = 0;
	int oom_boundary_adj = OOM_ADJUST_MAX + 1;

	// CAF patch for tmpfs -  NR_SHMEM pages are not be able to be used as NR_FILE_PAGES.
	int other_file = global_page_state(NR_FILE_PAGES) - global_page_state(NR_SHMEM);

	/*
	 * to avoid too many oom-killer, check reclaimable page condition
	 * Although lowmemorykiller got killed apks or selected adj well under the given condition, oom-killer is followed by sometime.
	 */
	int reclaimable_pages = global_reclaimable_pages();


	gfp_t gfp_mask_zone = gfp_mask & GFP_ZONEMASK;
	int rem_normal = 0, rem_movable = 0;
	
#else
	int other_file = global_page_state(NR_FILE_PAGES);
#endif
	unsigned long flags;

	/*
	 * If we already have a death outstanding, then
	 * bail out right away; indicating to vmscan
	 * that we have nothing further to offer on
	 * this pass.
	 *
	 */
	if (lowmem_deathpending)
		return 0;

	if (lowmem_adj_size < array_size)
		array_size = lowmem_adj_size;
	if (lowmem_minfree_size < array_size)
		array_size = lowmem_minfree_size;
	for (i = 0; i < array_size; i++) {
/* CR 292543 - General Performance
 * Fix check for other_free pages in lowmemorykiller
 * 
 * Problem description
 * The lowmemorykiller file checks only for file memory and does not check
 * for free memory. In that case lowmemorykiller will kill processes although there is enough memory available.
 * 
 * Failure frequency: Occasionally
 * Scenario frequency: Uncommon
 * Change description
 * Low memory killer now check for the file memory and also for free mempry pages available.
 * Files affected
 * kernel/drivers/staging/android/lowmemorykiller.c
*/
#if 1 //QCT SBA 404015
#ifdef CONFIG_LGE_ANDROID_LOW_MEMORY_KILLER_PATCH
		if (((other_free < lowmem_minfree[i]) && (other_file < lowmem_minfree[i])) 
			||((other_free < lowmem_minfree[i]) && (reclaimable_pages < lowmem_minfree[i]))){
			min_adj = lowmem_adj[i];
			break;
		}
#else
		if (other_free < lowmem_minfree[i] &&
				other_file < lowmem_minfree[i]) {
			min_adj = lowmem_adj[i];
			break;
		}
#endif	
#else
		if (other_file < lowmem_minfree[i]) {
			min_adj = lowmem_adj[i];
			break;
		}
#endif
	}

#ifdef CONFIG_LGE_ANDROID_LOW_MEMORY_KILLER_PATCH
	/* in rare cases, proper adj would not be selected well especially there're avaliable memory in ZONE_MOVABLE.
	 * adj will be calculated again by using propotional minfree of each zone.
	 */
	for_each_populated_zone(zone) {
		zone_reclaimable = zone_reclaimable_pages(zone);
		if(zone_reclaimable < (min_free_kbytes>> (PAGE_SHIFT - 10)))
		{
			lowmem_print(3, "%s : reached almost out of memoy condition\n", zone->name);
			lowmem_print(3, "reclaimable :%lukB, min_free :%lukB\n", K(zone_reclaimable), min_free_kbytes);
			oom_boundary_adj = check_oom_boundary(zone, array_size);
			if(oom_boundary_adj < min_adj)
			{
				lowmem_print(3, "prev adj %d will be changed to %d\n", min_adj, oom_boundary_adj);
				min_adj = oom_boundary_adj;
			}
		}
	}

#endif

	if (nr_to_scan > 0)
		lowmem_print(3, "lowmem_shrink %d, %x, ofree %d %d, ma %d\n",
			     nr_to_scan, gfp_mask, other_free, other_file,
			     min_adj);


#ifdef CONFIG_LGE_ANDROID_LOW_MEMORY_KILLER_PATCH
	for_each_populated_zone(zone) {
		if(!strncmp(zone->name, "Normal", strlen("Normal") ))
			rem_normal = zone_page_state(zone, NR_ACTIVE_ANON) +
				zone_page_state(zone, NR_ACTIVE_FILE) +
				zone_page_state(zone, NR_INACTIVE_ANON) +
				zone_page_state(zone, NR_INACTIVE_FILE);
		else if(!strncmp(zone->name, "Movable", strlen("Movable") ))
			rem_movable = zone_page_state(zone, NR_ACTIVE_ANON) +
				zone_page_state(zone, NR_ACTIVE_FILE) +
				zone_page_state(zone, NR_INACTIVE_ANON) +
				zone_page_state(zone, NR_INACTIVE_FILE);
		else
			rem += zone_page_state(zone, NR_ACTIVE_ANON) +
				zone_page_state(zone, NR_ACTIVE_FILE) +
				zone_page_state(zone, NR_INACTIVE_ANON) +
				zone_page_state(zone, NR_INACTIVE_FILE);
	}

	if((gfp_mask_zone & (~__GFP_MOVABLE)) == __GFP_MOVABLE)
	{
		rem += rem_movable;
	}
	else
	{
		rem += rem_normal;
	}

	if (nr_to_scan <= 0 || min_adj == OOM_ADJUST_MAX + 1) {
		lowmem_print(5, "lowmem_shrink %d, %x, return %d(%s)\n",
			     nr_to_scan, gfp_mask, rem, (gfp_mask_zone & (~__GFP_MOVABLE)) == __GFP_MOVABLE?"M":"N");
		return rem;
	}
#else
	rem = global_page_state(NR_ACTIVE_ANON) +
		global_page_state(NR_ACTIVE_FILE) +
		global_page_state(NR_INACTIVE_ANON) +
		global_page_state(NR_INACTIVE_FILE);
	if (nr_to_scan <= 0 || min_adj == OOM_ADJUST_MAX + 1) {
		lowmem_print(5, "lowmem_shrink %d, %x, return %d\n",
			     nr_to_scan, gfp_mask, rem);
		return rem;
	}
#endif

	selected_oom_adj = min_adj;

	read_lock(&tasklist_lock);
	for_each_process(p) {
		struct mm_struct *mm;
		struct signal_struct *sig;
		int oom_adj;

		task_lock(p);
		mm = p->mm;
		sig = p->signal;
		if (!mm || !sig) {
			task_unlock(p);
			continue;
		}
		oom_adj = sig->oom_adj;
		if (oom_adj < min_adj) {
			task_unlock(p);
			continue;
		}
		tasksize = get_mm_rss(mm);
		task_unlock(p);
		if (tasksize <= 0)
			continue;
		if (selected) {
			if (oom_adj < selected_oom_adj)
				continue;
			if (oom_adj == selected_oom_adj &&
			    tasksize <= selected_tasksize)
				continue;
		}
		selected = p;
		selected_tasksize = tasksize;
		selected_oom_adj = oom_adj;
		lowmem_print(2, "select %d (%s), adj %d, size %d, to kill\n",
			     p->pid, p->comm, oom_adj, tasksize);
	}

	if (selected) {
		spin_lock_irqsave(&lowmem_deathpending_lock, flags);
		if (!lowmem_deathpending) {
			lowmem_print(1,
				"send sigkill to %d (%s), adj %d, size %d\n",
				selected->pid, selected->comm,
				selected_oom_adj, selected_tasksize);
			lowmem_deathpending = selected;
			task_free_register(&task_nb);
			force_sig(SIGKILL, selected);
			rem -= selected_tasksize;
		}
		spin_unlock_irqrestore(&lowmem_deathpending_lock, flags);
	}
#ifdef CONFIG_LGE_ANDROID_LOW_MEMORY_KILLER_PATCH
	lowmem_print(4, "lowmem_shrink %d, %x, return %d(%s)\n",
		 nr_to_scan, gfp_mask, rem, (gfp_mask_zone & (~__GFP_MOVABLE)) == __GFP_MOVABLE?"M":"N");
#else
	lowmem_print(4, "lowmem_shrink %d, %x, return %d\n",
		     nr_to_scan, gfp_mask, rem);
#endif
	read_unlock(&tasklist_lock);
	return rem;
}

static struct shrinker lowmem_shrinker = {
	.shrink = lowmem_shrink,
	.seeks = DEFAULT_SEEKS * 16
};

static int __init lowmem_init(void)
{
	register_shrinker(&lowmem_shrinker);
	return 0;
}

static void __exit lowmem_exit(void)
{
	unregister_shrinker(&lowmem_shrinker);
}

module_param_named(cost, lowmem_shrinker.seeks, int, S_IRUGO | S_IWUSR);
module_param_array_named(adj, lowmem_adj, int, &lowmem_adj_size,
			 S_IRUGO | S_IWUSR);
module_param_array_named(minfree, lowmem_minfree, uint, &lowmem_minfree_size,
			 S_IRUGO | S_IWUSR);
module_param_named(debug_level, lowmem_debug_level, uint, S_IRUGO | S_IWUSR);

module_init(lowmem_init);
module_exit(lowmem_exit);

MODULE_LICENSE("GPL");

