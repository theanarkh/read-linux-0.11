/*
 *  linux/fs/bitmap.c
 *
 *  (C) 1991  Linus Torvalds
 */

/* bitmap.c contains the code that handles the inode and block bitmaps */
#include <string.h>

#include <linux/sched.h>
#include <linux/kernel.h>

#define clear_block(addr) \
__asm__("cld\n\t" \
	"rep\n\t" \
	"stosl" \
	::"a" (0),"c" (BLOCK_SIZE/4),"D" ((long) (addr)):"cx","di")
// setb把后面的寄存器置1，btsl把addr的第nr位置1
#define set_bit(nr,addr) ({\
register int res __asm__("ax"); \
__asm__ __volatile__("btsl %2,%3\n\tsetb %%al": \
"=a" (res):"0" (0),"r" (nr),"m" (*(addr))); \
res;})

#define clear_bit(nr,addr) ({\
register int res __asm__("ax"); \
__asm__ __volatile__("btrl %2,%3\n\tsetnb %%al": \
"=a" (res):"0" (0),"r" (nr),"m" (*(addr))); \
res;})

#define find_first_zero(addr) ({ \
int __res; \
__asm__("cld\n" \
	"1:\tlodsl\n\t" \
	"notl %%eax\n\t" \
	"bsfl %%eax,%%edx\n\t" \
	"je 2f\n\t" \
	"addl %%edx,%%ecx\n\t" \
	"jmp 3f\n" \
	"2:\taddl $32,%%ecx\n\t" \
	"cmpl $8192,%%ecx\n\t" \
	"jl 1b\n" \
	"3:" \
	:"=c" (__res):"c" (0),"S" (addr):"ax","dx","si"); \
__res;})
// 释放硬盘的某个块的数据，清除buffer里该数据块对应的数据，数据块位图对应的位置0，等待回写硬盘，硬盘的数据还存在
void free_block(int dev, int block)
{
	struct super_block * sb;
	struct buffer_head * bh;
	// 超级块里存在文件系统的元数据，包括位图缓存，数据块的块数，开始块号等
	if (!(sb = get_super(dev)))
		panic("trying to free block on nonexistent device");
	// 块号的范围，不小于最小，不大于最大
	if (block < sb->s_firstdatazone || block >= sb->s_nzones)
		panic("trying to free block not in datazone");
	// 从缓存中获取dev设备的第block块的数据，可能需要释放
	bh = get_hash_table(dev,block);
	if (bh) {
		// 还在使用
		if (bh->b_count != 1) {
			printk("trying to free block (%04x:%d), count=%d\n",
				dev,block,bh->b_count);
			return;
		}
		bh->b_dirt=0;
		bh->b_uptodate=0;
		// 释放该buffer给其他进程使用
		brelse(bh);
	}
	block -= sb->s_firstdatazone - 1 ;
	/*
		置为未使用状态,s_zmap保存的是硬盘数据块的位图数组，
		每个元素管理8192个数据块的使用情况,block/8192算出该块属于哪个位图元素的管理访问，
		然后把该元素中的地block为清0
	*/
	if (clear_bit(block&8191,sb->s_zmap[block/8192]->b_data)) {
		printk("block (%04x:%d) ",dev,block+sb->s_firstdatazone-1);
		panic("free_block: bit already cleared");
	}
	// 该位图对应的buffer需要回写到硬盘
	sb->s_zmap[block/8192]->b_dirt = 1;
}
/*
	新建一个数据块，首先利用超级块的块位图信息找到一个可用的数据块，
	然后读进来，清0，等待回写，返回块号
*/
int new_block(int dev)
{
	struct buffer_head * bh;
	struct super_block * sb;
	int i,j;
	// 获取文件系统的超级块信息
	if (!(sb = get_super(dev)))
		panic("trying to get new block from nonexistant device");
	j = 8192;
	// 找第一个未使用的块的位置
	for (i=0 ; i<8 ; i++)
		//s_zmap[i]为数据块位图的缓存 
		if (bh=sb->s_zmap[i])
			if ((j=find_first_zero(bh->b_data))<8192)
				break;
	if (i>=8 || !bh || j>=8192)
		return 0;
	// 置第j个数据块已使用标记
	if (set_bit(j,bh->b_data))
		panic("new_block: bit already set");
	// 该位图对应的buffer需要回写
	bh->b_dirt = 1;
	// 位图存在多个块中，i为第i个块，每个块对应的位图管理着8192个数据块
	j += i*8192 + sb->s_firstdatazone-1; // 算出块号
	// 超过了最大块号
	if (j >= sb->s_nzones)
		return 0;
	// 拿到一个buffer，然后清0，等待回写
	if (!(bh=getblk(dev,j)))
		panic("new_block: cannot get block");
	if (bh->b_count != 1)
		panic("new block: count is != 1");
	// 清0防止脏数据
	clear_block(bh->b_data);
	// 内容是最新的
	bh->b_uptodate = 1;
	// 需要回写硬盘，因为新建的内容在硬盘还没有
	bh->b_dirt = 1;
	brelse(bh);
	return j;
}
// 释放一个inode，把超级块中的inode位图清0，等待位图回写到硬盘，但没有清除硬盘里inode的内容
void free_inode(struct m_inode * inode)
{
	struct super_block * sb;
	struct buffer_head * bh;

	if (!inode)
		return;
	if (!inode->i_dev) {
		memset(inode,0,sizeof(*inode));
		return;
	}
	if (inode->i_count>1) {
		printk("trying to free inode with count=%d\n",inode->i_count);
		panic("free_inode");
	}
	if (inode->i_nlinks)
		panic("trying to free inode with links");
	if (!(sb = get_super(inode->i_dev)))
		panic("trying to free inode on nonexistent device");
	if (inode->i_num < 1 || inode->i_num > sb->s_ninodes)
		panic("trying to free inode 0 or nonexistant inode");
	// 有多个块保存着inode位图，每个块对应8192个，i_num保存的是绝对块号，除以8192取得该inode在哪个位图块中
	if (!(bh=sb->s_imap[inode->i_num>>13]))
		panic("nonexistent imap in superblock");
	if (clear_bit(inode->i_num&8191,bh->b_data))
		printk("free_inode: bit already cleared.\n\r");
	bh->b_dirt = 1;
	memset(inode,0,sizeof(*inode));
}

// 新建一个node，首先获取一个inode的结构，然后把超级块的inode位图置1
struct m_inode * new_inode(int dev)
{
	struct m_inode * inode;
	struct super_block * sb;
	struct buffer_head * bh;
	int i,j;

	if (!(inode=get_empty_inode()))
		return NULL;
	if (!(sb = get_super(dev)))
		panic("new_inode with unknown device");
	j = 8192;
	for (i=0 ; i<8 ; i++)
		if (bh=sb->s_imap[i])
			if ((j=find_first_zero(bh->b_data))<8192)
				break;
	if (!bh || j >= 8192 || j+i*8192 > sb->s_ninodes) {
		iput(inode);
		return NULL;
	}
	// 设置位图的地j位为1
	if (set_bit(j,bh->b_data))
		panic("new_inode: bit already set");
	// 位图回写硬盘
	bh->b_dirt = 1;
	inode->i_count=1;
	inode->i_nlinks=1;
	inode->i_dev=dev;
	inode->i_uid=current->euid;
	inode->i_gid=current->egid;
	// inode的内容也需要回写硬盘
	inode->i_dirt=1;
	// 保存的是绝对位置，即位置落在哪个位图块的第几个位置，每个位图块是8192
	inode->i_num = j + i*8192;
	inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME;
	return inode;
}
