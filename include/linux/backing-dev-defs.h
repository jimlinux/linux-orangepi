/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_BACKING_DEV_DEFS_H
#define __LINUX_BACKING_DEV_DEFS_H

#include <linux/list.h>
#include <linux/radix-tree.h>
#include <linux/rbtree.h>
#include <linux/spinlock.h>
#include <linux/percpu_counter.h>
#include <linux/percpu-refcount.h>
#include <linux/flex_proportions.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <linux/kref.h>
#include <linux/refcount.h>
#include <linux/android_kabi.h>

struct page;
struct device;
struct dentry;

/*
 * Bits in bdi_writeback.state
 */
enum wb_state {
	WB_registered,		/* bdi_register() was done */
	WB_writeback_running,	/* Writeback is in progress */
	WB_has_dirty_io,	/* Dirty inodes on ->b_{dirty|io|more_io} */
	WB_start_all,		/* nr_pages == 0 (all) work pending */
};

enum wb_congested_state {
	WB_async_congested,	/* The async (write) queue is getting full */
	WB_sync_congested,	/* The sync queue is getting full */
};

enum wb_stat_item {
	WB_RECLAIMABLE,
	WB_WRITEBACK,
	WB_DIRTIED,
	WB_WRITTEN,
	NR_WB_STAT_ITEMS
};

#define WB_STAT_BATCH (8*(1+ilog2(nr_cpu_ids)))

/*
 * why some writeback work was initiated
 */
enum wb_reason {
	WB_REASON_BACKGROUND,
	WB_REASON_VMSCAN,
	WB_REASON_SYNC,
	WB_REASON_PERIODIC,
	WB_REASON_LAPTOP_TIMER,
	WB_REASON_FS_FREE_SPACE,
	/*
	 * There is no bdi forker thread any more and works are done
	 * by emergency worker, however, this is TPs userland visible
	 * and we'll be exposing exactly the same information,
	 * so it has a mismatch name.
	 */
	WB_REASON_FORKER_THREAD,
	WB_REASON_FOREIGN_FLUSH,

	WB_REASON_MAX,
};

struct wb_completion {
	atomic_t		cnt;
	wait_queue_head_t	*waitq;
};

#define __WB_COMPLETION_INIT(_waitq)	\
	(struct wb_completion){ .cnt = ATOMIC_INIT(1), .waitq = (_waitq) }

/*
 * If one wants to wait for one or more wb_writeback_works, each work's
 * ->done should be set to a wb_completion defined using the following
 * macro.  Once all work items are issued with wb_queue_work(), the caller
 * can wait for the completion of all using wb_wait_for_completion().  Work
 * items which are waited upon aren't freed automatically on completion.
 */
#define WB_COMPLETION_INIT(bdi)		__WB_COMPLETION_INIT(&(bdi)->wb_waitq)

#define DEFINE_WB_COMPLETION(cmpl, bdi)	\
	struct wb_completion cmpl = WB_COMPLETION_INIT(bdi)

/*
 * Each wb (bdi_writeback) can perform writeback operations, is measured
 * and throttled, independently.  Without cgroup writeback, each bdi
 * (bdi_writeback) is served by its embedded bdi->wb.
 *
 * On the default hierarchy, blkcg implicitly enables memcg.  This allows
 * using memcg's page ownership for attributing writeback IOs, and every
 * memcg - blkcg combination can be served by its own wb by assigning a
 * dedicated wb to each memcg, which enables isolation across different
 * cgroups and propagation of IO back pressure down from the IO layer upto
 * the tasks which are generating the dirty pages to be written back.
 *
 * A cgroup wb is indexed on its bdi by the ID of the associated memcg,
 * refcounted with the number of inodes attached to it, and pins the memcg
 * and the corresponding blkcg.  As the corresponding blkcg for a memcg may
 * change as blkcg is disabled and enabled higher up in the hierarchy, a wb
 * is tested for blkcg after lookup and removed from index on mismatch so
 * that a new wb for the combination can be created.
 */
/*
 * bdi_writeback: bdi回写相关数据
 * cgroup writeback disable：bdi与wb为一对一
 * cgroup writeback enable：一个bdi可以有多个wb
 * cgroup writeback feature用来支持cgroup回写限速, 比较复杂暂不讨论
 * 参考：https://www.alibabacloud.com/help/zh/alinux/user-guide/enable-the-cgroup-writeback-feature
 * 需要memcg和blkcg协同工作
 * memcg和blkcg规则：
 * 1. 一对一：属于同一个memcg的进程A、B，只能映射到同一个blkcg
 * 2. 多对一：属于不同memcg的进程A、B，可以映射到同一个blkcg，也可以映射到不同blkcg
 * 3. 一个memcg - blkcg combination 对应一个cgroup wb
*/
struct bdi_writeback {
	struct backing_dev_info *bdi;	/* our parent bdi */

	unsigned long state;		/* Always use atomic bitops on this */
	unsigned long last_old_flush;	/* last old data flush */

	struct list_head b_dirty;	/* dirty inodes */
	struct list_head b_io;		/* parked for writeback */
	struct list_head b_more_io;	/* parked for more writeback */
	struct list_head b_dirty_time;	/* time stamps are dirty */
	spinlock_t list_lock;		/* protects the b_* lists */

	struct percpu_counter stat[NR_WB_STAT_ITEMS];

	unsigned long congested;	/* WB_[a]sync_congested flags */

	// 上次update bandwidth时间戳
	unsigned long bw_time_stamp;	/* last time write bw is updated */
	// 上次update bandwidth时，dirtied值
	unsigned long dirtied_stamp;
	// 上次update bandwidth时，written值
	unsigned long written_stamp;	/* pages written at bw_time_stamp */
	// 回写带宽
	unsigned long write_bandwidth;	/* the estimated write bandwidth */
	// 平滑后的回写带宽
	unsigned long avg_write_bandwidth; /* further smoothed write bw, > 0 */

	/*
	 * The base dirty throttle rate, re-calculated on every 200ms.
	 * All the bdi tasks' dirty rate will be curbed under it.
	 * @dirty_ratelimit tracks the estimated @balanced_dirty_ratelimit
	 * in small steps and is much more smooth/stable than the latter.
	 */
	// 平滑后的限速值，单位pages/s
	unsigned long dirty_ratelimit;
	// 原始限速值
	unsigned long balanced_dirty_ratelimit;

	/*
	 * percpu计数，回写完成一个page，+1
	 * 注意，计数先累加到percpu的count，当超过max_prop_frac后才会更新到总count，取总count有一定误差；
	 * completions不一定递增的，为了保证时效性，会随根据设置的周期衰减
	*/
	struct fprop_local_percpu completions;
	// dirty pages数是否超过thresh，超过后会更频繁调用balance_dirty_pages
	int dirty_exceeded;
	enum wb_reason start_all_reason;

	spinlock_t work_lock;		/* protects work_list & dwork scheduling */
	struct list_head work_list;
	// dwork：干活的，在这里执行回写
	struct delayed_work dwork;	/* work item used for writeback */

	unsigned long dirty_sleep;	/* last wait */

	struct list_head bdi_node;	/* anchored at bdi->wb_list */

#ifdef CONFIG_CGROUP_WRITEBACK
	struct percpu_ref refcnt;	/* used only for !root wb's */
	struct fprop_local_percpu memcg_completions;
	struct cgroup_subsys_state *memcg_css; /* the associated memcg */
	struct cgroup_subsys_state *blkcg_css; /* and blkcg */
	struct list_head memcg_node;	/* anchored at memcg->cgwb_list */
	struct list_head blkcg_node;	/* anchored at blkcg->cgwb_list */

	union {
		struct work_struct release_work;
		struct rcu_head rcu;
	};
#endif

	ANDROID_KABI_RESERVE(1);
	ANDROID_KABI_RESERVE(2);
};

/*
表示一个块设备对应的脏页回写相关的信息
对bdi的操作接口在backing-dev.c中实现：
bdi_alloc：alloc bdi设备并初始化bdi
bdi_register：注册，把bdi插入到bdi_tree，bdi_list中
bdi_put：dec bdi refcnt，为0后release bdi
bdi_dev_name：获取bdi name
min_ratio_store: 设置min ratio
max_ratio_store: 设置max ratio
*/
struct backing_dev_info {
	u64 id;
	// rb_node、bdi_list用于链接到红黑树和链表中
	struct rb_node rb_node; /* keyed by ->id */
	struct list_head bdi_list;
	unsigned long ra_pages;	/* max readahead in PAGE_SIZE units */
	unsigned long io_pages;	/* max allowed IO size */

	struct kref refcnt;	/* Reference counter for the structure */
	// BDI_CAP_WRITEBACK，BDI_CAP_WRITEBACK_ACCT，BDI_CAP_STRICTLIMIT
	unsigned int capabilities; /* Device capabilities */
	// 记录min_ratio，max_ratio
	unsigned int min_ratio;
	unsigned int max_ratio, max_prop_frac;

	/*
	 * Sum of avg_write_bw of wbs with dirty inodes.  > 0 if there are
	 * any dirty wbs, which is depended upon by bdi_has_dirty().
	 */
	// bdi下所有wbs avg_write_bw之和
	atomic_long_t tot_write_bandwidth;

	/* 
	 * 在cgroup writeback enable时，存在一个bdi下有多个cgwb情况，这些wbs链接到wb_list
	 * disable时，bdi和wb是一对一关系；
	 */
	struct bdi_writeback wb;  /* the root writeback info for this bdi */
	struct list_head wb_list; /* list of all wbs */
#ifdef CONFIG_CGROUP_WRITEBACK
	struct radix_tree_root cgwb_tree; /* radix tree of active cgroup wbs */
	struct mutex cgwb_release_mutex;  /* protect shutdown of wb structs */
	struct rw_semaphore wb_switch_rwsem; /* no cgwb switch while syncing */
#endif
	wait_queue_head_t wb_waitq;

	struct device *dev;
	char dev_name[64];  //bdi dev名
	struct device *owner;

	struct timer_list laptop_mode_wb_timer;

#ifdef CONFIG_DEBUG_FS
	struct dentry *debug_dir;
#endif

	ANDROID_KABI_RESERVE(1);
	ANDROID_KABI_RESERVE(2);
};

enum {
	BLK_RW_ASYNC	= 0,
	BLK_RW_SYNC	= 1,
};

void clear_bdi_congested(struct backing_dev_info *bdi, int sync);
void set_bdi_congested(struct backing_dev_info *bdi, int sync);

struct wb_lock_cookie {
	bool locked;
	unsigned long flags;
};

#ifdef CONFIG_CGROUP_WRITEBACK

/**
 * wb_tryget - try to increment a wb's refcount
 * @wb: bdi_writeback to get
 */
static inline bool wb_tryget(struct bdi_writeback *wb)
{
	if (wb != &wb->bdi->wb)
		return percpu_ref_tryget(&wb->refcnt);
	return true;
}

/**
 * wb_get - increment a wb's refcount
 * @wb: bdi_writeback to get
 */
static inline void wb_get(struct bdi_writeback *wb)
{
	if (wb != &wb->bdi->wb)
		percpu_ref_get(&wb->refcnt);
}

/**
 * wb_put - decrement a wb's refcount
 * @wb: bdi_writeback to put
 */
static inline void wb_put(struct bdi_writeback *wb)
{
	if (WARN_ON_ONCE(!wb->bdi)) {
		/*
		 * A driver bug might cause a file to be removed before bdi was
		 * initialized.
		 */
		return;
	}

	if (wb != &wb->bdi->wb)
		percpu_ref_put(&wb->refcnt);
}

/**
 * wb_dying - is a wb dying?
 * @wb: bdi_writeback of interest
 *
 * Returns whether @wb is unlinked and being drained.
 */
static inline bool wb_dying(struct bdi_writeback *wb)
{
	return percpu_ref_is_dying(&wb->refcnt);
}

#else	/* CONFIG_CGROUP_WRITEBACK */

static inline bool wb_tryget(struct bdi_writeback *wb)
{
	return true;
}

static inline void wb_get(struct bdi_writeback *wb)
{
}

static inline void wb_put(struct bdi_writeback *wb)
{
}

static inline bool wb_dying(struct bdi_writeback *wb)
{
	return false;
}

#endif	/* CONFIG_CGROUP_WRITEBACK */

#endif	/* __LINUX_BACKING_DEV_DEFS_H */
