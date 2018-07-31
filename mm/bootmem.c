/*
 *  bootmem - A boot-time physical memory allocator and configurator
 *
 *  Copyright (C) 1999 Ingo Molnar
 *                1999 Kanoj Sarcar, SGI
 *                2008 Johannes Weiner
 *
 * Access to this subsystem has to be serialized externally (which is true
 * for the boot process anyway).
 */
#include <linux/init.h>
#include <linux/pfn.h>
#include <linux/slab.h>
#include <linux/bootmem.h>
#include <linux/module.h>
#include <linux/kmemleak.h>
#include <linux/range.h>

#include <asm/bug.h>
#include <asm/io.h>
#include <asm/processor.h>

#include "internal.h"

/*
x86中，max_low_pfn变量是由find_max_low_pfn函数计算并且初始化的，
它被初始化成ZONE_NORMAL的最后一个page的位置。
这个位置是kernel可以直接访问的物理内存, 
也是关系到kernel/userspace通过“PAGE_OFFSET宏”把线性地址内存空间分开的内存地址位置
*/
unsigned long max_low_pfn;
/*
系统可用的第一个pfn是min_low_pfn变量, 
开始于_end标号的后面, 也就是kernel结束的地方
*/
unsigned long min_low_pfn;
/*系统可用的最后一个PFN是max_pfn变量*/
unsigned long max_pfn;

#ifdef CONFIG_CRASH_DUMP
/*
 * If we have booted due to a crash, max_pfn will be a very low value. We need
 * to know the amount of memory that the previous kernel used.
 */
unsigned long saved_max_pfn;
#endif

#ifndef CONFIG_NO_BOOTMEM
bootmem_data_t bootmem_node_data[MAX_NUMNODES] __initdata;

static struct list_head bdata_list __initdata = LIST_HEAD_INIT(bdata_list);

static int bootmem_debug;

static int __init bootmem_debug_setup(char *buf)
{
	bootmem_debug = 1;
	return 0;
}
early_param("bootmem_debug", bootmem_debug_setup);

#define bdebug(fmt, args...) ({				\
	if (unlikely(bootmem_debug))			\
		printk(KERN_INFO			\
			"bootmem::%s " fmt,		\
			__func__, ## args);		\
})

static unsigned long __init bootmap_bytes(unsigned long pages)
{
	unsigned long bytes = (pages + 7) / 8;

	return ALIGN(bytes, sizeof(long));
}

/**
 * bootmem_bootmap_pages - calculate bitmap size in pages
 * @pages: number of pages the bitmap has to represent
 */
unsigned long __init bootmem_bootmap_pages(unsigned long pages)
{
	unsigned long bytes = bootmap_bytes(pages);

	return PAGE_ALIGN(bytes) >> PAGE_SHIFT;
}

/*
 * link bdata in order
 */
static void __init link_bootmem(bootmem_data_t *bdata)
{
	struct list_head *iter;

	list_for_each(iter, &bdata_list) {
		bootmem_data_t *ent;

		ent = list_entry(iter, bootmem_data_t, list);
		if (bdata->node_min_pfn < ent->node_min_pfn)
			break;
	}
	list_add_tail(&bdata->list, iter);
}

/*
	初始化bootmem内存分配器的核心；初始化bootmem_data_t的变量
 * Called once to set up the allocator itself.
 */
static unsigned long __init init_bootmem_core(bootmem_data_t *bdata,
	unsigned long mapstart, unsigned long start, unsigned long end)
{
	unsigned long mapsize;

	mminit_validate_memmodel_limits(&start, &end);
	bdata->node_bootmem_map = phys_to_virt(PFN_PHYS(mapstart)); //位图
	bdata->node_min_pfn = start;  //起始页框
	bdata->node_low_pfn = end;    //结束页框
	link_bootmem(bdata);  //将多个bootmem_data_t链接到bdata_list链表中，UMA只有一个

	/*
	 * Initially all pages are reserved - setup_arch() has to
	 * register free RAM areas explicitly.
	 */
	/* 计算分配器中可用的内存页所需的BIT位数，
	就是一个页占用一个BIT的话，需要多少个bit位,然后按字对齐*/
	mapsize = bootmap_bytes(end - start);
	/* 调用memset将所有的页标识为已经使用 */
	memset(bdata->node_bootmem_map, 0xff, mapsize);

	bdebug("nid=%td start=%lx map=%lx end=%lx mapsize=%lx\n",
		bdata - bootmem_node_data, start, mapstart, end, mapsize);

	return mapsize;
}

/**
 * init_bootmem_node - register a node as boot memory
 * @pgdat: node to register
 * @freepfn: pfn where the bitmap for this node is to be placed
 * @startpfn: first pfn on the node
 * @endpfn: first pfn after the node
 *
 * Returns the number of bytes needed to hold the bitmap for this node.
 */
unsigned long __init init_bootmem_node(pg_data_t *pgdat, unsigned long freepfn,
				unsigned long startpfn, unsigned long endpfn)
{
	return init_bootmem_core(pgdat->bdata, freepfn, startpfn, endpfn);
}

/**
 * init_bootmem - register boot memory
 * @start: pfn where the bitmap is to be placed
 * @pages: number of available physical pages
 *
 * Returns the number of bytes needed to hold the bitmap.
 */
unsigned long __init init_bootmem(unsigned long start, unsigned long pages)
{
	max_low_pfn = pages;
	min_low_pfn = start;
	return init_bootmem_core(NODE_DATA(0)->bdata, start, 0, pages);
}
#endif
/*
 * free_bootmem_late - free bootmem pages directly to page allocator
 * @addr: starting address of the range
 * @size: size of the range in bytes
 *
 * This is only useful when the bootmem allocator has already been torn
 * down, but we are still initializing the system.  Pages are given directly
 * to the page allocator, no bootmem metadata is updated because it is gone.
 */
void __init free_bootmem_late(unsigned long addr, unsigned long size)
{
	unsigned long cursor, end;

	kmemleak_free_part(__va(addr), size);

	cursor = PFN_UP(addr);
	end = PFN_DOWN(addr + size);

	for (; cursor < end; cursor++) {
		__free_pages_bootmem(pfn_to_page(cursor), 0);
		totalram_pages++;
	}
}

#ifdef CONFIG_NO_BOOTMEM
static void __init __free_pages_memory(unsigned long start, unsigned long end)
{
	int i;
	unsigned long start_aligned, end_aligned;
	int order = ilog2(BITS_PER_LONG);

	start_aligned = (start + (BITS_PER_LONG - 1)) & ~(BITS_PER_LONG - 1);
	end_aligned = end & ~(BITS_PER_LONG - 1);

	if (end_aligned <= start_aligned) {
		for (i = start; i < end; i++)
			__free_pages_bootmem(pfn_to_page(i), 0);

		return;
	}

	for (i = start; i < start_aligned; i++)
		__free_pages_bootmem(pfn_to_page(i), 0);

	for (i = start_aligned; i < end_aligned; i += BITS_PER_LONG)
		__free_pages_bootmem(pfn_to_page(i), order);

	for (i = end_aligned; i < end; i++)
		__free_pages_bootmem(pfn_to_page(i), 0);
}

unsigned long __init free_all_memory_core_early(int nodeid)
{
	int i;
	u64 start, end;
	unsigned long count = 0;
	struct range *range = NULL;
	int nr_range;

	nr_range = get_free_all_memory_range(&range, nodeid);

	for (i = 0; i < nr_range; i++) {
		start = range[i].start;
		end = range[i].end;
		count += end - start;
		__free_pages_memory(start, end);
	}

	return count;
}
#else
/* 释放bdata启动内存块中所有页框到页框分配器 */
static unsigned long __init free_all_bootmem_core(bootmem_data_t *bdata)
{
	int aligned;
	struct page *page;
	unsigned long start, end, pages, count = 0;

	/* 此bootmem没有位图，也就是没有管理内存 */
	if (!bdata->node_bootmem_map)
		return 0;

	start = bdata->node_min_pfn;  /* 此bootmem包含的开始页框 */
	end = bdata->node_low_pfn;    /* 此bootmem包含的结束页框 */

	/*
	 * If the start is aligned to the machines wordsize, we might
	 * be able to free pages in bulks of that order.
	 */
	aligned = !(start & (BITS_PER_LONG - 1));

	bdebug("nid=%td start=%lx end=%lx aligned=%d\n",
		bdata - bootmem_node_data, start, end, aligned);

	/* 释放 bdata->node_min_pfn 到 bdata->node_low_pfn 之间空闲的页框到伙伴系统 */
	while (start < end) {
		unsigned long *map, idx, vec;

		map = bdata->node_bootmem_map;  /* 此bootmem的位图 */
		idx = start - bdata->node_min_pfn;

		/* 做个整理（进行对齐），因为有可能start并不是按long位数对其的，有可能出现在了vec的中间位数 */
		vec = ~map[idx / BITS_PER_LONG];

		/* 如果检查的这一块内存块（内存块大小为BITS_PER_LONG）全是空的，则一次性释放 */
		if (aligned && vec == ~0UL && start + BITS_PER_LONG < end) {
			/* 这一块长度的内存块都为空闲的，计算这块内存的order，如果这块内存块长度是8个页框，那order就是3(2的3次方) */
			int order = ilog2(BITS_PER_LONG);

			/* 从start开始，释放2的order次方的页框到伙伴系统 */
			__free_pages_bootmem(pfn_to_page(start), order);
			count += BITS_PER_LONG;  /* count用来记录总共释放的页框 */
		} else {
			/* 内存块中有部分是页框是空的，一页一页释放 */
			unsigned long off = 0;

			while (vec && off < BITS_PER_LONG) {
				if (vec & 1) {
					/* 获取页框描述符，页框号实际上就是页描述符在mem_map的偏移量 */
					page = pfn_to_page(start + off);
					/* 将此页释放到伙伴系统 */
					__free_pages_bootmem(page, 0);
					count++;
				}
				vec >>= 1;
				off++;
			}
		}
		start += BITS_PER_LONG;
	}

	page = virt_to_page(bdata->node_bootmem_map);
	pages = bdata->node_low_pfn - bdata->node_min_pfn;
	pages = bootmem_bootmap_pages(pages);
	count += pages;
	while (pages--)
		__free_pages_bootmem(page++, 0);

	bdebug("nid=%td released=%lx\n", bdata - bootmem_node_data, count);

	return count;
}
#endif

/**
 * free_all_bootmem_node - release a node's free pages to the buddy allocator
 * @pgdat: node to be released
 *
 * Returns the number of pages actually released.
 */
unsigned long __init free_all_bootmem_node(pg_data_t *pgdat)
{
	register_page_bootmem_info_node(pgdat);
#ifdef CONFIG_NO_BOOTMEM
	/* free_all_memory_core_early(MAX_NUMNODES) will be called later */
	return 0;
#else
	return free_all_bootmem_core(pgdat->bdata);
#endif
}

/**
 	释放所有启动后不需要的内存页框到伙伴系统

 * free_all_bootmem - release free pages to the buddy allocator
 *
 * Returns the number of pages actually released.
 */
unsigned long __init free_all_bootmem(void)
{
#ifdef CONFIG_NO_BOOTMEM
	/*
	 * We need to use MAX_NUMNODES instead of NODE_DATA(0)->node_id
	 *  because in some case like Node0 doesnt have RAM installed
	 *  low ram will be on Node1
	 * Use MAX_NUMNODES will make sure all ranges in early_node_map[]
	 *  will be used instead of only Node0 related
	 */
	return free_all_memory_core_early(MAX_NUMNODES);
#else
	unsigned long total_pages = 0;
	/* 系统会为每个node分配一个这种结构，这个管理着node中所有页框，可以叫做bootmem分配器 */
	bootmem_data_t *bdata;

	/* 遍历所有需要释放的启动内存数据块 */
	list_for_each_entry(bdata, &bdata_list, list)
	 	/* 释放bdata启动内存块中所有页框到页框分配器 */
		total_pages += free_all_bootmem_core(bdata);

	/* 返回总共释放的页数量 */
	return total_pages;
#endif
}

#ifndef CONFIG_NO_BOOTMEM
static void __init __free(bootmem_data_t *bdata,
			unsigned long sidx, unsigned long eidx)
{
	unsigned long idx;

	bdebug("nid=%td start=%lx end=%lx\n", bdata - bootmem_node_data,
		sidx + bdata->node_min_pfn,
		eidx + bdata->node_min_pfn);

	if (bdata->hint_idx > sidx)
		bdata->hint_idx = sidx;

	for (idx = sidx; idx < eidx; idx++)
		if (!test_and_clear_bit(idx, bdata->node_bootmem_map))
			BUG();
}

static int __init __reserve(bootmem_data_t *bdata, unsigned long sidx,
			unsigned long eidx, int flags)
{
	unsigned long idx;
	int exclusive = flags & BOOTMEM_EXCLUSIVE;

	bdebug("nid=%td start=%lx end=%lx flags=%x\n",
		bdata - bootmem_node_data,
		sidx + bdata->node_min_pfn,
		eidx + bdata->node_min_pfn,
		flags);

	for (idx = sidx; idx < eidx; idx++)
		if (test_and_set_bit(idx, bdata->node_bootmem_map)) {
			if (exclusive) {
				__free(bdata, sidx, idx);
				return -EBUSY;
			}
			bdebug("silent double reserve of PFN %lx\n",
				idx + bdata->node_min_pfn);
		}
	return 0;
}

static int __init mark_bootmem_node(bootmem_data_t *bdata,
				unsigned long start, unsigned long end,
				int reserve, int flags)
{
	unsigned long sidx, eidx;

	bdebug("nid=%td start=%lx end=%lx reserve=%d flags=%x\n",
		bdata - bootmem_node_data, start, end, reserve, flags);

	BUG_ON(start < bdata->node_min_pfn);
	BUG_ON(end > bdata->node_low_pfn);

	sidx = start - bdata->node_min_pfn;
	eidx = end - bdata->node_min_pfn;

	if (reserve)
		return __reserve(bdata, sidx, eidx, flags);
	else
		__free(bdata, sidx, eidx);
	return 0;
}

static int __init mark_bootmem(unsigned long start, unsigned long end,
				int reserve, int flags)
{
	unsigned long pos;
	bootmem_data_t *bdata;

	pos = start;
	list_for_each_entry(bdata, &bdata_list, list) {
		int err;
		unsigned long max;

		if (pos < bdata->node_min_pfn ||
		    pos >= bdata->node_low_pfn) {
			BUG_ON(pos != start);
			continue;
		}

		max = min(bdata->node_low_pfn, end);

		err = mark_bootmem_node(bdata, pos, max, reserve, flags);
		if (reserve && err) {
			mark_bootmem(start, pos, 0, 0);
			return err;
		}

		if (max == end)
			return 0;
		pos = bdata->node_low_pfn;
	}
	BUG();
}
#endif

/**
 * free_bootmem_node - mark a page range as usable
 * @pgdat: node the range resides on
 * @physaddr: starting address of the range
 * @size: size of the range in bytes
 *
 * Partial pages will be considered reserved and left as they are.
 *
 * The range must reside completely on the specified node.
 */
void __init free_bootmem_node(pg_data_t *pgdat, unsigned long physaddr,
			      unsigned long size)
{
#ifdef CONFIG_NO_BOOTMEM
	free_early(physaddr, physaddr + size);
#else
	unsigned long start, end;

	kmemleak_free_part(__va(physaddr), size);

	start = PFN_UP(physaddr);
	end = PFN_DOWN(physaddr + size);

	mark_bootmem_node(pgdat->bdata, start, end, 0, 0);
#endif
}

/**
 * free_bootmem - mark a page range as usable
 * @addr: starting address of the range
 * @size: size of the range in bytes
 *
 * Partial pages will be considered reserved and left as they are.
 *
 * The range must be contiguous but may span node boundaries.
 */
void __init free_bootmem(unsigned long addr, unsigned long size)
{
#ifdef CONFIG_NO_BOOTMEM
	free_early(addr, addr + size);
#else
	unsigned long start, end;

	kmemleak_free_part(__va(addr), size);

	start = PFN_UP(addr);
	end = PFN_DOWN(addr + size);

	mark_bootmem(start, end, 0, 0);
#endif
}

/**
 * reserve_bootmem_node - mark a page range as reserved
 * @pgdat: node the range resides on
 * @physaddr: starting address of the range
 * @size: size of the range in bytes
 * @flags: reservation flags (see linux/bootmem.h)
 *
 * Partial pages will be reserved.
 *
 * The range must reside completely on the specified node.
 */
int __init reserve_bootmem_node(pg_data_t *pgdat, unsigned long physaddr,
				 unsigned long size, int flags)
{
#ifdef CONFIG_NO_BOOTMEM
	panic("no bootmem");
	return 0;
#else
	unsigned long start, end;

	start = PFN_DOWN(physaddr);
	end = PFN_UP(physaddr + size);

	return mark_bootmem_node(pgdat->bdata, start, end, 1, flags);
#endif
}

/**
 * reserve_bootmem - mark a page range as usable
 * @addr: starting address of the range
 * @size: size of the range in bytes
 * @flags: reservation flags (see linux/bootmem.h)
 *
 * Partial pages will be reserved.
 *
 * The range must be contiguous but may span node boundaries.
 */
int __init reserve_bootmem(unsigned long addr, unsigned long size,
			    int flags)
{
#ifdef CONFIG_NO_BOOTMEM
	panic("no bootmem");
	return 0;
#else
	unsigned long start, end;

	start = PFN_DOWN(addr);
	end = PFN_UP(addr + size);

	return mark_bootmem(start, end, 1, flags);
#endif
}

#ifndef CONFIG_NO_BOOTMEM
static unsigned long __init align_idx(struct bootmem_data *bdata,
				      unsigned long idx, unsigned long step)
{
	unsigned long base = bdata->node_min_pfn;

	/*
	 * Align the index with respect to the node start so that the
	 * combination of both satisfies the requested alignment.
	 */

	return ALIGN(base + idx, step) - base;
}

static unsigned long __init align_off(struct bootmem_data *bdata,
				      unsigned long off, unsigned long align)
{
	unsigned long base = PFN_PHYS(bdata->node_min_pfn);

	/* Same as align_idx for byte offsets */

	return ALIGN(base + off, align) - base;
}

static void * __init alloc_bootmem_core(struct bootmem_data *bdata,
					unsigned long size, unsigned long align,
					unsigned long goal, unsigned long limit)
{
	unsigned long fallback = 0;
	unsigned long min, max, start, sidx, midx, step;

	bdebug("nid=%td size=%lx [%lu pages] align=%lx goal=%lx limit=%lx\n",
		bdata - bootmem_node_data, size, PAGE_ALIGN(size) >> PAGE_SHIFT,
		align, goal, limit);

	BUG_ON(!size);
	BUG_ON(align & (align - 1));
	BUG_ON(limit && goal + size > limit);

	if (!bdata->node_bootmem_map)
		return NULL;

	min = bdata->node_min_pfn;
	max = bdata->node_low_pfn;

	goal >>= PAGE_SHIFT;
	limit >>= PAGE_SHIFT;

	if (limit && max > limit)
		max = limit;
	if (max <= min)
		return NULL;

	step = max(align >> PAGE_SHIFT, 1UL);

	if (goal && min < goal && goal < max)
		start = ALIGN(goal, step);
	else
		start = ALIGN(min, step);

	sidx = start - bdata->node_min_pfn;
	midx = max - bdata->node_min_pfn;

	if (bdata->hint_idx > sidx) {
		/*
		 * Handle the valid case of sidx being zero and still
		 * catch the fallback below.
		 */
		fallback = sidx + 1;
		sidx = align_idx(bdata, bdata->hint_idx, step);
	}

	while (1) {
		int merge;
		void *region;
		unsigned long eidx, i, start_off, end_off;
find_block:
		sidx = find_next_zero_bit(bdata->node_bootmem_map, midx, sidx);
		sidx = align_idx(bdata, sidx, step);
		eidx = sidx + PFN_UP(size);

		if (sidx >= midx || eidx > midx)
			break;

		for (i = sidx; i < eidx; i++)
			if (test_bit(i, bdata->node_bootmem_map)) {
				sidx = align_idx(bdata, i, step);
				if (sidx == i)
					sidx += step;
				goto find_block;
			}

		if (bdata->last_end_off & (PAGE_SIZE - 1) &&
				PFN_DOWN(bdata->last_end_off) + 1 == sidx)
			start_off = align_off(bdata, bdata->last_end_off, align);
		else
			start_off = PFN_PHYS(sidx);

		merge = PFN_DOWN(start_off) < sidx;
		end_off = start_off + size;

		bdata->last_end_off = end_off;
		bdata->hint_idx = PFN_UP(end_off);

		/*
		 * Reserve the area now:
		 */
		if (__reserve(bdata, PFN_DOWN(start_off) + merge,
				PFN_UP(end_off), BOOTMEM_EXCLUSIVE))
			BUG();

		region = phys_to_virt(PFN_PHYS(bdata->node_min_pfn) +
				start_off);
		memset(region, 0, size);
		/*
		 * The min_count is set to 0 so that bootmem allocated blocks
		 * are never reported as leaks.
		 */
		kmemleak_alloc(region, size, 0, 0);
		return region;
	}

	if (fallback) {
		sidx = align_idx(bdata, fallback - 1, step);
		fallback = 0;
		goto find_block;
	}

	return NULL;
}

static void * __init alloc_arch_preferred_bootmem(bootmem_data_t *bdata,
					unsigned long size, unsigned long align,
					unsigned long goal, unsigned long limit)
{
	if (WARN_ON_ONCE(slab_is_available()))
		return kzalloc(size, GFP_NOWAIT);

#ifdef CONFIG_HAVE_ARCH_BOOTMEM
	{
		bootmem_data_t *p_bdata;

		p_bdata = bootmem_arch_preferred_node(bdata, size, align,
							goal, limit);
		if (p_bdata)
			return alloc_bootmem_core(p_bdata, size, align,
							goal, limit);
	}
#endif
	return NULL;
}
#endif

//申请在boot期间需要的内存（size）,核心操作
static void * __init ___alloc_bootmem_nopanic(unsigned long size,
					unsigned long align,
					unsigned long goal,
					unsigned long limit)
{
#ifdef CONFIG_NO_BOOTMEM
	void *ptr;

	if (WARN_ON_ONCE(slab_is_available()))
		return kzalloc(size, GFP_NOWAIT);

restart:

	ptr = __alloc_memory_core_early(MAX_NUMNODES, size, align, goal, limit);

	if (ptr)
		return ptr;

	if (goal != 0) {
		goal = 0;
		goto restart;
	}

	return NULL;
#else
	bootmem_data_t *bdata;
	void *region;

restart:
	//alloc_arch_preferred_bootmem同样调用了alloc_bootmem_core
	region = alloc_arch_preferred_bootmem(NULL, size, align, goal, limit);
	if (region)
		return region;

	list_for_each_entry(bdata, &bdata_list, list) {
		if (goal && bdata->node_low_pfn <= PFN_DOWN(goal))
			continue;
		if (limit && bdata->node_min_pfn >= PFN_DOWN(limit))
			break;
		//核心
		region = alloc_bootmem_core(bdata, size, align, goal, limit);
		if (region)
			return region;
	}

	if (goal) {
		goal = 0;
		goto restart;
	}

	return NULL;
#endif
}

/**
 * __alloc_bootmem_nopanic - allocate boot memory without panicking
 * @size: size of the request in bytes
 * @align: alignment of the region
 * @goal: preferred starting address of the region
 *
 * The goal is dropped if it can not be satisfied and the allocation will
 * fall back to memory below @goal.
 *
 * Allocation may happen on any node in the system.
 *
 * Returns NULL on failure.
 */
void * __init __alloc_bootmem_nopanic(unsigned long size, unsigned long align,
					unsigned long goal)
{
	unsigned long limit = 0;

#ifdef CONFIG_NO_BOOTMEM
	limit = -1UL;
#endif

	return ___alloc_bootmem_nopanic(size, align, goal, limit);
}

//申请在boot期间需要的内存（size）
static void * __init ___alloc_bootmem(unsigned long size, unsigned long align,
					unsigned long goal, unsigned long limit)
{
	void *mem = ___alloc_bootmem_nopanic(size, align, goal, limit);

	if (mem)
		return mem;
	/*
	 * Whoops, we cannot satisfy the allocation request.
	 */
	printk(KERN_ALERT "bootmem alloc of %lu bytes failed!\n", size);
	panic("Out of memory");
	return NULL;
}

/**
	申请在boot期间需要的内存（size）
 * __alloc_bootmem - allocate boot memory
 * @size: size of the request in bytes  内存区长度
 * @align: alignment of the region  对齐方式
 * @goal: preferred starting address of the region 起始地址
 *
 	如果在第一个NUMA节点中无法申请到足够的内存，则进入后备队列中申请
 * The goal is dropped if it can not be satisfied and the allocation will
 * fall back to memory below @goal.
 *
 * Allocation may happen on any node in the system.
 *
 * The function panics if the request can not be satisfied.
 */
void * __init __alloc_bootmem(unsigned long size, unsigned long align,
			      unsigned long goal)
{
	unsigned long limit = 0;

#ifdef CONFIG_NO_BOOTMEM
	limit = -1UL;
#endif

	//注意看，并不是同一个函数，此函数有三个下划线
	return ___alloc_bootmem(size, align, goal, limit);
}

#ifndef CONFIG_NO_BOOTMEM
static void * __init ___alloc_bootmem_node(bootmem_data_t *bdata,
				unsigned long size, unsigned long align,
				unsigned long goal, unsigned long limit)
{
	void *ptr;

	ptr = alloc_arch_preferred_bootmem(bdata, size, align, goal, limit);
	if (ptr)
		return ptr;

	ptr = alloc_bootmem_core(bdata, size, align, goal, limit);
	if (ptr)
		return ptr;

	return ___alloc_bootmem(size, align, goal, limit);
}
#endif

/**
 * __alloc_bootmem_node - allocate boot memory from a specific node
 * @pgdat: node to allocate from
 * @size: size of the request in bytes
 * @align: alignment of the region
 * @goal: preferred starting address of the region
 *
 * The goal is dropped if it can not be satisfied and the allocation will
 * fall back to memory below @goal.
 *
 * Allocation may fall back to any node in the system if the specified node
 * can not hold the requested memory.
 *
 * The function panics if the request can not be satisfied.
 */
void * __init __alloc_bootmem_node(pg_data_t *pgdat, unsigned long size,
				   unsigned long align, unsigned long goal)
{
	if (WARN_ON_ONCE(slab_is_available()))
		return kzalloc_node(size, GFP_NOWAIT, pgdat->node_id);

#ifdef CONFIG_NO_BOOTMEM
	return __alloc_memory_core_early(pgdat->node_id, size, align,
					 goal, -1ULL);
#else
	return ___alloc_bootmem_node(pgdat->bdata, size, align, goal, 0);
#endif
}

void * __init __alloc_bootmem_node_high(pg_data_t *pgdat, unsigned long size,
				   unsigned long align, unsigned long goal)
{
#ifdef MAX_DMA32_PFN
	unsigned long end_pfn;

	if (WARN_ON_ONCE(slab_is_available()))
		return kzalloc_node(size, GFP_NOWAIT, pgdat->node_id);

	/* update goal according ...MAX_DMA32_PFN */
	end_pfn = pgdat->node_start_pfn + pgdat->node_spanned_pages;

	if (end_pfn > MAX_DMA32_PFN + (128 >> (20 - PAGE_SHIFT)) &&
	    (goal >> PAGE_SHIFT) < MAX_DMA32_PFN) {
		void *ptr;
		unsigned long new_goal;

		new_goal = MAX_DMA32_PFN << PAGE_SHIFT;
#ifdef CONFIG_NO_BOOTMEM
		ptr =  __alloc_memory_core_early(pgdat->node_id, size, align,
						 new_goal, -1ULL);
#else
		ptr = alloc_bootmem_core(pgdat->bdata, size, align,
						 new_goal, 0);
#endif
		if (ptr)
			return ptr;
	}
#endif

	return __alloc_bootmem_node(pgdat, size, align, goal);

}

#ifdef CONFIG_SPARSEMEM
/**
 * alloc_bootmem_section - allocate boot memory from a specific section
 * @size: size of the request in bytes
 * @section_nr: sparse map section to allocate from
 *
 * Return NULL on failure.
 */
void * __init alloc_bootmem_section(unsigned long size,
				    unsigned long section_nr)
{
#ifdef CONFIG_NO_BOOTMEM
	unsigned long pfn, goal, limit;

	pfn = section_nr_to_pfn(section_nr);
	goal = pfn << PAGE_SHIFT;
	limit = section_nr_to_pfn(section_nr + 1) << PAGE_SHIFT;

	return __alloc_memory_core_early(early_pfn_to_nid(pfn), size,
					 SMP_CACHE_BYTES, goal, limit);
#else
	bootmem_data_t *bdata;
	unsigned long pfn, goal, limit;

	pfn = section_nr_to_pfn(section_nr);
	goal = pfn << PAGE_SHIFT;
	limit = section_nr_to_pfn(section_nr + 1) << PAGE_SHIFT;
	bdata = &bootmem_node_data[early_pfn_to_nid(pfn)];

	return alloc_bootmem_core(bdata, size, SMP_CACHE_BYTES, goal, limit);
#endif
}
#endif

void * __init __alloc_bootmem_node_nopanic(pg_data_t *pgdat, unsigned long size,
				   unsigned long align, unsigned long goal)
{
	void *ptr;

	if (WARN_ON_ONCE(slab_is_available()))
		return kzalloc_node(size, GFP_NOWAIT, pgdat->node_id);

#ifdef CONFIG_NO_BOOTMEM
	ptr =  __alloc_memory_core_early(pgdat->node_id, size, align,
						 goal, -1ULL);
#else
	ptr = alloc_arch_preferred_bootmem(pgdat->bdata, size, align, goal, 0);
	if (ptr)
		return ptr;

	ptr = alloc_bootmem_core(pgdat->bdata, size, align, goal, 0);
#endif
	if (ptr)
		return ptr;

	return __alloc_bootmem_nopanic(size, align, goal);
}

#ifndef ARCH_LOW_ADDRESS_LIMIT
#define ARCH_LOW_ADDRESS_LIMIT	0xffffffffUL
#endif

/**
 * __alloc_bootmem_low - allocate low boot memory
 * @size: size of the request in bytes
 * @align: alignment of the region
 * @goal: preferred starting address of the region
 *
 * The goal is dropped if it can not be satisfied and the allocation will
 * fall back to memory below @goal.
 *
 * Allocation may happen on any node in the system.
 *
 * The function panics if the request can not be satisfied.
 */
void * __init __alloc_bootmem_low(unsigned long size, unsigned long align,
				  unsigned long goal)
{
	return ___alloc_bootmem(size, align, goal, ARCH_LOW_ADDRESS_LIMIT);
}

/**
 * __alloc_bootmem_low_node - allocate low boot memory from a specific node
 * @pgdat: node to allocate from
 * @size: size of the request in bytes
 * @align: alignment of the region
 * @goal: preferred starting address of the region
 *
 * The goal is dropped if it can not be satisfied and the allocation will
 * fall back to memory below @goal.
 *
 * Allocation may fall back to any node in the system if the specified node
 * can not hold the requested memory.
 *
 * The function panics if the request can not be satisfied.
 */
void * __init __alloc_bootmem_low_node(pg_data_t *pgdat, unsigned long size,
				       unsigned long align, unsigned long goal)
{
	if (WARN_ON_ONCE(slab_is_available()))
		return kzalloc_node(size, GFP_NOWAIT, pgdat->node_id);

#ifdef CONFIG_NO_BOOTMEM
	return __alloc_memory_core_early(pgdat->node_id, size, align,
				goal, ARCH_LOW_ADDRESS_LIMIT);
#else
	return ___alloc_bootmem_node(pgdat->bdata, size, align,
				goal, ARCH_LOW_ADDRESS_LIMIT);
#endif
}
