/*
 *  linux/kernel/blk_drv/ramdisk.c
 *
 *  Written by Theodore Ts'o, 12/2/91
 */

#include <string.h>

#include <linux/config.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <asm/system.h>
#include <asm/segment.h>
#include <asm/memory.h>

#define MAJOR_NR 1
#include "blk.h"

// 虚拟盘的起始地址，系统启动时初始化
char	*rd_start;
int	rd_length = 0;
// 请求虚拟盘时的处理函数
void do_rd_request(void)
{
	int	len;
	char	*addr;

	INIT_REQUEST;
	// 第几个扇区，每个扇区512字节，所以addr等于扇区数*512，即左移9位	
	addr = rd_start + (CURRENT->sector << 9);
	// 要读写的扇区数，一个扇区512字节，所以需要读取字节数等于扇区数*512
	len = CURRENT->nr_sectors << 9;
	// 越界
	if ((MINOR(CURRENT->dev) != 1) || (addr+len > rd_start+rd_length)) {
		end_request(0);
		goto repeat;
	}
	// 和内存进行数据交互
	if (CURRENT-> cmd == WRITE) {
		(void ) memcpy(addr,
			      CURRENT->buffer,
			      len);
	} else if (CURRENT->cmd == READ) {
		(void) memcpy(CURRENT->buffer, 
			      addr,
			      len);
	} else
		panic("unknown ramdisk-command");
	end_request(1);
	// 继续处理下一个请求，repeat在blk.h的INIT_REQUEST中定义
	goto repeat;
}

/*
 * Returns amount of memory which needs to be reserved.
 */
// 操作系统初始化的时候执行，mem_start是在高速缓存后面
long rd_init(long mem_start, int length)
{
	int	i;
	char	*cp;

	blk_dev[MAJOR_NR].request_fn = DEVICE_REQUEST;
	// 记录虚拟盘的开始地址
	rd_start = (char *) mem_start;
	// 虚拟盘空间大小
	rd_length = length;
	cp = rd_start;
	// 清0
	for (i=0; i < length; i++)
		*cp++ = '\0';
	return(length);
}

/*
 * If the root device is the ram disk, try to load it.
 * In order to do this, the root device is originally set to the
 * floppy, and we later change it to be ram disk.
 */
// 如果根设备是虚拟盘，则系统初始化的时候先设置为软盘，把软盘数据读到虚拟盘后修改根设备为虚拟盘
void rd_load(void)
{
	struct buffer_head *bh;
	struct super_block	s;
	int		block = 256;	/* Start at block 256 */
	int		i = 1;
	int		nblocks;
	char		*cp;		/* Move pointer */
	// 虚拟盘在内存的地址
	if (!rd_length)
		return;
	printk("Ram disk: %d bytes, starting at 0x%x\n", rd_length,
		(int) rd_start);
	// 根设备不是软盘则直接返回
	if (MAJOR(ROOT_DEV) != 2)
		return;
	// 读入软盘的超级块
	bh = breada(ROOT_DEV,block+1,block,block+2,-1);
	if (!bh) {
		printk("Disk error while looking for ramdisk!\n");
		return;
	}
	*((struct d_super_block *) &s) = *((struct d_super_block *) bh->b_data);
	brelse(bh);
	// 超级块标记
	if (s.s_magic != SUPER_MAGIC)
		/* No ram disk image present, assume normal floppy boot */
		return;
	// 软盘的大小是否大于虚拟盘的大小 
	nblocks = s.s_nzones << s.s_log_zone_size;
	if (nblocks > (rd_length >> BLOCK_SIZE_BITS)) {
		printk("Ram disk image too big!  (%d blocks, %d avail)\n", 
			nblocks, rd_length >> BLOCK_SIZE_BITS);
		return;
	}
	printk("Loading %d bytes into ram disk... 0000k", 
		nblocks << BLOCK_SIZE_BITS);
	// 虚拟盘的内存起始地址
	cp = rd_start;
	// 把软盘的数据读到虚拟盘的地址空间
	while (nblocks) {
		if (nblocks > 2) 
			bh = breada(ROOT_DEV, block, block+1, block+2, -1);
		else
			bh = bread(ROOT_DEV, block);
		if (!bh) {
			printk("I/O error on block %d, aborting load\n", 
				block);
			return;
		}
		(void) memcpy(cp, bh->b_data, BLOCK_SIZE);
		brelse(bh);
		printk("\010\010\010\010\010%4dk",i);
		cp += BLOCK_SIZE;
		block++;
		nblocks--;
		i++;
	}
	printk("\010\010\010\010\010done \n");
	ROOT_DEV=0x0101;
}
