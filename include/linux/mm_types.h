#ifndef _LINUX_MM_TYPES_H
#define _LINUX_MM_TYPES_H

#include <linux/auxvec.h>
#include <linux/types.h>
#include <linux/threads.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/prio_tree.h>
#include <linux/rbtree.h>
#include <linux/rwsem.h>
#include <linux/completion.h>
#include <linux/cpumask.h>
#include <linux/page-debug-flags.h>
#include <asm/page.h>
#include <asm/mmu.h>

#ifndef AT_VECTOR_SIZE_ARCH
#define AT_VECTOR_SIZE_ARCH 0
#endif
#define AT_VECTOR_SIZE (2*(AT_VECTOR_SIZE_ARCH + AT_VECTOR_SIZE_BASE + 1))

struct address_space;

#define USE_SPLIT_PTLOCKS	(NR_CPUS >= CONFIG_SPLIT_PTLOCK_CPUS)

/*
 * Each physical page in the system has a struct page associated with
 * it to keep track of whatever it is we are using the page for at the
 * moment. Note that we have no way to track which tasks are using
 * a page, though if it is a pagecache page, rmap structures can tell us
 * who is mapping it.
 */
 //注意：page结构描述的是物理页，而不是虚拟页
struct page {
	// 页的状态（例如，是不是脏页或是否被锁定在内存中)
	//flags标志位于<linux/page-flags.h>
	unsigned long flags;		/*Atomic flags, some possibly
					 * updated asynchronously */

	/* 页框的引用计数，如果为-1，则此页框空闲，并可分配给任一进程或内核；
		如果大于或等于0，则说明页框被分配给了一个或多个进程，或用于存放内核数据。
		page_count()返回_count加1的值，也就是该页的使用者数目 */
	atomic_t _count;		/* 该页的引用计数Usage count, see below. */

	union {
		/* 页框中的页映射计数器（被页表映射的次数，即该page同时被多少个进程共享），如果没有为-1，
		如果为PAGE_BUDDY_MAPCOUNT_VALUE(-128)，说明此页及其后的一共2的private次方个数页框处于伙伴系统中，
		正在被只有一个进程的页表映射了，该值为0 */
		atomic_t _mapcount;	/* Count of ptes mapped in mms,
					 * to show when page is mapped
					 * & limit reverse map searches.
					 */
		struct {		/* SLUB使用 */
			u16 inuse;   //用于slub分配器：对象的数目
			u16 objects;
		};
	};
	union {
	    struct {
		/* 私有数据指针，由应用场景确定其具体的含义 
		
		1、如果设置了PG_private标志，则private字段指向struct buffer_head
		2、如果设置了PG_compound，则指向struct page
		3、如果设置了PG_swapcache标志，private存储了该page在交换分区中对应的位置信息swp_entry_t。
		4、如果_mapcount = PAGE_BUDDY_MAPCOUNT_VALUE，说明该page位于伙伴系统，private存储该伙伴的阶（即k，块 == 2的k次方个page）
		*/
		unsigned long private;		/* Mapping-private opaque data:
					 	 * usually used for buffer_heads
						 * if PagePrivate set; used for
						 * swp_entry_t if PageSwapCache;
						 * indicates order in the buddy
						 * system if PG_buddy is set.
						 */
		/* 用于页描述符，当页被插入页高速缓存中时使用，或者当页属于匿名区时使用 */
		struct address_space *mapping;	/* If low bit clear, points to
						 * inode address_space, or NULL.
						 * If page mapped as anonymous
						 * memory, low bit is set, and
						 * it points to anon_vma object:
						 * see PAGE_MAPPING_ANON below.
						 */
	    };
#if USE_SPLIT_PTLOCKS
	    spinlock_t ptl;
#endif
		/*  SLAB描述符使用，指向SLAB的高速缓存 */ 
	    struct kmem_cache *slab;	/* SLUB: Pointer to slab */
	    struct page *first_page;	/* Compound tail pages 用于复合页的尾页，指向首页*/
	};
	union {
		/* 在页所有者的地址空间中以页大小为单位的偏移量（以页为单位的偏移量），也就是在所有者的磁盘中页中数据的位置
		一个文件可能只映射一部分，假设映射了1M的空间，index指的是在1M空间内的偏移，而不是在整个文件内的偏移 */
		pgoff_t index;		/* Our offset within mapping. */
		void *freelist;		/* SLUB: freelist req. slab lock */
	};
	/* 包含到页的最近最少使用(LRU)双向链表的指针，用于插入伙伴系统的空闲链表中，只有块中头页框要被插入
		当lru用于伙伴系统时，lru字段指向的是同一链表中相邻的元素
	*/
	struct list_head lru;		/* Pageout list, eg. active_list  换出页列表
					 * protected by zone->lru_lock !
					 */
	/*
	 * On machines where all RAM is mapped into kernel address space,
	 * we can simply calculate the virtual address. On machines with
	 * highmem some memory is mapped into kernel virtual memory
	 * dynamically, so we need a place to store that address.
	 * Note that this field could be 16 bits on x86 ... ;)
	 *
	 * Architectures with slow multiplication can define
	 * WANT_PAGE_VIRTUAL in asm/page.h
	 */
#if defined(WANT_PAGE_VIRTUAL)
	/*页的虚拟地址（页在虚拟内存中的地址）
		如果是高端内存映射的话，映射是动态映射，不会永久的映射到内核地址空间上，此时设置为NULL
	*/
	void *virtual;			/* Kernel virtual address (NULL if
					   not kmapped, ie. highmem) */
#endif /* WANT_PAGE_VIRTUAL */
#ifdef CONFIG_WANT_PAGE_DEBUG_FLAGS
	unsigned long debug_flags;	/* Use atomic bitops on this */
#endif

#ifdef CONFIG_KMEMCHECK
	/*
	 * kmemcheck wants to track the status of each byte in a page; this
	 * is a pointer to such a status block. NULL if not tracked.
	 */
	void *shadow;
#endif
};

/*
 * A region containing a mapping of a non-memory backed file under NOMMU
 * conditions.  These are held in a global tree and are pinned by the VMAs that
 * map parts of them.
 */
struct vm_region {
	struct rb_node	vm_rb;		/* link in global region tree */
	unsigned long	vm_flags;	/* VMA vm_flags */
	unsigned long	vm_start;	/* start address of region */
	unsigned long	vm_end;		/* region initialised to here */
	unsigned long	vm_top;		/* region allocated to here */
	unsigned long	vm_pgoff;	/* the offset in vm_file corresponding to vm_start */
	struct file	*vm_file;	/* the backing file or NULL */

	int		vm_usage;	/* region usage count (access under nommu_region_sem) */
	bool		vm_icache_flushed : 1; /* true if the icache has been flushed for
						* this region */
};

/*
 * This struct defines a memory VMM memory area. There is one of these
 * per VM-area/task.  A VM area is any part of the process virtual memory
 * space that has a special rule for the page-fault handlers (ie a shared
 * library, the executable area etc).
 */
 //虚拟内存区域（也叫内存区域），一个内存描述符mm_struct有着若干个内存区域（vm_area_struct）
struct vm_area_struct {
	struct mm_struct * vm_mm;	/* 该内存区域属于哪一个mm_struct结构体，The address space we belong to. */
	unsigned long vm_start;		/* 内存区域的首地址（该地址是虚拟地址） Our start address within vm_mm. */
	unsigned long vm_end;		/* 内存区域的尾地址 The first byte after our end address
					   within vm_mm. */

	/* linked list of VM areas per task, sorted by address */
	struct vm_area_struct *vm_next;    // VMA链表

	pgprot_t vm_page_prot;		/* VMA的访问控制权限 Access permissions of this VMA. */
	unsigned long vm_flags;		/* 标志 Flags, see mm.h. */

	struct rb_node vm_rb;     /* 红黑树上该VMA的节点 */

	/*
	 * For areas with an address space and backing store,
	 * linkage into the address_space->i_mmap prio tree, or
	 * linkage to the list of like vmas hanging off its node, or
	 * linkage of vma in the address_space->i_mmap_nonlinear list.
	 */
	union {
		struct {
			struct list_head list;
			void *parent;	/* aligns with prio_tree_node parent */
			struct vm_area_struct *head;
		} vm_set;

		struct raw_prio_tree_node prio_tree_node;
	} shared;

	/*
	 * A file's MAP_PRIVATE vma can be in both i_mmap tree and anon_vma
	 * list, after a COW of one of the file pages.	A MAP_SHARED vma
	 * can only be in the i_mmap tree.  An anonymous MAP_PRIVATE, stack
	 * or brk vma (with NULL file) can only be in an anon_vma list.
	 */
	struct list_head anon_vma_chain; /* Serialized by mmap_sem &
					  * page_table_lock */
	struct anon_vma *anon_vma;	/* Serialized by page_table_lock */

	/* Function pointers to deal with this struct. */
	const struct vm_operations_struct *vm_ops;   /* 相关的操作表 */

	/* Information about our backing store: 
		后备存储器的相关信息
	*/
	unsigned long vm_pgoff;		/* 文件映射中的偏移量，该值用于只映射了文件部分内容时（映射了整个文件，偏移量为0） Offset (within vm_file) in PAGE_SIZE
					   units, *not* PAGE_CACHE_SIZE */
	struct file * vm_file;		/* 描述被映射的文件（如果存在）File we map to (can be NULL). */
	void * vm_private_data;		/* 私有数据 was vm_pte (shared mem) */
	unsigned long vm_truncate_count;/* truncate_count or restart_addr */

#ifndef CONFIG_MMU
	struct vm_region *vm_region;	/* NOMMU mapping region */
#endif
#ifdef CONFIG_NUMA
	struct mempolicy *vm_policy;	/* NUMA policy for the VMA */
#endif
};

struct core_thread {
	struct task_struct *task;
	struct core_thread *next;
};

struct core_state {
	atomic_t nr_threads;
	struct core_thread dumper;
	struct completion startup;
};

enum {
	MM_FILEPAGES,
	MM_ANONPAGES,
	MM_SWAPENTS,
	NR_MM_COUNTERS
};

#if USE_SPLIT_PTLOCKS && defined(CONFIG_MMU)
#define SPLIT_RSS_COUNTING
struct mm_rss_stat {
	atomic_long_t count[NR_MM_COUNTERS];
};
/* per-thread cached information, */
struct task_rss_stat {
	int events;	/* for synchronization threshold */
	int count[NR_MM_COUNTERS];
};
#else  /* !USE_SPLIT_PTLOCKS */
struct mm_rss_stat {
	unsigned long count[NR_MM_COUNTERS];
};
#endif /* !USE_SPLIT_PTLOCKS */

//内存描述符，表示进程地址空间(一个进程只有一个唯一的内存描述符mm_struct)
struct mm_struct {
	struct vm_area_struct * mmap;		/* list of VMAs 内存区域组成的链表*/
	struct rb_root mm_rb;            //内存区域VMA形成的红黑树，其实，mmap和mm_rb描述的对象的都是一样的，只是数据结构的区别；但是并不是冗余的做法；
	struct vm_area_struct * mmap_cache;	/* last find_vma result 最近使用的内存区域*/
#ifdef CONFIG_MMU
	unsigned long (*get_unmapped_area) (struct file *filp,
				unsigned long addr, unsigned long len,
				unsigned long pgoff, unsigned long flags);
	void (*unmap_area) (struct mm_struct *mm, unsigned long addr);
#endif
	unsigned long mmap_base;		/* 用于内存映射的区域 base of mmap area */
	unsigned long task_size;		/* size of task vm space */
	unsigned long cached_hole_size; 	/* if non-zero, the largest hole below free_area_cache */
	unsigned long free_area_cache;		/* 内核从这个地址开始搜索进程地址空间中线性地址的空闲区间 first hole of size cached_hole_size or larger */
	pgd_t * pgd;            //页全局目录(全局页表)，负责将进程地址空间的虚拟地址通过页表转换成物理地址。
	atomic_t mm_users;			/* 使用地址空间的用户数 How many users with user space? */
	atomic_t mm_count;			/* 主使用计数器 How many references to "struct mm_struct" (users count as 1) */
	int map_count;				/* 内存区域VMA的个数 number of VMAs */
	struct rw_semaphore mmap_sem;  //内存区域的信号量
	spinlock_t page_table_lock;		/* 页表自旋锁、操作和检索页表时，都需要使用页表锁，防止竞争 Protects page tables and some counters */

	struct list_head mmlist;		/* 所有mm_struct形成的链表 List of maybe swapped mm's.	These are globally strung
						 * together off init_mm.mmlist, and are protected
						 * by mmlist_lock
						 */


	unsigned long hiwater_rss;	/* High-watermark of RSS usage */
	unsigned long hiwater_vm;	/* High-water virtual memory usage */

	unsigned long total_vm, locked_vm, shared_vm, exec_vm;      //全部页面数量、上锁的页面数量、共享的页面数量、
	unsigned long stack_vm, reserved_vm, def_flags, nr_ptes;
	unsigned long start_code, end_code, start_data, end_data;  //代码段、数据段的首地址和尾地址
	unsigned long start_brk, brk, start_stack;                 //堆的首地址和尾地址，进程栈的首地址
	unsigned long arg_start, arg_end, env_start, env_end;      //命令行参数的首地址、尾地址；环境变量的首地址、尾地址

	unsigned long saved_auxv[AT_VECTOR_SIZE]; /* for /proc/PID/auxv */

	/*
	 * Special counters, in some configurations protected by the
	 * page_table_lock, in other configurations by being atomic.
	 */
	struct mm_rss_stat rss_stat;

	struct linux_binfmt *binfmt;

	cpumask_t cpu_vm_mask;

	/* Architecture-specific MM context 体系结构特殊数据*/
	mm_context_t context;

	/* Swap token stuff */
	/*
	 * Last value of global fault stamp as seen by this process.
	 * In other words, this value gives an indication of how long
	 * it has been since this task got the token.
	 * Look at mm/thrash.c
	 */
	unsigned int faultstamp;
	unsigned int token_priority;
	unsigned int last_interval;

	unsigned long flags; /* 状态标志 Must use atomic bitops to access the bits */

	struct core_state *core_state; /* coredumping support */
#ifdef CONFIG_AIO
	spinlock_t		ioctx_lock;   //AIO I/O链表锁
	struct hlist_head	ioctx_list;   //AIO I/O链表
#endif
#ifdef CONFIG_MM_OWNER
	/*
	 * "owner" points to a task that is regarded as the canonical
	 * user/owner of this mm. All of the following must be true in
	 * order for it to be changed:
	 *
	 * current == mm->owner
	 * current->mm != mm
	 * new_owner->mm == mm
	 * new_owner->alloc_lock is held
	 */
	struct task_struct *owner;
#endif

#ifdef CONFIG_PROC_FS
	/* store ref to file /proc/<pid>/exe symlink points to */
	struct file *exe_file;
	unsigned long num_exe_file_vmas;
#endif
#ifdef CONFIG_MMU_NOTIFIER
	struct mmu_notifier_mm *mmu_notifier_mm;
#endif
};

/* Future-safe accessor for struct mm_struct's cpu_vm_mask. */
#define mm_cpumask(mm) (&(mm)->cpu_vm_mask)

#endif /* _LINUX_MM_TYPES_H */
