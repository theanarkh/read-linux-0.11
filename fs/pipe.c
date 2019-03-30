/*
 *  linux/fs/pipe.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <signal.h>

#include <linux/sched.h>
#include <linux/mm.h>	/* for get_free_page */
#include <asm/segment.h>
// 负数二进制等于正数的二进制除了最高位的其他位取反加1
int read_pipe(struct m_inode * inode, char * buf, int count)
{
	int chars, size, read = 0;

	while (count>0) {
		// 判断能读的字节数
		while (!(size=PIPE_SIZE(*inode))) {
			wake_up(&inode->i_wait);
			if (inode->i_count != 2) /* are there any writers? */
				return read;
			sleep_on(&inode->i_wait);
		}
		// 这一次最多能读的数量
		chars = PAGE_SIZE-PIPE_TAIL(*inode);
		// 比较能读和要读的数量，取小的
		if (chars > count)
			chars = count;
		// 这一轮需要读的和能读的进行比较
		if (chars > size)
			chars = size;
		count -= chars;
		read += chars;
		size = PIPE_TAIL(*inode);
		PIPE_TAIL(*inode) += chars;
		PIPE_TAIL(*inode) &= (PAGE_SIZE-1);
		while (chars-->0)
			put_fs_byte(((char *)inode->i_size)[size++],buf++);
	}
	wake_up(&inode->i_wait);
	return read;
}
	
int write_pipe(struct m_inode * inode, char * buf, int count)
{
	int chars, size, written = 0;
	// 每次循环是以从head写到最后一个节点为单位，如果head后面都可写，则全写，如果后面只有部分可写，则通过size做了限制
	while (count>0) {
		// 还能写多少字节
		while (!(size=(PAGE_SIZE-1)-PIPE_SIZE(*inode))) {
			wake_up(&inode->i_wait);
			if (inode->i_count != 2) { /* no readers */
				current->signal |= (1<<(SIGPIPE-1));
				return written?written:-1;
			}
			sleep_on(&inode->i_wait);
		}
		// 从head开始到最后一个字节，还能写多少字节
		chars = PAGE_SIZE-PIPE_HEAD(*inode);
		// 这一次能写的比需要写的多，则取需要写的数量
		if (chars > count)
			chars = count;
		// 需要写的比剩下的全部字节数还多则取最多能写的数量
		if (chars > size)
			chars = size;
		// 这一次写了chars个字符，剩下的下一次写
		count -= chars;
		written += chars;
		// 指向可写的首地址
		size = PIPE_HEAD(*inode);
		// 更新可写的首地址
		PIPE_HEAD(*inode) += chars;
		// 越界则从头开始
		PIPE_HEAD(*inode) &= (PAGE_SIZE-1);
		// 复制这一次能写的字节
		while (chars-->0)
			((char *)inode->i_size)[size++]=get_fs_byte(buf++);
	}
	wake_up(&inode->i_wait);
	return written;
}

int sys_pipe(unsigned long * fildes)
{
	struct m_inode * inode;
	struct file * f[2];
	int fd[2];
	int i,j;

	j=0;
	// 找两个可用的file结构
	for(i=0;j<2 && i<NR_FILE;i++)
		if (!file_table[i].f_count)
			(f[j++]=i+file_table)->f_count++;
	// 如果只有一个可用，则把把释放
	if (j==1)
		f[0]->f_count=0;
	// 是否找到两个可用的file结构
	if (j<2)
		return -1;
	j=0;
	// 找两个可用的文件描述符
	for(i=0;j<2 && i<NR_OPEN;i++)
		if (!current->filp[i]) {
			current->filp[ fd[j]=i ] = f[j];
			j++;
		}
	// 只有一个则释放
	if (j==1)
		current->filp[fd[0]]=NULL;
	// 释放file结构
	if (j<2) {
		f[0]->f_count=f[1]->f_count=0;
		return -1;
	}
	// 释放文件描述符和file结构
	if (!(inode=get_pipe_inode())) {
		current->filp[fd[0]] =
			current->filp[fd[1]] = NULL;
		f[0]->f_count = f[1]->f_count = 0;
		return -1;
	}
	// 利用这个inode进行通信
	f[0]->f_inode = f[1]->f_inode = inode;
	f[0]->f_pos = f[1]->f_pos = 0;	
	f[0]->f_mode = 1;		/* read */
	f[1]->f_mode = 2;		/* write */
	put_fs_long(fd[0],0+fildes);
	put_fs_long(fd[1],1+fildes);
	return 0;
}
