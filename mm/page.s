/*
 *  linux/mm/page.s
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * page.s contains the low-level page-exception code.
 * the real work is done in mm.c
 */

.globl _page_fault

_page_fault:
	// 交换两个寄存器的值，esp指向的位置保存了错误码
	xchgl %eax,(%esp)
	// 压栈寄存器
	pushl %ecx
	pushl %edx
	push %ds
	push %es
	push %fs
	// 内核数据段描述符
	movl $0x10,%edx
	mov %dx,%ds
	mov %dx,%es
	mov %dx,%fs
	// 如果是缺页异常，cr2保存了引起缺页的线性地址
	movl %cr2,%edx
	// 线性地址（有的话）和错误码入参
	pushl %edx
	pushl %eax
	// 1和eax与，结果放到ZF中
	testl $1,%eax
	// zf=0则跳转，即0是写异常，1是缺页异常
	jne 1f
	call _do_no_page
	// 跳到标签2
	jmp 2f
1:	call _do_wp_page
// 出栈，返回中断，会重新执行异常指令
2:	addl $8,%esp
	pop %fs
	pop %es
	pop %ds
	popl %edx
	popl %ecx
	popl %eax
	iret
