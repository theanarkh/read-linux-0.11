/*
 *  linux/fs/super.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * super.c contains code to handle the super-block tables.
 */
#include <linux/config.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/system.h>

#include <errno.h>
#include <sys/stat.h>

int sync_dev(int dev);
void wait_for_keypress(void);

/* set_bit uses setb, as gas doesn't recognize setc */
#define set_bit(bitnr,addr) ({ \
register int __res __asm__("ax"); \
__asm__("bt %2,%3;setb %%al":"=a" (__res):"a" (0),"r" (bitnr),"m" (*(addr))); \
__res; })

// 保存所有的文件系统超级块
struct super_block super_block[NR_SUPER];
/* this is initialized in init/main.c */
int ROOT_DEV = 0;

static void lock_super(struct super_block * sb)
{
	cli();
	while (sb->s_lock)
		sleep_on(&(sb->s_wait));
	sb->s_lock = 1;
	sti();
}

static void free_super(struct super_block * sb)
{
	cli();
	sb->s_lock = 0;
	wake_up(&(sb->s_wait));
	sti();
}

static void wait_on_super(struct super_block * sb)
{
	cli();
	while (sb->s_lock)
		sleep_on(&(sb->s_wait));
	sti();
}
// 获取某个设备对应的超级块
struct super_block * get_super(int dev)
{
	struct super_block * s;

	if (!dev)
		return NULL;
	s = 0+super_block;
	while (s < NR_SUPER+super_block)
		if (s->s_dev == dev) {
			wait_on_super(s);
			// 在等待的过程中该超级块内容可能被修改
			if (s->s_dev == dev)
				return s;
			// 重头开始
			s = 0+super_block;
		} else
			s++;
	return NULL;
}

// 释放超级块
void put_super(int dev)
{
	struct super_block * sb;
	struct m_inode * inode;
	int i;

	if (dev == ROOT_DEV) {
		printk("root diskette changed: prepare for armageddon\n\r");
		return;
	}
	if (!(sb = get_super(dev)))
		return;
	if (sb->s_imount) {
		printk("Mounted disk changed - tssk, tssk\n\r");
		return;
	}
	// 加锁
	lock_super(sb);
	sb->s_dev = 0; // 置为未使用状态
	// 释放inode和block数据
	for(i=0;i<I_MAP_SLOTS;i++)
		brelse(sb->s_imap[i]);
	for(i=0;i<Z_MAP_SLOTS;i++)
		brelse(sb->s_zmap[i]);
	// 解锁
	free_super(sb);
	return;
}
// 读取dev对应的超级块
static struct super_block * read_super(int dev)
{
	struct super_block * s;
	struct buffer_head * bh;
	int i,block;

	if (!dev)
		return NULL;
	check_disk_change(dev);
	// 在超级块表中则直接返回
	if (s = get_super(dev))
		return s;
	// 找一个可用于存储超级块的空项
	for (s = 0+super_block ;; s++) {
		if (s >= NR_SUPER+super_block)
			return NULL;
		if (!s->s_dev)
			break;
	}
	s->s_dev = dev;
	s->s_isup = NULL;
	s->s_imount = NULL;
	s->s_time = 0;
	s->s_rd_only = 0;
	s->s_dirt = 0;
	// 加锁，避免其他进程使用超级块里的数据，这时候还没读进来
	lock_super(s);
	// 把设备的第一块读进来，即超级块的内容
	if (!(bh = bread(dev,1))) {
		// 释放
		s->s_dev=0;
		free_super(s);
		return NULL;
	}
	*((struct d_super_block *) s) =
		*((struct d_super_block *) bh->b_data);
	brelse(bh);
	// 不是超级块则rollback
	if (s->s_magic != SUPER_MAGIC) {
		s->s_dev = 0;
		free_super(s);
		return NULL;
	}
	for (i=0;i<I_MAP_SLOTS;i++)
		s->s_imap[i] = NULL;
	for (i=0;i<Z_MAP_SLOTS;i++)
		s->s_zmap[i] = NULL;
	block=2;
	// 读inode和块位图信息,s_imap_blocks块表示inode位图，读进来
	for (i=0 ; i < s->s_imap_blocks ; i++)
		if (s->s_imap[i]=bread(dev,block)) // s_imap_blocks > 8时会溢出
			block++;
		else
			break;
	for (i=0 ; i < s->s_zmap_blocks ; i++)
		if (s->s_zmap[i]=bread(dev,block))
			block++;
		else
			break;
	// 没全读成功全部释放
	if (block != 2+s->s_imap_blocks+s->s_zmap_blocks) {
		for(i=0;i<I_MAP_SLOTS;i++)
			brelse(s->s_imap[i]);
		for(i=0;i<Z_MAP_SLOTS;i++)
			brelse(s->s_zmap[i]);
		s->s_dev=0;
		free_super(s);
		return NULL;
	}
	// 第一个不能使用,置第一个为已使用,因为找空闲块的时候，返回0表示失败。所以第0块可用的话会有二义性
	s->s_imap[0]->b_data[0] |= 1;
	s->s_zmap[0]->b_data[0] |= 1;
	free_super(s);
	return s;
}

// 卸载文件系统
int sys_umount(char * dev_name)
{
	struct m_inode * inode;
	struct super_block * sb;
	int dev;
	// 找到name对应的inode
	if (!(inode=namei(dev_name)))
		return -ENOENT;
	// 取得设备号
	dev = inode->i_zone[0];
	if (!S_ISBLK(inode->i_mode)) {
		iput(inode);
		return -ENOTBLK;
	}
	iput(inode);
	// 根文件不能被卸载
	if (dev==ROOT_DEV)
		return -EBUSY;
	// 读取超级块内容，判断超级块是否没有被挂载到任何inode
	if (!(sb=get_super(dev)) || !(sb->s_imount))
		return -ENOENT;
	// inode没有挂载文件系统
	if (!sb->s_imount->i_mount)
		printk("Mounted inode has i_mount=0\n");
	/// 判断是否有进程在使用该inode，有的话不能卸载
	for (inode=inode_table+0 ; inode<inode_table+NR_INODE ; inode++)
		if (inode->i_dev==dev && inode->i_count)
				return -EBUSY;
	// 清除inode的挂载标记
	sb->s_imount->i_mount=0;
	iput(sb->s_imount);
	sb->s_imount = NULL;
	iput(sb->s_isup);
	sb->s_isup = NULL;
	put_super(dev);
	sync_dev(dev);
	return 0;
}
// 把某设备挂载到某目录
int sys_mount(char * dev_name, char * dir_name, int rw_flag)
{
	struct m_inode * dev_i, * dir_i;
	struct super_block * sb;
	int dev;
	// 找到该设备对应的inode
	if (!(dev_i=namei(dev_name)))
		return -ENOENT;
	// 取得设备号
	dev = dev_i->i_zone[0];
	if (!S_ISBLK(dev_i->i_mode)) {
		iput(dev_i);
		return -EPERM;
	}
	iput(dev_i);
	// 找到挂载目录的inode
	if (!(dir_i=namei(dir_name)))
		return -ENOENT;
	// inode正在被使用或者是根文件系统
	if (dir_i->i_count != 1 || dir_i->i_num == ROOT_INO) {
		iput(dir_i);
		return -EBUSY;
	}
	// 只能挂载到目录
	if (!S_ISDIR(dir_i->i_mode)) {
		iput(dir_i);
		return -EPERM;
	}
	// 读设备的超级块
	if (!(sb=read_super(dev))) {
		iput(dir_i);
		return -EBUSY;
	}
	// 是否已经挂载在其他地方
	if (sb->s_imount) {
		iput(dir_i);
		return -EBUSY;
	}
	// 挂载目录已经挂载了其他文件系统
	if (dir_i->i_mount) {
		iput(dir_i);
		return -EPERM;
	}
	// 设置该超级块已经挂载到某inode
	sb->s_imount=dir_i;
	// 设置该inode已经挂载了某文件系统
	dir_i->i_mount=1;
	dir_i->i_dirt=1;		/* NOTE! we don't iput(dir_i) */
	return 0;			/* we do that in umount */
}

// 系统初始化时挂载根文件系统
void mount_root(void)
{
	int i,free;
	struct super_block * p;
	struct m_inode * mi;

	if (32 != sizeof (struct d_inode))
		panic("bad i-node size");
	// 初始化file结构体列表，struct file file_table[NR_FILE];
	for(i=0;i<NR_FILE;i++)
		file_table[i].f_count=0;
	// 如果根文件系统是软盘提示插入软盘
	if (MAJOR(ROOT_DEV) == 2) {
		printk("Insert root floppy and press ENTER");
		wait_for_keypress();
	}
	// 初始化超级块列表
	for(p = &super_block[0] ; p < &super_block[NR_SUPER] ; p++) {
		p->s_dev = 0;
		p->s_lock = 0;
		p->s_wait = NULL;
	}
	// 读取某个设备（硬盘分区）中的超级块，即根文件系统的超级块
	if (!(p=read_super(ROOT_DEV)))
		panic("Unable to mount root");
	// 获取根文件系统的第一个inode节点，里面存的是根目录的数据
	if (!(mi=iget(ROOT_DEV,ROOT_INO)))
		panic("Unable to read root i-node");
	// mi在下面四个地方有赋值,iget里面的get_empty_inode函数已经设置i_count=1，所以这里加三就行
	mi->i_count += 3 ;	/* NOTE! it is logically used 4 times, not 1 */
	// 超级块挂载到了mi对应的inode节点，p->s_isup设置根文件系统的根节点
	p->s_isup = p->s_imount = mi;
	// 设置当前进程（进程1）的根文件目录和当前工作目录
	current->pwd = mi;
	current->root = mi;
	free=0;
	// 文件系统的逻辑数据块和inode数量
	i=p->s_nzones;
	while (-- i >= 0)
		if (!set_bit(i&8191,p->s_zmap[i>>13]->b_data))
			free++;
	printk("%d/%d free blocks\n\r",free,p->s_nzones);
	free=0;
	i=p->s_ninodes+1;
	while (-- i >= 0)
		if (!set_bit(i&8191,p->s_imap[i>>13]->b_data))
			free++;
	printk("%d/%d free inodes\n\r",free,p->s_ninodes);
}
