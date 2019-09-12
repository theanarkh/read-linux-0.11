!
! SYS_SIZE is the number of clicks (16 bytes) to be loaded.
! 0x3000 is 0x30000 bytes = 196kB, more than enough for current
! versions of linux
!
SYSSIZE = 0x3000
!
!	bootsect.s		(C) 1991 Linus Torvalds
!
! bootsect.s is loaded at 0x7c00 by the bios-startup routines, and moves
! iself out of the way to address 0x90000, and jumps there.
!
! It then loads 'setup' directly after itself (0x90200), and the system
! at 0x10000, using BIOS interrupts. 
!
! NOTE! currently system is at most 8*65536 bytes long. This should be no
! problem, even in the future. I want to keep it simple. This 512 kB
! kernel size should be enough, especially as this doesn't contain the
! buffer cache as in minix
!
! The loader has been made as simple as possible, and continuos
! read errors will result in a unbreakable loop. Reboot by hand. It
! loads pretty fast by getting whole sectors at a time whenever possible.

.globl begtext, begdata, begbss, endtext, enddata, endbss
.text
begtext:
.data
begdata:
.bss
begbss:
.text
// setup模块的扇区数
SETUPLEN = 4				! nr of setup-sectors
// bootsect模块被加载到的地址，这是bios规定的
BOOTSEG  = 0x07c0			! original address of boot-sector
// 把bootsect模块移到这里
INITSEG  = 0x9000			! we move boot here - out of the way
// bootsect模块占一个扇区，后面即0x90200,紧跟着setup模块
SETUPSEG = 0x9020			! setup starts here
// system模块加载到这
SYSSEG   = 0x1000			! system loaded at 0x10000 (65536).
// system模块的末地址，即首地址+大小
ENDSEG   = SYSSEG + SYSSIZE		! where to stop loading

! ROOT_DEV:	0x000 - same type of floppy as boot.
!		0x301 - first partition on first drive etc
// 根设备，见fs.h的定义，3开头为硬盘，306是第二个硬盘的第一个分区
ROOT_DEV = 0x306

entry start
start:
	// 把setup的代码复制到0x9000，256字节,BOOTSEG是系统代码被bios加载到的地址
	mov	ax,#BOOTSEG
	mov	ds,ax
	// INITSEG是系统将自己的代码复制过去的地址
	mov	ax,#INITSEG
	mov	es,ax
	// 从0x07c00复制256字节到0x90000
	mov	cx,#256
	// 清0，因为没有偏移
	sub	si,si
	sub	di,di
	rep
	// 每次传16位
	movw
	// 复制完后段间跳转到0x9000:go,CS = INITSEG,IP = go,即跳过前面复制代码的逻辑，go是段内偏移
	jmpi	go,INITSEG
// 新的代码段和数据段基址，即0x9000，
go:	mov	ax,cs
	mov	ds,ax
	mov	es,ax
! put stack at 0x9ff00.
	mov	ss,ax
	// 即0x9000:0xFF00
	mov	sp,#0xFF00		! arbitrary value >>512

! load the setup-sectors directly after the bootblock.
! Note that 'es' is already set up.
// 加载setup模块
load_setup:
	/*
		bios 13号中断。对应的功能有很多，由ax传入使用哪个功能。这里使用的是功能2，读取扇区数据

		AH＝功能号

		AL＝扇区数

		CH＝柱面

		CL＝扇区

		DH＝磁头

		DL＝驱动器，00H~7FH：软盘；80H~0FFH：硬盘

		ES:BX＝缓冲区的地址

		返回：CF＝0说明操作成功，否则，AH＝错误代码
	*/
	mov	dx,#0x0000		! drive 0, head 0
	// 第一个扇区存的是bootsect.s的代码，setup模块的代码在第二个扇区开始的四个扇区
	mov	cx,#0x0002		! sector 2, track 0
	// 读取到es:bx的地址中，es等于cs等于0x9000,即读取的地址刚好落在bootsect.s之后（0x9000:0x0200）
	mov	bx,#0x0200		! address = 512, in INITSEG
	// 读4个扇区
	mov	ax,#0x0200+SETUPLEN	! service 2, nr of sectors
	int	0x13			! read it
	/*
		读取软盘的setup模块代码，jc在CF=1时跳转，jnc则在CF=0时跳转，
		读取软盘出错则CF=1，ah是出错码，所以下面是CF等于1，说明加载成功，则跳转，
		否则则重试
	*/
	jnc	ok_load_setup		! ok - continue
	// 驱动器是0
	mov	dx,#0x0000
	// 功能号0是复位磁盘
	mov	ax,#0x0000		! reset the diskette
	// 再次触发中断，重置磁盘
	int	0x13
	// 继续尝试加载
	j	load_setup

// 加载setup模块成功
ok_load_setup:

! Get disk drive parameters, specifically nr of sectors/track
	// 读取的驱动器是0，即软盘
	mov	dl,#0x00
	// 调用读取驱动器参数功能，即8号服务
	mov	ax,#0x0800		! AH=8 is get drive parameters
	int	0x13
	// cx高位清0
	mov	ch,#0x00
	// 段超越，下面的一条指令段寄存器是cs，默认是ds。这时候，其实cs和ds的值是一样的，都是0x9000
	seg cs
	/*
	磁头数*柱面数（磁道数）*扇区数*2（两面）=总大小
	BL:
	= 01H — 360K

	= 02H — 1.2M

	= 03H — 720K

	= 04H — 1.44M

	CH = 柱面数的低8位

	CL的位7-6 = 柱面数的高2位

	CL的位5-0 = 扇区数

	DH = 磁头数

	DL = 驱动器数
	 把cx的低8位内容写到cs:sectors中，sectors见下面定义，两个字节。1.44MB的软盘柱面数是80，
	 所以cl的高两位肯定是0，因为ch已经足够保存柱面数，所以cl的值就是每柱面的扇区数，下面的代码会用
	*/
	mov	sectors,cx
	// 置es为0x9000
	mov	ax,#INITSEG
	mov	es,ax

! Print some inane message
	// 中断10的3号功能是读光标位置
	mov	ah,#0x03		! read cursor pos
	// 页数是0
	xor	bh,bh
	int	0x10
	// ch低4位是终止位置，cl的低4位是开始位置
	mov	cx,#24
	// 显示模式
	mov	bx,#0x0007		! page 0, attribute 7 (normal)
	// ES:BP字符串的段:偏移地址
	mov	bp,#msg1
	// ah是功能号，13是显示字符串。al是显示模式。1表示字符串只包含字符码，显示之后更新光标位置
	mov	ax,#0x1301		! write string, move cursor
	int	0x10

! ok, we've written the message, now
! we want to load the system (at 0x10000)
	// 加载system模块代码
	mov	ax,#SYSSEG
	mov	es,ax		! segment of 0x010000
	call	read_it
	call	kill_motor

! After that we check which root-device to use. If the device is
! defined (!= 0), nothing is done and the given device is used.
! Otherwise, either /dev/PS0 (2,28) or /dev/at0 (2,8), depending
! on the number of sectors that the BIOS reports currently.

	seg cs
	// 根设备，本版本代码里定义为0x306
	mov	ax,root_dev
	// 非0说明定义了，本版本代码定义了根设备的值。为第二个硬盘的第一个分区
	cmp	ax,#0
	// 非0 则跳到root_defined
	jne	root_defined
	seg cs
	// 每个柱面的扇区数，该信息是bois读取软盘的时得到的，然后判断软盘的类型
	mov	bx,sectors
	/*
		软盘的主设备号是2，次设备号是type * 4 + n （n = 0-3）
		1.2mb的软盘type是2，1.44mb的软盘type是7,
		对比bios读取的信息和1.2、1.44软盘的信息，是否一样。
		一样则更新根设备号，否则出错
	*/
	mov	ax,#0x0208		! /dev/ps0 - 1.2Mb
	cmp	bx,#15
	je	root_defined
	mov	ax,#0x021c		! /dev/PS0 - 1.44Mb
	cmp	bx,#18
	je	root_defined
undef_root:
	jmp undef_root
root_defined:
	seg cs
	mov	root_dev,ax

! after that (everyting loaded), we jump to
! the setup-routine loaded directly after
! the bootblock:
	// 加载完setup和system模块，跳到setup模块执行
	jmpi	0,SETUPSEG

! This routine loads the system at address 0x10000, making sure
! no 64kB boundaries are crossed. We try to load it as fast as
! possible, loading whole tracks whenever we can.
!
! in:	es - starting address segment (normally 0x1000)
!
// 已经读取的扇区，1是bootsect模块，SETUPLEN是setup模块，四个扇区
sread:	.word 1+SETUPLEN	! sectors read of current track
head:	.word 0			! current head
track:	.word 0			! current track
// 读取system模块
read_it:
	mov ax,es
	// 判断es的值，目前定义是0x1000，结果非0则有问题
	test ax,#0x0fff
die:	jne die			! es must be at 64kB boundary
	// 清0，组成es:bx，即0x1000:0
	xor bx,bx		! bx is starting address within segment
rp_read:
	// 判断是否读完了
	mov ax,es
	cmp ax,#ENDSEG		! have we loaded all yet?
	/*
		小于则跳转，根据CF判断，CF的值反映运算是否产生进位或借位。
		如果运算结果的最高位产生一个进位或借位，则CF置1，否则置0,
		所以cmp ax,#ENDSEG，即ax - #ENDSEG的结果如果小于0，则CF是1，则跳转，说明还没读完
	*/
	jb ok1_read
	ret
// 算出需要读取的扇区数，并且判断读取完后会不会超过当前es:bx所能表示的范围，不会则开始读，否则只读所能表示的范围内的扇区数
ok1_read:
	// 段超越,cs是0x9000
	seg cs
	// 每柱面的扇区数
	mov ax,sectors
	// 减去已经读取扇区数，一开始的时候是五个扇区（bootsect+setup模块）
	sub ax,sread
	// 要读取的扇区数放到cx
	mov cx,ax
	// 左移9位即乘以512，扇区数*每扇区的字节数。得到总字节数
	shl cx,#9
	// 相加，会影响CF寄存器。bx一开始等于0。这里先判断读取完毕后，最后一个字节地址是否超过了es:bx所能表示的范围
	add cx,bx
	// jump not carry，即CF=0,则跳转，说明读取成功后，最后一个字节没有超过es:bx所能表示的范围
	jnc ok2_read
	// 等于0则跳转，即zf等于1，说明读完后刚好等于当前es:bx所能表示的最大范围+1
	je ok2_read
	// 读取后会大于当前es:bx所能表示的范围
	xor ax,ax
	sub ax,bx
	// 判断在当前es:bx可表示的范围内，还能读取多少个扇区
	shr ax,#9
/*
	读取所需的扇区后，然后判断该柱面的扇区是否都读完了，
	不是则跳到ok3_read，否则继续判断是否两个磁头都读完了，
	不是则跳到ok4_read,否则柱面数加一，准备读下一个柱面的数据
*/
ok2_read:
	// 读取某个柱面的多个扇区
	call read_track
	// ax是刚才读取的扇区数
	mov cx,ax
	// 累加得到当前已经读取的扇区数
	add ax,sread
	seg cs
	// 和每柱面的扇区数比较，看这个柱面的扇区是不是已经读取完毕
	cmp ax,sectors
	// 还没读完则跳到ok3_read
	jne ok3_read
	// 等于说明读完了一个柱面，再判断是不是读完了两个磁头
	mov ax,#1
	// head是0或1即两面磁头
	sub ax,head
	// 不等于0说明head是0，则继续读磁头1，即对面的磁头
	jne ok4_read
	// 等于0说明读完了该柱面的两个磁头的扇区，磁头号加一，track是轨道的意思，即磁道
	inc track
/*
	记录准备读的磁头号,
	如果是跳转过来的，说明ax是1，即读取一号磁头，已读取扇区是0，即ax清0，
	如果是从inc track执行下来的，说明ax是0，即读完了两个磁头了，mov head ax，即重置磁头为0.已读扇区数为0
	3.5英寸软盘片，其上、下两面各被划分为80个磁道，每个磁道被划分为18个扇区，每个扇区的存储容量固定为512字节。
*/
ok4_read:
	mov head,ax
	xor ax,ax
// 读取完后，更新bx的值，即下一个写入的位置
ok3_read:
	// 已读取的扇区，更新sread的值
	mov sread,ax
	// cx是刚才读取成功的扇区数
	shl cx,#9
	// 更新bx的值，es:bx指向的内存存放着从软盘读取的数据
	add bx,cx
	// CF=0则跳转，说明还没有超过es:bx所能指向的内存范围，继续读，往es:bx继续写数据
	jnc rp_read
	/*
		否则CF=1则往下走，说明刚才读取的数据超过了当前es:bx表示的范围，更新es和bx的值
		ok3_read是由ok2_read调用的，ok2_read由ok1_read调用。但有个前提是，读取完数据后，
		bx的大小是小于等于64kb的，所以走到这里说明是等于64kb。因为CF=1才会走到这，CF=1说明add bx cx进位了，
		然后es执行下一个64kb的基地址。bx等于0
	*/
	mov ax,es
	// es加64kb，执行下一个段基址
	add ax,#0x1000
	mov es,ax
	// bx清0
	xor bx,bx
	// 继续读
	jmp rp_read
// 读取一个柱面的多个扇区
read_track:
	// ax当前记录了要读取的扇区数
	push ax
	push bx
	push cx
	push dx
	// 磁道号，初始化是0
	mov dx,track
	// 已读的扇区数
	mov cx,sread
	// 加一表示即将读取的扇区号，ax的高位，即ah记录需要读取的扇区数
	inc cx
	// 磁道号
	mov ch,dl
	// 磁头号，0或1
	mov dx,head
	// dh是磁头号
	mov dh,dl
	// 驱动器号
	mov dl,#0
	// 相与，保证磁头号是0或1，即只有dh低一位是0或1
	and dx,#0x0100
	// 二号功能，读取扇区，调用中断前，al记录需要读取的扇区数，中断后，al记录了读取成功的扇区数
	mov ah,#2
	int 0x13
	// CF＝0表示成功，jc是jump carry，表示进位则跳转。即CF=1时跳转，CF=1表示读取失败
	jc bad_rt
	pop dx
	pop cx
	pop bx
	pop ax
	ret
// 磁盘系统复位，重新读
bad_rt:	mov ax,#0
	mov dx,#0
	int 0x13
	// 出栈
	pop dx
	pop cx
	pop bx
	pop ax
	// 重新读
	jmp read_track

/*
 * This procedure turns off the floppy drive motor, so
 * that we enter the kernel in a known state, and
 * don't have to worry about it later.
 */
kill_motor:
	push dx
	// 目的端口
	mov dx,#0x3f2
	// 写入到目的端口的值
	mov al,#0
	outb
	pop dx
	ret

sectors:
	.word 0

msg1:
	.byte 13,10
	.ascii "Loading system ..."
	.byte 13,10,13,10

.org 508
root_dev:
	.word ROOT_DEV
boot_flag:
	.word 0xAA55

.text
endtext:
.data
enddata:
.bss
endbss:
