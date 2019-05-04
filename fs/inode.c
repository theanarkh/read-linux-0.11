/*
 *  linux/fs/inode.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <string.h>
#include <sys/stat.h>

#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <asm/system.h>
// 系统的inode表，整个系统的所有进程共享
struct m_inode inode_table[NR_INODE]={{0,},};

static void read_inode(struct m_inode * inode);
static void write_inode(struct m_inode * inode);
// 互斥访问
static inline void wait_on_inode(struct m_inode * inode)
{
	cli();
	while (inode->i_lock)
		sleep_on(&inode->i_wait);
	sti();
}

static inline void lock_inode(struct m_inode * inode)
{
	cli();
	while (inode->i_lock)
		sleep_on(&inode->i_wait);
	inode->i_lock=1;
	sti();
}

static inline void unlock_inode(struct m_inode * inode)
{
	inode->i_lock=0;
	wake_up(&inode->i_wait);
}
// 置属于dev的inode无效
void invalidate_inodes(int dev)
{
	int i;
	struct m_inode * inode;

	inode = 0+inode_table;
	for(i=0 ; i<NR_INODE ; i++,inode++) {
		wait_on_inode(inode);
		if (inode->i_dev == dev) {
			if (inode->i_count)
				printk("inode in use on removed disk\n\r");
			inode->i_dev = inode->i_dirt = 0;
		}
	}
}
// 遍历所有inode，从硬盘读包括该inode的数据块，然后用内存的inode覆盖硬盘读进来的，存在buffer里，等待回写	
void sync_inodes(void)
{
	int i;
	struct m_inode * inode;
		
	inode = 0+inode_table;
	for(i=0 ; i<NR_INODE ; i++,inode++) {
		wait_on_inode(inode);
		// 管道的内容存放在内存，所以不需要同步
		if (inode->i_dirt && !inode->i_pipe)
			write_inode(inode);
	}
}

// 找到inode中块号为block的块对应哪个硬盘块号或如果没有该块则在硬盘中新建一个块
static int _bmap(struct m_inode * inode,int block,int create)
{
	struct buffer_head * bh;
	int i;

	if (block<0)
		panic("_bmap: block<0");
	// 文件的大小最大值,(7+512+512*512) * 硬盘每块的大小
	if (block >= 7+512+512*512)
		panic("_bmap: block>big");
	// 块号小于7则直接在i_zone数组的前面7个中找就行
	if (block<7) {
		// 如果是创建模式并且该索引为空则创建一个块
		if (create && !inode->i_zone[block])
			// 保存块号
			if (inode->i_zone[block]=new_block(inode->i_dev)) {
				inode->i_ctime=CURRENT_TIME;
				// 该inode需要回写硬盘
				inode->i_dirt=1;
			}
		// 返回硬盘中的块号
		return inode->i_zone[block];
	}
	block -= 7;
	if (block<512) {
		if (create && !inode->i_zone[7])
			if (inode->i_zone[7]=new_block(inode->i_dev)) {
				inode->i_dirt=1;
				inode->i_ctime=CURRENT_TIME;
			}
		if (!inode->i_zone[7])
			return 0;
		// 索引为7的块是间接块，需要把内容读进来才知道具体的硬盘块号
		if (!(bh = bread(inode->i_dev,inode->i_zone[7])))
			return 0;
		// 直接根据block取得对应的硬盘块号
		i = ((unsigned short *) (bh->b_data))[block];
		// 之前没有，新建一个块
		if (create && !i)
			if (i=new_block(inode->i_dev)) {
				((unsigned short *) (bh->b_data))[block]=i;
				bh->b_dirt=1;
			}
		brelse(bh);
		return i;
	}
	block -= 512;
	if (create && !inode->i_zone[8])
		if (inode->i_zone[8]=new_block(inode->i_dev)) {
			inode->i_dirt=1;
			inode->i_ctime=CURRENT_TIME;
		}
	if (!inode->i_zone[8])
		return 0;
	// 先取得一级索引对应的数据，数据中的每一项对应512个项
	if (!(bh=bread(inode->i_dev,inode->i_zone[8])))
		return 0;
	// 每一个索引对应512个项，所以除以512，即右移9位，取得二级索引
	i = ((unsigned short *)bh->b_data)[block>>9];
	if (create && !i)
		if (i=new_block(inode->i_dev)) {
			((unsigned short *) (bh->b_data))[block>>9]=i;
			bh->b_dirt=1;
		}
	brelse(bh);
	if (!i)
		return 0;
	// 取得二级索引对应的数据
	if (!(bh=bread(inode->i_dev,i)))
		return 0;
	// 算出偏移，最大偏移是511，所以&511
	i = ((unsigned short *)bh->b_data)[block&511];
	if (create && !i)
		if (i=new_block(inode->i_dev)) {
			((unsigned short *) (bh->b_data))[block&511]=i;
			bh->b_dirt=1;
		}
	brelse(bh);
	return i;
}
// 查找inode中第block块对应硬盘的块号
int bmap(struct m_inode * inode,int block)
{
	return _bmap(inode,block,0);
}

// 查找inode中的第block块对应的硬盘哪个块，如果有则返回，没有则创建，返回硬盘中的块号
int create_block(struct m_inode * inode, int block)
{
	return _bmap(inode,block,1);
}
// 释放inode，如果没有被引用了，则销毁，否则引用数减一即可
void iput(struct m_inode * inode)
{
	if (!inode)
		return;
	// 有进程在使用该inode则阻塞
	wait_on_inode(inode);
	// 没有进程引用该inode
	if (!inode->i_count)
		panic("iput: trying to free free inode");
	// 管道inode
	if (inode->i_pipe) {
		// 唤醒等待队列，因为该管道可能要被销毁了，不然那会使等待者无限等待，这句是不是可以放到if后
		wake_up(&inode->i_wait);
		// 引用数减一，还有进程在引用则先不销毁
		if (--inode->i_count)
			return;
		// 释放管道对应的一页大小
		free_page(inode->i_size);
		// 该inode可以重用，因为inode指向inode_table的元素
		inode->i_count=0;
		inode->i_dirt=0;
		inode->i_pipe=0;
		return;
	}
	// 没有dev说明不是硬盘文件对应的inode，不需要回写硬盘，引用数减一即可
	if (!inode->i_dev) {
		inode->i_count--;
		return;
	}
	if (S_ISBLK(inode->i_mode)) {
		// 块文件，inode->i_zone[0]保存的是设备号，把buffer中属于该dev设备的回写到硬盘
		sync_dev(inode->i_zone[0]);
		wait_on_inode(inode);
	}
repeat:
	// 还有进程引用该inode节点，引用数减一后返回
	if (inode->i_count>1) {
		inode->i_count--;
		return;
	}
	// 该inode没有进程引用了，inode对应的文件也没有被其他目录项引用了，删除该inode的内容，并释放该inode
	if (!inode->i_nlinks) {
		truncate(inode);
		free_inode(inode);
		return;
	}
	// 需要回写硬盘，则回写
	if (inode->i_dirt) {
		write_inode(inode);	/* we can sleep - so do again */
		wait_on_inode(inode);
		goto repeat;
	}
	inode->i_count--;
	return;
}
// 从inode表（数组）里找到一个未使用的inode结构
struct m_inode * get_empty_inode(void)
{
	struct m_inode * inode;
	static struct m_inode * last_inode = inode_table;
	int i;

	do {
		inode = NULL;
		for (i = NR_INODE; i ; i--) {
			// 越界则从头再找，因为for会执行很多次，但last_inode是一直往前走
			if (++last_inode >= inode_table + NR_INODE)
				last_inode = inode_table;
			// 找到一个未被使用的inode
			if (!last_inode->i_count) {
				// 找到未使用的并且没有被锁、数据是有效的才返回，否则先保存一个备选的
				inode = last_inode;
				if (!inode->i_dirt && !inode->i_lock)
					break;
			}
		}
		if (!inode) {
			for (i=0 ; i<NR_INODE ; i++)
				printk("%04x: %6d\t",inode_table[i].i_dev,
					inode_table[i].i_num);
			panic("No free inodes in mem");
		}
		wait_on_inode(inode);
		// 没有被引用还会有需要回写的数据？
		while (inode->i_dirt) {
			write_inode(inode);
			wait_on_inode(inode);
		}
	// 找到后该inode又被引用了，继续找
	} while (inode->i_count);
	memset(inode,0,sizeof(*inode));
	inode->i_count = 1;
	return inode;
}
// 获取一个用于管道的inode节点
struct m_inode * get_pipe_inode(void)
{
	struct m_inode * inode;

	if (!(inode = get_empty_inode()))
		return NULL;
	// 分配一页大小的内存，首地址赋给i_size
	if (!(inode->i_size=get_free_page())) {
		inode->i_count = 0;
		return NULL;
	}
	inode->i_count = 2;	/* sum of readers/writers */
	// 初始化读写指针
	PIPE_HEAD(*inode) = PIPE_TAIL(*inode) = 0;
	// 标记该inode是管道类型
	inode->i_pipe = 1;
	return inode;
}

// 在inode表中找到对应的inode节点，如果找到的是挂载的文件系统，则要查找的等于挂载点的设备和，nr为文件系统的根目录
struct m_inode * iget(int dev,int nr)
{
	struct m_inode * inode, * empty;

	if (!dev)
		panic("iget with dev==0");
	empty = get_empty_inode();
	inode = inode_table;
	while (inode < NR_INODE+inode_table) {
		// 不相等则比较下一个节点
		if (inode->i_dev != dev || inode->i_num != nr) {
			inode++;
			continue;
		}
		wait_on_inode(inode);
		// 阻塞的时候数据可能发生了变化，继续比较，不一样了则从头开始再找
		if (inode->i_dev != dev || inode->i_num != nr) {
			inode = inode_table;
			continue;
		}
		inode->i_count++;
		// 另一个文件系统挂载在该inode下
		if (inode->i_mount) {
			int i;

			for (i = 0 ; i<NR_SUPER ; i++)
				// 找到挂载在该inode节点的超级块结构
				if (super_block[i].s_imount==inode)
					break;
			// 没找到对应的超级块，直接返回找到的inode
			if (i >= NR_SUPER) {
				printk("Mounted inode hasn't got sb\n");
				if (empty)
					iput(empty);
				return inode;
			}
			iput(inode);
			// 找到了该超级块，更新dev为该超级块的的设备号，块号为第一块，从新的起点开始找
			dev = super_block[i].s_dev;
			nr = ROOT_INO;
			inode = inode_table;
			continue;
		}
		if (empty)
			iput(empty);
		return inode;
	}
	if (!empty)
		return (NULL);
	// 找不到则返回一个新的inode
	inode=empty;
	inode->i_dev = dev;
	inode->i_num = nr;
	read_inode(inode);
	return inode;
}
// 把inode的数据从硬盘中读进来，通过超级块的信息和inode中的编号算出inode在硬盘的块号，读进来
static void read_inode(struct m_inode * inode)
{
	struct super_block * sb;
	struct buffer_head * bh;
	int block;

	lock_inode(inode);
	if (!(sb=get_super(inode->i_dev)))
		panic("trying to read inode without dev");
	// 文件系统第一块是超级块，然后inode位图块，数据块块位图块，inode节点块，数据块,i_num为inode节点在硬盘inode表中编号
	block = 2 + sb->s_imap_blocks + sb->s_zmap_blocks +
		(inode->i_num-1)/INODES_PER_BLOCK;
	// 从硬盘中把inode节点的内容读进来
	if (!(bh=bread(inode->i_dev,block)))
		panic("unable to read i-node block");
	// 读进来整个数据块，包含了要找的inode，算出inode的索引然后取值，d_inode为硬盘中的结构，m_inode为内存的结构
	*(struct d_inode *)inode =
		((struct d_inode *)bh->b_data)
			[(inode->i_num-1)%INODES_PER_BLOCK]; // 取得偏移
	brelse(bh);
	unlock_inode(inode);
}

// 先把inode从硬盘中读进来，然后覆盖，等待回写
static void write_inode(struct m_inode * inode)
{
	struct super_block * sb;
	struct buffer_head * bh;
	int block;

	lock_inode(inode);
	if (!inode->i_dirt || !inode->i_dev) {
		unlock_inode(inode);
		return;
	}
	if (!(sb=get_super(inode->i_dev)))
		panic("trying to write inode without device");
	// 算出inode的块号，2 + inode位图块数 + 块位图块数 + inode的相对偏移
	block = 2 + sb->s_imap_blocks + sb->s_zmap_blocks +
		(inode->i_num-1)/INODES_PER_BLOCK;
	// 读入包含该inode的整个数据块
	if (!(bh=bread(inode->i_dev,block)))
		panic("unable to read i-node block");
	// 找到数据块中inode所属的位置，写到高速缓存等待回写到硬盘
	((struct d_inode *)bh->b_data)
		[(inode->i_num-1)%INODES_PER_BLOCK] =
			*(struct d_inode *)inode;
	bh->b_dirt=1;
	inode->i_dirt=0;
	brelse(bh);
	unlock_inode(inode);
}
