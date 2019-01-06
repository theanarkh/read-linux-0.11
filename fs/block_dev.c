/*
 *  linux/fs/block_dev.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <errno.h>

#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/segment.h>
#include <asm/system.h>

int block_write(int dev, long * pos, char * buf, int count)
{	// 算出哪一块的第几个，pos为相对硬盘数据区域的绝对偏移
	int block = *pos >> BLOCK_SIZE_BITS;
	int offset = *pos & (BLOCK_SIZE-1);
	int chars;
	int written = 0;
	struct buffer_head * bh;
	register char * p;
	// 还没写完
	while (count>0) {
		// 当前块还能写多少字节，需要写的比能写的还多则先写完当前块的空间，剩下的写到下一块
		chars = BLOCK_SIZE - offset;
		// 能写的大于需要写的，大小则取需要写的
		if (chars > count)
			chars=count;
		// 需要写一块
		if (chars == BLOCK_SIZE)
			bh = getblk(dev,block);
		else
			bh = breada(dev,block,block+1,block+2,-1);
		// 因为本次循环写不满，则不会再进入循环，如果写完说明当前块已经写满，下一次循环需要写到下一块
		block++;
		if (!bh)
			return written?written:-EIO;
		// 可写空间的起始地址
		p = offset + bh->b_data;
		// 下一块从0开始写
		offset = 0;
		// 更新位置
		*pos += chars;
		// 更新已写的大小
		written += chars;
		// 更新还需要写多少字节
		count -= chars;
		// 写入
		while (chars-->0)
			*(p++) = get_fs_byte(buf++);
		bh->b_dirt = 1;
		brelse(bh);
	}
	return written;
}

int block_read(int dev, unsigned long * pos, char * buf, int count)
{
	int block = *pos >> BLOCK_SIZE_BITS;
	int offset = *pos & (BLOCK_SIZE-1);
	int chars;
	int read = 0;
	struct buffer_head * bh;
	register char * p;

	while (count>0) {
		chars = BLOCK_SIZE-offset;
		if (chars > count)
			chars = count;
		if (!(bh = breada(dev,block,block+1,block+2,-1)))
			return read?read:-EIO;
		block++;
		// 读的起始地址
		p = offset + bh->b_data;
		// 下一块从0开始读
		offset = 0;
		// 更新位置，已读，还需要读的数量
		*pos += chars;
		read += chars;
		count -= chars;
		// 读入
		while (chars-->0)
			put_fs_byte(*(p++),buf++);
		brelse(bh);
	}
	return read;
}
