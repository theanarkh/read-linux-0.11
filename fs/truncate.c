/*
 *  linux/fs/truncate.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <linux/sched.h>

#include <sys/stat.h>

// 释放一级数据块
static void free_ind(int dev,int block)
{
	struct buffer_head * bh;
	unsigned short * p;
	int i;

	if (!block)
		return;
	// 读入第block块数据，里面保存了文件内容对应的512个块号
	if (bh=bread(dev,block)) {
		p = (unsigned short *) bh->b_data;
		for (i=0;i<512;i++,p++)
			if (*p)
				free_block(dev,*p);
		brelse(bh);
	}
	// 最后把一级数据块也释放掉
	free_block(dev,block);
}

// 释放二级数据块
static void free_dind(int dev,int block)
{
	struct buffer_head * bh;
	unsigned short * p;
	int i;

	if (!block)
		return;
	if (bh=bread(dev,block)) {
		p = (unsigned short *) bh->b_data;
		for (i=0;i<512;i++,p++)
			if (*p)
				free_ind(dev,*p);
		brelse(bh);
	}
	free_block(dev,block);
}
// 清空文件
void truncate(struct m_inode * inode)
{
	int i;
	// 是目录或一般文件
	if (!(S_ISREG(inode->i_mode) || S_ISDIR(inode->i_mode)))
		return;
	// 释放全部的直接数据块
	for (i=0;i<7;i++)
		if (inode->i_zone[i]) {
			free_block(inode->i_dev,inode->i_zone[i]);
			inode->i_zone[i]=0;
		}
	// 释放一级数据块
	free_ind(inode->i_dev,inode->i_zone[7]);
	// 释放二级数据块
	free_dind(inode->i_dev,inode->i_zone[8]);
	inode->i_zone[7] = inode->i_zone[8] = 0;
	// 文件大小为0
	inode->i_size = 0;
	inode->i_dirt = 1;
	inode->i_mtime = inode->i_ctime = CURRENT_TIME;
}

