/*
 * linux/arch/arm/mm/sys-cacheinfo.c
 *
 * sysfs support for exporting ARM cache information
 *
 * Copyright (C) 2011 National Instruments
 * Author: Gratian Crisan <gratian.crisan@ni.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * Implements cache information exporting for ARM trough sysfs under the
 * cpu subsystem.
 * An 'index<leaf number>' directory is created for each detected cache:
 *	/sys/devices/system/cpu/cpu<X>/cache/index<leaf>
 *
 * The directory contains the following files:
 *	level - integer value representing the cache level for this cache leaf
 *	type - string describing the cache type: "Instruction", "Data" etc.
 *	coherency_line_size
 *	physical_line_partition
 *	ways_of_associativity
 *	number_of_sets
 *	size - total cache size in bytes for this cache leaf
 *	flags - ARM specific cache flags not covered elsewhere:
 *			WT, WB, RA, WA flags for ARMv7
 *			CTR Ctype field for ARMv4-v6
 * This implementaion is based on linux/arch/x86/kernel/cpu/intel_cacheinfo.c
 */

#include <linux/init.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/compiler.h>
#include <linux/cpu.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>

#include <asm/system_info.h>
#include <asm/cputype.h>
#include <asm/cachetype.h>
#include <asm/cacheflush.h>

/* ARMv7 cache type CLIDR mask (CLIDR: cache level ID register)*/
#define ARM_V7_CLIDR_Ctype		(0x07)

/* ARMv7 cache size ID registers (CCSIDR) */
/* CCSIDR: WT, WB, RA, WA flags */
#define ARM_V7_CCSIDR_FLAGS(r)		((r >> 28) & 0x0F)
/* CCSIDR: (number of sets in cache) - 1 */
#define ARM_V7_CCSIDR_NumSets(r)	((r >> 13) & 0x7FFF)
/* CCSIDR: (associativity of cache) - 1 */
#define ARM_V7_CCSIDR_Associativity(r)	((r >> 3) & 0x3FF)
/* CCSIDR: (Log2(Number of words in cache line)) - 2 */
#define ARM_V7_CCSIDR_LineSize(r)	((r >> 0) & 0x07)

/* ARM v4-v6 Cache Type Register (CTR) */
/* CTR: cache type field */
#define ARM_V4_6_CTR_Ctype(r)		((r >> 25) & 0x0F)
/* CTR: separate caches bit
 * 0 = unified, ISize and DSize fields both describe the unified cache;
 * 1 = separate instruction and data caches
 */
#define ARM_V4_6_CTR_S(r)		((r >> 24) & 0x01)
/* CTR: data cache page coloring restriction bit for VMSA architectures */
#define ARM_V4_6_CTR_DSize_P(r)		((r >> 23) & 0x01)
/* CTR: size of the data cache, qualified by the M bit */
#define ARM_V4_6_CTR_DSize_Size(r)	((r >> 18) & 0x0F)
/* CTR: cache associativity qualified by the M bit */
#define ARM_V4_6_CTR_DSize_Assoc(r)	((r >> 15) & 0x07)
/* CTR: M-bit qualifies the values in the Size and Assoc subfields, data cache*/
#define ARM_V4_6_CTR_DSize_M(r)		((r >> 14) & 0x01)
/* CTR: line length of the data cache */
#define ARM_V4_6_CTR_DSize_Len(r)	((r >> 12) & 0x03)
/* CTR: instruction cache page coloring restriction for VMSA architectures*/
#define ARM_V4_6_CTR_ISize_P(r)		((r >> 11) & 0x01)
/* CTR: size of the cache, qualified by the M bit */
#define ARM_V4_6_CTR_ISize_Size(r)	((r >>  6) & 0x0F)
/* CTR: cache associativity qualified by the M bit */
#define ARM_V4_6_CTR_ISize_Assoc(r)	((r >>  3) & 0x07)
/* CTR: qualifies the values in the Size and Assoc subfields,instruction cache*/
#define ARM_V4_6_CTR_ISize_M(r)		((r >>  2) & 0x01)
/* CTR: line length of the instruction cache */
#define ARM_V4_6_CTR_ISize_Len(r)	((r >>  0) & 0x03)


static unsigned short num_cache_leaves;

enum _arm_cache_type {
	CACHE_TYPE_NULL = 0,
	CACHE_TYPE_INST,	/* instruction cache only */
	CACHE_TYPE_DATA,	/* data cache only */
	CACHE_TYPE_INSTnDATA,	/* separate instruction and data caches */
	CACHE_TYPE_UNIFIED,	/* unified cache */

	CACHE_TYPE_NUM		/* number of fields in the enum */
};
static const char *arm_cache_type_str[CACHE_TYPE_NUM] = {"NULL",
							 "Instruction",
							 "Data",
							 "InstructionAndData",
							 "Unified"};

struct _arm_cache_info {
	int level;			/* zero based cache level (i.e. an L1
					 * cache will be represented as 0)
					 */
	enum _arm_cache_type type;	/* cache type (instruction, data etc.)*/
	unsigned long flags;
	unsigned long size;		/* total cache size in bytes */
	unsigned long number_of_sets;
	unsigned long associativity;
	unsigned long line_size;	/* cache line length in bytes */
};

/* pointer to _arm_cache_info array (for each cache leaf) */
static DEFINE_PER_CPU(struct _arm_cache_info *, arm_cache_info);
#define ARM_CACHE_INFO_IDX(x, y) (&((per_cpu(arm_cache_info, x))[y]))

#ifdef CONFIG_CPU_CP15
#define read_armv7_cache_level_id()			\
	({						\
		unsigned int __val;			\
		asm("mrc	p15, 1, %0, c0, c0, 1"	\
			: "=r" (__val)			\
			:				\
			: "cc");			\
		__val;					\
	})

static inline unsigned int
read_armv7_cache_size_id(unsigned int level, enum _arm_cache_type type)
{
	unsigned int csselr = (level<<1) | (type == CACHE_TYPE_INST);
	unsigned int ccsidr;

	if (level > 7)
		return 0;

	/* select and read the cache size ID register(CCSIDR) */
	asm("mcr p15, 2, %1, c0, c0, 0\n"
	    "mrc p15, 1, %0, c0, c0, 0"
		: "=r" (ccsidr)
		: "r" (csselr)
		: "cc");

	return ccsidr;
}
#else
#define read_armv7_cache_level_id() 0
#define read_armv7_cache_size_id(level) 0
#endif

static unsigned short find_num_cache_leaves(void)
{
	unsigned short leaves = 0;
	unsigned int ctr = read_cpuid_cachetype();

	switch (cpu_architecture()) {
	case CPU_ARCH_ARMv7: {
		unsigned int clidr = read_armv7_cache_level_id();
		int i;

		for (i = 0; i < 7; i++) {
			unsigned int type = (clidr >> (i*3)) &
				ARM_V7_CLIDR_Ctype;
			if ((type < CACHE_TYPE_INST) ||
				(type > CACHE_TYPE_UNIFIED))
				break;

			/* if we have separate instruction and data
			 * caches count them individually
			 */
			if (type == CACHE_TYPE_INSTnDATA)
				leaves++;

			/* cache present increment the count */
			leaves++;
		}
		break;
	}
	case CPU_ARCH_ARMv6:
	default:
		/* before ARMv7 the best we can do is detect the L1
		 * cache configuration
		 */
		if (ARM_V4_6_CTR_S(ctr)) {
			leaves = 2;	/* we have separate instruction
					 * and data caches
					 */
		} else {
			leaves = 1;	/* unified L1 cache */
		}
	}

	return leaves;
}

static int
arm_v7_get_cache_level_and_type(int leaf, struct _arm_cache_info *this_leaf)
{
	unsigned int clidr = read_armv7_cache_level_id();
	int leaves;
	int cache_level;

	this_leaf->level = -1;
	this_leaf->type = CACHE_TYPE_NULL;

	leaves = 0;
	cache_level = 0;
	do {
		unsigned int cache_type = (clidr >> (cache_level*3)) &
			ARM_V7_CLIDR_Ctype;
		if ((cache_type < CACHE_TYPE_INST) ||
			(cache_type > CACHE_TYPE_UNIFIED))
			break;

		if (leaf == leaves) {
			this_leaf->level = cache_level;
			/* we count the instruction and data caches on the same
			 * level as separate leaves (instruction first)
			 */
			this_leaf->type = (cache_type == CACHE_TYPE_INSTnDATA) ?
				CACHE_TYPE_INST : cache_type;
			break;
		}

		if (cache_type == CACHE_TYPE_INSTnDATA) {
			/* if we have separate instruction and data caches
			 * count them individually
			 */
			leaves++;
			if (leaf == leaves) {
				this_leaf->level = cache_level;
				this_leaf->type = CACHE_TYPE_DATA;
				break;
			}
		}
		leaves++;
		cache_level++;
	} while (cache_level < 7);

	return 0;
}

static int arm_v7_cache_lookup(int index, struct _arm_cache_info *this_leaf)
{
	unsigned int ccsidr;
	int retval;

	retval = arm_v7_get_cache_level_and_type(index, this_leaf);
	if (retval < 0)
		return -EIO;

	ccsidr = read_armv7_cache_size_id(this_leaf->level, this_leaf->type);

	this_leaf->flags = ARM_V7_CCSIDR_FLAGS(ccsidr);
	this_leaf->number_of_sets = ARM_V7_CCSIDR_NumSets(ccsidr) + 1;
	this_leaf->associativity = ARM_V7_CCSIDR_Associativity(ccsidr) + 1;
	this_leaf->line_size = (1 << (ARM_V7_CCSIDR_LineSize(ccsidr) + 2)) *
		4/*bytes per word*/;
	this_leaf->size = this_leaf->number_of_sets *
		this_leaf->associativity *
		this_leaf->line_size;

	return 0;
}

static int arm_v4_6_cache_lookup(int index, struct _arm_cache_info *this_leaf)
{
	unsigned int ctr = read_cpuid_cachetype();
	unsigned long cache_size;
	unsigned long associativity;
	unsigned long m;
	unsigned long line_length;

	this_leaf->level = 0;		/* before ARMv7 the best we can do is
					 * detect the L1 cache configuration
					 */
	this_leaf->flags = ARM_V4_6_CTR_Ctype(ctr);

	if (index == 0) {
		this_leaf->type = (ARM_V4_6_CTR_S(ctr)) ?
			CACHE_TYPE_INST : CACHE_TYPE_UNIFIED;
		cache_size = ARM_V4_6_CTR_ISize_Size(ctr);
		associativity = ARM_V4_6_CTR_ISize_Assoc(ctr);
		m = ARM_V4_6_CTR_ISize_M(ctr);
		line_length = ARM_V4_6_CTR_ISize_Len(ctr);
	} else {
		this_leaf->type = CACHE_TYPE_DATA;
		cache_size = ARM_V4_6_CTR_DSize_Size(ctr);
		associativity = ARM_V4_6_CTR_DSize_Assoc(ctr);
		m = ARM_V4_6_CTR_DSize_M(ctr);
		line_length = ARM_V4_6_CTR_DSize_Len(ctr);
	}

	if (m)
		this_leaf->size = 768 * (1 << cache_size);
	else
		this_leaf->size = 512 * (1 << cache_size);

	if (m) {
		this_leaf->associativity = (associativity) ?
			(3 * (1 << (associativity - 1))) : 0; /* 3*2^(x-1) */
	} else {
		this_leaf->associativity = 1 << associativity;
	}

	this_leaf->line_size = (1 << (line_length + 1)) * 4/*bytes per word*/;

	this_leaf->number_of_sets = this_leaf->size /
		(this_leaf->associativity * this_leaf->line_size);

	return 0;
}

static int arm_cache_lookup(int index, struct _arm_cache_info *this_leaf)
{
	int retval = 0;

	switch (cpu_architecture()) {
	case CPU_ARCH_ARMv7:
		retval = arm_v7_cache_lookup(index, this_leaf);
		break;
	case CPU_ARCH_ARMv6:
		/* fall trough */
	default:
		retval = arm_v4_6_cache_lookup(index, this_leaf);
	}

	return retval;
}

/* detect cache configuration and store the results */
static void get_cpu_leaves(void *_retval)
{
	int i, *retval = _retval, cpu = smp_processor_id();

	*retval = 0;

	for (i = 0; i < num_cache_leaves; i++) {
		struct _arm_cache_info *this_leaf;

		this_leaf = ARM_CACHE_INFO_IDX(cpu, i);
		*retval = arm_cache_lookup(i, this_leaf);
		if (unlikely(*retval < 0))
			break;
	}
}

static int detect_cache_attributes(unsigned int cpu)
{
	int retval;

	if (num_cache_leaves == 0)
		return -ENOENT;

	per_cpu(arm_cache_info, cpu) = kzalloc(
		sizeof(struct _arm_cache_info) * num_cache_leaves, GFP_KERNEL);
	if (per_cpu(arm_cache_info, cpu) == NULL)
		return -ENOMEM;

	smp_call_function_single(cpu, get_cpu_leaves, &retval, true);

	if (retval) {
		kfree(per_cpu(arm_cache_info, cpu));
		per_cpu(arm_cache_info, cpu) = NULL;
	}

	return retval;
}

static void free_cache_attributes(unsigned int cpu)
{
	kfree(per_cpu(arm_cache_info, cpu));
	per_cpu(arm_cache_info, cpu) = NULL;
}

/* pointer to kobject for cpuX/cache */
static DEFINE_PER_CPU(struct kobject *, arm_cache_kobject);

struct _index_kobject {
	struct kobject kobj;
	unsigned int cpu;
	unsigned short index;
};

/* pointer to array of kobjects for cpuX/cache/indexY */
static DEFINE_PER_CPU(struct _index_kobject *, arm_index_kobject);
#define INDEX_KOBJECT_PTR(x, y)	(&((per_cpu(arm_index_kobject, x))[y]))

static ssize_t
show_level(struct _arm_cache_info *leaf, char *buf)
{
	return sprintf(buf, "%d\n", leaf->level+1);
}

static ssize_t
show_type(struct _arm_cache_info *leaf, char *buf)
{
	if ((leaf->type > CACHE_TYPE_NULL) && (leaf->type < CACHE_TYPE_NUM))
		return sprintf(buf, "%s\n", arm_cache_type_str[leaf->type]);
	else
		return sprintf(buf, "Unknown\n");
}

static ssize_t
show_coherency_line_size(struct _arm_cache_info *leaf, char *buf)
{
	return sprintf(buf, "%lu\n", leaf->line_size);
}

static ssize_t
show_physical_line_partition(struct _arm_cache_info *leaf, char *buf)
{
	return sprintf(buf, "1\n");
}

static ssize_t
show_ways_of_associativity(struct _arm_cache_info *leaf, char *buf)
{
	return sprintf(buf, "%lu\n", leaf->associativity);
}

static ssize_t
show_number_of_sets(struct _arm_cache_info *leaf, char *buf)
{
	return sprintf(buf, "%lu\n", leaf->number_of_sets);
}

static ssize_t
show_size(struct _arm_cache_info *leaf, char *buf)
{
	return sprintf(buf, "%lu\n", leaf->size);
}

static ssize_t
show_flags(struct _arm_cache_info *leaf, char *buf)
{
	return sprintf(buf, "0x%lx\n", leaf->flags);
}

struct _cache_attr {
	struct attribute attr;
	ssize_t (*show)(struct _arm_cache_info *, char *);
	ssize_t (*store)(struct _arm_cache_info *, const char *, size_t count);
};

#define to_object(k)	container_of(k, struct _index_kobject, kobj)
#define to_attr(a)	container_of(a, struct _cache_attr, attr)

#define define_one_ro(_name)				\
	static struct _cache_attr _name =		\
		__ATTR(_name, 0444, show_##_name, NULL)

define_one_ro(level);
define_one_ro(type);
define_one_ro(coherency_line_size);
define_one_ro(physical_line_partition);
define_one_ro(ways_of_associativity);
define_one_ro(number_of_sets);
define_one_ro(size);
define_one_ro(flags);

static struct attribute *default_attrs[] = {
	&type.attr,
	&level.attr,
	&coherency_line_size.attr,
	&physical_line_partition.attr,
	&ways_of_associativity.attr,
	&number_of_sets.attr,
	&size.attr,
	&flags.attr,
	NULL
};

static ssize_t show(struct kobject *kobj, struct attribute *attr, char *buf)
{
	struct _cache_attr *fattr = to_attr(attr);
	struct _index_kobject *leaf = to_object(kobj);

	return fattr->show ?
		fattr->show(ARM_CACHE_INFO_IDX(leaf->cpu, leaf->index), buf)
		: 0;
}

static ssize_t store(struct kobject *kobj, struct attribute *attr,
		const char *buf, size_t count)
{
	struct _cache_attr *fattr = to_attr(attr);
	struct _index_kobject *leaf = to_object(kobj);

	return fattr->store ?
		fattr->store(ARM_CACHE_INFO_IDX(leaf->cpu, leaf->index),
				buf, count)
		: 0;
}

static const struct sysfs_ops sysfs_ops = {
	.show   = show,
	.store  = store,
};

static struct kobj_type ktype_cache = {
	.sysfs_ops	= &sysfs_ops,
	.default_attrs	= default_attrs,
};

static struct kobj_type ktype_percpu_entry = {
	.sysfs_ops	= &sysfs_ops,
};

static void arm_cache_sysfs_exit(unsigned int cpu)
{
	kfree(per_cpu(arm_cache_kobject, cpu));
	kfree(per_cpu(arm_index_kobject, cpu));
	per_cpu(arm_cache_kobject, cpu) = NULL;
	per_cpu(arm_index_kobject, cpu) = NULL;
	free_cache_attributes(cpu);
}

static int arm_cache_sysfs_init(unsigned int cpu)
{
	int err;

	if (num_cache_leaves == 0)
		return -ENOENT;

	err = detect_cache_attributes(cpu);
	if (err)
		return err;

	per_cpu(arm_cache_kobject, cpu) =
		kzalloc(sizeof(struct kobject), GFP_KERNEL);
	if (unlikely(per_cpu(arm_cache_kobject, cpu) == NULL))
		goto err_out;

	per_cpu(arm_index_kobject, cpu) = kzalloc(
		sizeof(struct _index_kobject) * num_cache_leaves, GFP_KERNEL);
	if (unlikely(per_cpu(arm_index_kobject, cpu) == NULL))
		goto err_out;

	return 0;

err_out:
	arm_cache_sysfs_exit(cpu);
	return -ENOMEM;
}


static DECLARE_BITMAP(cache_dev_map, NR_CPUS);

static int cache_add_dev(struct device *sys_dev)
{
	unsigned int cpu = sys_dev->id;
	struct _index_kobject *this_object;
	int i, j;
	int retval;

	retval = arm_cache_sysfs_init(cpu);
	if (unlikely(retval < 0))
		return retval;

	retval = kobject_init_and_add(per_cpu(arm_cache_kobject, cpu),
				&ktype_percpu_entry,
				&sys_dev->kobj, "%s", "cache");

	if (retval < 0) {
		arm_cache_sysfs_exit(cpu);
		return retval;
	}

	for (i = 0; i < num_cache_leaves; i++) {
		this_object = INDEX_KOBJECT_PTR(cpu, i);
		this_object->cpu = cpu;
		this_object->index = i;

		retval = kobject_init_and_add(&(this_object->kobj),
					&ktype_cache,
					per_cpu(arm_cache_kobject, cpu),
					"index%1d", i);
		if (unlikely(retval)) {
			for (j = 0; j < i; j++)
				kobject_put(&(INDEX_KOBJECT_PTR(cpu, j)->kobj));
			kobject_put(per_cpu(arm_cache_kobject, cpu));
			arm_cache_sysfs_exit(cpu);
			return retval;
		}
		kobject_uevent(&(this_object->kobj), KOBJ_ADD);
	}
	cpumask_set_cpu(cpu, to_cpumask(cache_dev_map));

	kobject_uevent(per_cpu(arm_cache_kobject, cpu), KOBJ_ADD);

	return 0;
}

static void cache_remove_dev(struct device *sys_dev)
{
	unsigned int cpu = sys_dev->id;
	unsigned long i;

	if (!cpumask_test_cpu(cpu, to_cpumask(cache_dev_map)))
		return;
	cpumask_clear_cpu(cpu, to_cpumask(cache_dev_map));

	for (i = 0; i < num_cache_leaves; i++)
		kobject_put(&(INDEX_KOBJECT_PTR(cpu, i)->kobj));
	kobject_put(per_cpu(arm_cache_kobject, cpu));
	arm_cache_sysfs_exit(cpu);
}

static int cacheinfo_cpu_callback(struct notifier_block *nfb,
				unsigned long action, void *hcpu)
{
	unsigned int cpu = (unsigned long)hcpu;
	struct device *sys_dev;

	sys_dev = get_cpu_device(cpu);
	switch (action) {
	case CPU_ONLINE:
	case CPU_ONLINE_FROZEN:
		cache_add_dev(sys_dev);
		break;
	case CPU_DEAD:
	case CPU_DEAD_FROZEN:
		cache_remove_dev(sys_dev);
		break;
	}
	return NOTIFY_OK;
}

static struct notifier_block cacheinfo_cpu_notifier = {
	.notifier_call = cacheinfo_cpu_callback,
};

static int cache_sysfs_init(void)
{
	int i;

	num_cache_leaves = find_num_cache_leaves();

	if (num_cache_leaves == 0)
		return 0;

	for_each_online_cpu(i) {
		int err;
		struct device *sys_dev = get_cpu_device(i);

		err = cache_add_dev(sys_dev);
		if (err)
			return err;
	}
	register_hotcpu_notifier(&cacheinfo_cpu_notifier);

	return 0;
}

device_initcall(cache_sysfs_init);

