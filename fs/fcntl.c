/*
 *  linux/fs/fcntl.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <string.h>
#include <errno.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/segment.h>

#include <fcntl.h>
#include <sys/stat.h>

extern int sys_close(int fd);

static int dupfd(unsigned int fd, unsigned int arg)
{
	if (fd >= NR_OPEN || !current->filp[fd])
		return -EBADF;
	if (arg >= NR_OPEN)
		return -EINVAL;
	// 返回的文件描述符大于等于arg，即用户传进来的
	while (arg < NR_OPEN)
		if (current->filp[arg])
			arg++;
		else
			break;
	// 没有可用的文件描述符了
	if (arg >= NR_OPEN)
		return -EMFILE;
	// 清除该文件描述符的close_on_exec flag
	current->close_on_exec &= ~(1<<arg);
	// file结构体引用数加一，因为多了一个文件描述符指向他
	(current->filp[arg] = current->filp[fd])->f_count++;
	return arg;
}

int sys_dup2(unsigned int oldfd, unsigned int newfd)
{	
	// 关闭newfd文件描述符，然后返回一个大于等于newfd的文件描述符
	sys_close(newfd);
	return dupfd(oldfd,newfd);
}
// 返回最小的文件描述符，指向的内容和fildes一样
int sys_dup(unsigned int fildes)
{
	return dupfd(fildes,0);
}

int sys_fcntl(unsigned int fd, unsigned int cmd, unsigned long arg)
{	
	struct file * filp;

	if (fd >= NR_OPEN || !(filp = current->filp[fd]))
		return -EBADF;
	switch (cmd) {
		// 复制文件描述符
		case F_DUPFD:
			return dupfd(fd,arg);
		// 读写close_on_exec标记，该标记控制进程执行exec后该文件是不是要关闭
		case F_GETFD:
			return (current->close_on_exec>>fd)&1;
		case F_SETFD:
			if (arg&1)
				current->close_on_exec |= (1<<fd);
			else
				current->close_on_exec &= ~(1<<fd);
			return 0;
		case F_GETFL:
			return filp->f_flags;
		case F_SETFL:
			// 先把这两位清除
			filp->f_flags &= ~(O_APPEND | O_NONBLOCK);
			// 只能设置这两个flag
			filp->f_flags |= arg & (O_APPEND | O_NONBLOCK);
			return 0;
		case F_GETLK:	case F_SETLK:	case F_SETLKW:
			return -1;
		default:
			return -1;
	}
}
