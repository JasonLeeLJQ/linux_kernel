/*
 * High memory handling common code and variables.
 *
 * (C) 1999 Andrea Arcangeli, SuSE GmbH, andrea@suse.de
 *          Gerhard Wichert, Siemens AG, Gerhard.Wichert@pdb.siemens.de
 *
 *
 * Redesigned the x86 32-bit VM architecture to deal with
 * 64-bit physical space. With current x86 CPUs this
 * means up to 64 Gigabytes physical RAM.
 *
 * Rewrote high memory support to move the page cache into
 * high memory. Implemented permanent (schedulable) kmaps
 * based on Linus' idea.
 *
 * Copyright (C) 1999 Ingo Molnar <mingo@redhat.com>
 */

#include <linux/mm.h>
#include <linux/module.h>
#include <linux/swap.h>
#include <linux/bio.h>
#include <linux/pagemap.h>
#include <linux/mempool.h>
#include <linux/blkdev.h>
#include <linux/init.h>
#include <linux/hash.h>
#include <linux/highmem.h>
#include <asm/tlbflush.h>

/*
 * Virtual_count is not a pure "count".
 *  0 means that it is not mapped, and has not been mapped
 *    since a TLB flush - it is usable.
 *  1 means that there are no users, but it has been mapped
 *    since the last TLB flush - so we can't use it.
 *  n means that there are (n-1) current users of it.
 */
#ifdef CONFIG_HIGHMEM

unsigned long totalhigh_pages __read_mostly;
EXPORT_SYMBOL(totalhigh_pages);

unsigned int nr_free_highpages (void)
{
	pg_data_t *pgdat;
	unsigned int pages = 0;

	for_each_online_pgdat(pgdat) {
		pages += zone_page_state(&pgdat->node_zones[ZONE_HIGHMEM],
			NR_FREE_PAGES);
		if (zone_movable_is_highmem())
			pages += zone_page_state(
					&pgdat->node_zones[ZONE_MOVABLE],
					NR_FREE_PAGES);
	}

	return pages;
}

static int pkmap_count[LAST_PKMAP];
static unsigned int last_pkmap_nr;  //全局变量，起始值为0
static  __cacheline_aligned_in_smp DEFINE_SPINLOCK(kmap_lock);

pte_t * pkmap_page_table;

static DECLARE_WAIT_QUEUE_HEAD(pkmap_map_wait);

/*
 * Most architectures have no use for kmap_high_get(), so let's abstract
 * the disabling of IRQ out of the locking in that case to save on a
 * potential useless overhead.
 */
#ifdef ARCH_NEEDS_KMAP_HIGH_GET
#define lock_kmap()             spin_lock_irq(&kmap_lock)
#define unlock_kmap()           spin_unlock_irq(&kmap_lock)
#define lock_kmap_any(flags)    spin_lock_irqsave(&kmap_lock, flags)
#define unlock_kmap_any(flags)  spin_unlock_irqrestore(&kmap_lock, flags)
#else
#define lock_kmap()             spin_lock(&kmap_lock)
#define unlock_kmap()           spin_unlock(&kmap_lock)
#define lock_kmap_any(flags)    \
		do { spin_lock(&kmap_lock); (void)(flags); } while (0)
#define unlock_kmap_any(flags)  \
		do { spin_unlock(&kmap_lock); (void)(flags); } while (0)
#endif

/* 第一次遍历中未能找到计数值为0的页表项，执行该函数
第一趟遍历pkmap_count数组找不到计数值为0的页表项时，就要调用flush_all_zero_pkmaps(),
将计数值为1的页表项置为0，撤销已经不用了的映射，并且刷新TLB*/
static void flush_all_zero_pkmaps(void)
{
	int i;
	int need_flush = 0;

	flush_cache_kmaps();

	for (i = 0; i < LAST_PKMAP; i++) {
		struct page *page;

		/*
		 * zero means we don't have anything to do,
		 * >1 means that it is still in use. Only
		 * a count of 1 means that it is free but
		 * needs to be unmapped
		 */
		/*将计数值为1的页面的计数值设为0，*/
		if (pkmap_count[i] != 1)
			continue;
		pkmap_count[i] = 0;

		/* sanity check */
		BUG_ON(pte_none(pkmap_page_table[i]));

		/*
		 * Don't need an atomic fetch-and-clear op here;
		 * no-one has the page mapped, and cannot get at
		 * its virtual address (and hence PTE) without first
		 * getting the kmap_lock (which is held here).
		 * So no dangers, even with speculative execution.
		 */
		/* 撤销之前的映射关系，并将page从page_address_htable散列表中删除 */
		page = pte_page(pkmap_page_table[i]);
		pte_clear(&init_mm, (unsigned long)page_address(page),
			  &pkmap_page_table[i]);

		set_page_address(page, NULL);
		need_flush = 1;
	}
	if (need_flush)  /* 刷新TLB */
		flush_tlb_kernel_range(PKMAP_ADDR(0), PKMAP_ADDR(LAST_PKMAP));
}

/**
 * kmap_flush_unused - flush all unused kmap mappings in order to remove stray mappings
 */
void kmap_flush_unused(void)
{
	lock_kmap();
	flush_all_zero_pkmaps();
	unlock_kmap();
}

//map_new_virtual将页框的物理地址插入到pkmap_page_table页表的一个项中，并在page_address_htable散列表中加入一个元素
static inline unsigned long map_new_virtual(struct page *page)
{
	unsigned long vaddr;
	int count;

start:
	 /*LAST_PKMAP为永久映射区可以映射的页框数，在禁用PAE的情况下为512，开启PAE的情况下为1024， 
    也就是说内核通过kmap,一次最多能映射2M/4M的高端内存*/
	count = LAST_PKMAP;
	/* Find an empty entry 
	在pkmap_count数组中找到一个可用的页表项
	*/
	for (;;) {
		/*last_pkmap_nr记录了上次遍历pkmap_count数组找到一个空闲页表项后的位置，首先从 
        last_pkmap_nr出开始遍历，如果未能在pkmap_count中找到计数值为0的页表项,则last_pkmap_nr 
        和LAST_PKMAP_MASK相与后又回到0，进行第二轮遍历*/
		last_pkmap_nr = (last_pkmap_nr + 1) & LAST_PKMAP_MASK;

		/*last_pkmap_nr变为了0，也就是说第一次遍历中未能找到计数值为0的页表项*/
		if (!last_pkmap_nr) {
			flush_all_zero_pkmaps();
			count = LAST_PKMAP;
		}
		if (!pkmap_count[last_pkmap_nr])  /*找到一个计数值为0的页表项，即空闲可用的页表项*/
			break;	/* Found a usable entry */
		if (--count)
			continue;

		/*
		 * Sleep for somebody else to unmap their entries
		 */
		  /*在pkmap_count数组中，找不到计数值为0或1的页表项，即所有页表项都被内核映射了， 
            则声明一个等待队列，并将当前要求映射高端内存的进程添加到等待队列中然后 
            阻塞该进程，等待其他的进程释放了KMAP区的某个页框的映射*/
		{
			DECLARE_WAITQUEUE(wait, current);

			__set_current_state(TASK_UNINTERRUPTIBLE);
			add_wait_queue(&pkmap_map_wait, &wait);
			unlock_kmap();
			schedule();
			remove_wait_queue(&pkmap_map_wait, &wait);
			lock_kmap();

			/* Somebody else might have mapped it while we slept */
			/*在睡眠的时候，可能有其他的进程映射了该页面，所以先试图获取页面的虚拟地址，成功的话直接返回*/
			if (page_address(page))
				return (unsigned long)page_address(page);

			/* Re-start */
			goto start;  /*唤醒后重新执行遍历操作*/
		}
	}
	/*寻找到了一个未被映射的页表项，获取该页表项对应的线性地址并赋给vaddr*/
	vaddr = PKMAP_ADDR(last_pkmap_nr);
	/*将pkmap_page_table中对应的pte设为申请映射的页框的pte,完成永久内核映射区中的页表项到物理页框的映射*/
	set_pte_at(&init_mm, vaddr,
		   &(pkmap_page_table[last_pkmap_nr]), mk_pte(page, kmap_prot));

	pkmap_count[last_pkmap_nr] = 1;
	/*设置页面的虚拟地址，将该页面添加到page_address_htable散列表中*/ 
	set_page_address(page, (void *)vaddr);

	return vaddr;
}

/**
 * kmap_high - map a highmem page into memory
 * @page: &struct page to map
 *
 * Returns the page's virtual memory address.
 *
 * We cannot call this from interrupts, as it may block.
 */
void *kmap_high(struct page *page)
{
	unsigned long vaddr;

	/*
	 * For highmem pages, we can't trust "virtual" until
	 * after we have the lock.
	 */
	lock_kmap();
	vaddr = (unsigned long)page_address(page);  //返回page对应的线性地址
	if (!vaddr)  //检查该page是否被映射了，
		vaddr = map_new_virtual(page);//如果没有，则调用map_new_virtual将页框的物理地址插入到pkmap_page_table页表的一个项中，并在page_address_htable散列表中加入一个元素
	pkmap_count[PKMAP_NR(vaddr)]++;  //页框的线性地址所对应的计数器+1
	BUG_ON(pkmap_count[PKMAP_NR(vaddr)] < 2);
	unlock_kmap();
	return (void*) vaddr;
}

EXPORT_SYMBOL(kmap_high);

#ifdef ARCH_NEEDS_KMAP_HIGH_GET
/**
 * kmap_high_get - pin a highmem page into memory
 * @page: &struct page to pin
 *
 * Returns the page's current virtual memory address, or NULL if no mapping
 * exists.  If and only if a non null address is returned then a
 * matching call to kunmap_high() is necessary.
 *
 * This can be called from any context.
 */
void *kmap_high_get(struct page *page)
{
	unsigned long vaddr, flags;

	lock_kmap_any(flags);
	vaddr = (unsigned long)page_address(page);
	if (vaddr) {
		BUG_ON(pkmap_count[PKMAP_NR(vaddr)] < 1);
		pkmap_count[PKMAP_NR(vaddr)]++;
	}
	unlock_kmap_any(flags);
	return (void*) vaddr;
}
#endif

/**
 撤销由kmap()建立的永久内核映射

 * kunmap_high - map a highmem page into memory
 * @page: &struct page to unmap
 *
 * If ARCH_NEEDS_KMAP_HIGH_GET is not defined then this may be called
 * only from user context.
 */
void kunmap_high(struct page *page)
{
	unsigned long vaddr;
	unsigned long nr;
	unsigned long flags;
	int need_wakeup;

	lock_kmap_any(flags);
	vaddr = (unsigned long)page_address(page);
	BUG_ON(!vaddr);
	nr = PKMAP_NR(vaddr);

	/*
	 * A count must never go down to zero
	 * without a TLB flush!
	 */
	need_wakeup = 0;
	switch (--pkmap_count[nr]) { //该页面对应的页表项计数减1。如果原先pkmap_count[nr]==1，现在pkmap_count[nr]=0,该页表项变为可用，撤销了永久内核映射
	case 0:
		BUG();
	case 1:
		/*
		 * Avoid an unnecessary wake_up() function call.
		 * The common case is pkmap_count[] == 1, but
		 * no waiters.
		 * The tasks queued in the wait-queue are guarded
		 * by both the lock in the wait-queue-head and by
		 * the kmap_lock.  As the kmap_lock is held here,
		 * no need for the wait-queue-head's lock.  Simply
		 * test if the queue is empty.
		 */
		 /*确定pkmap_map_wait等待队列是否为空*/
		need_wakeup = waitqueue_active(&pkmap_map_wait);
	}
	unlock_kmap_any(flags);

	/* do wake-up, if needed, race-free outside of the spin lock */
	/*等待队列不为空，则唤醒由map_new_virtual（）添加在等待队列中的进程，因为这些进程一直在阻塞，等待空闲的高端内存区域。
	该等待队列是在map_new_virtual（）中添加的*/
	if (need_wakeup)  
		wake_up(&pkmap_map_wait);
}

EXPORT_SYMBOL(kunmap_high);
#endif

#if defined(HASHED_PAGE_VIRTUAL)

#define PA_HASH_ORDER	7

/*
 * Describes one page->virtual association
 */
struct page_address_map {
	struct page *page;
	void *virtual;
	struct list_head list;
};

/*
 * page_address_map freelist, allocated from page_address_maps.
 */
static struct list_head page_address_pool;	/* freelist */
static spinlock_t pool_lock;			/* protects page_address_pool */

/*
 * Hash table bucket
 */
static struct page_address_slot {
	struct list_head lh;			/* List of page_address_maps */
	spinlock_t lock;			/* Protect this bucket's list */
} ____cacheline_aligned_in_smp page_address_htable[1<<PA_HASH_ORDER];

static struct page_address_slot *page_slot(struct page *page)
{
	return &page_address_htable[hash_ptr(page, PA_HASH_ORDER)];
}

/**
	将物理高端内存上的page映射到 内核的虚拟地址（线性地址） 上
 * page_address - get the mapped virtual address of a page
 * @page: &struct page to get the virtual address of
 *
 * Returns the page's virtual address.
 */
void *page_address(struct page *page)
{
	unsigned long flags;
	void *ret;
	struct page_address_slot *pas;

	//页框不在高端内存中
	if (!PageHighMem(page))
		return lowmem_page_address(page);

	//页框位于高端内存中
	pas = page_slot(page);  //（永久内核映射存放在page_address_htable），获得page_address_htable
	ret = NULL;
	spin_lock_irqsave(&pas->lock, flags);

	//在page_address_htable中查找对应的线性地址，如果查找到，返回该page_address_htable中的线性地址，否则返回NULL
	if (!list_empty(&pas->lh)) {
		struct page_address_map *pam;

		list_for_each_entry(pam, &pas->lh, list) {
			if (pam->page == page) {
				ret = pam->virtual;
				goto done;
			}
		}
	}
done:
	spin_unlock_irqrestore(&pas->lock, flags);
	return ret;
}

EXPORT_SYMBOL(page_address);

/**
 * set_page_address - set a page's virtual address
 * @page: &struct page to set
 * @virtual: virtual address to use
 */
 /*
	将page与该页表项对应的线性地址进行关联，
	这里并不是简单的给page结构中的virtual给赋上相应的值，而是将该映射的page添加到page_address_htable散列表中，
	该散列表维护着所有被映射到永久内核映射区的页框，散列表中的每一项记录了页框的page结构地址和映射页框的线性地址。
*/
void set_page_address(struct page *page, void *virtual)
{
	unsigned long flags;
	struct page_address_slot *pas;
	struct page_address_map *pam;

	BUG_ON(!PageHighMem(page));

	pas = page_slot(page);
	if (virtual) {		/* Add */
		BUG_ON(list_empty(&page_address_pool));

		spin_lock_irqsave(&pool_lock, flags);

		/*从page_address_pool中得到一个空闲的page_address_map*/
		pam = list_entry(page_address_pool.next,
				struct page_address_map, list);
		list_del(&pam->list);  /*将该节点从page_address_pool中删除*/
		spin_unlock_irqrestore(&pool_lock, flags);

		/*将页框的page结构地址和虚拟地址保存到page_address_map中，可以看到并没有直接设定page的虚拟地址*/
		pam->page = page;
		pam->virtual = virtual;

		spin_lock_irqsave(&pas->lock, flags);
		list_add_tail(&pam->list, &pas->lh);  /*将pam添入散列表*/
		spin_unlock_irqrestore(&pas->lock, flags);
	} else {		/* Remove */ /*从散列表中删除一个节点，执行与上面相反的操作*/
		spin_lock_irqsave(&pas->lock, flags);
		list_for_each_entry(pam, &pas->lh, list) {
			if (pam->page == page) {
				list_del(&pam->list);
				spin_unlock_irqrestore(&pas->lock, flags);
				spin_lock_irqsave(&pool_lock, flags);
				list_add_tail(&pam->list, &page_address_pool);  /*将该节点加入到page_address_pool中*/
				spin_unlock_irqrestore(&pool_lock, flags);
				goto done;
			}
		}
		spin_unlock_irqrestore(&pas->lock, flags);
	}
done:
	return;
}

static struct page_address_map page_address_maps[LAST_PKMAP];

void __init page_address_init(void)
{
	int i;

	INIT_LIST_HEAD(&page_address_pool);
	for (i = 0; i < ARRAY_SIZE(page_address_maps); i++)
		list_add(&page_address_maps[i].list, &page_address_pool);
	for (i = 0; i < ARRAY_SIZE(page_address_htable); i++) {
		INIT_LIST_HEAD(&page_address_htable[i].lh);
		spin_lock_init(&page_address_htable[i].lock);
	}
	spin_lock_init(&pool_lock);
}

#endif	/* defined(CONFIG_HIGHMEM) && !defined(WANT_PAGE_VIRTUAL) */

#if defined(CONFIG_DEBUG_HIGHMEM) && defined(CONFIG_TRACE_IRQFLAGS_SUPPORT)

void debug_kmap_atomic(enum km_type type)
{
	static int warn_count = 10;

	if (unlikely(warn_count < 0))
		return;

	if (unlikely(in_interrupt())) {
		if (in_nmi()) {
			if (type != KM_NMI && type != KM_NMI_PTE) {
				WARN_ON(1);
				warn_count--;
			}
		} else if (in_irq()) {
			if (type != KM_IRQ0 && type != KM_IRQ1 &&
			    type != KM_BIO_SRC_IRQ && type != KM_BIO_DST_IRQ &&
			    type != KM_BOUNCE_READ && type != KM_IRQ_PTE) {
				WARN_ON(1);
				warn_count--;
			}
		} else if (!irqs_disabled()) {	/* softirq */
			if (type != KM_IRQ0 && type != KM_IRQ1 &&
			    type != KM_SOFTIRQ0 && type != KM_SOFTIRQ1 &&
			    type != KM_SKB_SUNRPC_DATA &&
			    type != KM_SKB_DATA_SOFTIRQ &&
			    type != KM_BOUNCE_READ) {
				WARN_ON(1);
				warn_count--;
			}
		}
	}

	if (type == KM_IRQ0 || type == KM_IRQ1 || type == KM_BOUNCE_READ ||
			type == KM_BIO_SRC_IRQ || type == KM_BIO_DST_IRQ ||
			type == KM_IRQ_PTE || type == KM_NMI ||
			type == KM_NMI_PTE ) {
		if (!irqs_disabled()) {
			WARN_ON(1);
			warn_count--;
		}
	} else if (type == KM_SOFTIRQ0 || type == KM_SOFTIRQ1) {
		if (irq_count() == 0 && !irqs_disabled()) {
			WARN_ON(1);
			warn_count--;
		}
	}
}

#endif
