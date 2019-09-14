/*
 *  linux/kernel/fork.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 *  'fork.c' contains the help-routines for the 'fork' system call
 * (see also system_call.s), and some misc functions ('verify_area').
 * Fork is rather simple, once you get the hang of it, but the memory
 * management can be a bitch. See 'mm/mm.c': 'copy_page_tables()'
 */
#include <errno.h>

#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/segment.h>
#include <asm/system.h>

extern void write_verify(unsigned long address);

long last_pid=0;

void verify_area(void * addr,int size)
{
	unsigned long start;

	start = (unsigned long) addr;
	size += start & 0xfff;
	start &= 0xfffff000;
	start += get_base(current->ldt[2]);
	while (size>0) {
		size -= 4096;
		write_verify(start);
		start += 4096;
	}
}

int copy_mem(int nr,struct task_struct * p)
{
	unsigned long old_data_base,new_data_base,data_limit;
	unsigned long old_code_base,new_code_base,code_limit;
	// 代码段限长
	code_limit=get_limit(0x0f);
	// 数据段限长
	data_limit=get_limit(0x17);
	old_code_base = get_base(current->ldt[1]);
	old_data_base = get_base(current->ldt[2]);
	if (old_data_base != old_code_base)
		panic("We don't support separate I&D");
	if (data_limit < code_limit)
		panic("Bad data_limit");
	// 设置进程的线性地址的首地址，每个进程占64MB
	new_data_base = new_code_base = nr * 0x4000000;
	p->start_code = new_code_base;
	// 设置线性地址到ldt的描述符中
	set_base(p->ldt[1],new_code_base);
	set_base(p->ldt[2],new_data_base);
	// 把父进程的页目录项和页表复制到子进程,old_data_base,new_data_base是线性地址,父子进程共享物理页面，即copy on write
	if (copy_page_tables(old_data_base,new_data_base,data_limit)) {
		free_page_tables(new_data_base,data_limit);
		return -ENOMEM;
	}
	return 0;
}

/*
 *  Ok, this is the main fork-routine. It copies the system process
 * information (task[nr]) and sets up the necessary registers. It
 * also copies the data segment in it's entirety.
 */
int copy_process(int nr,long ebp,long edi,long esi,long gs,long none,
		long ebx,long ecx,long edx,
		long fs,long es,long ds,
		long eip,long cs,long eflags,long esp,long ss)
{
	struct task_struct *p;
	int i;
	struct file *f;
	// 申请一页存pcb
	p = (struct task_struct *) get_free_page();
	if (!p)
		return -EAGAIN;
	// 挂载到全局pcb数组
	task[nr] = p;
	// 复制当前进程的数据
	*p = *current;	/* NOTE! this doesn't copy the supervisor stack */
	p->state = TASK_UNINTERRUPTIBLE;
	p->pid = last_pid;
	p->father = current->pid;
	p->counter = p->priority;
	p->signal = 0;
	p->alarm = 0;
	p->leader = 0;		/* process leadership doesn't inherit */
	p->utime = p->stime = 0;
	p->cutime = p->cstime = 0;
	// 当前时间
	p->start_time = jiffies;
	p->tss.back_link = 0;
	// 内核栈，在页末
	p->tss.esp0 = PAGE_SIZE + (long) p;
	p->tss.ss0 = 0x10;
	// 调用fork时压入栈的ip，子进程创建完成会从这开始执行，即if (__res >= 0) 
	p->tss.eip = eip;
	p->tss.eflags = eflags;
	// 子进程从fork返回的是0，eax会赋值给__res
	p->tss.eax = 0;
	p->tss.ecx = ecx;
	p->tss.edx = edx;
	p->tss.ebx = ebx;
	p->tss.esp = esp;
	p->tss.ebp = ebp;
	p->tss.esi = esi;
	p->tss.edi = edi;
	// 段选择子是16位
	p->tss.es = es & 0xffff;
	p->tss.cs = cs & 0xffff;
	p->tss.ss = ss & 0xffff;
	p->tss.ds = ds & 0xffff;
	p->tss.fs = fs & 0xffff;
	p->tss.gs = gs & 0xffff;
	/*
		计算第nr进程在GDT中关于LDT的索引，切换任务的时候，
		这个索引会被加载到ldt寄存器，cpu会自动根据ldt的值，把
		GDT中相应位置的段描述符加载到ldt寄存器(共16+32+16位)
	*/
	p->tss.ldt = _LDT(nr); 
	p->tss.trace_bitmap = 0x80000000;
	if (last_task_used_math == current)
		__asm__("clts ; fnsave %0"::"m" (p->tss.i387));
	/*
	设置线性地址范围，挂载线性地址首地址和限长到ldt，赋值页目录项和页表
	执行进程的时候，tss选择子被加载到tss寄存器，然后把tss里的上下文
	也加载到对应的寄存器，比如cr3，ldt选择子。tss信息中的ldt索引首先从gdt找到进程ldt
	结构体数据的首地址，然后根据当前段的属性，比如代码段，
	则从cs中取得选择子，系统从ldt表中取得进程线性空间
	的首地址、限长、权限等信息。用线性地址的首地址加上ip
	中的偏移，得到线性地址，然后再通过页目录和页表得到物理
	地址，物理地址还没有分配则进行缺页异常等处理。
	*/
	if (copy_mem(nr,p)) {
		task[nr] = NULL;
		free_page((long) p);
		return -EAGAIN;
	}
	// 父子进程都有同样的文件描述符，file结构体加一
	for (i=0; i<NR_OPEN;i++)
		if (f=p->filp[i])
			f->f_count++;
	// inode节点加一
	if (current->pwd)
		current->pwd->i_count++;
	if (current->root)
		current->root->i_count++;
	if (current->executable)
		current->executable->i_count++;
	/*
		挂载tss和ldt地址到gdt，nr << 1即乘以2，这里算出的是第nr个进程距离第一个tss描述符地址的偏移，
		单位是8个字节，即选择描述符大小,_LDT是偏移的大小，单位是1，这里是8
	*/
	set_tss_desc(gdt+(nr<<1)+FIRST_TSS_ENTRY,&(p->tss));
	set_ldt_desc(gdt+(nr<<1)+FIRST_LDT_ENTRY,&(p->ldt));
	p->state = TASK_RUNNING;	/* do this last, just in case */
	return last_pid;
}

int find_empty_process(void)
{
	int i;

	repeat:
		// 先找到一个可用的pid
		if ((++last_pid)<0) last_pid=1;
		for(i=0 ; i<NR_TASKS ; i++)
			if (task[i] && task[i]->pid == last_pid) goto repeat;
	// 再找一个可用的pcb项，从1开始，0是init进程
	for(i=1 ; i<NR_TASKS ; i++)
		if (!task[i])
			return i;
	return -EAGAIN;
}
		