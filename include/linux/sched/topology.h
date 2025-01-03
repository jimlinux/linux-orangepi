/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SCHED_TOPOLOGY_H
#define _LINUX_SCHED_TOPOLOGY_H

#include <linux/topology.h>
#include <linux/android_kabi.h>
#include <linux/android_vendor.h>

#include <linux/sched/idle.h>

/*
 * sched-domains (multiprocessor balancing) declarations:
 */
#ifdef CONFIG_SMP

/* Generate SD flag indexes */
#define SD_FLAG(name, mflags) __##name,
enum {
	#include <linux/sched/sd_flags.h>
	__SD_FLAG_CNT,
};
#undef SD_FLAG
/* Generate SD flag bits */
#define SD_FLAG(name, mflags) name = 1 << __##name,
enum {
	#include <linux/sched/sd_flags.h>
};
#undef SD_FLAG

#ifdef CONFIG_SCHED_DEBUG

struct sd_flag_debug {
	unsigned int meta_flags;
	char *name;
};
extern const struct sd_flag_debug sd_flag_debug[];

#endif

#ifdef CONFIG_SCHED_SMT
static inline int cpu_smt_flags(void)
{
	return SD_SHARE_CPUCAPACITY | SD_SHARE_PKG_RESOURCES;
}
#endif

#ifdef CONFIG_SCHED_MC
static inline int cpu_core_flags(void)
{
	return SD_SHARE_PKG_RESOURCES;
}
#endif

#ifdef CONFIG_NUMA
static inline int cpu_numa_flags(void)
{
	return SD_NUMA;
}
#endif

extern int arch_asym_cpu_priority(int cpu);

struct sched_domain_attr {
	int relax_domain_level;
};

#define SD_ATTR_INIT	(struct sched_domain_attr) {	\
	.relax_domain_level = -1,			\
}

extern int sched_domain_level_max;

struct sched_group;

struct sched_domain_shared {
	atomic_t	ref;
	atomic_t	nr_busy_cpus;
	int		has_idle_cores;

	ANDROID_VENDOR_DATA(1);
};

struct sched_domain {
	/* These fields must be setup */
	// MC sd->parent == DIE sd
	struct sched_domain __rcu *parent;	/* top domain must be null terminated */
	// DIE sd->child == MC sd
	struct sched_domain __rcu *child;	/* bottom domain must be null terminated */
	// 调度组
	struct sched_group *groups;	/* the balancing groups of the domain */
	// 定义检查该sched domain均衡状态的时间间隔范围
	unsigned long min_interval;	/* Minimum balance interval ms */
	unsigned long max_interval;	/* Maximum balance interval ms */
	// 如果cpu繁忙，那么均衡要时间间隔长一些，即时间间隔定义为busy_factor * balance_interval
	unsigned int busy_factor;	/* less balancing by factor if busy */
	// 定义判定不均衡的水位线
	unsigned int imbalance_pct;	/* No balance until over watermark */
	// 和nr_balance_failed配合控制负载均衡过程的迁移力度。
	// 当nr_balance_failed大于cache_nice_tries的时候，负载均衡会变得更加激进。
	unsigned int cache_nice_tries;	/* Leave cache hot tasks for # tries */

	int nohz_idle;			/* NOHZ IDLE status */
	// 调度域标志, sched/sd_flags.h定义
	int flags;			/* See SD_* */
	// Base sched domain的level等于0，向上依次加一
	int level;

	/* Runtime fields. */
	// 上次进行balance的时间点
	unsigned long last_balance;	/* init to jiffies. units in jiffies */
	// 该sched domain均衡的基础时间间隔
	unsigned int balance_interval;	/* initialise to 1. units in ms. */
	// 本sched domain中进行负载均衡失败的次数。
	// 当失败次数大于cache_nice_tries的时候，我们考虑迁移cache hot的任务，进行更激进的均衡操作
	unsigned int nr_balance_failed; /* initialise to 0 */

	/* idle_balance() stats */
	// 在该domain上进行newidle balance的最大耗时（即newidle balance的开销）
	u64 max_newidle_lb_cost;
	// max_newidle_lb_cost会记录最近在该sched domain上进行newidle balance的最大耗时，
	// 这个max cost不是一成不变的，它有一个衰减过程，每秒衰减1%，这个成员就是用来控制衰减的。
	unsigned long next_decay_max_lb_cost;

	// 平均扫描成本
	u64 avg_scan_cost;		/* select_idle_sibling */

	// 负载均衡的统计信息
#ifdef CONFIG_SCHEDSTATS
	/* load_balance() stats */
	unsigned int lb_count[CPU_MAX_IDLE_TYPES];
	unsigned int lb_failed[CPU_MAX_IDLE_TYPES];
	unsigned int lb_balanced[CPU_MAX_IDLE_TYPES];
	unsigned int lb_imbalance[CPU_MAX_IDLE_TYPES];
	unsigned int lb_gained[CPU_MAX_IDLE_TYPES];
	unsigned int lb_hot_gained[CPU_MAX_IDLE_TYPES];
	unsigned int lb_nobusyg[CPU_MAX_IDLE_TYPES];
	unsigned int lb_nobusyq[CPU_MAX_IDLE_TYPES];

	/* Active load balancing */
	unsigned int alb_count;
	unsigned int alb_failed;
	unsigned int alb_pushed;

	/* SD_BALANCE_EXEC stats */
	unsigned int sbe_count;
	unsigned int sbe_balanced;
	unsigned int sbe_pushed;

	/* SD_BALANCE_FORK stats */
	unsigned int sbf_count;
	unsigned int sbf_balanced;
	unsigned int sbf_pushed;

	/* try_to_wake_up() stats */
	unsigned int ttwu_wake_remote;
	unsigned int ttwu_move_affine;
	unsigned int ttwu_move_balance;
#endif
#ifdef CONFIG_SCHED_DEBUG
	char *name;  // MC or DIE
#endif
	union {
		void *private;		/* used during construction */
		struct rcu_head rcu;	/* used during destruction */
	};
	// Sched domain是per-CPU的，然而有一些信息是需要在per-CPU 的sched domain之间共享的，
	// 不能在每个sched domain上构建。这些共享信息是：
	// 1、该sched domain中的busy cpu的个数
	// 2、该sched domain中是否有idle的cpu
	struct sched_domain_shared *shared;

	// 该调度域下包含的cpu数量
	unsigned int span_weight;

	ANDROID_KABI_RESERVE(1);
	ANDROID_KABI_RESERVE(2);

	/*
	 * Span of all CPUs in this domain.
	 *
	 * NOTE: this field is variable length. (Allocated dynamically
	 * by attaching extra space to the end of the structure,
	 * depending on how many CPUs the kernel has booted up with)
	 */
	// 该调度域下包含的cpu号，比如一个MC调度域，cpu6,cpu7 则值为0xc0 （11000000）
	// 如果是DIE调度域，则包含全部，0xff
	unsigned long span[];
};

static inline struct cpumask *sched_domain_span(struct sched_domain *sd)
{
	return to_cpumask(sd->span);
}

extern void partition_sched_domains_locked(int ndoms_new,
					   cpumask_var_t doms_new[],
					   struct sched_domain_attr *dattr_new);

extern void partition_sched_domains(int ndoms_new, cpumask_var_t doms_new[],
				    struct sched_domain_attr *dattr_new);

/* Allocate an array of sched domains, for partition_sched_domains(). */
cpumask_var_t *alloc_sched_domains(unsigned int ndoms);
void free_sched_domains(cpumask_var_t doms[], unsigned int ndoms);

bool cpus_share_cache(int this_cpu, int that_cpu);

typedef const struct cpumask *(*sched_domain_mask_f)(int cpu);
typedef int (*sched_domain_flags_f)(void);

#define SDTL_OVERLAP	0x01

struct sd_data {
	struct sched_domain *__percpu *sd;
	struct sched_domain_shared *__percpu *sds;
	struct sched_group *__percpu *sg;
	struct sched_group_capacity *__percpu *sgc;
};

struct sched_domain_topology_level {
	sched_domain_mask_f mask;
	sched_domain_flags_f sd_flags;
	int		    flags;
	int		    numa_level;
	struct sd_data      data;
#ifdef CONFIG_SCHED_DEBUG
	char                *name;
#endif
};

extern void set_sched_topology(struct sched_domain_topology_level *tl);

#ifdef CONFIG_SCHED_DEBUG
# define SD_INIT_NAME(type)		.name = #type
#else
# define SD_INIT_NAME(type)
#endif

#else /* CONFIG_SMP */

struct sched_domain_attr;

static inline void
partition_sched_domains_locked(int ndoms_new, cpumask_var_t doms_new[],
			       struct sched_domain_attr *dattr_new)
{
}

static inline void
partition_sched_domains(int ndoms_new, cpumask_var_t doms_new[],
			struct sched_domain_attr *dattr_new)
{
}

static inline bool cpus_share_cache(int this_cpu, int that_cpu)
{
	return true;
}

#endif	/* !CONFIG_SMP */

#ifndef arch_scale_cpu_capacity
/**
 * arch_scale_cpu_capacity - get the capacity scale factor of a given CPU.
 * @cpu: the CPU in question.
 *
 * Return: the CPU scale factor normalized against SCHED_CAPACITY_SCALE, i.e.
 *
 *             max_perf(cpu)
 *      ----------------------------- * SCHED_CAPACITY_SCALE
 *      max(max_perf(c) : c \in CPUs)
 */
static __always_inline
unsigned long arch_scale_cpu_capacity(int cpu)
{
	return SCHED_CAPACITY_SCALE;
}
#endif

#ifndef arch_scale_thermal_pressure
static __always_inline
unsigned long arch_scale_thermal_pressure(int cpu)
{
	return 0;
}
#endif

#ifndef arch_set_thermal_pressure
static __always_inline
void arch_set_thermal_pressure(const struct cpumask *cpus,
			       unsigned long th_pressure)
{ }
#endif

static inline int task_node(const struct task_struct *p)
{
	return cpu_to_node(task_cpu(p));
}

#endif /* _LINUX_SCHED_TOPOLOGY_H */
