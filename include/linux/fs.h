/*
 * This file has definitions for some important file table
 * structures etc.
 */

#ifndef _FS_H
#define _FS_H

#include <sys/types.h>

/* devices are as follows: (same as minix, so we can use the minix
 * file system. These are major numbers.)
 *
 * 0 - unused (nodev)
 * 1 - /dev/mem
 * 2 - /dev/fd
 * 3 - /dev/hd
 * 4 - /dev/ttyx
 * 5 - /dev/tty
 * 6 - /dev/lp
 * 7 - unnamed pipes
 */

#define IS_SEEKABLE(x) ((x)>=1 && (x)<=3)

#define READ 0
#define WRITE 1
// 预读写
#define READA 2		/* read-ahead - don't pause */
#define WRITEA 3	/* "write-ahead" - silly, but somewhat useful */

void buffer_init(long buffer_end);
// 设备的主设备号和次设备号，低八位是次设备号，高位是主设备号
#define MAJOR(a) (((unsigned)(a))>>8)
#define MINOR(a) ((a)&0xff)
// 文件名长度
#define NAME_LEN 14
// 文件系统的根inode节点号
#define ROOT_INO 1
// 块位图和inode位图占据的最大硬盘块数
#define I_MAP_SLOTS 8
#define Z_MAP_SLOTS 8
// 超级块的魔数，说明是有效的超级块
#define SUPER_MAGIC 0x137F
// 一个进程打开文件数的大小
#define NR_OPEN 20
// 系统同时打开的inode数大小，即同时只能打开32个文件
#define NR_INODE 32
// file结构体数，进程间共享的
#define NR_FILE 64
// 超级块数，即文件系统的个数
#define NR_SUPER 8
#define NR_HASH 307
// 缓存文件系统数据的buffer个数，操作系统启动的时候初始化该变量
#define NR_BUFFERS nr_buffers
// 硬盘一块对应的字节数
#define BLOCK_SIZE 1024
// 2 >> 10 即块的大小，用于计算字节数到块数
#define BLOCK_SIZE_BITS 10
#ifndef NULL
#define NULL ((void *) 0)
#endif
// 每个硬盘块有几个inode节点，即块大小除以硬盘中每个inode结构的大小，硬盘里是d_inode，内存是m_inode结构
#define INODES_PER_BLOCK ((BLOCK_SIZE)/(sizeof (struct d_inode)))
// 每个硬盘块包含的目录项数
#define DIR_ENTRIES_PER_BLOCK ((BLOCK_SIZE)/(sizeof (struct dir_entry)))

// 下面宏用于匿名管道，可结合管道的实现理解
#define PIPE_HEAD(inode) ((inode).i_zone[0])
#define PIPE_TAIL(inode) ((inode).i_zone[1])
#define PIPE_SIZE(inode) ((PIPE_HEAD(inode)-PIPE_TAIL(inode))&(PAGE_SIZE-1))
#define PIPE_EMPTY(inode) (PIPE_HEAD(inode)==PIPE_TAIL(inode))
#define PIPE_FULL(inode) (PIPE_SIZE(inode)==(PAGE_SIZE-1))
#define INC_PIPE(head) \
__asm__("incl %0\n\tandl $4095,%0"::"m" (head))
// 该版本没有用这个定义
typedef char buffer_block[BLOCK_SIZE];
// 管理文件系统的数据缓存的结构
struct buffer_head {
	char * b_data;			/* pointer to data block (1024 bytes) */
	unsigned long b_blocknr;	/* block number */
	unsigned short b_dev;		/* device (0 = free) */
	unsigned char b_uptodate;
	unsigned char b_dirt;		/* 0-clean,1-dirty */
	unsigned char b_count;		/* users using this block */
	unsigned char b_lock;		/* 0 - ok, 1 -locked */
	struct task_struct * b_wait;
	struct buffer_head * b_prev;
	struct buffer_head * b_next;
	struct buffer_head * b_prev_free;
	struct buffer_head * b_next_free;
};
// 文件系统在硬盘里的inode节点结构
struct d_inode {
	// 各种标记位，读写执行等，我们ls时看到的
	unsigned short i_mode;
	// 文件的用户id
	unsigned short i_uid;
	// 文件大小
	unsigned long i_size;
	unsigned long i_time;
	// 文件的用户组id
	unsigned char i_gid;
	// 文件入度，即有多少个目录指向他
	unsigned char i_nlinks;
	// 存储文件内容对应的硬盘块号
	unsigned short i_zone[9];
};
// 内存中的inode节点结构
struct m_inode {
	// 和d_inode一样
	unsigned short i_mode;
	unsigned short i_uid;
	unsigned long i_size;
	unsigned long i_mtime;
	unsigned char i_gid;
	unsigned char i_nlinks;
	unsigned short i_zone[9];
/* these are in memory also */
	// 在内存中使用的字段
	// 等待该inode节点的进程队列
	struct task_struct * i_wait;
	// access time文件被访问就会修改这个字段
	unsigned long i_atime;
	/*
		change time，修改文件内容和属性就会修改这个字段，
		还有一个是mtime，修改文件内容就会修改这个字段，但是这个版本没有实现
	*/
	unsigned long i_ctime;
	// inode所属的设备号
	unsigned short i_dev;
	// 该结构引用的inode在硬盘里的号
	unsigned short i_num;
	// 多少个进程在使用这个inode
	unsigned short i_count;
	// 互斥锁
	unsigned char i_lock;
	// inode内容是否被修改过
	unsigned char i_dirt;
	// 是不是管道文件
	unsigned char i_pipe;
	// 该节点是否挂载了另外的文件系统
	unsigned char i_mount;
	// 这个版本没用到
	unsigned char i_seek;
	/*
		数据是否是最新的，或者说有效的，
		update代表数据的有效性，dirt代表文件是否需要回写,
		比如写入文件的时候，a进程写入的时候，dirt是1，因为需要回写到硬盘，
		但是数据是最新的，update是1，这时候b进程读取这个文件的时候，可以从
		缓存里直接读取。
	*/
	unsigned char i_update;
};
// 管理打开文件的内存属性的结构，比如操作位置(inode没有读取操作位置这个概念，),实现系统进程共享inode
struct file {
	unsigned short f_mode;
	unsigned short f_flags;
	unsigned short f_count;
	struct m_inode * f_inode;
	off_t f_pos;
};
// 超级块，管理一个文件系统元数据的结构
struct super_block {
	// inode节点个数
	unsigned short s_ninodes;
	// 数据块占据的逻辑块总块数
	unsigned short s_nzones;
	// inode和数据块位图占据的硬盘块数，位图是记录哪个块或者inode节点被使用了
	unsigned short s_imap_blocks;
	unsigned short s_zmap_blocks;
	/*
		第一块在硬盘的块号，一个硬盘可以有几个文件系统，
		每个文件系统占据一部分，所以要记录开始的块号和总块数
	*/
	unsigned short s_firstdatazone;
	/*
		用于计算文件系统块等于多少个硬盘块，硬盘块大小乘以2
		的s_log_zone_size次方等于文件系统的块大小（硬盘块的
		大小和文件系统块的大小不是一回事，比如硬盘块的大小是1kb，
		文件系统是4kb）
	*/ 
	unsigned short s_log_zone_size;
	// 文件名最大字节数 
	unsigned long s_max_size;
	// 魔数
	unsigned short s_magic;
/* These are only in memory */
	// 缓存inode位图的内容
	struct buffer_head * s_imap[8];
	// 缓存数据块位图的内容
	struct  buffer_head * s_zmap[8];
	// 设备号
	unsigned short s_dev;
	// 挂载在哪个文件的inode下
	struct m_inode * s_isup;
	struct m_inode * s_imount;
	unsigned long s_time;
	// 等待使用该超级块的进程队列
	struct task_struct * s_wait;
	// 互斥访问变量
	unsigned char s_lock;
	// 文件系统是否只能读不能写
	unsigned char s_rd_only;
	// 是否需要回写到硬盘
	unsigned char s_dirt;
};
// 超级块在硬盘的结构
struct d_super_block {
	// inode节点数量
	unsigned short s_ninodes;
	// 硬盘块数量
	unsigned short s_nzones;
	// inode位图块数量
	unsigned short s_imap_blocks;
	// 数据块位图数量
	unsigned short s_zmap_blocks;
	// 文件系统的第一块块号，不是数据块第一块块号
	unsigned short s_firstdatazone;
	// 参考上面
	unsigned short s_log_zone_size;
	// 最大文件长度
	unsigned long s_max_size;
	// 判断是否是超级块的标记
	unsigned short s_magic;
};
// 目录项结构
struct dir_entry {
	// inode号
	unsigned short inode;
	// 文件名
	char name[NAME_LEN];
};
// 进程共享的inode列表
extern struct m_inode inode_table[NR_INODE];
// 进程共享的file结构列表
extern struct file file_table[NR_FILE];
// 超级块列表
extern struct super_block super_block[NR_SUPER];
// 缓存区管理
extern struct buffer_head * start_buffer;
// 缓冲区个数
extern int nr_buffers;

extern void check_disk_change(int dev);
extern int floppy_change(unsigned int nr);
extern int ticks_to_floppy_on(unsigned int dev);
extern void floppy_on(unsigned int dev);
extern void floppy_off(unsigned int dev);
extern void truncate(struct m_inode * inode);
extern void sync_inodes(void);
extern void wait_on(struct m_inode * inode);
extern int bmap(struct m_inode * inode,int block);
extern int create_block(struct m_inode * inode,int block);
extern struct m_inode * namei(const char * pathname);
extern int open_namei(const char * pathname, int flag, int mode,
	struct m_inode ** res_inode);
extern void iput(struct m_inode * inode);
extern struct m_inode * iget(int dev,int nr);
extern struct m_inode * get_empty_inode(void);
extern struct m_inode * get_pipe_inode(void);
extern struct buffer_head * get_hash_table(int dev, int block);
extern struct buffer_head * getblk(int dev, int block);
extern void ll_rw_block(int rw, struct buffer_head * bh);
extern void brelse(struct buffer_head * buf);
extern struct buffer_head * bread(int dev,int block);
extern void bread_page(unsigned long addr,int dev,int b[4]);
extern struct buffer_head * breada(int dev,int block,...);
extern int new_block(int dev);
extern void free_block(int dev, int block);
extern struct m_inode * new_inode(int dev);
extern void free_inode(struct m_inode * inode);
extern int sync_dev(int dev);
extern struct super_block * get_super(int dev);
extern int ROOT_DEV;

extern void mount_root(void);

#endif
