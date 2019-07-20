/*
 *  linux/fs/open.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <utime.h>
#include <sys/stat.h>

#include <linux/sched.h>
#include <linux/tty.h>
#include <linux/kernel.h>
#include <asm/segment.h>

int sys_ustat(int dev, struct ustat * ubuf)
{
	return -ENOSYS;
}

int sys_utime(char * filename, struct utimbuf * times)
{
	struct m_inode * inode;
	long actime,modtime;
	// 更新文件的访问和修改时间，没传times则使用当前时间
	if (!(inode=namei(filename)))
		return -ENOENT;
	if (times) {
		actime = get_fs_long((unsigned long *) &times->actime);
		modtime = get_fs_long((unsigned long *) &times->modtime);
	} else
		actime = modtime = CURRENT_TIME;
	inode->i_atime = actime;
	inode->i_mtime = modtime;
	inode->i_dirt = 1;
	iput(inode);
	return 0;
}

/*
 * XXX should we use the real or effective uid?  BSD uses the real uid,
 * so as to make this call useful to setuid programs.
 */
int sys_access(const char * filename,int mode)
{
	struct m_inode * inode;
	int res, i_mode;

	mode &= 0007;
	// 根据路径找到文件的inode节点
	if (!(inode=namei(filename)))
		return -EACCES;
	i_mode = res = inode->i_mode & 0777;
	iput(inode);
	// 根据身份判断对应的权限
	if (current->uid == inode->i_uid)
		res >>= 6;
	else if (current->gid == inode->i_gid)
		res >>= 6;
	// 判断是否有mode的权限
	if ((res & 0007 & mode) == mode)
		return 0;
	/*
	 * XXX we are doing this test last because we really should be
	 * swapping the effective with the real user id (temporarily),
	 * and then calling suser() routine.  If we do call the
	 * suser() routine, it needs to be called last. 
	 */
	if ((!current->uid) &&
	    (!(mode & 1) || (i_mode & 0111)))
		return 0;
	return -EACCES;
}
// 改变当前进程的工作目录
int sys_chdir(const char * filename)
{
	struct m_inode * inode;
	// 找到该文件对应的inode节点
	if (!(inode = namei(filename)))
		return -ENOENT;
	// 参数不是目录
	if (!S_ISDIR(inode->i_mode)) {
		iput(inode);
		return -ENOTDIR;
	}
	// 之前的已经不用
	iput(current->pwd);
	// 设置新的工作目录
	current->pwd = inode;
	return (0);
}
// 改变进程的根目录
int sys_chroot(const char * filename)
{
	struct m_inode * inode;

	if (!(inode=namei(filename)))
		return -ENOENT;
	if (!S_ISDIR(inode->i_mode)) {
		iput(inode);
		return -ENOTDIR;
	}
	iput(current->root);
	current->root = inode;
	return (0);
}
// 改变文件目录或文件的权限
int sys_chmod(const char * filename,int mode)
{
	struct m_inode * inode;

	if (!(inode=namei(filename)))
		return -ENOENT;
	// 判断权限，只能修改自己创建的文件的权限，除非超级管理员
	if ((current->euid != inode->i_uid) && !suser()) {
		iput(inode);
		return -EACCES;
	}
	inode->i_mode = (mode & 07777) | (inode->i_mode & ~07777);
	inode->i_dirt = 1;
	iput(inode);
	return 0;
}
// 改变文件属主
int sys_chown(const char * filename,int uid,int gid)
{
	struct m_inode * inode;

	if (!(inode=namei(filename)))
		return -ENOENT;
	// 不是超级管理员则返回
	if (!suser()) {
		iput(inode);
		return -EACCES;
	}
	// 修改字段
	inode->i_uid=uid;
	inode->i_gid=gid;
	inode->i_dirt=1;
	iput(inode);
	return 0;
}
// 打开一个文件即首先找到文件对应的inode，然后建立起文件描述符->file结构体->inode的关联
int sys_open(const char * filename,int flag,int mode)
{
	struct m_inode * inode;
	struct file * f;
	int i,fd;
	// umask代表只允许有umask中置位的位的权限
	mode &= 0777 & ~current->umask;
	// 先在当前进程的文件描述符表中找个可用的文件描述符
	for(fd=0 ; fd<NR_OPEN ; fd++)
		if (!current->filp[fd])
			break;
	if (fd>=NR_OPEN)
		return -EINVAL;
	// 清除close_on_exec位，代表fork后要关闭fd对应的文件
	current->close_on_exec &= ~(1<<fd);
	// 从file表中获取一个空项
	f=0+file_table;
	for (i=0 ; i<NR_FILE ; i++,f++)
		if (!f->f_count) break;
	if (i>=NR_FILE)
		return -EINVAL;
	// 引用数加一
	(current->filp[fd]=f)->f_count++;
	// 找到文件对应的inode节点，inode为文件对应的inode节点
	if ((i=open_namei(filename,flag,mode,&inode))<0) {
		current->filp[fd]=NULL;
		f->f_count=0;
		return i;
	}
/* ttys are somewhat special (ttyxx major==4, tty major==5) */
	// 打开的是一个字符设备,终端设备，如键盘，串口等
	if (S_ISCHR(inode->i_mode))
		// i_zone[0]保存的是设备号，如果打开的是tty[1-63]则说明打开的是一个虚拟终端
		if (MAJOR(inode->i_zone[0])==4) {
			// 当前进程是领头进程并且还没有对应的控制终端
			if (current->leader && current->tty<0) {
				// 领头进程打开的第一个终端成为该会话的控制终端
				current->tty = MINOR(inode->i_zone[0]);
				tty_table[current->tty].pgrp = current->pgrp;
			}
		// 打开的是进程的控制终端
		} else if (MAJOR(inode->i_zone[0])==5)
			// 但是进程还没有控制终端，则保存
			if (current->tty<0) {
				iput(inode);
				current->filp[fd]=NULL;
				f->f_count=0;
				return -EPERM;
			}
/* Likewise with block-devices: check for floppy_change */
	if (S_ISBLK(inode->i_mode))
		check_disk_change(inode->i_zone[0]);
	f->f_mode = inode->i_mode;
	f->f_flags = flag;
	f->f_count = 1;
	// 指向代表文件内容的inode
	f->f_inode = inode;
	// 初始化文件读写指针位置是0
	f->f_pos = 0;
	return (fd);
}

int sys_creat(const char * pathname, int mode)
{
	return sys_open(pathname, O_CREAT | O_TRUNC, mode);

// 解除文件描述符->file结构体->inode的关联
int sys_close(unsigned int fd)
{	
	struct file * filp;

	if (fd >= NR_OPEN)
		return -EINVAL;
	// 清除close_on_exec标记，该标记表示fork+exec时关闭该文件
	current->close_on_exec &= ~(1<<fd);
	if (!(filp = current->filp[fd]))
		return -EINVAL;
	// 当前进程的文件描述符指针置空
	current->filp[fd] = NULL;
	if (filp->f_count == 0)
		panic("Close: file count is 0");
	// file结构引用数减一，非0说明还有其他进程或描述符在使用该结构，所以还不能释放file和inode
	if (--filp->f_count)
		return (0);
	// 没有进程使用了则释放该inode或需要回写到硬盘
	iput(filp->f_inode);
	return (0);
}
