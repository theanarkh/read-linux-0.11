/*
 *  linux/fs/buffer.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 *  'buffer.c' implements the buffer-cache functions. Race-conditions have
 * been avoided by NEVER letting a interrupt change a buffer (except for the
 * data, of course), but instead letting the caller do it. NOTE! As interrupts
 * can wake up a caller, some cli-sti sequences are needed to check for
 * sleep-on-calls. These should be extremely quick, though (I hope).
 */

/*
 * NOTE! There is one discordant note here: checking floppies for
 * disk change. This is where it fits best, I think, as it should
 * invalidate changed floppy-disk-caches.
 */

#include <stdarg.h>
 
#include <linux/config.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/system.h>
#include <asm/io.h>

extern int end;
// 内存中开辟的一块内存，end是内核代码的结束地址
struct buffer_head * start_buffer = (struct buffer_head *) &end;
// 哈希链表，主要是为了快速找到数据
struct buffer_head * hash_table[NR_HASH];
// 缓存区的结构是双向循环链表，free_list指向第一个理论上可用的节点，他的最后一个节点是最近被使用的节点
static struct buffer_head * free_list;
// 没有buffer可用而被阻塞的进程挂载这个队列上
static struct task_struct * buffer_wait = NULL;
// 一共有多少个buffer块
int NR_BUFFERS = 0;
// 加锁，互斥访问
static inline void wait_on_buffer(struct buffer_head * bh)
{
	cli();
	while (bh->b_lock)
		sleep_on(&bh->b_wait);
	sti();
}

int sys_sync(void)
{
	int i;
	struct buffer_head * bh;
	// 把所有inode写入buffer，等待回写，见下面代码
	sync_inodes();		/* write out inodes into buffers */
	bh = start_buffer;
	for (i=0 ; i<NR_BUFFERS ; i++,bh++) {
		wait_on_buffer(bh);
		if (bh->b_dirt)
			// 请求底层写硬盘操作，等待底层驱动回写到硬盘，不一定立刻写入
			ll_rw_block(WRITE,bh);
	}
	return 0;
}

// 把buffer中属于dev设备的缓存全部回写到硬盘
int sync_dev(int dev)
{
	int i;
	struct buffer_head * bh;

	bh = start_buffer;
	// 先把属于该dev的缓存回写硬盘
	for (i=0 ; i<NR_BUFFERS ; i++,bh++) {
		if (bh->b_dev != dev)
			continue;
		wait_on_buffer(bh);
		if (bh->b_dev == dev && bh->b_dirt)
			ll_rw_block(WRITE,bh);
	}
	// 同步所有inode到buffer中
	sync_inodes();
	// 把属于该dev的buffer再写一次
	bh = start_buffer;
	for (i=0 ; i<NR_BUFFERS ; i++,bh++) {
		if (bh->b_dev != dev)
			continue;
		wait_on_buffer(bh);
		if (bh->b_dev == dev && bh->b_dirt)
			ll_rw_block(WRITE,bh);
	}
	return 0;
}
// 使属于dev的buffer全部失效
void inline invalidate_buffers(int dev)
{
	int i;
	struct buffer_head * bh;

	bh = start_buffer;
	for (i=0 ; i<NR_BUFFERS ; i++,bh++) {
		if (bh->b_dev != dev)
			continue;
		wait_on_buffer(bh);
		if (bh->b_dev == dev)
			bh->b_uptodate = bh->b_dirt = 0;
	}
}

/*
 * This routine checks whether a floppy has been changed, and
 * invalidates all buffer-cache-entries in that case. This
 * is a relatively slow routine, so we have to try to minimize using
 * it. Thus it is called only upon a 'mount' or 'open'. This
 * is the best way of combining speed and utility, I think.
 * People changing diskettes in the middle of an operation deserve
 * to loose :-)
 *
 * NOTE! Although currently this is only for floppies, the idea is
 * that any additional removable block-device will use this routine,
 * and that mount/open needn't know that floppies/whatever are
 * special.
 */
void check_disk_change(int dev)
{
	int i;

	if (MAJOR(dev) != 2)
		return;
	if (!floppy_change(dev & 0x03))
		return;
	for (i=0 ; i<NR_SUPER ; i++)
		if (super_block[i].s_dev == dev)
			put_super(super_block[i].s_dev);
	invalidate_inodes(dev);
	invalidate_buffers(dev);
}
// 通过dev和block算出在哈希表的位置
#define _hashfn(dev,block) (((unsigned)(dev^block))%NR_HASH)
// 取得哈希链表中某条链表
#define hash(dev,block) hash_table[_hashfn(dev,block)]
// 把节点移出哈希链表和空闲链表
static inline void remove_from_queues(struct buffer_head * bh)
{
/* remove from hash-queue */
	if (bh->b_next)
		bh->b_next->b_prev = bh->b_prev;
	if (bh->b_prev)
		bh->b_prev->b_next = bh->b_next;
	// bh是哈希链表的第一个节点的话则更新哈希链表的头指针
	if (hash(bh->b_dev,bh->b_blocknr) == bh)
		hash(bh->b_dev,bh->b_blocknr) = bh->b_next;
/* remove from free list */
	// 双向循环链表不能存在这种情况
	if (!(bh->b_prev_free) || !(bh->b_next_free))
		panic("Free block list corrupted");
	bh->b_prev_free->b_next_free = bh->b_next_free;
	bh->b_next_free->b_prev_free = bh->b_prev_free;
	// bh是当前空闲链表的第一个节点则更新空闲链表的头指针
	if (free_list == bh)
		free_list = bh->b_next_free;
}


static inline void insert_into_queues(struct buffer_head * bh)
{
/* put at end of free list */
	/* 
		free_list指向第一个空闲的buffer，
		free_list->b_prev_free指向最近刚使用的buffer，即每次找到一个可用buffer的时候
		都成为free_list的尾节点
	*/
	bh->b_next_free = free_list;
	bh->b_prev_free = free_list->b_prev_free;
	free_list->b_prev_free->b_next_free = bh;
	free_list->b_prev_free = bh;
/* put the buffer in new hash-queue if it has a device */
	bh->b_prev = NULL;
	bh->b_next = NULL;
 	if (!bh->b_dev)
		return;
	// 头插法插到哈希链表
	// 指向哈希链表当前的第一个节点
	bh->b_next = hash(bh->b_dev,bh->b_blocknr);
	// 哈希链表头指针指向bh
	hash(bh->b_dev,bh->b_blocknr) = bh;
	// 旧的头指针的prev指针指向bh
	bh->b_next->b_prev = bh;
}
// 从哈希链表中找到某个节点
static struct buffer_head * find_buffer(int dev, int block)
{		
	struct buffer_head * tmp;
	// 先找到哈希链表中的某条链表的头指针
	for (tmp = hash(dev,block) ; tmp != NULL ; tmp = tmp->b_next)
		if (tmp->b_dev==dev && tmp->b_blocknr==block)
			return tmp;
	return NULL;
}

/*
 * Why like this, I hear you say... The reason is race-conditions.
 * As we don't lock buffers (unless we are readint them, that is),
 * something might happen to it while we sleep (ie a read-error
 * will force it bad). This shouldn't really happen currently, but
 * the code is ready.
 */
// 找dev+block对应的buffer
struct buffer_head * get_hash_table(int dev, int block)
{
	struct buffer_head * bh;

	for (;;) {
		// 找不到直接返回NULL
		if (!(bh=find_buffer(dev,block)))
			return NULL;
		// 存在则先把引用数加1，防止别人释放
		bh->b_count++;
		// 看该buffer是不是正在被使用
		wait_on_buffer(bh);
		// 可能在阻塞的时候buffer已经被修改过
		if (bh->b_dev == dev && bh->b_blocknr == block)
			return bh;
		// 引用数减一，重新再找
		bh->b_count--;
	}
}

/*
 * Ok, this is getblk, and it isn't very clear, again to hinder
 * race-conditions. Most of the code is seldom used, (ie repeating),
 * so it should be much more efficient than it looks.
 *
 * The algoritm is changed: hopefully better, and an elusive bug removed.
 */
// 数据脏或者被锁，脏的位置比锁更高，即被锁了可以接收，脏数据的buffer尽量不用，除非找不到buffer
#define BADNESS(bh) (((bh)->b_dirt<<1)+(bh)->b_lock)
struct buffer_head * getblk(int dev,int block)
{
	struct buffer_head * tmp, * bh;

repeat:
	// 找到直接返回
	if (bh = get_hash_table(dev,block))
		return bh
	
	tmp = free_list;
	do {
		// 已经被使用则找下一个节点
		if (tmp->b_count)
			continue;
		/*
			尽量找到一个数据是干净并且没有被锁的buffer，如果没有，则找干净但被锁的，
			还没有就找不干净又被锁的
			1 找到第一个buffer的时候，!bh成立，bh等于第一个可用的buffer，如果干净又没有被锁直接返回，
			  继续尝试查找更好的buffer
			2 后面再找到buffer的时候，!bh就不会成立了，从而执行BADNESS(tmp)<BADNESS(bh)，根据定义，
			  我们知道BADNESS的结果和数据是否干净、被锁相关，其中干净的权重更大，即如果两个buffer，一个脏，一个被锁，
			  则被锁是更好的选择，所以BADNESS(tmp)<BADNESS(bh)的意思是，找到一个比当前节点更好的，如果没有，则继续找
			  如果有则执行bh=tmp，即记录当前最好的节点，然后再判断该节点是不是干净又没有被锁的，是则返回，否则继续找
			  更好的节点。
		*/
		if (!bh || BADNESS(tmp)<BADNESS(bh)) {
			bh = tmp; // 记录当前最好的节点
			// 当前最好的节点是否满足要求，是则返回，否则继续找更好的
			if (!BADNESS(tmp))
				break;
		}
/* and repeat until we find something good */
	} while ((tmp = tmp->b_next_free) != free_list);
	// 没有buffer可用，则阻塞等待
	if (!bh) {
		sleep_on(&buffer_wait);
		goto repeat;
	}
	// 到这里说明有buffer可用，但是情况有，1被锁的 2 数据不干净的 3 不干净且被锁 4 干净又没有被锁
	// 处理lock的情况
	wait_on_buffer(bh);
	// 阻塞的时候被其他进程使用了，则继续找
	if (bh->b_count)
		goto repeat;
	// 处理数据脏的情况
	while (bh->b_dirt) {
		// 回写数据到硬盘
		sync_dev(bh->b_dev);
		wait_on_buffer(bh);
		if (bh->b_count)
			goto repeat;
	}
/* NOTE!! While we slept waiting for this block, somebody else might */
/* already have added "this" block to the cache. check it */
	if (find_buffer(dev,block))
		goto repeat;
/* OK, FINALLY we know that this buffer is the only one of it's kind, */
/* and that it's unused (b_count=0), unlocked (b_lock=0), and clean */
	bh->b_count=1;
	bh->b_dirt=0;
	bh->b_uptodate=0;
	// 移除空闲链表
	remove_from_queues(bh);
	bh->b_dev=dev;
	bh->b_blocknr=block;
	// 插入空
	insert_into_queues(bh);
	return bh;
}
// 有buffer可用， 唤醒等待buffer的队列
void brelse(struct buffer_head * buf)
{
	if (!buf)
		return;
	wait_on_buffer(buf);
	if (!(buf->b_count--))
		panic("Trying to free free buffer");
	wake_up(&buffer_wait);
}

/*
 * bread() reads a specified block and returns the buffer that contains
 * it. It returns NULL if the block was unreadable.
 */
struct buffer_head * bread(int dev,int block)
{
	struct buffer_head * bh;
	// 先从buffer链表中获取一个buffer
	if (!(bh=getblk(dev,block)))
		panic("bread: getblk returned NULL\n");
	// 之前已经读取过并且有效，则直接返回
	if (bh->b_uptodate)
		return bh;
	// 返回读取硬盘的数据
	ll_rw_block(READ,bh);
	//ll_rw_block会锁住bh，所以会先阻塞在这然后等待唤醒 
	wait_on_buffer(bh);
	// 底层读取数据成功后会更新该字段为1，否则就是读取出错了
	if (bh->b_uptodate)
		return bh;
	brelse(bh);
	return NULL;
}
// movsl每次传送四个字节，所以cx等于BLOCK_SIZE除以4
#define COPYBLK(from,to) \
__asm__("cld\n\t" \
	"rep\n\t" \
	"movsl\n\t" \
	::"c" (BLOCK_SIZE/4),"S" (from),"D" (to) \
	:"cx","di","si")

/*
 * bread_page reads four buffers into memory at the desired address. It's
 * a function of its own, as there is some speed to be got by reading them
 * all at the same time, not waiting for one to be read, and then another
 * etc.
 */
// 读取四个块到address
void bread_page(unsigned long address,int dev,int b[4])
{
	struct buffer_head * bh[4];
	int i;

	for (i=0 ; i<4 ; i++)
		if (b[i]) {
			if (bh[i] = getblk(dev,b[i]))
				if (!bh[i]->b_uptodate)
					ll_rw_block(READ,bh[i]);
		} else
			bh[i] = NULL;
	for (i=0 ; i<4 ; i++,address += BLOCK_SIZE)
		if (bh[i]) {
			wait_on_buffer(bh[i]);
			if (bh[i]->b_uptodate)
				COPYBLK((unsigned long) bh[i]->b_data,address);
			brelse(bh[i]);
		}
}

/*
 * Ok, breada can be used as bread, but additionally to mark other
 * blocks for reading as well. End the argument list with a negative
 * number.
 */
// 预读多个块，最后一个参数以负数结尾,只返回第一块的buffer指针，预读得存在buffer哈希链表里，等待以后用
struct buffer_head * breada(int dev,int first, ...)
{
	va_list args;
	struct buffer_head * bh, *tmp;

	va_start(args,first);
	if (!(bh=getblk(dev,first)))
		panic("bread: getblk returned NULL\n");
	if (!bh->b_uptodate)
		ll_rw_block(READ,bh);
	while ((first=va_arg(args,int))>=0) {
		tmp=getblk(dev,first);
		if (tmp) {
			if (!tmp->b_uptodate)
				// bh应该是tmp，因为需要每次给底层传一个新的buffer结构，如果一直用bh，预读的数据会覆盖前面的数据
				ll_rw_block(READA,bh);
			tmp->b_count--;
		}
	}
	va_end(args);
	// 等待底层唤醒
	wait_on_buffer(bh);
	if (bh->b_uptodate)
		return bh;
	brelse(bh);
	return (NULL);
}
// 系统初始化的时候执行该函数，主要是建立buffer对应的数据结构，一个双向循环链表
void buffer_init(long buffer_end)
{
	struct buffer_head * h = start_buffer;
	void * b;
	int i;
	// buffer的结束地址
	if (buffer_end == 1<<20)
		b = (void *) (640*1024);
	else
		b = (void *) buffer_end;
	// buffer_head在头部分配，data字段对应的内容在末端分配，data字段的地址和buffer_head结构的地址要相差至少一个struct buffer_head
	while ( (b -= BLOCK_SIZE) >= ((void *) (h+1)) ) {
		h->b_dev = 0;
		h->b_dirt = 0;
		h->b_count = 0;
		h->b_lock = 0;
		h->b_uptodate = 0;
		h->b_wait = NULL;
		h->b_next = NULL;
		h->b_prev = NULL;
		h->b_data = (char *) b;
		// 初始化时每个节点都是空闲的，形成一条freelist
		h->b_prev_free = h-1;
		h->b_next_free = h+1;
		h++;
		// buffer个数
		NR_BUFFERS++;
		if (b == (void *) 0x100000)
			b = (void *) 0xA0000;
	}
	// h--后h为最后一个buffer_head结构的地址
	h--;
	// 整条链都是空闲的
	free_list = start_buffer;
	// 更新第一个节点的prev指针和最后一个节点的next指针，因为在while循环的时候他们指向了无效的地址
	free_list->b_prev_free = h;
	h->b_next_free = free_list;
	for (i=0;i<NR_HASH;i++)
		hash_table[i]=NULL;
}	
