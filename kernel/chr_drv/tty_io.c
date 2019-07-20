/*
 *  linux/kernel/tty_io.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * 'tty_io.c' gives an orthogonal feeling to tty's, be they consoles
 * or rs-channels. It also implements echoing, cooked mode etc.
 *
 * Kill-line thanks to John T Kohl.
 */
#include <ctype.h>
#include <errno.h>
#include <signal.h>

#define ALRMMASK (1<<(SIGALRM-1))
#define KILLMASK (1<<(SIGKILL-1))
#define INTMASK (1<<(SIGINT-1))
#define QUITMASK (1<<(SIGQUIT-1))
#define TSTPMASK (1<<(SIGTSTP-1))

#include <linux/sched.h>
#include <linux/tty.h>
#include <asm/segment.h>
#include <asm/system.h>
// 判断f位是否为1
#define _L_FLAG(tty,f)	((tty)->termios.c_lflag & f)
#define _I_FLAG(tty,f)	((tty)->termios.c_iflag & f)
#define _O_FLAG(tty,f)	((tty)->termios.c_oflag & f)

// 判断对应的位是否为1
#define L_CANON(tty)	_L_FLAG((tty),ICANON)
#define L_ISIG(tty)	_L_FLAG((tty),ISIG)
#define L_ECHO(tty)	_L_FLAG((tty),ECHO)
#define L_ECHOE(tty)	_L_FLAG((tty),ECHOE)
#define L_ECHOK(tty)	_L_FLAG((tty),ECHOK)
#define L_ECHOCTL(tty)	_L_FLAG((tty),ECHOCTL)
#define L_ECHOKE(tty)	_L_FLAG((tty),ECHOKE)

#define I_UCLC(tty)	_I_FLAG((tty),IUCLC)
#define I_NLCR(tty)	_I_FLAG((tty),INLCR)
#define I_CRNL(tty)	_I_FLAG((tty),ICRNL)
#define I_NOCR(tty)	_I_FLAG((tty),IGNCR)

#define O_POST(tty)	_O_FLAG((tty),OPOST)
#define O_NLCR(tty)	_O_FLAG((tty),ONLCR)
#define O_CRNL(tty)	_O_FLAG((tty),OCRNL)
#define O_NLRET(tty)	_O_FLAG((tty),ONLRET)
#define O_LCUC(tty)	_O_FLAG((tty),OLCUC)

struct tty_struct tty_table[] = {
	{
		{ICRNL,		/* change incoming CR to NL */
		OPOST|ONLCR,	/* change outgoing NL to CRNL */
		0,
		ISIG | ICANON | ECHO | ECHOCTL | ECHOKE,
		0,		/* console termio */
		INIT_C_CC},
		0,			/* initial pgrp */
		0,			/* initial stopped */
		con_write,
		{0,0,0,0,""},		/* console read-queue */
		{0,0,0,0,""},		/* console write-queue */
		{0,0,0,0,""}		/* console secondary queue */
	},{
		{0, /* no translation */
		0,  /* no translation */
		B2400 | CS8,
		0,
		0,
		INIT_C_CC},
		0,
		0,
		rs_write,
		{0x3f8,0,0,0,""},		/* rs 1 */
		{0x3f8,0,0,0,""},
		{0,0,0,0,""}
	},{
		{0, /* no translation */
		0,  /* no translation */
		B2400 | CS8,
		0,
		0,
		INIT_C_CC},
		0,
		0,
		rs_write,
		{0x2f8,0,0,0,""},		/* rs 2 */
		{0x2f8,0,0,0,""},
		{0,0,0,0,""}
	}
};

/*
 * these are the tables used by the machine code handlers.
 * you can implement pseudo-tty's or something by changing
 * them. Currently not done.
 */
struct tty_queue * table_list[]={
	&tty_table[0].read_q, &tty_table[0].write_q,
	&tty_table[1].read_q, &tty_table[1].write_q,
	&tty_table[2].read_q, &tty_table[2].write_q
	};

void tty_init(void)
{
	rs_init();
	con_init();
}
// 给某个进程组内的所有进程发送mask信息
void tty_intr(struct tty_struct * tty, int mask)
{
	int i;

	if (tty->pgrp <= 0)
		return;
	for (i=0;i<NR_TASKS;i++)
		if (task[i] && task[i]->pgrp==tty->pgrp)
			task[i]->signal |= mask;
}

static void sleep_if_empty(struct tty_queue * queue)
{
	cli();
	// 没有信号需要处理并且队列空，挂起，等待数据
	while (!current->signal && EMPTY(*queue))
		interruptible_sleep_on(&queue->proc_list);
	sti();
}

static void sleep_if_full(struct tty_queue * queue)
{
	// 没满则返回
	if (!FULL(*queue))
		return;
	cli();
	// 没有信号需要处理并且剩下的空间小于128个字节则挂起，这里LEFT似乎是恒成立的
	while (!current->signal && LEFT(*queue)<128)
		interruptible_sleep_on(&queue->proc_list);
	sti();
}

void wait_for_keypress(void)
{
	sleep_if_empty(&tty_table[0].secondary);
}

void copy_to_cooked(struct tty_struct * tty)
{
	signed char c;

	while (!EMPTY(tty->read_q) && !FULL(tty->secondary)) {
		// 从队列获取一个字符
		GETCH(tty->read_q,c);
		// 是回车键
		if (c==13)
			// 定义了回车换成换行键，则code是10.即转成了换行键
			if (I_CRNL(tty))
				c=10;
			// 定义了忽略回车键则跳过这个字符
			else if (I_NOCR(tty))
				continue;
			else ;
		// 如果输入的换行键并且定义了换行键缓存回车键则把code转成回车键的code
		else if (c==10 && I_NLCR(tty))
			c=13;
		// 定义了大写转小写，则进行转换
		if (I_UCLC(tty))
			c=tolower(c);
		// 开始了标准模式，说明if里面的字符只在标准模式下有意义
		if (L_CANON(tty)) {
			// 是删除一行的键
			if (c==KILL_CHAR(tty)) {
				/* deal with killing the input line */
				while(!(EMPTY(tty->secondary) ||
				        (c=LAST(tty->secondary))==10 ||
				        c==EOF_CHAR(tty))) {
					if (L_ECHO(tty)) {
						if (c<32)
							PUTCH(127,tty->write_q);
						PUTCH(127,tty->write_q);
						tty->write(tty);
					}
					DEC(tty->secondary.head);
				}
				continue;
			}
			// 删除前一个字符
			if (c==ERASE_CHAR(tty)) {
				if (EMPTY(tty->secondary) ||
				   (c=LAST(tty->secondary))==10 ||
				   c==EOF_CHAR(tty))
					continue;
				if (L_ECHO(tty)) {
					if (c<32)
						PUTCH(127,tty->write_q);
					PUTCH(127,tty->write_q);
					tty->write(tty);
				}
				DEC(tty->secondary.head);
				continue;
			}
			// 停止输出
			if (c==STOP_CHAR(tty)) {
				// 设flag
				tty->stopped=1;
				continue;
			}
			// 重新开始输出
			if (c==START_CHAR(tty)) {
				// 清掉flag
				tty->stopped=0;
				continue;
			}
		}
		// 是否需要给进程发送信号
		if (L_ISIG(tty)) {
			// 是SIGINT信号对应的键，给tty对应的组发送SIGINT信号
			if (c==INTR_CHAR(tty)) {
				tty_intr(tty,INTMASK);
				continue;
			}
			// 同上，发送SIGQUIT信号
			if (c==QUIT_CHAR(tty)) {
				tty_intr(tty,QUITMASK);
				continue;
			}
		}
		// 如果键是换行或者结束符
		if (c==10 || c==EOF_CHAR(tty))
			tty->secondary.data++;
		// 设置了回显
		if (L_ECHO(tty)) {
			// 是换行键
			if (c==10) {
				// 把换行回车两个键写入写队列
				PUTCH(10,tty->write_q);
				PUTCH(13,tty->write_q);
			} else if (c<32) {
				// code小于32的字符，是一些控制字符，shift，ctrl等，并且设置了回显控制字符，则显示^（c+100）
				if (L_ECHOCTL(tty)) {
					PUTCH('^',tty->write_q);
					PUTCH(c+64,tty->write_q);
				}
			} else
				// 其他字符直接回显
				PUTCH(c,tty->write_q);
			tty->write(tty);
		}
		PUTCH(c,tty->secondary);
	}
	wake_up(&tty->secondary.proc_list);
}

int tty_read(unsigned channel, char * buf, int nr)
{
	struct tty_struct * tty;
	char c, * b=buf;
	int minimum,time,flag=0;
	long oldalarm;

	if (channel>2 || nr<0) return -1;
	tty = &tty_table[channel];
	oldalarm = current->alarm;
	// 最小的读取间隔
	time = 10L*tty->termios.c_cc[VTIME];
	// 最少读取的字节数
	minimum = tty->termios.c_cc[VMIN];
	// 如果没有定义最小读取间隔，则可以随时取，如果定义了最小读取间隔，则最少读取的字符数是1
	if (time && !minimum) {
		minimum=1;
		// 还没有设置过下次读取时间或者最小读取的时间间隔已经到
		if (flag=(!oldalarm || time+jiffies<oldalarm))
			// 更新下次读取的时间
			current->alarm = time+jiffies;
	}
	// 
	if (minimum>nr)
		minimum=nr;
	while (nr>0) {
		// flag等于false说明还没到读取间隔
		if (flag && (current->signal & ALRMMASK)) {
			current->signal &= ~ALRMMASK;
			break;
		}
		if (current->signal)
			break;
		if (EMPTY(tty->secondary) || (L_CANON(tty) &&
		!tty->secondary.data && LEFT(tty->secondary)>20)) {
			sleep_if_empty(&tty->secondary);
			continue;
		}
		do {
			GETCH(tty->secondary,c);
			if (c==EOF_CHAR(tty) || c==10)
				tty->secondary.data--;
			if (c==EOF_CHAR(tty) && L_CANON(tty))
				return (b-buf);
			else {
				put_fs_byte(c,b++);
				if (!--nr)
					break;
			}
		} while (nr>0 && !EMPTY(tty->secondary));
		if (time && !L_CANON(tty))
			if (flag=(!oldalarm || time+jiffies<oldalarm))
				current->alarm = time+jiffies;
			else
				current->alarm = oldalarm;
		if (L_CANON(tty)) {
			if (b-buf)
				break;
		} else if (b-buf >= minimum)
			break;
	}
	current->alarm = oldalarm;
	if (current->signal && !(b-buf))
		return -EINTR;
	return (b-buf);
}

int tty_write(unsigned channel, char * buf, int nr)
{
	static cr_flag=0;
	struct tty_struct * tty;
	char c, *b=buf;

	if (channel>2 || nr<0) return -1;
	tty = channel + tty_table;
	while (nr>0) {
		// 写队列满的话则挂起
		sleep_if_full(&tty->write_q);
		// 有信号需要处理
		if (current->signal)
			break;
		// 还没写完并且还有空间可以写
		while (nr>0 && !FULL(tty->write_q)) {
			c=get_fs_byte(b);
			// 设置了OPOST标记，才能使得其他设置有效
			if (O_POST(tty)) {
				// 当前字符是回车，并且设置了回车换成换行字符，则进行转换
				if (c=='\r' && O_CRNL(tty))
					c='\n';
				// 当前字符是换行，并且设置了ONLRET，则换成回车，下面继续处理ONLRET标记
				else if (c=='\n' && O_NLRET(tty))
					c='\r';
				// 如果当前是换行，并且没有设cr_flag标记，并设置了ONLRE，则标记需要把光标移到下一行开始
				if (c=='\n' && !cr_flag && O_NLCR(tty)) {
					cr_flag = 1;
					PUTCH(13,tty->write_q);
					// 这里直接continue，下一次还是走到这个if，但是是false了然后继续走下一轮
					continue;
				}
				// 设置了大写转小写，则转换
				if (O_LCUC(tty))
					c=toupper(c);
			}
			// 指向下一个待写入的字符，待写入个数减一
			b++; nr--;
			// 清标记
			cr_flag = 0;
			PUTCH(c,tty->write_q);
		}
		tty->write(tty);
		// 还没有写完，说明写空间已满，则挂起
		if (nr>0)
			schedule();
	}
	return (b-buf);
}

/*
 * Jeh, sometimes I really like the 386.
 * This routine is called from an interrupt,
 * and there should be absolutely no problem
 * with sleeping even in an interrupt (I hope).
 * Of course, if somebody proves me wrong, I'll
 * hate intel for all time :-). We'll have to
 * be careful and see to reinstating the interrupt
 * chips before calling this, though.
 *
 * I don't think we sleep here under normal circumstances
 * anyway, which is good, as the task sleeping might be
 * totally innocent.
 */
void do_tty_interrupt(int tty)
{
	copy_to_cooked(tty_table+tty);
}

void chr_dev_init(void)
{
}
