#ifndef _LINUX_SLAB_DEF_H
#define	_LINUX_SLAB_DEF_H

/*
 * Definitions unique to the original Linux SLAB allocator.
 *
 * What we provide here is a way to optimize the frequent kmalloc
 * calls in the kernel by selecting the appropriate general cache
 * if kmalloc was called with a size that can be established at
 * compile time.
 */

#include <linux/init.h>
#include <asm/page.h>		/* kmalloc_sizes.h needs PAGE_SIZE */
#include <asm/cache.h>		/* kmalloc_sizes.h needs L1_CACHE_BYTES */
#include <linux/compiler.h>
#include <linux/kmemtrace.h>

/*
	每一个高速缓存对应着一个struct kmem_cache，可以有若干个高速缓存存放不同的对象
	每一个kmem_cache存放的对象大小都是相同的；
 * struct kmem_cache
 *
 * manages a cache.
 */

struct kmem_cache {
/* 1) per-cpu data, touched during every alloc/free */
	/*
	本地CPU空闲链表
	将释放的对象加入到这个链表中*/
	struct array_cache *array[NR_CPUS];
/* 2) Cache tunables. Protected by cache_chain_mutex */
	 /* 要转移进本地高速缓存或从本地高速缓存中转移出去的对象的数量 */
	unsigned int batchcount;
	/* 本地高速缓存中空闲对象的最大数目 */
	unsigned int limit;
	 /* 是否存在CPU共享高速缓存，CPU共享高速缓存指针保存在kmem_cache_node结构中 
	也就是CPU共享的空闲对象链表（区别于本地空闲对象列表）；
	=0：没有CPU共享的链表
	=1：存在
	*/
	unsigned int shared;
	 
	/* 对象长度 + 填充字节 */
	unsigned int buffer_size;
	/* size的倒数，加快计算 */
	u32 reciprocal_buffer_size;
/* 3) touched by every alloc & free from the backend */
	 /* 高速缓存永久属性的标识，
	 如果SLAB描述符放在外部(不放在SLAB中)，则CFLAGS_OFF_SLAB置1 */
	unsigned int flags;		/* constant flags */
	/* 每个SLAB中对象的个数(在同一个高速缓存中slab中对象个数相同) */
	unsigned int num;		/* # of objs per slab */

/* 4) cache_grow/shrink */
	/* order of pgs per slab (2^n) */
	/* 一个单独SLAB中包含的连续页框数目的对数 log2(N)*/
	unsigned int gfporder;

	/* force GFP flags, e.g. GFP_DMA */
	 /* 分配页框时传递给伙伴系统的一组标识 */
	gfp_t gfpflags;

	/* SLAB使用的最大颜色个数，slab使用的着色值不得超过它，超过它就重新置为0 */
	size_t colour;			/* cache colouring range */
	/* SLAB中基本对齐偏移，
	当新SLAB着色时，偏移量的值需要乘上这个基本对齐偏移量，
	理解就是1个偏移量等于多少个B大小的值 */
	unsigned int colour_off;	/* colour offset */
	/*???*/
	struct kmem_cache *slabp_cache;
	unsigned int slab_size;
	unsigned int dflags;		/* dynamic flags */

	/* constructor func */
	/* 构造函数，一般用于初始化这个SLAB高速缓存中的对象 */
	void (*ctor)(void *obj);

/* 5) cache creation/removal */
	/* 存放高速缓存名字 */
	const char *name;
	/* 高速缓存描述符双向链表指针 ，指向下一个、上一个kmem_cache结构*/
	struct list_head next;

/* 6) statistics */
	/* 统计 */
#ifdef CONFIG_DEBUG_SLAB
	unsigned long num_active;
	unsigned long num_allocations;
	unsigned long high_mark;
	unsigned long grown;
	unsigned long reaped;
	unsigned long errors;
	unsigned long max_freeable;
	unsigned long node_allocs;
	unsigned long node_frees;
	unsigned long node_overflow;
	atomic_t allochit;
	atomic_t allocmiss;
	atomic_t freehit;
	atomic_t freemiss;

	/*
	 * If debugging is enabled, then the allocator can add additional
	 * fields and/or padding to every object. buffer_size contains the total
	 * object size including these internal fields, the following two
	 * variables contain the offset to the user object and its size.、
	 在使用debug的情况下，分配器会给每一个对象增加一些格外的字段
	 所以，下面两个变量表示：每个对象之间的偏移和每个对象的大小。
	 */
	 /* 对象间的偏移 */
	int obj_offset;
	/*对象的大小相同*/
	int obj_size;
#endif /* CONFIG_DEBUG_SLAB */

	/*
	 * We put nodelists[] at the end of kmem_cache, because we want to size
	 * this array to nr_node_ids slots instead of MAX_NUMNODES
	 * (see kmem_cache_init())
	 * We still use [MAX_NUMNODES] and not [1] or [0] because cache_cache
	 * is statically defined, so we reserve the max number of nodes.
	 */
	 //该nodelist是一个指针数组，重点是数组，每个数组元素是一个指针
	  /* 结点链表，此高速缓存可能在不同NUMA的结点都有SLAB链表，所以要设置成数组的形式 */
	struct kmem_list3 *nodelists[MAX_NUMNODES];
	/*
	 * Do not add fields after nodelists[]
	 */
};

/* Size description struct for general caches. */
struct cache_sizes {
	size_t		 	cs_size;
	struct kmem_cache	*cs_cachep;
#ifdef CONFIG_ZONE_DMA
	struct kmem_cache	*cs_dmacachep;
#endif
};
extern struct cache_sizes malloc_sizes[];

void *kmem_cache_alloc(struct kmem_cache *, gfp_t);
void *__kmalloc(size_t size, gfp_t flags);

#ifdef CONFIG_TRACING
extern void *kmem_cache_alloc_notrace(struct kmem_cache *cachep, gfp_t flags);
extern size_t slab_buffer_size(struct kmem_cache *cachep);
#else
static __always_inline void *
kmem_cache_alloc_notrace(struct kmem_cache *cachep, gfp_t flags)
{
	return kmem_cache_alloc(cachep, flags);
}
static inline size_t slab_buffer_size(struct kmem_cache *cachep)
{
	return 0;
}
#endif

static __always_inline void *kmalloc(size_t size, gfp_t flags)
{
	struct kmem_cache *cachep;
	void *ret;

	if (__builtin_constant_p(size)) {
		int i = 0;

		if (!size)
			return ZERO_SIZE_PTR;

#define CACHE(x) \
		if (size <= x) \
			goto found; \
		else \
			i++;
#include <linux/kmalloc_sizes.h>
#undef CACHE
		return NULL;
found:
#ifdef CONFIG_ZONE_DMA
		if (flags & GFP_DMA)
			cachep = malloc_sizes[i].cs_dmacachep;
		else
#endif
			cachep = malloc_sizes[i].cs_cachep;

		ret = kmem_cache_alloc_notrace(cachep, flags);

		trace_kmalloc(_THIS_IP_, ret,
			      size, slab_buffer_size(cachep), flags);

		return ret;
	}
	return __kmalloc(size, flags);
}

#ifdef CONFIG_NUMA
extern void *__kmalloc_node(size_t size, gfp_t flags, int node);
extern void *kmem_cache_alloc_node(struct kmem_cache *, gfp_t flags, int node);

#ifdef CONFIG_TRACING
extern void *kmem_cache_alloc_node_notrace(struct kmem_cache *cachep,
					   gfp_t flags,
					   int nodeid);
#else
static __always_inline void *
kmem_cache_alloc_node_notrace(struct kmem_cache *cachep,
			      gfp_t flags,
			      int nodeid)
{
	return kmem_cache_alloc_node(cachep, flags, nodeid);
}
#endif

static __always_inline void *kmalloc_node(size_t size, gfp_t flags, int node)
{
	struct kmem_cache *cachep;
	void *ret;

	if (__builtin_constant_p(size)) {
		int i = 0;

		if (!size)
			return ZERO_SIZE_PTR;

#define CACHE(x) \
		if (size <= x) \
			goto found; \
		else \
			i++;
#include <linux/kmalloc_sizes.h>
#undef CACHE
		return NULL;
found:
#ifdef CONFIG_ZONE_DMA
		if (flags & GFP_DMA)
			cachep = malloc_sizes[i].cs_dmacachep;
		else
#endif
			cachep = malloc_sizes[i].cs_cachep;

		ret = kmem_cache_alloc_node_notrace(cachep, flags, node);

		trace_kmalloc_node(_THIS_IP_, ret,
				   size, slab_buffer_size(cachep),
				   flags, node);

		return ret;
	}
	return __kmalloc_node(size, flags, node);
}

#endif	/* CONFIG_NUMA */

#endif	/* _LINUX_SLAB_DEF_H */
