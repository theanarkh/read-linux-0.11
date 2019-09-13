/*
 *  linux/kernel/sched.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * 'sched.c' is the main kernel file. It contains scheduling primitives
 * (sleep_on, wakeup, schedule etc) as well as a number of simple system
 * call functions (type getpid(), which just extracts a field from
 * current-task
 */
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/sys.h>
#include <linux/fdreg.h>
#include <asm/system.h>
#include <asm/io.h>
#include <asm/segment.h>

#include <signal.h>

#define _S(nr) (1<<((nr)-1))
// 可以阻塞的信号
#define _BLOCKABLE (~(_S(SIGKILL) | _S(SIGSTOP)))

void show_task(int nr,struct task_struct * p)
{
	int i,j = 4096-sizeof(struct task_struct);

	printk("%d: pid=%d, state=%d, ",nr,p->pid,p->state);
	i=0;
	while (i<j && !((char *)(p+1))[i])
		i++;
	printk("%d (of %d) chars free in kernel stack\n\r",i,j);
}

void show_stat(void)
{
	int i;

	for (i=0;i<NR_TASKS;i++)
		if (task[i])
			show_task(i,task[i]);
}

#define LATCH (1193180/HZ)

extern void mem_use(void);

extern int timer_interrupt(void);
extern int system_call(void);

union task_union {
	struct task_struct task;
	char stack[PAGE_SIZE];
};

static union task_union init_task = {INIT_TASK,};

long volatile jiffies=0;
long startup_time=0;
struct task_struct *current = &(init_task.task);
struct task_struct *last_task_used_math = NULL;

struct task_struct * task[NR_TASKS] = {&(init_task.task), };

long user_stack [ PAGE_SIZE>>2 ] ;

struct {
	long * a;
	short b;
	} stack_start = { & user_stack [PAGE_SIZE>>2] , 0x10 };
/*
 *  'math_state_restore()' saves the current math information in the
 * old math state array, and gets the new ones from the current task
 */
void math_state_restore()
{
	if (last_task_used_math == current)
		return;
	__asm__("fwait");
	if (last_task_used_math) {
		__asm__("fnsave %0"::"m" (last_task_used_math->tss.i387));
	}
	last_task_used_math=current;
	if (current->used_math) {
		__asm__("frstor %0"::"m" (current->tss.i387));
	} else {
		__asm__("fninit"::);
		current->used_math=1;
	}
}

/*
 *  'schedule()' is the scheduler function. This is GOOD CODE! There
 * probably won't be any reason to change this, as it should work well
 * in all circumstances (ie gives IO-bound processes good response etc).
 * The one thing you might take a look at is the signal-handler code here.
 *
 *   NOTE!!  Task 0 is the 'idle' task, which gets called when no other
 * tasks can run. It can not be killed, and it cannot sleep. The 'state'
 * information in task[0] is never used.
 */
void schedule(void)
{
	int i,next,c;
	struct task_struct ** p;

/* check alarm, wake up any interruptible tasks that have got a signal */
	// 处理进程的信号和状态
	for(p = &LAST_TASK ; p > &FIRST_TASK ; --p)
		if (*p) {
			/*
				alarm的值是调用alarm函数设置的，见alarm函数，进程可以调用alarm函数，设置一个时间，
				然后到期后会触发alram信号，alarm < jiffies说明过期了。设置alarm信号
			*/
			if ((*p)->alarm && (*p)->alarm < jiffies) {
					(*p)->signal |= (1<<(SIGALRM-1));
					(*p)->alarm = 0;
				}
			/*
				_BLOCKABLE为可以阻塞的信号集合，blocked为当前进程设置的阻塞集合，相与
				得到进程当前阻塞的集合，即排除进程阻塞了不能阻塞的信号，然后取反得到可以接收的
				信号集合，再和signal相与，得到当前进程当前收到的信号。如果进程处于挂起状态，则改成可执行	
			*/
			if (((*p)->signal & ~(_BLOCKABLE & (*p)->blocked)) &&
			(*p)->state==TASK_INTERRUPTIBLE)
				(*p)->state=TASK_RUNNING;
		}

/* this is the scheduler proper: */
	// 开始调度，选择合适的进程执行
	while (1) {
		c = -1;
		next = 0;
		i = NR_TASKS;
		p = &task[NR_TASKS];
		while (--i) {
			if (!*--p)
				continue;
				// 找出时间片最大的进程，说明他执行的时间最短
			if ((*p)->state == TASK_RUNNING && (*p)->counter > c)
				c = (*p)->counter, next = i;
		}
		/*
			如果没有进程可以执行，则c等于-1，会执行进程0.如果，如果c不等于-1，
			说明有进程可以执行，但是c可能等于0或者等于1，等于0说明，进程时间片执行完了，
			则执行下面的代码重新计算时间片，c等于0则说明有进程可以执行，则进行切换
		*/
		if (c) break;
		// 没有break说明c等于0，即所有的进程时间片已经执行完，需要重新设置
		for(p = &LAST_TASK ; p > &FIRST_TASK ; --p)
			if (*p)
				// 优先级越高，执行的时间越长，被选中执行的机会越大
				(*p)->counter = ((*p)->counter >> 1) +
						(*p)->priority;
	}
	// 切换进程
	switch_to(next);
}

int sys_pause(void)
{
	current->state = TASK_INTERRUPTIBLE;
	schedule();
	return 0;
}
// 当前进程挂载到睡眠队列p中，p指向队列头指针的地址
void sleep_on(struct task_struct **p)
{
	struct task_struct *tmp;

	if (!p)
		return;
	if (current == &(init_task.task))
		panic("task[0] trying to sleep");
	/*
		*p为第一个睡眠节点的地址，即tmp指向第一个睡眠节点
		头指针指向当前进程，这个版本的实现没有采用真正链表的形式，
		他通过每个进程在栈中的临时变量形成一个链表，每个睡眠的进程，
		在栈里有一个变量指向后面一个睡眠节点，然后把链表的头指针指向当前进程，
		然后切换到其他进程执行，当被wake_up唤醒的时候，wake_up会唤醒链表的第一个
		睡眠节点，因为第一个节点里保存了后面一个节点的地址，所以他唤醒后面一个节点，
		后面一个节点以此类推，从而把整个链表的节点唤醒，这里的实现类似nginx的filter，
		即每个模块保存后面一个节点的地址，然后把全局指针指向自己。
	*/
	tmp = *p;
	*p = current;
	// 不可中断睡眠只能通过wake_up唤醒，即使有信号也无法唤醒
	current->state = TASK_UNINTERRUPTIBLE;
	schedule();
	// 唤醒后面一个节点
	if (tmp)
		tmp->state=0;
}

void interruptible_sleep_on(struct task_struct **p)
{
	struct task_struct *tmp;

	if (!p)
		return;
	if (current == &(init_task.task))
		panic("task[0] trying to sleep");
	tmp=*p;
	*p=current;
/*
	可中断地睡眠，可以通过wake_up和接收信号唤醒，不可中断的时候，
	能保证唤醒的时候，是从前往后逐个唤醒,但是可中断睡眠无法保证这一点，
	因为进程可能被信号唤醒了，所以需要判断全局指针是否指向了自己，即自己插入
	链表后，还有没有进程也插入了该链表
*/
repeat:	current->state = TASK_INTERRUPTIBLE;
	schedule();
	/*
		这里为true，说明是信号唤醒，因为wake_up能保证唤醒的是第一个节点，
		这里先唤醒链表中比当前进程后插入链表的节点，有点奇怪，自己被信号唤醒了，
		去唤醒别的进程，自己却还睡眠
	*/
	if (*p && *p != current) {
		(**p).state=0;
		goto repeat;
	}
	// 类似sleep_on的原理
	*p=NULL;
	if (tmp)
		tmp->state=0;
}
// 唤醒队列中的第一个节点，并清空链表，因为第一个节点会向后唤醒其他节点
void wake_up(struct task_struct **p)
{
	if (p && *p) {
		(**p).state=0;
		*p=NULL;
	}
}

/*
 * OK, here are some floppy things that shouldn't be in the kernel
 * proper. They are here because the floppy needs a timer, and this
 * was the easiest way of doing it.
 */
static struct task_struct * wait_motor[4] = {NULL,NULL,NULL,NULL};
static int  mon_timer[4]={0,0,0,0};
static int moff_timer[4]={0,0,0,0};
unsigned char current_DOR = 0x0C;

int ticks_to_floppy_on(unsigned int nr)
{
	extern unsigned char selected;
	unsigned char mask = 0x10 << nr;

	if (nr>3)
		panic("floppy_on: nr>3");
	moff_timer[nr]=10000;		/* 100 s = very big :-) */
	cli();				/* use floppy_off to turn it off */
	mask |= current_DOR;
	if (!selected) {
		mask &= 0xFC;
		mask |= nr;
	}
	if (mask != current_DOR) {
		outb(mask,FD_DOR);
		if ((mask ^ current_DOR) & 0xf0)
			mon_timer[nr] = HZ/2;
		else if (mon_timer[nr] < 2)
			mon_timer[nr] = 2;
		current_DOR = mask;
	}
	sti();
	return mon_timer[nr];
}

void floppy_on(unsigned int nr)
{
	cli();
	while (ticks_to_floppy_on(nr))
		sleep_on(nr+wait_motor);
	sti();
}

void floppy_off(unsigned int nr)
{
	moff_timer[nr]=3*HZ;
}

void do_floppy_timer(void)
{
	int i;
	unsigned char mask = 0x10;

	for (i=0 ; i<4 ; i++,mask <<= 1) {
		if (!(mask & current_DOR))
			continue;
		if (mon_timer[i]) {
			if (!--mon_timer[i])
				wake_up(i+wait_motor);
		} else if (!moff_timer[i]) {
			current_DOR &= ~mask;
			outb(current_DOR,FD_DOR);
		} else
			moff_timer[i]--;
	}
}

#define TIME_REQUESTS 64

// 定时器数组，其实是个链表
static struct timer_list {
	long jiffies;
	void (*fn)();
	struct timer_list * next;
} timer_list[TIME_REQUESTS], * next_timer = NULL;

void add_timer(long jiffies, void (*fn)(void))
{
	struct timer_list * p;

	if (!fn)
		return;
	// 关中断，防止多个进程”同时“操作
	cli();
	// 直接到期，直接执行回调
	if (jiffies <= 0)
		(fn)();
	else {
		// 遍历定时器数组，找到一个空项
		for (p = timer_list ; p < timer_list + TIME_REQUESTS ; p++)
			if (!p->fn)
				break;
		// 没有空项了
		if (p >= timer_list + TIME_REQUESTS)
			panic("No more time requests free");
		// 给空项赋值
		p->fn = fn;
		p->jiffies = jiffies;
		// 在数组中形成链表
		p->next = next_timer;
		// next_timer指向第一个节点，即最早到期的
		next_timer = p;
		/*
			修改链表，保证超时时间是从小到大的顺序
			原理：
				每个节点都是以前面一个节点的到时时间为坐标，节点里的jiffies即超时时间
				是前一个节点到期后的多少个jiffies后该节点到期。
		*/
		while (p->next && p->next->jiffies < p->jiffies) {
			// 前面的节点比后面节点大，则前面节点减去后面节点的值，算出偏移值，下面准备置换位置
			p->jiffies -= p->next->jiffies;
			// 先保存一下
			fn = p->fn;
			// 置换两个节点的回调
			p->fn = p->next->fn;
			p->next->fn = fn;
			jiffies = p->jiffies;
			// 置换两个节点是超时时间
			p->jiffies = p->next->jiffies;
			p->next->jiffies = jiffies;
			/*
				到这，第一个节点是最快到期的，还需要更新后续节点的值，其实就是找到一个合适的位置
				插入，因为内核是用数组实现的定时器队列，所以是通过置换位置实现插入，
				如果是链表，则直接找到合适的位置，插入即可，所谓合适的位置，
				就是找到第一个比当前节点大的节点，插入到他前面。
			*/
			p = p->next;
		}
		/*
			内核这里实现有个bug，当当前节点是最小时，需要更新原链表中第一个节点的值，，
			否则会导致原链表中第一个节点的过期时间延长，修复代码如下：
			if (p->next && p->next->jiffies > p->jiffies) {
				p->next->jiffies = p->next->jiffies - p->jiffies;
			}	
			即更新原链表中第一个节点相对于新的第一个节点的偏移，剩余的节点不需要更新，因为他相对于
			他前面的节点的偏移不变，但是原链表中的第一个节点之前前面没有节点，所以偏移就是他自己的值，
			而现在在他前面插入了一个节点，则他的偏移是相对于前面一个节点的偏移
		*/
	}
	sti();
}
// 定时中断处理函数
void do_timer(long cpl)
{
	extern int beepcount;
	extern void sysbeepstop(void);

	if (beepcount)
		if (!--beepcount)
			sysbeepstop();
	// 当前在用户态，增加用户态的执行时间，否则增加该进程的系统执行时间
	if (cpl)
		current->utime++;
	else
		current->stime++;
	// next_timer为空说明还没有定时节点
	if (next_timer) {
		// 第一个节点减去一个jiffies，因为其他节点都是相对第一个节点的偏移，所以其他节点的值不需要变
		next_timer->jiffies--;
		// 当前节点到期，如果有多个节点超时时间一样，即相对第一个节点偏移是0，则会多次进入while循环
		while (next_timer && next_timer->jiffies <= 0) {
			void (*fn)(void);
			
			fn = next_timer->fn;
			next_timer->fn = NULL;
			// 下一个节点
			next_timer = next_timer->next;
			// 执行定时回调函数
			(fn)();
		}
	}
	if (current_DOR & 0xf0)
		do_floppy_timer();
	// 当前进程的可用时间减一，不为0则接着执行，否则可能需要重新调度
	if ((--current->counter)>0) return;
	current->counter=0;
	// 是系统进程则继续执行
	if (!cpl) return;
	// 进程调度
	schedule();
}

int sys_alarm(long seconds)
{
	int old = current->alarm;

	if (old)
		old = (old - jiffies) / HZ;
	// 1秒等于100个jiffies
	current->alarm = (seconds>0)?(jiffies+HZ*seconds):0;
	return (old);
}

int sys_getpid(void)
{
	return current->pid;
}

int sys_getppid(void)
{
	return current->father;
}

int sys_getuid(void)
{
	return current->uid;
}

int sys_geteuid(void)
{
	return current->euid;
}

int sys_getgid(void)
{
	return current->gid;
}

int sys_getegid(void)
{
	return current->egid;
}
// 修改进程执行的优先级,满足条件的情况下increment越大优先权越低
int sys_nice(long increment)
{
	if (current->priority-increment>0)
		current->priority -= increment;
	return 0;
}

void sched_init(void)
{
	int i;
	struct desc_struct * p;

	if (sizeof(struct sigaction) != 16)
		panic("Struct sigaction MUST be 16 bytes");
	// 设置进程0在gdt中的tss和ldt描述符
	set_tss_desc(gdt+FIRST_TSS_ENTRY,&(init_task.task.tss));
	set_ldt_desc(gdt+FIRST_LDT_ENTRY,&(init_task.task.ldt));
	// 初始化剩下的tss和ldt项
	p = gdt+2+FIRST_TSS_ENTRY;
	// 一个进程一项，所以值需要处理一部分
	for(i=1;i<NR_TASKS;i++) {
		// 清空pcb数组
		task[i] = NULL;
		// tss段描述符清0
		p->a=p->b=0;
		p++;
		// ldt描述符清0
		p->a=p->b=0;
		p++;
	}
/* Clear NT, so that we won't have troubles with that later on */
	// 压栈eflags寄存器到栈，修改压栈的内容，清NT位，再回写到eflags中，NT是标记当前执行的任务是否是嵌套的任务，比如通过call调用的则置1
	__asm__("pushfl ; andl $0xffffbfff,(%esp) ; popfl");
	// 加载第一个任务的tss选择子到tr寄存器，然后cpu会找到GDT中的描述符，把基地址和段限长加载到tr寄存器
	ltr(0);
	// 加载第一个任务的ldt选择子到ldt寄存器，然后cpu会找到GDT中的描述符，把基地址和段限长加载到ldtr寄存器
	lldt(0);
	// 43是控制字端口，0x36=0x00110110,即二进制，方式3，先读写低8位再读写高8位，选择计算器0
	outb_p(0x36,0x43);		/* binary, mode 3, LSB/MSB, ch 0 */
	/*
		写入初始值，40端口是计数通道0，初始值
		的含义是，8253每一个波动，初始值会减一，减到0则输出一个通知，
		LATCH = (1193180/100),1193180是8253的工作频率，
		一秒钟波动1193180次。(1193180/100)就是(1193180/1000)*10，即
		(1193180/1000)减到0的时候，过去了1毫秒。乘以10即过去10毫秒。
	*/
	outb_p(LATCH & 0xff , 0x40);	/* LSB */
	// 再写8位
	outb(LATCH >> 8 , 0x40);	/* MSB */
	// 设置定时中断处理函数，中断号是20,8253会触发该中断
	set_intr_gate(0x20,&timer_interrupt);
	// 开中断，即清除中断屏蔽字
	outb(inb_p(0x21)&~0x01,0x21);
	// 系统调用处理函数
	set_system_gate(0x80,&system_call);
}
