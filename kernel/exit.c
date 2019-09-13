/*
 *  linux/kernel/exit.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <errno.h>
#include <signal.h>
#include <sys/wait.h>

#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/tty.h>
#include <asm/segment.h>

int sys_pause(void);
int sys_close(int fd);

// 释放pcb的一页内存，重新调度进程
void release(struct task_struct * p)
{
	int i;

	if (!p)
		return;
	for (i=1 ; i<NR_TASKS ; i++)
		if (task[i]==p) {
			task[i]=NULL;
			free_page((long)p);
			schedule();
			return;
		}
	panic("trying to release non-existent task");
}
/*
  发送信号给进程sig是发送的信号，p是接收信号的进程，priv是权限，
  1是代表可以直接设置，比如给自己发信息，priv为0说明需要一定的权限
*/
static inline int send_sig(long sig,struct task_struct * p,int priv)
{
	if (!p || sig<1 || sig>32)
		return -EINVAL;
	// 这里使用euid，即进程设置了suid位的话，可以扩大权限，即拥有文件属主的权限
	if (priv || (current->euid==p->euid) || suser())
		p->signal |= (1<<(sig-1));
	else
		return -EPERM;
	return 0;
}
// 结束会话，给该会话的所有进程发SIGHUP信号,因为子进程会继承父进程的sessionid，所以if可能会多次成立
static void kill_session(void)
{
	struct task_struct **p = NR_TASKS + task;
	
	while (--p > &FIRST_TASK) {
		if (*p && (*p)->session == current->session)
			(*p)->signal |= 1<<(SIGHUP-1);
	}
}

/*
 * XXX need to check permissions needed to send signals to process
 * groups, etc. etc.  kill() permissions semantics are tricky!
 */
int sys_kill(int pid,int sig)
{
	struct task_struct **p = NR_TASKS + task;
	int err, retval = 0;
	// pid等于0则给当前进程的整个组发信号，大于0则给某个进程发信号，-1则给全部进程发，小于-1则给某个组发信号
	if (!pid) while (--p > &FIRST_TASK) {
		if (*p && (*p)->pgrp == current->pid) 
			if (err=send_sig(sig,*p,1))
				retval = err;
	} else if (pid>0) while (--p > &FIRST_TASK) {
		if (*p && (*p)->pid == pid) 
			if (err=send_sig(sig,*p,0))
				retval = err;
	} else if (pid == -1) while (--p > &FIRST_TASK)
		if (err = send_sig(sig,*p,0))
			retval = err;
	else while (--p > &FIRST_TASK)
		if (*p && (*p)->pgrp == -pid)
			if (err = send_sig(sig,*p,0))
				retval = err;
	return retval;
}

// 子进程退出，通知进程id是pid的父进程
static void tell_father(int pid)
{
	int i;

	if (pid)
		for (i=0;i<NR_TASKS;i++) {
			if (!task[i])
				continue;
			if (task[i]->pid != pid)
				continue;
			// 根据pid找到父进程，设置子进程退出的信号
			task[i]->signal |= (1<<(SIGCHLD-1));
			return;
		}
/* if we don't find any fathers, we just release ourselves */
/* This is not really OK. Must change it to make father 1 */
	printk("BAD BAD - no father found\n\r");
	// 释放pcb结构
	release(current);
}

int do_exit(long code)
{
	int i;
	// 释放代码段和数据段页表,页目录，物理地址
	free_page_tables(get_base(current->ldt[1]),get_limit(0x0f));
	free_page_tables(get_base(current->ldt[2]),get_limit(0x17));
	for (i=0 ; i<NR_TASKS ; i++)
		// 找出当前进程的子进程
		if (task[i] && task[i]->father == current->pid) {
			// 子进程的新父进程是进程id为1的进程
			task[i]->father = 1;
			/*
			 如果子进程刚把自己的状态改成TASK_ZOMBIE,执行到tell_father里的代码时，时间片到了，
			 然后调度父进程执行，这时候父进程退出了，再切换到子进程执行的时候，
			 子进程给父进程发信号就丢失了，所以这里补充一下这个逻辑，给新的父进程发信号
			*/
			if (task[i]->state == TASK_ZOMBIE)
				/* assumption task[1] is always init */
				(void) send_sig(SIGCHLD, task[1], 1);
		}
	// 关闭文件
	for (i=0 ; i<NR_OPEN ; i++)
		if (current->filp[i])
			sys_close(i);
	// 回写inode到硬盘
	iput(current->pwd);
	current->pwd=NULL;
	iput(current->root);
	current->root=NULL;
	iput(current->executable);
	current->executable=NULL;
	// 是会话首进程并打开了终端
	if (current->leader && current->tty >= 0)
		tty_table[current->tty].pgrp = 0;
	if (last_task_used_math == current)
		last_task_used_math = NULL;
	// 是会话首进程，则通知会话里的所有进程会话结束
	if (current->leader)
		kill_session();
	// 更新状态
	current->state = TASK_ZOMBIE;
	current->exit_code = code;
	// 通知父进程
	tell_father(current->father);
	// 重新调度进程（tell_father里已经调度过了）
	schedule();
	return (-1);	/* just to suppress warnings */
}

int sys_exit(int error_code)
{
	return do_exit((error_code&0xff)<<8);
}
// 等待pid进程退出，并且把退出码写到stat_addr变量
int sys_waitpid(pid_t pid,unsigned long * stat_addr, int options)
{
	int flag, code;
	struct task_struct ** p;

	verify_area(stat_addr,4);
repeat:
	flag=0;
	for(p = &LAST_TASK ; p > &FIRST_TASK ; --p) {
		// 过滤不符合条件的
		if (!*p || *p == current)
			continue;
		// 不是当前进程的子进程则跳过
		if ((*p)->father != current->pid)
			continue;
		// pid大于0说明等待某一个子进程
		if (pid>0) {
			// 不是等待的子进程则跳过
			if ((*p)->pid != pid)
				continue;
		} else if (!pid) {
			// pid等于0则等待进程组中的进程，不是当前进程组的进程则跳过
			if ((*p)->pgrp != current->pgrp)
				continue;
		} else if (pid != -1) {
			// 不等于-1说明是等待某一个组的，但不是当前进程的组，组id是-pid的组，不是该组则跳过
			if ((*p)->pgrp != -pid)
				continue;
		} 
		// else {
		//	等待所有进程
		// }
		// 找到了一个符合条件的进程
		switch ((*p)->state) {
			// 子进程已经退出,这个版本没有这个状态
			case TASK_STOPPED:
				if (!(options & WUNTRACED))
					continue;
				put_fs_long(0x7f,stat_addr);
				return (*p)->pid;
			case TASK_ZOMBIE:
				// 子进程已经退出，则返回父进程
				current->cutime += (*p)->utime;
				current->cstime += (*p)->stime;
				flag = (*p)->pid;
				code = (*p)->exit_code;
				release(*p);
				put_fs_long(code,stat_addr);
				return flag;
			default:
				// flag等于1说明子进程还没有退出
				flag=1;
				continue;
		}
	}
	// 还没有退出的进程
	if (flag) {
		// 设置了非阻塞则返回
		if (options & WNOHANG)
			return 0;
		// 否则父进程挂起
		current->state=TASK_INTERRUPTIBLE;
		// 重新调度
		schedule();
		/*
			在schedule函数里，如果当前进程收到了信号，会变成running状态，
			如果current->signal &= ~(1<<(SIGCHLD-1)))为0，即...0000000100000... & ...111111110111111...
			说明当前需要处理的信号是SIGCHLD，因为signal不可能为全0，否则进程不可能被唤醒，
			即有子进程退出，跳到repeat找到该退出的进程，否则说明是其他信号导致了进程变成可执行状态，
			阻塞的进程被信号唤醒，返回EINTR
		*/
		if (!(current->signal &= ~(1<<(SIGCHLD-1))))
			goto repeat;
		else
			return -EINTR;
	}
	return -ECHILD;
}


