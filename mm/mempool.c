/*
 *  linux/mm/mempool.c
 *
 *  memory buffer pool support. Such pools are mostly used
 *  for guaranteed, deadlock-free memory allocations during
 *  extreme VM load.
 *
 *  started by Ingo Molnar, Copyright (C) 2001
 */

#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/mempool.h>
#include <linux/blkdev.h>
#include <linux/writeback.h>

static void add_element(mempool_t *pool, void *element)
{
	BUG_ON(pool->curr_nr >= pool->min_nr);
	pool->elements[pool->curr_nr++] = element;
}

static void *remove_element(mempool_t *pool)
{
	BUG_ON(pool->curr_nr <= 0);
	return pool->elements[--pool->curr_nr];
}

/* 销毁内存池，由mempool_destory（）调用 */
static void free_pool(mempool_t *pool)
{
	/* 使用自定义的free方法，释放elements指向的数组中的对象，一共有min_nr个 */
	while (pool->curr_nr) {
		void *element = remove_element(pool);
		pool->free(element, pool->pool_data);
	}
	/*  释放elements数组 */
	kfree(pool->elements);
	/* 释放内存池描述符 */
	kfree(pool);
}

/**
	创建一个内存池
 * mempool_create - create a memory pool
 * @min_nr:    the minimum number of elements guaranteed to be
 *             allocated for this pool.
 * @alloc_fn:  user-defined element-allocation function.  用户自己定义的alloc方法
 * @free_fn:   user-defined element-freeing function.  用户自己定义的free方法
 * @pool_data: optional private data available to the user-defined functions.
 *
 * this function creates and allocates a guaranteed size, preallocated
 * memory pool. The pool can be used from the mempool_alloc() and mempool_free()
 * functions. This function might sleep. Both the alloc_fn() and the free_fn()
 * functions might sleep - as long as the mempool_alloc() function is not called
 * from IRQ contexts.
 */
mempool_t *mempool_create(int min_nr, mempool_alloc_t *alloc_fn,
				mempool_free_t *free_fn, void *pool_data)
{
	return  mempool_create_node(min_nr,alloc_fn,free_fn, pool_data,-1);
}
EXPORT_SYMBOL(mempool_create);

mempool_t *mempool_create_node(int min_nr, mempool_alloc_t *alloc_fn,
			mempool_free_t *free_fn, void *pool_data, int node_id)
{
	mempool_t *pool;
	//创建一个内存池描述符mempool_t（使用kmalloc，由slab分配器为描述符分配一个对象）
	pool = kmalloc_node(sizeof(*pool), GFP_KERNEL | __GFP_ZERO, node_id);
	if (!pool)
		return NULL;
	/*为数组指针申请空间，大小固定，因为每个元素都是指针，大小 = min_nr * sizeof(void *)
		elements指向一个数组，数组中存放的是内存池中保存的元素的指针
	*/
	pool->elements = kmalloc_node(min_nr * sizeof(void *),
					GFP_KERNEL, node_id);
	if (!pool->elements) {
		kfree(pool);
		return NULL;
	}
	spin_lock_init(&pool->lock);

	//使用传进来的参数 为内存池描述符 赋值
	pool->min_nr = min_nr;
	pool->pool_data = pool_data;
	init_waitqueue_head(&pool->wait);  //初始化等待队列
	pool->alloc = alloc_fn;
	pool->free = free_fn;

	/*首先预先分配一定数量的缓冲区。
	 * First pre-allocate the guaranteed number of buffers.
	 */
	 /*  使用自定义的alloc方法，申请min_nr个pool_data数据对象，作为内存池的缓冲区*/
	while (pool->curr_nr < pool->min_nr) {
		void *element;
		
		//使用自定义的alloc方法
		element = pool->alloc(GFP_KERNEL, pool->pool_data);
		if (unlikely(!element)) {
			free_pool(pool);
			return NULL;
		}
		/* 将element加入到elements数组中 */
		add_element(pool, element);
	}
	return pool;
}
EXPORT_SYMBOL(mempool_create_node);

/**
 * mempool_resize - resize an existing memory pool
 * @pool:       pointer to the memory pool which was allocated via
 *              mempool_create().
 * @new_min_nr: the new minimum number of elements guaranteed to be
 *              allocated for this pool.
 * @gfp_mask:   the usual allocation bitmask.
 *
 * This function shrinks/grows the pool. In the case of growing,
 * it cannot be guaranteed that the pool will be grown to the new
 * size immediately, but new mempool_free() calls will refill it.
 *
 * Note, the caller must guarantee that no mempool_destroy is called
 * while this function is running. mempool_alloc() & mempool_free()
 * might be called (eg. from IRQ contexts) while this function executes.
 */
int mempool_resize(mempool_t *pool, int new_min_nr, gfp_t gfp_mask)
{
	void *element;
	void **new_elements;
	unsigned long flags;

	BUG_ON(new_min_nr <= 0);

	spin_lock_irqsave(&pool->lock, flags);
	if (new_min_nr <= pool->min_nr) {
		while (new_min_nr < pool->curr_nr) {
			element = remove_element(pool);
			spin_unlock_irqrestore(&pool->lock, flags);
			pool->free(element, pool->pool_data);
			spin_lock_irqsave(&pool->lock, flags);
		}
		pool->min_nr = new_min_nr;
		goto out_unlock;
	}
	spin_unlock_irqrestore(&pool->lock, flags);

	/* Grow the pool */
	new_elements = kmalloc(new_min_nr * sizeof(*new_elements), gfp_mask);
	if (!new_elements)
		return -ENOMEM;

	spin_lock_irqsave(&pool->lock, flags);
	if (unlikely(new_min_nr <= pool->min_nr)) {
		/* Raced, other resize will do our work */
		spin_unlock_irqrestore(&pool->lock, flags);
		kfree(new_elements);
		goto out;
	}
	memcpy(new_elements, pool->elements,
			pool->curr_nr * sizeof(*new_elements));
	kfree(pool->elements);
	pool->elements = new_elements;
	pool->min_nr = new_min_nr;

	while (pool->curr_nr < pool->min_nr) {
		spin_unlock_irqrestore(&pool->lock, flags);
		element = pool->alloc(gfp_mask, pool->pool_data);
		if (!element)
			goto out;
		spin_lock_irqsave(&pool->lock, flags);
		if (pool->curr_nr < pool->min_nr) {
			add_element(pool, element);
		} else {
			spin_unlock_irqrestore(&pool->lock, flags);
			pool->free(element, pool->pool_data);	/* Raced */
			goto out;
		}
	}
out_unlock:
	spin_unlock_irqrestore(&pool->lock, flags);
out:
	return 0;
}
EXPORT_SYMBOL(mempool_resize);

/**
	销毁内存池
 * mempool_destroy - deallocate a memory pool
 * @pool:      pointer to the memory pool which was allocated via
 *             mempool_create().
 *
 * this function only sleeps if the free_fn() function sleeps. The caller
 * has to guarantee that all elements have been returned to the pool (ie:
 * freed) prior to calling mempool_destroy().
 */
void mempool_destroy(mempool_t *pool)
{
	/* Check for outstanding elements 仍然有内存池的元素被使用，则报错，不可销毁内存池*/
	BUG_ON(pool->curr_nr != pool->min_nr);
	free_pool(pool);
}
EXPORT_SYMBOL(mempool_destroy);

/**
	从内存池中申请一个元素

 * mempool_alloc - allocate an element from a specific memory pool
 * @pool:      pointer to the memory pool which was allocated via
 *             mempool_create().
 * @gfp_mask:  the usual allocation bitmask. 内存分配标志
 *
 * this function only sleeps if the alloc_fn() function sleeps or
 * returns NULL. Note that due to preallocation, this function
 * *never* fails when called from process contexts. (it might
 * fail if called from an IRQ context.)
 */
void * mempool_alloc(mempool_t *pool, gfp_t gfp_mask)
{
	void *element;
	unsigned long flags;
	wait_queue_t wait;
	gfp_t gfp_temp;

	might_sleep_if(gfp_mask & __GFP_WAIT);

	gfp_mask |= __GFP_NOMEMALLOC;	/* don't allocate emergency reserves */
	gfp_mask |= __GFP_NORETRY;	/* don't loop in __alloc_pages */
	gfp_mask |= __GFP_NOWARN;	/* failures are OK */

	gfp_temp = gfp_mask & ~(__GFP_WAIT|__GFP_IO);

repeat_alloc:

	/* 调用malloc方法从基本内存分配器中分配一个内存元素；分配成功，则不触及内存池 */
	element = pool->alloc(gfp_temp, pool->pool_data);
	if (likely(element != NULL))
		return element;

	/*  分配不成功，则从内存池中分配，内存池中有足够元素可以分配 */
	spin_lock_irqsave(&pool->lock, flags);
	if (likely(pool->curr_nr)) {
		//从elements数组中取出一个元素
		element = remove_element(pool);
		spin_unlock_irqrestore(&pool->lock, flags);
		return element;
	}
	spin_unlock_irqrestore(&pool->lock, flags);
	
	/* We must not sleep in the GFP_ATOMIC case */
	if (!(gfp_mask & __GFP_WAIT))
		return NULL;
	
	/* 内存池中没有足够的元素分配，且在__GFP_WAIT置位的情况下，阻塞当前进程直到有一个内存元素被释放到内存池中 */
	/* Now start performing page reclaim */
	gfp_temp = gfp_mask;

	//阻塞当前进程
	init_wait(&wait);
	prepare_to_wait(&pool->wait, &wait, TASK_UNINTERRUPTIBLE);
	smp_mb();
	//直到有一个元素释放到内存池，才结束阻塞
	if (!pool->curr_nr) {
		/*
		 * FIXME: this should be io_schedule().  The timeout is there
		 * as a workaround for some DM problems in 2.6.18.
		 */
		io_schedule_timeout(5*HZ);
	}
	finish_wait(&pool->wait, &wait);

	goto repeat_alloc;
}
EXPORT_SYMBOL(mempool_alloc);

/**
	释放一个元素到内存池
 * mempool_free - return an element to the pool.
 * @element:   pool element pointer.
 * @pool:      pointer to the memory pool which was allocated via
 *             mempool_create().
 *
 * this function only sleeps if the free_fn() function sleeps.
 */
void mempool_free(void *element, mempool_t *pool)
{
	unsigned long flags;

	if (unlikely(element == NULL))
		return;

	smp_mb();

	/* 内存池未满（curr_nr < min_nr），则将元素加入到内存池 */
	if (pool->curr_nr < pool->min_nr) {
		spin_lock_irqsave(&pool->lock, flags);
		if (pool->curr_nr < pool->min_nr) {
			add_element(pool, element);
			spin_unlock_irqrestore(&pool->lock, flags);
			wake_up(&pool->wait);
			return;
		}
		spin_unlock_irqrestore(&pool->lock, flags);
	}
	/* 内存池满的话，释放到基本内存分配器中 */
	pool->free(element, pool->pool_data);
}
EXPORT_SYMBOL(mempool_free);

/*
 * A commonly used alloc and free fn.
 */
void *mempool_alloc_slab(gfp_t gfp_mask, void *pool_data)
{
	struct kmem_cache *mem = pool_data;
	return kmem_cache_alloc(mem, gfp_mask);
}
EXPORT_SYMBOL(mempool_alloc_slab);

void mempool_free_slab(void *element, void *pool_data)
{
	struct kmem_cache *mem = pool_data;
	kmem_cache_free(mem, element);
}
EXPORT_SYMBOL(mempool_free_slab);

/*
 * A commonly used alloc and free fn that kmalloc/kfrees the amount of memory
 * specified by pool_data
 */
void *mempool_kmalloc(gfp_t gfp_mask, void *pool_data)
{
	size_t size = (size_t)pool_data;
	return kmalloc(size, gfp_mask);
}
EXPORT_SYMBOL(mempool_kmalloc);

void mempool_kfree(void *element, void *pool_data)
{
	kfree(element);
}
EXPORT_SYMBOL(mempool_kfree);

/*
 * A simple mempool-backed page allocator that allocates pages
 * of the order specified by pool_data.
 */
void *mempool_alloc_pages(gfp_t gfp_mask, void *pool_data)
{
	int order = (int)(long)pool_data;
	return alloc_pages(gfp_mask, order);
}
EXPORT_SYMBOL(mempool_alloc_pages);

void mempool_free_pages(void *element, void *pool_data)
{
	int order = (int)(long)pool_data;
	__free_pages(element, order);
}
EXPORT_SYMBOL(mempool_free_pages);
