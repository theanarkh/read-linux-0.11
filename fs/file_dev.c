/*
 *  linux/fs/file_dev.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <errno.h>
#include <fcntl.h>

#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/segment.h>

#define MIN(a,b) (((a)<(b))?(a):(b))
#define MAX(a,b) (((a)>(b))?(a):(b))

int file_read(struct m_inode * inode, struct file * filp, char * buf, int count)
{
	int left,chars,nr;
	struct buffer_head * bh;

	if ((left=count)<=0)
		return 0;
	while (left) {
		// bmap取得该文件偏移对应的硬盘块号，然后读进来
		if (nr = bmap(inode,(filp->f_pos)/BLOCK_SIZE)) {
			if (!(bh=bread(inode->i_dev,nr)))
				break;
		} else
			bh = NULL;
		// 偏移
		nr = filp->f_pos % BLOCK_SIZE;
		// 读进来的数据中，可读的长度和还需要读的长度，取小的，如果还没读完继续把块从硬盘读进来
		chars = MIN( BLOCK_SIZE-nr , left );
		filp->f_pos += chars; // 更新偏移指针
		left -= chars; // 更新还需药读取的长度
		if (bh) {
			char * p = nr + bh->b_data;
			while (chars-->0)
				put_fs_byte(*(p++),buf++); //复制到buf里 
			brelse(bh);
		} else {
			while (chars-->0)
				put_fs_byte(0,buf++);
		}
	}
	// 更新访问时间
	inode->i_atime = CURRENT_TIME;
	// 返回读取的长度，如果一个都没读则返回错误
	return (count-left)?(count-left):-ERROR;
}

int file_write(struct m_inode * inode, struct file * filp, char * buf, int count)
{
	off_t pos;
	int block,c;
	struct buffer_head * bh;
	char * p;
	int i=0;

/*
 * ok, append may not work when many processes are writing at the same time
 * but so what. That way leads to madness anyway.
 */
	// 如果设置了追加标记位，则更新当前位置指针到文件最后一个字节
	if (filp->f_flags & O_APPEND)
		pos = inode->i_size;
	else
		pos = filp->f_pos;
	// i为已经写入的长度，count为需要写入的长度
	while (i<count) {
		// 读取一个硬盘的数据块，如果没有则创建一个块，即标记硬盘中这个块已经被使用
		if (!(block = create_block(inode,pos/BLOCK_SIZE)))
			break;
		// 然后根据返回的块号把这个块内容读进来
		if (!(bh=bread(inode->i_dev,block)))
			break;
		c = pos % BLOCK_SIZE;
		p = c + bh->b_data; // 开始写入数据的位置
		bh->b_dirt = 1; // 标记数据需要回写硬盘
		c = BLOCK_SIZE-c; // 算出能写的长度
		if (c > count-i) c = count-i; // 比较能写的长度和还需要写的长度，取小的
		pos += c; // 更新偏移指针，c为准备写入的长度
		// 如果超过原来长度则需要更新i_size字段，标记inode需要回写
		if (pos > inode->i_size) {
			inode->i_size = pos;
			inode->i_dirt = 1;
		}
		i += c; // 更新已经写入的长度
		while (c-->0)
			*(p++) = get_fs_byte(buf++);
		brelse(bh);
	}
	inode->i_mtime = CURRENT_TIME;
	if (!(filp->f_flags & O_APPEND)) {
		filp->f_pos = pos;
		inode->i_ctime = CURRENT_TIME;
	}
	return (i?i:-1);
}
