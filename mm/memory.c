/*
 *  linux/mm/memory.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * demand-loading started 01.12.91 - seems it is high on the list of
 * things wanted, and it should be easy to implement. - Linus
 */

/*
 * Ok, demand-loading was easy, shared pages a little bit tricker. Shared
 * pages started 02.12.91, seems to work. - Linus.
 *
 * Tested sharing by executing about 30 /bin/sh: under the old kernel it
 * would have taken more than the 6M I have free, but it worked well as
 * far as I could see.
 *
 * Also corrected some "invalidate()"s - I wasn't doing enough of them.
 */

#include <signal.h>

#include <asm/system.h>

#include <linux/sched.h>
#include <linux/head.h>
#include <linux/kernel.h>

volatile void do_exit(long code);

static inline volatile void oom(void)
{
	printk("out of memory\n\r");
	do_exit(SIGSEGV);
}
// 置cr3为0，把0赋给eax，eax赋给cr3，cr3是保存页目录基地址的
#define invalidate() \
__asm__("movl %%eax,%%cr3"::"a" (0))

/* these are not to be changed without changing head.s etc */
#define LOW_MEM 0x100000
#define PAGING_MEMORY (15*1024*1024)
// 多少页，>>12即除以4kb
#define PAGING_PAGES (PAGING_MEMORY>>12)
// 给定一个地址，算出在哪一页
#define MAP_NR(addr) (((addr)-LOW_MEM)>>12)
#define USED 100

#define CODE_SPACE(addr) ((((addr)+4095)&~4095) < \
current->start_code + current->end_code)

static long HIGH_MEMORY = 0;
// 把一页的内容从from复制到to
#define copy_page(from,to) \
__asm__("cld ; rep ; movsl"::"S" (from),"D" (to),"c" (1024):"cx","di","si")

static unsigned char mem_map [ PAGING_PAGES ] = {0,};

/*
 * Get physical address of first (actually last :-) free page, and mark it
 * used. If no free pages left, return 0.
 */
unsigned long get_free_page(void)
{
register unsigned long __res asm("ax");
/*
	清方向，查找和al(0)相等的项，scab是以ecx为循环次数，
	edi为首地址开始循环对比。知道找到等于0或ecx为0结束循环。
	找到的话CF等于1。jne 1f说明cf等于0的时候跳到标签1处，即找不到，
	找到后对一页的内容清0
*/
__asm__("std ; repne ; scasb\n\t"
	"jne 1f\n\t"
	"movb $1,1(%%edi)\n\t"
	"sall $12,%%ecx\n\t"
	"addl %2,%%ecx\n\t"
	"movl %%ecx,%%edx\n\t"
	"movl $1024,%%ecx\n\t"
	"leal 4092(%%edx),%%edi\n\t"
	"rep ; stosl\n\t"
	"movl %%edx,%%eax\n"
	"1:"
	// a即eax，是输入也是输出，输入时值是0，输出时把eax赋给__res
	:"=a" (__res)
	:"0" (0),"i" (LOW_MEM),"c" (PAGING_PAGES),// c即ecx，最多遍历的次数
	// D是edi，即从后往前遍历
	"D" (mem_map+PAGING_PAGES-1)
	:"di","cx","dx");
return __res;
}

/*
 * Free a page of memory at physical address 'addr'. Used by
 * 'free_page_tables()'
 */
// addr：要释放的物理地址，修改标记位即可，再次分配的时候会清0
void free_page(unsigned long addr)
{
	if (addr < LOW_MEM) return;
	if (addr >= HIGH_MEMORY)
		panic("trying to free nonexistent page");
	// 减去低端内存，得到主内存首地址
	addr -= LOW_MEM;
	// 算出第几页
	addr >>= 12;
	// 引用数减一，不为0则说明还有进程引用，否则置0
	if (mem_map[addr]--) return;
	mem_map[addr]=0;
	panic("trying to free free page");
}

/*
 * This function frees a continuos block of page tables, as needed
 * by 'exit()'. As does copy_page_tables(), this handles only 4Mb blocks.
 */
// from是线性地址。释放from开始，连续的n个大小为4MB的页面对应的物理地址。最后释放页表、页目录项
int free_page_tables(unsigned long from,unsigned long size)
{
	unsigned long *pg_table;
	unsigned long * dir, nr;
	// 判断是否按4MB对齐
	if (from & 0x3fffff)
		panic("free_page_tables called with wrong alignment");
	if (!from)
		panic("Trying to free up swapper memory space");
	// 算出size包含多少个MB，比如size是0 - 1>>22，则计算机后是1
	size = (size + 0x3fffff) >> 22;
	/*
		页目录在地址0开始的地方，首先右移得到页目录索引，
		根据索引得到页目录项内容，因为页目录项的内容占4个字节，
		其中高20位是页表地址，低12位是标记位，，所以要乘以4得到
		from对应的页目录项的地址。即dir = from >> 22 << 2 = from >> 20,
		但是代码里是直接右移20位，所以需要和0xffc与，把低两位置0，最后得到from
		对应的页目录项的地址
	*/
	dir = (unsigned long *) ((from>>20) & 0xffc); /* _pg_dir = 0 */
	for ( ; size-->0 ; dir++) {
		// 低位是1说明该页目录项有效
		if (!(1 & *dir))
			continue;
		// *dir为页表首地址，与0xfffff000是因为高二十位是有效地址，低12位是标记位
		pg_table = (unsigned long *) (0xfffff000 & *dir);
		// 释放每个页表指向的物理地址
		for (nr=0 ; nr<1024 ; nr++) {
			// 页表是否有效，有效则释放*pg_table指向物理地址，以4kb对齐
			if (1 & *pg_table)
				// 与0xfffff000是因为高二十位是有效地址，低12位是标记位 
				free_page(0xfffff000 & *pg_table);
			// 置页表无效
			*pg_table = 0;
			// 下一个页表
			pg_table++;
		}
		// 释放页表占据的物理地址
		free_page(0xfffff000 & *dir);
		// 置页目录项为无效
		*dir = 0;
	}
	invalidate();
	return 0;
}

/*
 *  Well, here is one of the most complicated functions in mm. It
 * copies a range of linerar addresses by copying only the pages.
 * Let's hope this is bug-free, 'cause this one I don't want to debug :-)
 *
 * Note! We don't copy just any chunks of memory - addresses have to
 * be divisible by 4Mb (one page-directory entry), as this makes the
 * function easier. It's used only by fork anyway.
 *
 * NOTE 2!! When from==0 we are copying kernel space for the first
 * fork(). Then we DONT want to copy a full page-directory entry, as
 * that would lead to some serious memory waste - we just copy the
 * first 160 pages - 640kB. Even that is more than we need, but it
 * doesn't take any more memory - we don't copy-on-write in the low
 * 1 Mb-range, so the pages can be shared with the kernel. Thus the
 * special case for nr=xxxx.
 */
// 在fork的时候调用，复制父进程页表。把线性地址from开始的n个MB地址对应的页表和页目录项的内容复制给to对应的页表和页目录项
int copy_page_tables(unsigned long from,unsigned long to,long size)
{
	unsigned long * from_page_table;
	unsigned long * to_page_table;
	unsigned long this_page;
	unsigned long * from_dir, * to_dir;
	unsigned long nr;
	// 4MB对齐
	if ((from&0x3fffff) || (to&0x3fffff))
		panic("copy_page_tables called with wrong alignment");
	// 源页目录项物理地址
	from_dir = (unsigned long *) ((from>>20) & 0xffc); /* _pg_dir = 0 */
	// 目的目录项物理地址
	to_dir = (unsigned long *) ((to>>20) & 0xffc);
	// 多少个MB
	size = ((unsigned) (size+0x3fffff)) >> 22;
	for( ; size-->0 ; from_dir++,to_dir++) {
		// 目的页目录项已经指向了一个有效的页表
		if (1 & *to_dir)
			panic("copy_page_tables: already exist");
		// 源目录项没有指向有效的页表
		if (!(1 & *from_dir))
			continue;
		// 获取页表地址
		from_page_table = (unsigned long *) (0xfffff000 & *from_dir);
		// 分配新的一页物理内存
		if (!(to_page_table = (unsigned long *) get_free_page()))
			return -1;	/* Out of memory, see freeing */
		// 把新分配的物理地址记录在页表中
		*to_dir = ((unsigned long) to_page_table) | 7;
		// 复制的页数，即页表项数
		nr = (from==0)?0xA0:1024;
		for ( ; nr-- > 0 ; from_page_table++,to_page_table++) {
			// *from_page_table是页表项内容
			this_page = *from_page_table;
			// 该页表项没有指向有效的物理地址，则不需要复制
			if (!(1 & this_page))
				continue;
			/*
				置低位的第二位为0，即置该页表项对应的物理内存为不可写，
				可读、可执行，因为有多个进程共享该物理页面，即copy_on_write
			*/
			this_page &= ~2;
			// 复制源页表项内容到目的页表项 
			*to_page_table = this_page;
			// 高于低端地址，即用户进程
			if (this_page > LOW_MEM) {
				// 保存当前的页表项内容
				*from_page_table = this_page;
				/*
					this_page应该只取高20位，因为高20位才是有效地址（再加低位12个0即物理地址）
					但是，LOW_MEN的低12位都是0，所以不影响计算。
				*/
				this_page -= LOW_MEM;
				this_page >>= 12;
				// 算出物理地址对应的页偏移后，把mem_map对应的位加1，代表有多个进程在使用该物理地址
				mem_map[this_page]++;
			}
		}
	}
	// 刷新tlb
	invalidate();
	return 0;
}

/*
 * This function puts a page in memory at the wanted address.
 * It returns the physical address of the page gotten, 0 if
 * out of memory (either when trying to access page-table or
 * page.)
 */
// page是物理地址，address是线性地址。建立物理地址和线性地址的关联，即给页表和页目录项赋值
unsigned long put_page(unsigned long page,unsigned long address)
{
	unsigned long tmp, *page_table;

/* NOTE !!! This uses the fact that _pg_dir=0 */

	if (page < LOW_MEM || page >= HIGH_MEMORY)
		printk("Trying to put page %p at %p\n",page,address);
	// page对应的物理页面没有被分配则说明有问题
	if (mem_map[(page-LOW_MEM)>>12] != 1)
		printk("mem_map disagrees with %p at %p\n",page,address);
	// 计算页目录项的偏移地址，页目录首地址再物理地址0处。这里算出偏移地址后，就是绝对地址，与0xffc即四字节对齐
	page_table = (unsigned long *) ((address>>20) & 0xffc);
	// 页目录项已经指向了一个有效的页表
	if ((*page_table)&1)
		// 算出页表首地址，*page_table的高20位是有效地址
		page_table = (unsigned long *) (0xfffff000 & *page_table);
	else {
		// 页目录项还没有指向有效的页表，分配一个新的物理页
		if (!(tmp=get_free_page()))
			return 0;
		// 把页表地址写到页目录项，tmp为页表的物理地址，或7代表页面是用户级、可读、写、执行、有效
		*page_table = tmp|7;
		// 页目录项指向页表的物理地址
		page_table = (unsigned long *) tmp;
	}
	/* 
		address是32位，右移12为变成20位，再与3ff就是取得低10位，
		即address在页表中的索引,或7代表该页面是用户级、可读、写、执行、有效
	*/
	page_table[(address>>12) & 0x3ff] = page | 7;
/* no need for invalidate */
	// 返回线性地址
	return page;
}
// 共享的页面被写入的时候会执行该函数。该函数申请新的一页物理地址，解除共享状态
void un_wp_page(unsigned long * table_entry)
{
	unsigned long old_page,new_page;
	// table_entry是页表项地址，算出该页的物理首地址
	old_page = 0xfffff000 & *table_entry;
	// LOW_MEM以下是内核使用的内存。old_page对应的物理页引用数为1，可以直接修改内容，置可写标记位（第二位）
	if (old_page >= LOW_MEM && mem_map[MAP_NR(old_page)]==1) {
		*table_entry |= 2;
		invalidate();
		return;
	}
	// 分配一个新的物理页
	if (!(new_page=get_free_page()))
		oom();
	// 页的引用数减一，因为有一个进程不使用这块内存了
	if (old_page >= LOW_MEM)
		mem_map[MAP_NR(old_page)]--;
	// 修改页表项的内容，使其指向新分配的内存页，置用户级、有效、可读写、可执行标记位
	*table_entry = new_page | 7;
	// 刷新tlb
	invalidate();
	// 把数据赋值到新分配的页上
	copy_page(old_page,new_page);
}	

/*
 * This routine handles present pages, when users try to write
 * to a shared page. It is done by copying the page to a new address
 * and decrementing the shared-page counter for the old page.
 *
 * If it's in code space we exit with a segment error.
 */

void do_wp_page(unsigned long error_code,unsigned long address)
{
#if 0
/* we cannot do this yet: the estdio library writes to code space */
/* stupid, stupid. I really want the libc.a from GNU */
	if (CODE_SPACE(address))
		do_exit(SIGSEGV);
#endif
	/*
		address为线性地址，
		address>>10 = address>>12<<2，得到页表项的地址，
		address>>20 = address>>22<<2，得到页目录项地址，
		页目录项里存着页表地址+页表偏移得到页表项地址
	*/
	un_wp_page((unsigned long *)
		(((address>>10) & 0xffc) + (0xfffff000 &
		*((unsigned long *) ((address>>20) &0xffc)))));

}
// address是线性地址,判断页面是否可写，不可写则新申请页面，解除共享状态
void write_verify(unsigned long address)
{
	unsigned long page;
	// address>>20 = address>>22<<2,page指向目录项内容，if判断页目录项是否指向了有效的页表项
	if (!( (page = *((unsigned long *) ((address>>20) & 0xffc)) )&1))
		return;
	page &= 0xfffff000;// 取页目录项内容的高二十位，即页表的物理首地址
	page += ((address>>10) & 0xffc); // 页表首地址+页表项偏移，算出页表项的地址
	// 取出页表项的内容 & 3,即判断标记位是不是01，即不可写，则解除共享
	if ((3 & *(unsigned long *) page) == 1)  /* non-writeable, present */
		un_wp_page((unsigned long *) page);
	return;
}
// 给address分配一个新的页，并且把页对应的物理地址存储在页面项中
void get_empty_page(unsigned long address)
{
	unsigned long tmp;

	if (!(tmp=get_free_page()) || !put_page(tmp,address)) {
		free_page(tmp);		/* 0 is ok - ignored */
		oom();
	}
}

/*
 * try_to_share() checks the page at address "address" in the task "p",
 * to see if it exists, and if it is clean. If so, share it with the current
 * task.
 *
 * NOTE! This assumes we have checked that p != current, and that they
 * share the same executable.
 */
// 使得另一个进程的页目录和页表项指向另一个进程的正在使用的物理地址
static int try_to_share(unsigned long address, struct task_struct * p)
{
	unsigned long from;
	unsigned long to;
	unsigned long from_page;
	unsigned long to_page;
	unsigned long phys_addr;
	/*
		address是距离start_code的偏移。这里算出这个距离跨了多少个页目录项，
		然后加上start_code的页目录偏移就得到address在页目录里的绝对偏移
	*/
	from_page = to_page = ((address>>20) & 0xffc);
	// p进程的代码开始地址（线性地址），取得p进程的页目录项地址，再加上address算出的偏移
	from_page += ((p->start_code>>20) & 0xffc);
	// 取得当前进程的页目录项地址，页目录物理地址是0，所以这里就是该地址对应的页目录项的物理地址
	to_page += ((current->start_code>>20) & 0xffc);
/* is there a page-directory at from? */
	// from是页表的物理地址和标记位
	from = *(unsigned long *) from_page;
	// 没有指向有效的页表则返回
	if (!(from & 1))
		return 0;
	// 取出页表地址
	from &= 0xfffff000;
	// 算出address对应的页表项地址，((address>>10) & 0xffc)算出页表项偏移，0xffc说明是4字节对齐
	from_page = from + ((address>>10) & 0xffc);
	// 页表项的内容，包括物理地址和标记位信息
	phys_addr = *(unsigned long *) from_page;
/* is the page clean and present? */
	// 是否有效和是否是脏的，如果不是有效并且干净的则返回
	if ((phys_addr & 0x41) != 0x01)
		return 0;
	// 取出物理地址的页首地址
	phys_addr &= 0xfffff000;
	if (phys_addr >= HIGH_MEMORY || phys_addr < LOW_MEM)
		return 0;
	// 目的页目录项内容
	to = *(unsigned long *) to_page;
	// 目的页目录项是否指向有效的页表
	if (!(to & 1))
		// 没有则新分配一页，并初始化标记位
		if (to = get_free_page())
			*(unsigned long *) to_page = to | 7;
		else
			oom();
	// 取得页表地址
	to &= 0xfffff000;
	// 取得address对应的页表项地址
	to_page = to + ((address>>10) & 0xffc);
	// 是否指向了有效的物理页，是的话说明不需要再建立线性地址到物理地址的映射了
	if (1 & *(unsigned long *) to_page)
		panic("try_to_share: to_page already exists");
/* share them: write-protect */
	// 标记位不可写
	*(unsigned long *) from_page &= ~2;
	// 把address对应的源页表项内容复制到目的页表项中
	*(unsigned long *) to_page = *(unsigned long *) from_page;
	// 使tlb失效
	invalidate();
	// 算出页数，物理页引用数加一
	phys_addr -= LOW_MEM;
	phys_addr >>= 12;
	mem_map[phys_addr]++;
	return 1;
}

/*
 * share_page() tries to find a process that could share a page with
 * the current one. Address is the address of the wanted page relative
 * to the current data space.
 *
 * We first check if it is at all feasible by checking executable->i_count.
 * It should be >1 if there are other tasks sharing this inode.
 */
// 判断有没有多个进程执行了同一个可执行文件
static int share_page(unsigned long address)
{
	struct task_struct ** p;

	if (!current->executable)
		return 0;
	// 只有当前进程使用这个可执行文件则返回
	if (current->executable->i_count < 2)
		return 0;
	for (p = &LAST_TASK ; p > &FIRST_TASK ; --p) {
		if (!*p)
			continue;
		if (current == *p)
			continue;
		if ((*p)->executable != current->executable)
			continue;
		// 找到一个不是当前进程，但都执行了同一个可执行文件的进程
		if (try_to_share(address,*p))
			return 1;
	}
	return 0;
}
// 缺页处理，进程的内容还没有加载到内存，访问的时候导致缺页异常
void do_no_page(unsigned long error_code,unsigned long address)
{
	int nr[4];
	unsigned long tmp;
	unsigned long page;
	int block,i;
	// 取得线性地址对应页的页首地址,与0xfffff000即减去页偏移 
	address &= 0xfffff000;
	// 算出离代码段首地址的偏移
	tmp = address - current->start_code;
	// tmp大于等于end_data说明是访问堆或者栈的空间时发生的缺页,直接申请一页
	if (!current->executable || tmp >= current->end_data) {
		get_empty_page(address);
		return;
	}
	// 是否有进程已经使用了
	if (share_page(tmp))
		return;
	// 获取一页，4kb
	if (!(page = get_free_page()))
		oom();
/* remember that 1 block is used for header */
	/*
	 算出要读的硬盘块号，但是最多读四块。
	 tmp/BLOCK_SIZE算出线性地址对应页的
	 页首地址离代码块距离了多少块，然后读取页首
	 地址对应的块号，因为逻辑块号从0开始算，tmp/BLOCK_SIZE则是需要读取的逻辑块号，
	 因为执行文件头还有一页，所以加上1
	*/
	block = 1 + tmp/BLOCK_SIZE;
	// 查找文件前4块对应的硬盘号
	for (i=0 ; i<4 ; block++,i++)
		// bmap算出逻辑块号对应的物理块号
		nr[i] = bmap(current->executable,block);
	// 从硬盘读四块数据进来，并且复制到物理页中
	bread_page(page,current->executable->i_dev,nr);
	/*
	 tmp是小于end_data的，因为从tmp开始加载了4kb的数据，
     所以tmp+4kb（4096）后大于end_data，所以大于的部分需要清0，
	 i即超出的字节数
	*/
	i = tmp + 4096 - current->end_data;
	// page是物理页首地址，加上4kb，从后往前清0
	tmp = page + 4096;
	while (i-- > 0) {
		tmp--;
		*(char *)tmp = 0;
	}
	// 建立线性地址和物理地址的映射
	if (put_page(page,address))
		return;
	// 失败则是否刚才申请的物理页
	free_page(page);
	oom();
}
// 系统初始化的时候初始化管理内存的数据结构
void mem_init(long start_mem, long end_mem)
{
	int i;
	// 高端内存末地址
	HIGH_MEMORY = end_mem;
	// 置全部页面为已使用
	for (i=0 ; i<PAGING_PAGES ; i++)
		mem_map[i] = USED;
	// 主存首地址对应的第几页（绝对页数）
	i = MAP_NR(start_mem);
	// 主存的大小
	end_mem -= start_mem;
	// 主存页数
	end_mem >>= 12;
	// 把主存的页置为未使用，end_men是页数，i是主存第一页的索引
	while (end_mem-->0)
		mem_map[i++]=0;
}

void calc_mem(void)
{
	int i,j,k,free=0;
	long * pg_tbl;
	for(i=0 ; i<PAGING_PAGES ; i++)
		if (!mem_map[i]) free++;
	printk("%d pages free (of %d)\n\r",free,PAGING_PAGES);
	for(i=2 ; i<1024 ; i++) {
		if (1&pg_dir[i]) {
			pg_tbl=(long *) (0xfffff000 & pg_dir[i]);
			for(j=k=0 ; j<1024 ; j++)
				if (pg_tbl[j]&1)
					k++;
			printk("Pg-dir[%d] uses %d pages\n",i,k);
		}
	}
}
