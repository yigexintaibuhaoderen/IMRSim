#include <linux/module.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/device-mapper.h>
#include <linux/delay.h>
#include <linux/bitops.h>
#include <linux/kthread.h>
#include <linux/crc32.h>
#include <linux/gfp.h>
#include <linux/mutex.h>
#include <linux/math64.h>
#include <linux/version.h>
#include <asm/ptrace.h>
#include "imrsim_types.h"
#include "imrsim_ioctl.h"
#include "imrsim_kapi.h"
#include "imrsim_zerror.h"

/*
 The kernel module in the Device Mapper framework is mainly responsible for 
 building the target driver to build the disk structure and function.
*/

/* Some basic disk information */
#define IMR_ZONE_SIZE_SHIFT_DEFAULT      16      /* number of blocks/zone, e.g. 2^16=65536 */  //一个zone由多少块组成，256MB
#define IMR_BLOCK_SIZE_SHIFT_DEFAULT     3       /* number of sectors/block, 8   */  //一个块由多少扇区组成，8个扇区为512b*8=4kb
#define IMR_PAGE_SIZE_SHIFT_DEFAULT      3       /* number of sectors/page, 8    */  //一个页由多少扇区组成
#define IMR_SECTOR_SIZE_SHIFT_DEFAULT    9       /* number of bytes/sector, 512  */  //一个扇区包括512字节
#define IMR_TRANSFER_PENALTY             60      /* usec */   //延迟
#define IMR_TRANSFER_PENALTY_MAX         1000    /* usec */
#define IMR_ROTATE_PENALTY               11000   /* usec ,  5400rpm->  rotate time: 11ms*/


#define IMR_ALLOCATION_PHASE             2     /* phase of data distribution (2-3)*/

#define TOP_TRACK_NUM_TOTAL              64      //一个zone中顶部磁道数量
/*
 * The size of a zone is 256MB, divided into 64 track groups (top-bottom), with an average track of 2MB.
 * A group of top-bottom has 4MB, that is, 1024 blocks, and there are 64 groups of top-bottom in a zone.
 */

#define IMR_MAX_CAPACITY                 21474836480

static __u64   IMR_CAPACITY;            /* disk capacity (in sectors) */  //磁盘容量（sector为单位）
static __u32   IMR_NUMZONES;            /* number of zones */   //zone的数量
static __u32   IMR_NUMZONES_DEFAULT;                            //zone的数量
static __u32   IMR_ZONE_SIZE_SHIFT;                             //一个zone有多少块
static __u32   IMR_BLOCK_SIZE_SHIFT;                            //一个块由多少扇区组成

static __u32 IMR_TOP_TRACK_SIZE = 456;      /* number of blocks/topTrack  456 */  //一个顶部磁道中有456个块
static __u32 IMR_BOTTOM_TRACK_SIZE = 568;   /* number of blocks/bottomTrack  568 */ //一个底部磁道有568个块

__u32 VERSION = IMRSIM_VERSION(1,1,0);      /* The version number of IMRSIM：VERSION(x,y,z)=>((x<<16)|(y<<8)|z) */

struct imrsim_c{             /* Mapped devices in the Device Mapper framework, also known as logical devices. */
    struct dm_dev *dev;      /* block device */
    sector_t       start;    /* starting address */
};

/* Mutex resource locks */
/*互斥资源锁*/
struct mutex                     imrsim_zone_lock;
struct mutex                     imrsim_ioctl_lock;

/* IMRSIM Statistics */
/*IMRSim统计数据*/
static struct imrsim_state       *zone_state = NULL;
/* Array of zone status information */
/*zone状态信息数组*/
static struct imrsim_zone_status *zone_status = NULL;

/* error log */
static __u32 imrsim_dbg_rerr;
static __u32 imrsim_dbg_werr;
static __u32 imrsim_dbg_log_enabled = 0;
static unsigned long imrsim_dev_idle_checkpoint = 0;

/* Multi-device support, currently not supported */
int imrsim_single = 0;

/* Constants representing configuration changes */
/*表示配置更改的常量*/
enum imrsim_conf_change{
    IMR_NO_CHANGE     = 0x00,
    IMR_CONFIG_CHANGE = 0x01,
    IMR_STATS_CHANGE  = 0x02,
    IMR_STATUS_CHANGE = 0x04
};

/* persistent storage */
#define IMR_PSTORE_PG_EDG 92
#define IMR_PSTORE_PG_OFF 40
#define IMR_PSTORE_CHECK  1000
#define IMR_PSTORE_QDEPTH 128
#define IMR_PSTORE_PG_GAP 2

/* persistent storage task structure */
static struct imrsim_pstore_task    //元数据持久化任务，在主线程之外的一个线程中执行
{
    struct task_struct  *pstore_thread;   // 进程描述符（process descriptor) 结构  持久化线程
    __u32                sts_zone_idx;
    __u32                stu_zone_idx[IMR_PSTORE_QDEPTH];
    __u8                 stu_zone_idx_cnt;
    __u8                 stu_zone_idx_gap;
    sector_t             pstore_lba;       //持久化开始的地址
    unsigned char        flag;              /* three bit for imrsim_conf_change */  //持计划类型标识
                                            //利用该数据的最低3位分别表示3种磁盘配置改变的事件，
                                            //0x01表示IMR_CONFIG_CHANGE，0x02表示IMR_STATS_CHANGE，
                                            //0x04表示IMR_STATUS_CHANGE，判断时只需要用flag按位与不同类型的宏就能判断那种元数据发生了改变。
}imrsim_ptask;

/* RMW scheme structure */
/*RMW方案结构*/
static struct imrsim_RMW_task
{
    struct task_struct  *task;
    struct bio          *bio;
    sector_t            lba[2];
    __u8                lba_num;
}imrsim_rmw_task;

/* read/write completion structure */
static struct imrsim_completion_control
{
    struct completion   read_event;
    struct completion   write_event;
    struct completion   rmw_event;
}imrsim_completion;

/* To get the size of the imrsim_stats structure. */
static __u32 imrsim_stats_size(void)
{
    return (sizeof(struct imrsim_dev_stats) + sizeof(__u32) + sizeof(__u64)*2 +
            sizeof(struct imrsim_zone_stats) * IMR_NUMZONES);
}

/* To get the size of the imrsim_state structure. */
static __u32 imrsim_state_size(void)
{
    return (sizeof(struct imrsim_state_header) + 
            sizeof(struct imrsim_config) + 
            sizeof(struct imrsim_dev_stats) + sizeof(__u32) +
            IMR_NUMZONES * sizeof(struct imrsim_zone_stats) + 
            IMR_NUMZONES * sizeof(struct imrsim_zone_status) +
            sizeof(__u32));
}

/* To get how many sectors a zone has. */
static __u32 num_sectors_zone(void)
{
    return (1 << IMR_BLOCK_SIZE_SHIFT << IMR_ZONE_SIZE_SHIFT);
}

/* To get the sector address where the zone starts. */
/*获取zone的起始地址*/
static __u64 zone_idx_lba(__u64 idx){
    return (idx << IMR_BLOCK_SIZE_SHIFT << IMR_ZONE_SIZE_SHIFT);  
}

/* Returns the exponent of a power of 2. */
static __u64 index_power_of_2(__u64 num)
{
    __u64 index = 0;
    while(num >>= 1){
        ++index;
    }
    return index;
}

/* Device idle time initialization. */
static void imrsim_dev_idle_init(void)
{
    imrsim_dev_idle_checkpoint = jiffies;
    zone_state->stats.dev_stats.idle_stats.dev_idle_time_max = 0;
    zone_state->stats.dev_stats.idle_stats.dev_idle_time_min = jiffies / HZ;
}

/* Basic information for initializing zone. */
static void imrsim_init_zone_default(__u64 sizedev)   /* sizedev: in sectors */ //siedev以sector为单位
{
    IMR_CAPACITY = sizedev;
    IMR_ZONE_SIZE_SHIFT = IMR_ZONE_SIZE_SHIFT_DEFAULT;
    IMR_BLOCK_SIZE_SHIFT = IMR_BLOCK_SIZE_SHIFT_DEFAULT;
    IMR_NUMZONES = (IMR_CAPACITY >> IMR_BLOCK_SIZE_SHIFT >> IMR_ZONE_SIZE_SHIFT);
    IMR_NUMZONES_DEFAULT = IMR_NUMZONES;
    printk(KERN_INFO "imrsim_init_zone_state: numzones=%d sizedev=%llu\n",
        IMR_NUMZONES, sizedev); 
}

/* Basic information for initializing the device state (zone_state) */
/*磁盘统计信息*/
static void imrsim_init_zone_state_default(__u32 state_size)
{
    __u32 i;
    __u32 j;
    __u32 *magic;   /* magic number to identify the device (equipment identity) */

    /* head info. */
    zone_state->header.magic = 0xBEEFBEEF;
    zone_state->header.length = state_size;
    zone_state->header.version = VERSION;
    zone_state->header.crc32 = 0;

    /* config info. */
    zone_state->config.dev_config.out_of_policy_read_flag = 0;
    zone_state->config.dev_config.out_of_policy_write_flag = 0;
    zone_state->config.dev_config.r_time_to_rmw_zone = IMR_TRANSFER_PENALTY;
    zone_state->config.dev_config.w_time_to_rmw_zone = IMR_TRANSFER_PENALTY;

    zone_state->stats.num_zones = IMR_NUMZONES;
    zone_state->stats.extra_write_total = 0;
    zone_state->stats.write_total = 0;
    imrsim_reset_stats();  
    /* To allocate space for the zone_status array and initialize it. */
    /*为 zone_status 数组分配空间并初始化它。*/
    zone_status = (struct imrsim_zone_status *)&zone_state->stats.zone_stats[IMR_NUMZONES];//强制转换成(struct imrsim_zone_status *)类型指针
    for(i=0; i<IMR_NUMZONES; i++){
        zone_status[i].z_start = i;
        zone_status[i].z_length = num_sectors_zone();
        zone_status[i].z_type = Z_TYPE_CONVENTIONAL;
        zone_status[i].z_conds = Z_COND_NO_WP;
        zone_status[i].z_flag = 0;
        for(j=0;j<TOP_TRACK_NUM_TOTAL;j++){
            memset(zone_status[i].z_tracks[j].isUsedBlock,0,IMR_TOP_TRACK_SIZE*sizeof(__u8));
        }
        zone_status[i].z_map_size = 0;
        memset(zone_status[i].z_pba_map,-1,TOP_TRACK_NUM_TOTAL*(IMR_TOP_TRACK_SIZE+IMR_BOTTOM_TRACK_SIZE)*sizeof(int));
    }
    printk(KERN_INFO "imrsim: %s zone_status init!\n", __FUNCTION__);
    magic = (__u32 *)&zone_status[IMR_NUMZONES];
    *magic = 0xBEEFBEEF;
}

/* To initial a device. */
int imrsim_init_zone_state(__u64 sizedev)
{
    __u32 state_size;

    if(!sizedev){
        printk(KERN_ERR "imrsim: zero capacity detected\n");
        return -EINVAL;
    }
    imrsim_init_zone_default(sizedev);     /* Initialize the basic information of the zone.初始化zone的基本信息 */
    /* zone_state should not have allocated space, if it already exists, reclaim the space. */
    if(zone_state){
        vfree(zone_state);
    }
    state_size = imrsim_state_size();  // 获取imrsim_state结构的大小
    zone_state = vzalloc(state_size);     /* Allocate memory space for zone_state */
    if(!zone_state){
        printk(KERN_ERR "imrsim: memory alloc failed for zone state\n");
        return -ENOMEM;
    }
    imrsim_init_zone_state_default(state_size);   // 初始化设备状态（zone_state）的基本信息
    imrsim_dev_idle_init();      //设备空间初始化
    return 0;
}

/* read completion */
static void imrsim_read_completion(struct bio *bio, int err)
{
    if(err){
        printk(KERN_ERR "imrsim: bio read err: %d\n", err);
    }
    if(bio){
        complete((struct completion *)bio->bi_private);
    }
}

/* write completion */
static void imrsim_write_completion(struct bio *bio, int err)
{
    if(err){
        printk(KERN_ERR "imrsim: bio write err:%d\n", err);
    }
    if(bio){
        complete((struct completion *)bio->bi_private);
    }
}

/* To get device mapping offset.获取device映射偏移量。 */
static sector_t imrsim_map_sector(struct dm_target *ti, 
                                  sector_t bi_sector)
{
    struct imrsim_c *c = ti->private;  // dm_target-private:target specific data 
    return c->start + dm_target_offset(ti, bi_sector);  // Sector offset taken relative to the start of the target instead of
                                                        // relative to the start of the device.
                                                        //根据target device的起始地址和该bio请求在mapped device设备上的偏移值
                                                        //改变IO请求开始的扇区号bi_sector，从而完成IO请求的重定向
}

/* read page (for meta-data) 读取页面（用于元数据）*/
static int imrsim_read_page(struct block_device *dev, sector_t lba,
                            int size, struct page *page)
{
    //dump_stack();   // #include<asm/ptrace.h> Debugging: View function call stacks
    int ret = 0;
    struct bio *bio = bio_alloc(GFP_NOIO, 1); //bio初始化

    if(!bio){
        printk(KERN_ERR "imrsim: %s bio_alloc failed\n", __FUNCTION__);
        return -EFAULT;
    }
    bio->bi_bdev = dev;  //存放对应的块设备
    #if LINUX_VERSION_CODE < KERNEL_VERSION(3,14,0)
    bio->bi_sector = lba;
    #else
    bio->bi_iter.bi_sector = lba;   //请求的逻辑块地址，lba以扇区为单位
    #endif
    bio_add_page(bio, page, size, 0);
    init_completion(&imrsim_completion.read_event);
    bio->bi_private = &imrsim_completion.read_event;
    bio->bi_end_io = imrsim_read_completion;
    submit_bio(READ | REQ_SYNC, bio);
    wait_for_completion(&imrsim_completion.read_event);
    ret = test_bit(BIO_UPTODATE, &bio->bi_flags);//返回位的当前值
    if(!ret){
        printk(KERN_ERR "imrsim: pstore bio read failed\n");
        ret = -EIO;
    }
    bio_put(bio);
    return ret;
}

/* write page (for meta-data) 写页（用于元数据）*/
static int imrsim_write_page(struct block_device *dev, sector_t lba,
                            __u32 size, struct page *page)
{
    int ret = 0;
    struct bio *bio = bio_alloc(GFP_NOIO, 1);

    if(!bio){
        printk(KERN_ERR "imrsim: %s bio_alloc failed\n", __FUNCTION__);
        return -EFAULT;
    }
    bio->bi_bdev = dev;
    #if LINUX_VERSION_CODE < KERNEL_VERSION(3,14,0)
    bio->bi_sector = lba;
    #else
    bio->bi_iter.bi_sector = lba;
    #endif
    bio_add_page(bio, page, size, 0);
    init_completion(&imrsim_completion.write_event);
    bio->bi_private = &imrsim_completion.write_event;
    bio->bi_end_io = imrsim_write_completion;
    submit_bio(WRITE_FLUSH_FUA, bio);
    wait_for_completion(&imrsim_completion.write_event);
    ret = test_bit(BIO_UPTODATE, &bio->bi_flags);
    if(!ret){
        printk(KERN_ERR "imrsim: pstore bio write failed\n");
        ret = -EIO;
    }
    bio_put(bio);  //bio_put()函数减少bio中引用计数器(bi_cnt)的值，如果该值等于0，则释放bio结构以及相关的bio_vec结构。
    return ret;
}

/* End event for rmw bio */
/*rmw bio 的结束事件*/
static void imrsim_end_rmw(struct bio *bio, int err)
{
    int i;
    //struct bio_vec *bvec;

    if(bio){
        // if(bio_data_dir(bio) == WRITE){
        //     printk(KERN_INFO "imrsim: release pages.\n");
        //     bio_for_each_segment_all(bvec, bio, i)
        //         __free_page(bvec->bv_page);
        // }
        // if(bio_data_dir(bio) == READ){
        //     printk(KERN_INFO "imrsim: read bio end.\n");
        // }
        complete((struct completion *)bio->bi_private);   // rmw 同步控制。发送完成量，唤醒一个等待
        bio_put(bio);
    }
}

/* rmw task - sub thread*/
/*rmw 任务 -子线程 */
int read_modify_write_task(void *arg)
{
    
    struct dm_target * ti = (struct dm_target *)arg;
    struct imrsim_c *c = ti->private;
    __u8 i;
    __u8 n = imrsim_rmw_task.lba_num;//从mrsim_write_rule_check函数接收imrsim_rmw_task.lba_num
    struct page *pages[2];
    void  *page_addrs[2];

    if(imrsim_rmw_task.bio)
    {
        printk(KERN_INFO "imrsim: enter rmw process and back up\n");
        // read the blocks needed to back up  读取需要备份块
        for(i=0; i<n; i++)
        {
            pages[i] = alloc_page(GFP_KERNEL);  //alloc_page(GFP_KERNEL,3) 分配2^3个物理页
            if(!pages[i]){
                printk(KERN_ERR "imrsim: no enough memory to allocate a page\n");
                return -ENOMEM;
            }
            page_addrs[i] = page_address(pages[i]);
            if(!page_addrs[i]){
                printk(KERN_ERR "imrsim: read page vm addr null\n");
                __free_page(pages[i]);
            }
            //printk(KERN_INFO "imrsim: page_addr:0x%lx\n", page_addrs[i]);
            memset(page_addrs[i], 0, PAGE_SIZE);
            struct bio *rbio = bio_alloc(GFP_NOIO, 1);
            init_completion(&imrsim_completion.read_event);
            rbio->bi_private = &imrsim_completion.read_event;
            rbio->bi_bdev = c->dev->bdev;
            rbio->bi_iter.bi_sector = imrsim_map_sector(ti, imrsim_rmw_task.lba[i]);//根据相邻顶部磁道的位置映射rmw位置
            rbio->bi_end_io = imrsim_end_rmw;
            bio_add_page(rbio, pages[i], PAGE_SIZE, 0);
            submit_bio(READ | REQ_SYNC, rbio);
            wait_for_completion(&imrsim_completion.read_event);
            cond_resched();
        }

        printk(KERN_INFO "imrsim: write bio.\n");
        // write current bio  写当前bio
        submit_bio(WRITE_FUA, imrsim_rmw_task.bio);
        cond_resched();

        printk(KERN_INFO "imrsim: write back.\n");
        // write back  回写。
        //如果修改rmw策略为mom，则写回位置应该通过计算寻找一个较为cold磁道中的位置。
        //即imrsim_rmw_task.lba[i])应该重新通过算法计算得到。
        for(i=0; i<n; i++)
        {
            struct bio *wbio = bio_alloc(GFP_NOIO, 1);
            init_completion(&imrsim_completion.write_event);
            wbio->bi_private = &imrsim_completion.write_event;
            wbio->bi_bdev = c->dev->bdev;
            wbio->bi_iter.bi_sector = imrsim_map_sector(ti, imrsim_rmw_task.lba[i]);
            wbio->bi_end_io = imrsim_end_rmw;
            bio_add_page(wbio, pages[i], PAGE_SIZE, 0);
            submit_bio(WRITE_FUA, wbio);
            wait_for_completion(&imrsim_completion.write_event);
            cond_resched();
        }

        // release pages  释放页
        for(i=0; i<n; i++)
        {
            __free_page(pages[i]);
        }
        printk(KERN_INFO "imrsim: release pages.\n");
        imrsim_rmw_task.lba_num = 0;
        complete(&imrsim_completion.rmw_event);   // rmw completion release
    }
    return 0;
}

/* RMW event caused by update to bottom track  底部磁道更新引起的RMW事件*/
void imrsim_rmw_thread(struct dm_target *ti)
{
    imrsim_rmw_task.task = kthread_create(read_modify_write_task, ti, "rmw thread");
    if(imrsim_rmw_task.task){
        printk(KERN_INFO "imrsim: rmw thread created : %d.\n", imrsim_rmw_task.task->pid);
        init_completion(&imrsim_completion.rmw_event);  //初始化一个信号量
        wake_up_process(imrsim_rmw_task.task);  //唤醒处于睡眠状态的进程
        wait_for_completion(&imrsim_completion.rmw_event);  //等待信号量的释放
        //kthread_stop(imrsim_rmw_task.task);
        printk(KERN_INFO "imrsim: rmw task end.\n");
    }
}


/* To get page number and next page. */
/*获取页码和下一页*/
static __u32 imrsim_pstore_pg_idx(__u32 idx, __u32 *pg_nxt)
{
    __u32 tmp = IMR_PSTORE_PG_OFF + sizeof(struct imrsim_zone_stats)*idx;
    __u32 pg_cur = tmp / PAGE_SIZE;

    *pg_nxt = tmp % PAGE_SIZE ? pg_cur+1 : pg_cur;
    return pg_cur;
}

/* Persistent storage - handled in a variety of cases depending on the type of metadata change.*/
/*持久存储 -根据元数据更改的类型在各种情况下进行处理。*/
static int imrsim_flush_persistence(struct dm_target *ti)//元数据同步磁盘
{
    void             *page_addr;
    struct page      *page;
    struct imrsim_c  *zdev;
    __u32            idx;
    __u32            crc;    
    __u32            pg_cur;
    __u32            pg_nxt;
    __u32            qidx;

    zdev = ti->private;
    page = alloc_pages(GFP_KERNEL, 0);  //内存分配物理页面alloc_pages(gfp, order)，2的order次。GFP_KERNEL为掩码
    if(!page){
        printk(KERN_ERR "imrsim: no enough memory to allocate a page\n");
        return -ENOMEM;
    }
    page_addr = page_address(page);
    if(!page_addr){
        printk(KERN_ERR "imrsim: write page vm addr null\n");
        __free_pages(page, 0);
        return -EINVAL;
    }
    crc = crc32(0, (unsigned char *)zone_state + sizeof(struct imrsim_state_header),//生成str的32位循环冗余校验码多项式。这通常用于检查传输的数据是否完整。
                zone_state->header.length - sizeof(struct imrsim_state_header));
    zone_state->header.crc32 = crc;
    if(imrsim_ptask.flag &= IMR_CONFIG_CHANGE){
        imrsim_ptask.flag &= ~IMR_CONFIG_CHANGE;
    }
    memcpy(page_addr, (unsigned char *)zone_state, PAGE_SIZE);
    imrsim_write_page(zdev->dev->bdev, imrsim_ptask.pstore_lba, PAGE_SIZE, page);

    /* Handling of Disk Statistics Changes. */
    if(imrsim_ptask.flag &= IMR_STATS_CHANGE){
        imrsim_ptask.flag &= ~IMR_STATS_CHANGE;
        if(imrsim_ptask.sts_zone_idx > IMR_PSTORE_PG_EDG){
            pg_cur = imrsim_pstore_pg_idx(imrsim_ptask.sts_zone_idx, &pg_nxt);
            imrsim_ptask.sts_zone_idx = 0;
            for(idx = pg_cur; idx <= pg_nxt; idx++){
                memcpy(page_addr, ((unsigned char *)zone_state + idx * PAGE_SIZE), PAGE_SIZE);
                imrsim_write_page(zdev->dev->bdev, imrsim_ptask.pstore_lba + 
                                (idx << IMR_PAGE_SIZE_SHIFT_DEFAULT),
                                PAGE_SIZE, page);
            }
        }
    }

    /* Handling of disk state changes. */
    if(imrsim_ptask.flag &= IMR_STATUS_CHANGE){
        imrsim_ptask.flag &= ~IMR_STATUS_CHANGE;
        for(qidx = 0; qidx < imrsim_ptask.stu_zone_idx_cnt; qidx++){
            pg_cur = imrsim_pstore_pg_idx(imrsim_ptask.stu_zone_idx[qidx], &pg_nxt);
            imrsim_ptask.stu_zone_idx[qidx] = 0;
            for(idx = pg_cur; idx <= pg_nxt; idx++){
                memcpy(page_addr, ((unsigned char *)zone_state + idx * PAGE_SIZE), PAGE_SIZE);
                imrsim_write_page(zdev->dev->bdev, imrsim_ptask.pstore_lba + 
                                (idx << IMR_PAGE_SIZE_SHIFT_DEFAULT),
                                PAGE_SIZE, page);
            }
        }
        imrsim_ptask.stu_zone_idx_cnt = 0;
        imrsim_ptask.stu_zone_idx_gap = 0;
    }

    if(imrsim_dbg_log_enabled && printk_ratelimit()){
        printk(KERN_ERR "imrsim: flush persist success\n");
    }
    __free_pages(page, 0);
    return 0;
}

/* To persist meta-data. */
/*持久化元数据*/
static int imrsim_save_persistence(struct dm_target *ti)
{
    void             *page_addr;
    struct page      *page;
    struct imrsim_c  *zdev;
    __u32            num_pages;
    __u32            part_page;
    __u32            idx;
    __u32            crc;

    zdev = ti->private;
    page = alloc_pages(GFP_KERNEL, 0);
    if(!page){
        printk(KERN_ERR "imrsim: no enough memory to allocate a page\n");
        return -ENOMEM;
    }
    page_addr = page_address(page);
    if(!page_addr){
        printk(KERN_ERR "imrsim: write page vm addr null\n");
        __free_pages(page, 0);
        return -EINVAL;
    }
    num_pages = div_u64_rem(zone_state->header.length, PAGE_SIZE, &part_page);
    crc = crc32(0, (unsigned char *)zone_state + sizeof(struct imrsim_state_header),
                zone_state->header.length - sizeof(struct imrsim_state_header));
    zone_state->header.crc32 = crc;
    for(idx = 0; idx < num_pages; idx++){
        memcpy(page_addr, ((unsigned char *)zone_state + 
               idx * PAGE_SIZE), PAGE_SIZE);
        imrsim_write_page(zdev->dev->bdev, imrsim_ptask.pstore_lba + 
                         (idx << IMR_PAGE_SIZE_SHIFT_DEFAULT), PAGE_SIZE, page);
    }
    if(part_page){
        memcpy(page_addr, ((unsigned char *)zone_state + 
              num_pages * PAGE_SIZE), part_page);
        imrsim_write_page(zdev->dev->bdev, imrsim_ptask.pstore_lba + 
                          (num_pages << IMR_PAGE_SIZE_SHIFT_DEFAULT), PAGE_SIZE, page);
    }
    if(imrsim_dbg_log_enabled && printk_ratelimit()){
        printk(KERN_INFO "imrsim: save persist success\n");
    }
    __free_pages(page, 0);
    return 0;
}

/* To load metadata from persistent storage. */
static int imrsim_load_persistence(struct dm_target *ti)
{
    __u64            sizedev;
    void             *page_addr;
    struct page      *page;
    struct imrsim_c  *zdev;
    __u32            num_pages;
    __u32            part_page;     
    __u32            idx;
    __u32            crc;
    struct imrsim_state_header header;

    printk(KERN_INFO "imrsim: load persistence\n");

    zdev = ti->private;
    sizedev = ti->len;
    imrsim_init_zone_default(sizedev);
    /* The starting address for persistent storage. */
    imrsim_ptask.pstore_lba = IMR_NUMZONES_DEFAULT      //元数据的起始地址，元数据包括磁盘统计信息和zone状态信息
                              << IMR_ZONE_SIZE_SHIFT_DEFAULT
                              << IMR_BLOCK_SIZE_SHIFT_DEFAULT;
    page = alloc_pages(GFP_KERNEL, 0);
    if(!page){
        printk(KERN_ERR "imrsim: no enough memory to allocate a page\n");
        goto pgerr;
    }
    page_addr = page_address(page);
    if(!page_addr){
        printk(KERN_ERR "imrsim: read page vm addr null\n");
        goto rderr;
    }
    memset(page_addr, 0, PAGE_SIZE);
    imrsim_read_page(zdev->dev->bdev, imrsim_ptask.pstore_lba, PAGE_SIZE, page);
    memcpy(&header, page_addr, sizeof(struct imrsim_state_header));
    if(header.magic == 0xBEEFBEEF){
        zone_state = vzalloc(header.length);  //vzalloc将申请到连续物理内存数据置为0
        if(!zone_state){
            printk(KERN_ERR "imrsim: zone_state error: no enough memory\n");
            goto rderr;
        }
        num_pages = div_u64_rem(header.length, PAGE_SIZE, &part_page);
        if(num_pages){
            memcpy((unsigned char *)zone_state, page_addr, PAGE_SIZE);  // load header
        }
        for(idx = 1; idx < num_pages; idx++){
            memset(page_addr, 0, PAGE_SIZE);
            imrsim_read_page(zdev->dev->bdev, imrsim_ptask.pstore_lba + 
                            (idx << IMR_PAGE_SIZE_SHIFT_DEFAULT), PAGE_SIZE, page);
            memcpy(((unsigned char *)zone_state + 
                  idx * PAGE_SIZE), page_addr, PAGE_SIZE);
        }
        if(part_page){
            if(num_pages){
                memset(page_addr, 0, PAGE_SIZE);
                imrsim_read_page(zdev->dev->bdev, imrsim_ptask.pstore_lba + 
                                (num_pages << IMR_PAGE_SIZE_SHIFT_DEFAULT), 
                                PAGE_SIZE, page);
            }
            memcpy(((unsigned char *)zone_state + 
                   idx * PAGE_SIZE), page_addr, part_page);
        }
        crc = crc32(0, (unsigned char *)zone_state + sizeof(struct imrsim_state_header), 
                   zone_state->header.length - sizeof(struct imrsim_state_header));
        if(crc != zone_state->header.crc32){
            printk(KERN_ERR "imrsim: error: crc checking. apply default config ...\n");
            goto rderr;
        }
        IMR_NUMZONES = zone_state->stats.num_zones;
        zone_status = (struct imrsim_zone_status *)&zone_state->stats.zone_stats[IMR_NUMZONES];
        IMR_ZONE_SIZE_SHIFT = index_power_of_2(zone_status[0].z_length >> IMR_BLOCK_SIZE_SHIFT);
        printk(KERN_INFO "imrsim: load persist success\n");
    }else{
        printk(KERN_ERR "imrsim: load persistence magic doesn't match. Setup the default\n");
        goto rderr;
    }
    __free_pages(page, 0);
    return 0;
    rderr:
        __free_pages(page, 0);
    pgerr:
        imrsim_init_zone_state(sizedev);
    return -EINVAL;
}

/* persistent storage task */
static int imrsim_persistence_task(void *arg)
{
    struct dm_target *ti = (struct dm_target *)arg;

    while(!kthread_should_stop()){
        if(imrsim_ptask.flag){
            mutex_lock(&imrsim_zone_lock);
            if(imrsim_ptask.flag & IMR_CONFIG_CHANGE){
                if(IMR_NUMZONES == 0){
                    imrsim_ptask.flag &= IMR_NO_CHANGE;
                }else{
                    imrsim_save_persistence(ti);
                    imrsim_ptask.flag &= IMR_NO_CHANGE;
                }
            }else{
                if(imrsim_ptask.stu_zone_idx_gap >= IMR_PSTORE_PG_GAP){
                    imrsim_save_persistence(ti);
                    imrsim_ptask.flag &= IMR_NO_CHANGE;
                    imrsim_ptask.stu_zone_idx_gap = 0;
                    memset(imrsim_ptask.stu_zone_idx, 0, sizeof(__u32) * IMR_PSTORE_QDEPTH);
                    imrsim_ptask.stu_zone_idx_cnt = 0;
                }else{
                    imrsim_flush_persistence(ti);
                }
            }
            mutex_unlock(&imrsim_zone_lock);
        }
        msleep_interruptible(IMR_PSTORE_CHECK);
    }
    return 0;
}

/* persistent storage thread */
static int imrsim_persistence_thread(struct dm_target *ti)
{
    int ret = 0;

    if(!ti){
        printk(KERN_ERR "imrsim: warning: null device target. Improper usage\n");
        return -EINVAL;
    }
    imrsim_ptask.flag = 0;
    imrsim_ptask.stu_zone_idx_cnt = 0;
    imrsim_ptask.stu_zone_idx_gap = 0;
    memset(imrsim_ptask.stu_zone_idx, 0, sizeof(__u32) * IMR_PSTORE_QDEPTH);
    ret = imrsim_load_persistence(ti);
    if(ret){
        imrsim_save_persistence(ti);
    }
    // create thread
    imrsim_ptask.pstore_thread = kthread_create(imrsim_persistence_task, 
                                                ti, "imrsim pthread");
    if(imrsim_ptask.pstore_thread){
        printk(KERN_INFO "imrsim persistence thread created\n");
        // After a thread is created with kthread_create, the thread will not start immediately, 
		// but needs to be started after calling the wake_up_process function.
        wake_up_process(imrsim_ptask.pstore_thread);
    }else{
        printk(KERN_ERR "imrsim persistence thread create failed\n");
        return -EAGAIN;
    }
    return 0;
}

/* To update device idle time. */
/*更新设备空闲时间*/
static void imrsim_dev_idle_update(void)
{
    __u32 dt = 0;
    if(jiffies > imrsim_dev_idle_checkpoint){            //  Jiffies记录系统自开机以来，已经过了多少tick。
        dt = (jiffies - imrsim_dev_idle_checkpoint) / HZ;//每发生一次timer interrupt，Jiffies变数会被加一。
    }else{
        dt = (~(__u32)0 - imrsim_dev_idle_checkpoint + jiffies) / HZ;
    }
    if (dt > zone_state->stats.dev_stats.idle_stats.dev_idle_time_max) {
      zone_state->stats.dev_stats.idle_stats.dev_idle_time_max = dt;
   } else if (dt && (dt < zone_state->stats.dev_stats.idle_stats.dev_idle_time_min)) {
      zone_state->stats.dev_stats.idle_stats.dev_idle_time_min = dt;
   }
}

/* status report */
static void imrsim_report_stats(struct imrsim_stats *stats)
{
    __u32 i;
    __u32 num32 = stats->num_zones;

    if (!stats) {
       printk(KERN_ERR "imrsim: NULL pointer passed through\n");
       return;
    }
    printk("Device idle time max: %u\n",
            stats->dev_stats.idle_stats.dev_idle_time_max);
    printk("Device idle time min: %u\n",
            stats->dev_stats.idle_stats.dev_idle_time_min);
    for (i = 0; i < num32; i++) {
        printk("zone[%u] imrsim out of policy read stats: span zones count: %u\n",
                    i, stats->zone_stats[i].out_of_policy_read_stats.span_zones_count);
        printk("zone[%u] imrsim out of policy write stats: span zones count: %u\n",
                    i, stats->zone_stats[i].out_of_policy_write_stats.span_zones_count);
        printk("zone[%u] imrsim out of policy write stats: unaligned count: %u\n",
                    i, stats->zone_stats[i].out_of_policy_write_stats.unaligned_count);
        printk("zone[%u] extra write count: %u\n",
                    i, stats->zone_stats[i].z_extra_write_total);    
        printk("zone[%u] write total count: %u\n",
                    i, stats->zone_stats[i].z_write_total); 
    }

    printk("imrsim extra write total count: %llu\n", stats->extra_write_total);
    printk("imrsim write total count: %llu\n", stats->write_total);
}

/* The following are interface methods with EXPORT_SYMBOL. */

/* To get the last read error. */
int imrsim_get_last_rd_error(__u32 *last_error)
{
    __u32 tmperr = imrsim_dbg_rerr;

    imrsim_dbg_rerr = 0;
    if(last_error){
        *last_error = tmperr;
    }
    return 0;
}
EXPORT_SYMBOL(imrsim_get_last_rd_error);

/* To get the last write error. */
int imrsim_get_last_wd_error(__u32 *last_error)
{
   __u32 tmperr = imrsim_dbg_werr;

   imrsim_dbg_werr  = 0;
   if(last_error)
      *last_error = tmperr;
   return 0;
}
EXPORT_SYMBOL(imrsim_get_last_wd_error);

/* Enable logging. */
int imrsim_set_log_enable(__u32 zero_is_disable)
{
   imrsim_dbg_log_enabled = zero_is_disable;
   return 0;
}
EXPORT_SYMBOL(imrsim_set_log_enable);

/* Disable logging. */
int imrsim_get_num_zones(__u32* num_zones)
{
   printk(KERN_INFO "imrsim: %s: called.\n", __FUNCTION__);
   if (!num_zones) {
      printk(KERN_ERR "imrsim: NULL pointer passed through\n");
      return -EINVAL;
   }
   mutex_lock(&imrsim_zone_lock);
   *num_zones = IMR_NUMZONES;
   mutex_unlock(&imrsim_zone_lock);
   return 0;
}
EXPORT_SYMBOL(imrsim_get_num_zones);//使用EXPORT_SYMBOL可以将一个函数以符号的方式导出给其他模块使用

/* To get the number of sectors in a zone. */
/*计算一个zone有多少扇区*/
int imrsim_get_size_zone_default(__u32 *size_zone)
{
    printk(KERN_INFO "imrsim: %s called.\n", __FUNCTION__);
    if(!size_zone){
        printk(KERN_ERR "imrsim: NULL pointer passed through\n");
        return -EINVAL;
    }
    mutex_lock(&imrsim_zone_lock);
    *size_zone = num_sectors_zone();
    mutex_unlock(&imrsim_zone_lock);
    return 0;
}
EXPORT_SYMBOL(imrsim_get_size_zone_default);//使用EXPORT_SYMBOL可以将一个函数以符号的方式导出给其他模块使用

/* To set the default zone size. */
int imrsim_set_size_zone_default(__u32 size_zone)
{
    struct imrsim_state *sta_tmp;

    printk(KERN_INFO "imrsim: %s called.\n", __FUNCTION__);
    if((size_zone % (1 << IMR_BLOCK_SIZE_SHIFT)) || !(is_power_of_2(size_zone))){
        printk(KERN_ERR "imrsim: Wrong zone size specified\n");
        return -EINVAL;
    }
    mutex_lock(&imrsim_zone_lock);
    IMR_ZONE_SIZE_SHIFT = index_power_of_2((size_zone) >> IMR_BLOCK_SIZE_SHIFT);
    IMR_NUMZONES = ((IMR_CAPACITY >> IMR_BLOCK_SIZE_SHIFT) >> IMR_ZONE_SIZE_SHIFT);
    sta_tmp = vzalloc(imrsim_state_size());
    if(!sta_tmp){
        mutex_unlock(&imrsim_zone_lock);
        printk(KERN_ERR "imrsim: zone_state memory realloc failed\n");
        return -EINVAL;
    }
    vfree(zone_state);
    zone_state = sta_tmp;
    imrsim_init_zone_state_default(imrsim_state_size());
    mutex_unlock(&imrsim_zone_lock);
    return 0;
}
EXPORT_SYMBOL(imrsim_set_size_zone_default);//使用EXPORT_SYMBOL可以将一个函数以符号的方式导出给其他模块使用

/* To reset default config. */
int imrsim_reset_default_config(void)
{
    printk(KERN_INFO "imrsim: %s called.\n", __FUNCTION__);
    imrsim_reset_default_zone_config();
    imrsim_reset_default_device_config();
    return 0;
}
EXPORT_SYMBOL(imrsim_reset_default_config);//使用EXPORT_SYMBOL可以将一个函数以符号的方式导出给其他模块使用

/* To reset default device config. */
int imrsim_reset_default_device_config(void)
{
    printk(KERN_INFO "imrsim: %s called.\n", __FUNCTION__);
    mutex_lock(&imrsim_zone_lock);
    zone_state->config.dev_config.out_of_policy_read_flag = 0;
    zone_state->config.dev_config.out_of_policy_write_flag = 0;
    zone_state->config.dev_config.r_time_to_rmw_zone = IMR_TRANSFER_PENALTY;
    zone_state->config.dev_config.w_time_to_rmw_zone = IMR_TRANSFER_PENALTY;
    mutex_unlock(&imrsim_zone_lock);
    return 0;
}
EXPORT_SYMBOL(imrsim_reset_default_device_config);//使用EXPORT_SYMBOL可以将一个函数以符号的方式导出给其他模块使用

/* To get device config. */
int imrsim_get_device_config(struct imrsim_dev_config *device_config)
{
    printk(KERN_INFO "imrsim: %s called.\n", __FUNCTION__);
    if(!device_config){
        printk(KERN_ERR "imrsim: NULL pointer passed through\n");
        return -EINVAL;
    }
    mutex_lock(&imrsim_zone_lock);
    memcpy(device_config, &(zone_state->config.dev_config), 
           sizeof(struct imrsim_dev_config));
    mutex_unlock(&imrsim_zone_lock);
    return 0;
}
EXPORT_SYMBOL(imrsim_get_device_config);//使用EXPORT_SYMBOL可以将一个函数以符号的方式导出给其他模块使用

/* To set device read config. */
int imrsim_set_device_rconfig(struct imrsim_dev_config *device_config)
{
    printk(KERN_INFO "imrsim: %s called.\n", __FUNCTION__);
    if(!device_config){
        printk(KERN_ERR "imrsim: NULL pointer passed through\n");
        return -EINVAL;
    }
    mutex_lock(&imrsim_zone_lock);
    zone_state->config.dev_config.out_of_policy_read_flag = 
        device_config->out_of_policy_read_flag;
    mutex_unlock(&imrsim_zone_lock);
    return 0;
}
EXPORT_SYMBOL(imrsim_set_device_rconfig);//使用EXPORT_SYMBOL可以将一个函数以符号的方式导出给其他模块使用

/* To set device write config. */
int imrsim_set_device_wconfig(struct imrsim_dev_config *device_config)
{
    printk(KERN_INFO "imrsim: %s called.\n", __FUNCTION__);
    if(!device_config){
        printk(KERN_ERR "imrsim: NULL pointer passed through\n");
        return -EINVAL;
    }
    mutex_lock(&imrsim_zone_lock);
    zone_state->config.dev_config.out_of_policy_write_flag = 
        device_config->out_of_policy_write_flag;
    mutex_unlock(&imrsim_zone_lock);
    return 0;
}
EXPORT_SYMBOL(imrsim_set_device_wconfig);//使用EXPORT_SYMBOL可以将一个函数以符号的方式导出给其他模块使用

/* To set read delay. */
int imrsim_set_device_rconfig_delay(struct imrsim_dev_config *device_config)
{
    printk(KERN_INFO "imrsim: %s called.\n", __FUNCTION__);
    if(!device_config){
        printk(KERN_ERR "imrsim: NULL pointer passed through\n");
        return -EINVAL;
    }
    if(device_config->r_time_to_rmw_zone >= IMR_TRANSFER_PENALTY_MAX){
        printk(KERN_ERR "time delay exceeds default maximum\n");
        return -EINVAL;
    }
    mutex_lock(&imrsim_zone_lock);
    zone_state->config.dev_config.r_time_to_rmw_zone = 
        device_config->r_time_to_rmw_zone;
    mutex_unlock(&imrsim_zone_lock);
    return 0;
}
EXPORT_SYMBOL(imrsim_set_device_rconfig_delay);//使用EXPORT_SYMBOL可以将一个函数以符号的方式导出给其他模块使用

/* To set write delay. */
int imrsim_set_device_wconfig_delay(struct imrsim_dev_config *device_config)
{
    printk(KERN_INFO "imrsim: %s called.\n", __FUNCTION__);
    if(!device_config){
        printk(KERN_ERR "imrsim: NULL pointer passed through\n");
        return -EINVAL;
    }
    if(device_config->w_time_to_rmw_zone >= IMR_TRANSFER_PENALTY_MAX){
        printk(KERN_ERR "time delay exceeds default maximum\n");
        return -EINVAL;
    }
    mutex_lock(&imrsim_zone_lock);
    zone_state->config.dev_config.w_time_to_rmw_zone = 
        device_config->w_time_to_rmw_zone;
    mutex_unlock(&imrsim_zone_lock);
    return 0;
}
EXPORT_SYMBOL(imrsim_set_device_wconfig_delay);//使用EXPORT_SYMBOL可以将一个函数以符号的方式导出给其他模块使用

/* To reset default zone config. */
int imrsim_reset_default_zone_config(void)
{
    struct imrsim_state *sta_tmp;

    printk(KERN_INFO "imrsim: %s called.\n", __FUNCTION__);
    mutex_lock(&imrsim_zone_lock);
    IMR_NUMZONES = IMR_NUMZONES_DEFAULT;
    IMR_ZONE_SIZE_SHIFT = IMR_ZONE_SIZE_SHIFT_DEFAULT;
    sta_tmp = vzalloc(imrsim_state_size());
    vfree(zone_state);
    if(!sta_tmp){
        printk(KERN_ERR "imrsim: zone_state memory realloc failed\n");
        return -EINVAL;
    }
    zone_state = sta_tmp;
    imrsim_init_zone_state_default(imrsim_state_size());
    mutex_unlock(&imrsim_zone_lock);
    return 0;
}
EXPORT_SYMBOL(imrsim_reset_default_zone_config);//使用EXPORT_SYMBOL可以将一个函数以符号的方式导出给其他模块使用

/* To clear config of a zone. */
int imrsim_clear_zone_config(void)
{
    printk(KERN_INFO "imrsim: %s called.\n", __FUNCTION__);
    memset(zone_state->stats.zone_stats, 0, 
       zone_state->stats.num_zones * sizeof(struct imrsim_zone_stats));
    mutex_lock(&imrsim_zone_lock);
    zone_state->stats.num_zones = 0;
    memset(zone_status, 0, IMR_NUMZONES * sizeof(struct imrsim_zone_status));
    IMR_NUMZONES = 0;
    mutex_unlock(&imrsim_zone_lock);
    return 0;
}
EXPORT_SYMBOL(imrsim_clear_zone_config);//使用EXPORT_SYMBOL可以将一个函数以符号的方式导出给其他模块使用

/* Count the number of Z_TYPE_SEQUENTIAL type zones. @Deprecated */
static int imrsim_zone_seq_count(void)
{
    __u32 count = 0;
    __u32 index;

    for(index = 0; index < IMR_NUMZONES; index++){
        if(zone_status[index].z_type == Z_TYPE_SEQUENTIAL){
            count++;
        }
    }
    return count;
}

/* To check if the zone status is correct. */
static int imrsim_zone_cond_check(__u16 cond)
{
    switch(cond){
        case Z_COND_NO_WP:
        case Z_COND_EMPTY:
        case Z_COND_CLOSED:
        case Z_COND_RO:
        case Z_COND_FULL:
        case Z_COND_OFFLINE:
            return 1;
        default:
            return 0;
    }
    return 0;
}

/* To modify zone configuration. @Deprecated */
int imrsim_modify_zone_config(struct imrsim_zone_status *z_status)
{
    __u32 count = imrsim_zone_seq_count();

    printk(KERN_INFO "imrsim: %s called.\n", __FUNCTION__); 
    if(!z_status){
        printk(KERN_ERR "imrsim: NULL pointer passed through\n");
        return -EINVAL;
    }
    if(IMR_NUMZONES <= z_status->z_start){
        printk(KERN_ERR "imrsim: config does not exist\n");
        return -EINVAL;
    }
    if(1 >= count && (Z_TYPE_SEQUENTIAL == z_status->z_type) &&
      (Z_TYPE_SEQUENTIAL == zone_status[z_status->z_start].z_type))
    {
          printk(KERN_ERR "imrsim: zone type is not allowed to modify\n");
          return -EINVAL;
    }
    if(z_status->z_length != num_sectors_zone()){
        printk(KERN_ERR "imrsim: zone size is not allowed to change individually\n");
        return -EINVAL;
    }
    if(!imrsim_zone_cond_check(z_status->z_conds)){
        printk(KERN_ERR "imrsim: wrong zone condition\n");
        return -EINVAL;
    }
    if((z_status->z_conds == Z_COND_NO_WP) && 
        (z_status->z_type != Z_TYPE_CONVENTIONAL))
    {
        printk(KERN_ERR "imrsim: condition and type mismatch\n");
        return -EINVAL;
    }
    if ((Z_COND_EMPTY == z_status->z_conds) && 
       (Z_TYPE_SEQUENTIAL == z_status->z_type) ) {
      printk(KERN_ERR "imrsim: empty zone isn't empty\n");
      return -EINVAL;
    }

    mutex_lock(&imrsim_zone_lock);
    zone_status[z_status->z_start].z_conds = 
      (enum imrsim_zone_conditions)z_status->z_conds;
    zone_status[z_status->z_start].z_type = 
      (enum imrsim_zone_type)z_status->z_type;
    zone_status[z_status->z_start].z_flag = 0;
    mutex_unlock(&imrsim_zone_lock);
    printk(KERN_DEBUG "imrsim: zone[%lu] modified. type:0x%x conds:0x%x\n",
      zone_status[z_status->z_start].z_start,
      zone_status[z_status->z_start].z_type, 
      zone_status[z_status->z_start].z_conds);
    return 0;
}
EXPORT_SYMBOL(imrsim_modify_zone_config);////使用EXPORT_SYMBOL可以将一个函数以符号的方式导出给其他模块使用

/* To add zone configuration. @Deprecated */
int imrsim_add_zone_config(struct imrsim_zone_status *zone_sts)
{
    printk(KERN_INFO "imrsim: %s called.\n", __FUNCTION__);
    if(!zone_sts){
        printk(KERN_ERR "imrsim: NULL pointer passed through\n");
        return -EINVAL;
    }
    if(zone_sts->z_start >= IMR_NUMZONES_DEFAULT){
        printk(KERN_ERR "imrsim: zone config start lba is out of range\n");
        return -EINVAL;
    }
    if(zone_sts->z_start != IMR_NUMZONES){
        printk(KERN_ERR "imrsim: zone config does not start at the end of current zone\n");
        printk(KERN_INFO "imrsim: z_start: %u  IMR_NUMZONES: %u\n", (__u32)zone_sts->z_start,
             IMR_NUMZONES);
        return -EINVAL;
    }
    if ((zone_sts->z_type != Z_TYPE_CONVENTIONAL) && (zone_sts->z_type != Z_TYPE_SEQUENTIAL)) {
      printk(KERN_ERR "imrsim: zone config type is not allowed with current config\n");
      return -EINVAL;
   }
   if ((zone_sts->z_type == Z_TYPE_CONVENTIONAL) && (zone_sts->z_conds != Z_COND_NO_WP)) {
      printk(KERN_ERR "imrsim: zone config condition is wrong. Need to be NO WP\n");
      return -EINVAL;
   }
   if ((zone_sts->z_type == Z_TYPE_SEQUENTIAL) && (zone_sts->z_conds != Z_COND_EMPTY)) {
      printk(KERN_ERR "imrsim: zone config condition is wrong. Need to be EMPTY\n");
      return -EINVAL;
   }
   if (zone_sts->z_length != (1 << IMR_ZONE_SIZE_SHIFT << IMR_BLOCK_SIZE_SHIFT)) {
      printk(KERN_ERR "imrsim: zone config size is not allowed with current config\n");
      return -EINVAL;
   }
   zone_sts->z_flag = 0;
   mutex_lock(&imrsim_zone_lock);
   memcpy(&(zone_status[IMR_NUMZONES]), zone_sts, sizeof(struct imrsim_zone_status));
   zone_state->stats.num_zones++;
   IMR_NUMZONES++;
   mutex_unlock(&imrsim_zone_lock);
   return 0;
}
EXPORT_SYMBOL(imrsim_add_zone_config);////使用EXPORT_SYMBOL可以将一个函数以符号的方式导出给其他模块使用

/* To reset statistics for a zone. */
/*重置zone的统计信息。*/
int imrsim_reset_zone_stats(sector_t start_sector)
{
    __u32 zone_idx = start_sector >> IMR_BLOCK_SIZE_SHIFT >> IMR_ZONE_SIZE_SHIFT;   //lba所在的zone编号zone_idx
                                                                                    //除以一个zone的块数量以及一个块的扇区数量就可以获得
    printk(KERN_INFO "imrsim: %s called.\n", __FUNCTION__);
    if(IMR_NUMZONES <= zone_idx){
        printk(KERN_ERR "imrsim: %s start sector is out of range\n", __FUNCTION__);
        return -EINVAL;
    }
    memset(&(zone_state->stats.zone_stats[zone_idx].out_of_policy_read_stats),
          0, sizeof(struct imrsim_out_of_policy_read_stats));
    memset(&(zone_state->stats.zone_stats[zone_idx].out_of_policy_write_stats),
          0, sizeof(struct imrsim_out_of_policy_write_stats));
    memset(&(zone_state->stats.zone_stats[zone_idx].z_extra_write_total),
          0, sizeof(__u32));
    memset(&(zone_state->stats.zone_stats[zone_idx].z_write_total),
          0, sizeof(__u32));
    return 0;
}
EXPORT_SYMBOL(imrsim_reset_zone_stats);  //使用EXPORT_SYMBOL可以将一个函数以符号的方式导出给其他模块使用

/* To reset zone_stats. */
/*重置zone_stats*/
int imrsim_reset_stats(void)
{
    printk(KERN_INFO "imrsim: %s: called.\n", __FUNCTION__);
    memset(&zone_state->stats.dev_stats.idle_stats, 0, sizeof(struct imrsim_idle_stats));
    memset(&zone_state->stats.extra_write_total, 0, sizeof(__u64));
    memset(&zone_state->stats.write_total, 0, sizeof(__u64)); //memset(void *s, int ch, size_t n) 
    memset(zone_state->stats.zone_stats, 0, zone_state->stats.num_zones * //将s中当前位置后面的n个字节 （typedef unsigned int size_t ）
          sizeof(struct imrsim_zone_stats));                              //用 ch 替换并返回 s。对结构体或数组清零最快的方法
    return 0;
}
EXPORT_SYMBOL(imrsim_reset_stats);  //使用EXPORT_SYMBOL可以将一个函数以符号的方式导出给其他模块使用

/* To get zone_stats. */
int imrsim_get_stats(struct imrsim_stats *stats)
{
    printk(KERN_INFO "imrsim: %s called.\n", __FUNCTION__);
    if(!stats){
        printk(KERN_ERR "imrsim: NULL pointer passed through\n");
        return -EINVAL;
    }
    memcpy(stats, &(zone_state->stats), imrsim_stats_size()); //拷贝函数，将zone_state->stats拷贝到stats
    return 0;
}
EXPORT_SYMBOL(imrsim_get_stats); //使用EXPORT_SYMBOL可以将一个函数以符号的方式导出给其他模块使用

/* @Deprecated */
int imrsim_blkdev_reset_zone_ptr(sector_t start_sector)
{
    //__u32 rem;
    __u32 zone_idx;

    printk(KERN_INFO "imrsim: %s called.\n", __FUNCTION__);
    mutex_lock(&imrsim_zone_lock);
    zone_idx = start_sector >> IMR_BLOCK_SIZE_SHIFT >> IMR_ZONE_SIZE_SHIFT;
    if(IMR_NUMZONES <= zone_idx){
        mutex_unlock(&imrsim_zone_lock);
        printk(KERN_ERR "imrsim: %s start_sector is out of range\n", __FUNCTION__);
        return -EINVAL;
    }
    if (zone_status[zone_idx].z_type == Z_TYPE_CONVENTIONAL) {
      mutex_unlock(&imrsim_zone_lock);
      printk(KERN_ERR "imrsim:error: CMR zone dosen't have a write pointer.\n");
      return -EINVAL;
    }
    mutex_unlock(&imrsim_zone_lock);
    return 0;
}
EXPORT_SYMBOL(imrsim_blkdev_reset_zone_ptr);//使用EXPORT_SYMBOL可以将一个函数以符号的方式导出给其他模块使用

/* error log */
void imrsim_log_error(struct bio* bio, __u32 uerr)
{
    __u64 lba;

    if (!bio) {
        printk(KERN_ERR "imrsim: NULL pointer passed through\n");
        return;
    }
    #if LINUX_VERSION_CODE < KERNEL_VERSION(3, 14, 0)
    lba = bio->bi_sector;
    #else
    lba = bio->bi_iter.bi_sector;
    #endif
    if (imrsim_dbg_log_enabled) {
        switch(uerr)
        {
            case IMR_ERR_READ_BORDER:
                printk(KERN_DEBUG "%s: lba:%llu IMR_ERR_READ_BORDER\n", __FUNCTION__, lba);
                imrsim_dbg_rerr = uerr;
                break;
            case IMR_ERR_READ_POINTER: 
                printk(KERN_DEBUG "%s: lba:%llu: IMR_ERR_READ_POINTER\n",__FUNCTION__, lba);
                imrsim_dbg_rerr = uerr;
                break;
            case IMR_ERR_WRITE_RO:
                printk(KERN_DEBUG "%s: lba:%llu: IMR_ERR_WRITE_RO\n", __FUNCTION__, lba);
                imrsim_dbg_werr = uerr;
                break;
            case IMR_ERR_WRITE_POINTER :
                printk(KERN_DEBUG "%s: lba:%llu: IMR_ERR_WRITE_POINTER\n",__FUNCTION__, lba);
                imrsim_dbg_werr = uerr;
                break;
            case IMR_ERR_WRITE_ALIGN :
                printk(KERN_DEBUG "%s: lba:%llu: IMR_ERR_WRITE_ALIGN\n", __FUNCTION__, lba);
                imrsim_dbg_werr = uerr;
                break;
            case IMR_ERR_WRITE_BORDER:
                printk(KERN_DEBUG "%s: lba:%llu: IMR_ERR_WRITE_BORDER\n", __FUNCTION__, lba);
                imrsim_dbg_werr = uerr;
                break;
            case IMR_ERR_WRITE_FULL:
                printk(KERN_DEBUG "%s: lba:%llu: IMR_ERR_WRITE_FULL\n", __FUNCTION__, lba);
                imrsim_dbg_werr = uerr;
                break;
            default:
                printk(KERN_DEBUG "%s: lba:%llu: UNKNOWN ERR=%u\n", __FUNCTION__, lba, uerr);
        }
    }
}

/* The following is the relevant method to build the target_type structure. */
/* device creation */
static int imrsim_ctr(struct dm_target *ti,    //创建imrsim_c结构，初始化一些元数据
                      unsigned int argc,
                      char **argv)
{
    unsigned long long tmp;
    int iRet;
    char dummy;
    struct imrsim_c *c = NULL;
    __u64 num;

    printk(KERN_INFO "imrsim: %s called\n", __FUNCTION__);
    if(imrsim_single){
        printk(KERN_ERR "imrsim: No multiple device support currently\n");
        return -EINVAL;
    }
    if(!ti){
        printk(KERN_ERR "imrsim: error: invalid device\n");
        return -EINVAL;
    }
    if(2 != argc){
        ti->error = "dm-imrsim: error: invalid argument count; !=2";
        return -EINVAL;
    }
    if(1 != sscanf(argv[1], "%llu%c", &tmp, &dummy)){
        ti->error = "dm-imrsim: error: invalid argument device sector";
        return -EINVAL;
    }
    c = kmalloc(sizeof(*c), GFP_KERNEL);    // To allocate physically contiguous memory.  分配物理上的连续内存。
    if(!c){
        ti->error = "dm-imrsim: error: no enough memory";
        return -ENOMEM;
    }
    c->start = tmp;
    // Fill in the bdev of the device specified by path and the corresponding interval, permission, mode, etc. into ti->table.
    iRet = dm_get_device(ti, argv[0], dm_table_get_mode(ti->table), &c->dev);
    if(iRet){
        ti->error = "dm-imrsim: error: device lookup failed";
        kfree(c);
        return iRet;
    }
    if(ti->len > IMR_MAX_CAPACITY){
        printk(KERN_ERR "imrsim: capacity %llu exceeds the maximum 10TB\n", (__u64)ti->len);
        kfree(c);
        return -EINVAL;
    }
    num = ti->len >> IMR_BLOCK_SIZE_SHIFT >> IMR_ZONE_SIZE_SHIFT;
    if((num << IMR_BLOCK_SIZE_SHIFT << IMR_ZONE_SIZE_SHIFT) != ti->len){
        printk(KERN_ERR "imrsim:error: total size must be zone size (256MB) aligned\n");
    }
    if (ti->len < (1 << IMR_BLOCK_SIZE_SHIFT << IMR_ZONE_SIZE_SHIFT)) {
      printk(KERN_INFO "imrsim: capacity: %llu sectors\n", (__u64)ti->len);
      printk(KERN_ERR "imrsim:error: capacity is too small. The default config is multiple of 256MB\n"); 
      kfree(c);
      return -EINVAL;
   }
   ti->num_flush_bios = ti->num_discard_bios = ti->num_write_same_bios = 1;
   ti->private = c;
   imrsim_dbg_rerr = imrsim_dbg_werr = imrsim_dbg_log_enabled = 0;
   mutex_init(&imrsim_zone_lock);
   mutex_init(&imrsim_ioctl_lock);
   // To open a persistent thread.
   if(imrsim_persistence_thread(ti)){
       printk(KERN_ERR "imrsim: error: metadata will not be persisted\n");
   }
   imrsim_single = 1;
   return 0;
}

/* device destory */
static void imrsim_dtr(struct dm_target *ti)  //释放imrsim_c以及元数据的空间
{
    struct imrsim_c *c = (struct imrsim_c *) ti->private;

    kthread_stop(imrsim_ptask.pstore_thread);  // To kill the persistent thread.
    mutex_destroy(&imrsim_zone_lock);
    mutex_destroy(&imrsim_ioctl_lock);
    dm_put_device(ti, c->dev);
    kfree(c);
    vfree(zone_state);
    imrsim_single = 0;
    printk(KERN_INFO "imrsim target destructed\n");
}

/* Device Write Rules */
int imrsim_write_rule_check(struct bio *bio, __u32 zone_idx,
                            sector_t bio_sectors, int policy_flag)
{
    __u64  lba;
    __u64  lba_offset;    // The offset of lba in the zone
    __u64  block_offset;  // The offset of the block in the zone
    __u64  boundary;
    __u64  elba;
    __u64  zlba;          //zone的起始地址
    __u32  relocateTrackno;   // In a stage allocation, how many tracks are the relocated lba on?
    __u32  rv;       // rule violation  违反规则
    __u32  z_size;
    __u32  trackno;  // on the top-bottom track group  lba在当前zone的第几号磁道组trackno
    __u32  blockno;  // The number of the block corresponding to lba on the track
    __u32  trackrate;  // Track ratio, p.s. linux kernel does not support floating point calculation.
    __u16  wa_penalty;  //写放大惩罚?延迟
    __u8   isTopTrack;
    __u8   rewriteSign;   //重写标志？
    __u8   ret=1;       // Determine whether the block requested by lba is in the mapping table.
                        //判断lba请求的block是否在映射表中。

    zlba = zone_idx_lba(zone_idx);

    /* Relocate bio according to phase. */
    if(bio->bi_private != &imrsim_completion.write_event)
    {
        /* 根据phase来重定位bio */
        #if LINUX_VERSION_CODE < KERNEL_VERSION(3, 14, 0)
        switch(IMR_ALLOCATION_PHASE)
        {
            case 1:
				lba = bio->bi_sector;
				break;
			case 2:
				lba = bio->bi_sector;
				//printk(KERN_INFO "imrsim: request- lba(sectors) is %llu\n", lba);
				lba_offset = bio->bi_sector - zlba;
				block_offset = lba_offset >> IMR_BLOCK_SIZE_SHIFT;
				//Check the mapping table, ret indicates whether the block where lba is located is in the mapping table
				ret = zone_status[zone_idx].z_pba_map[block_offset]!=-1?1:0;
				if(!ret){         // lba is not in the mapping table, indicating a new write operation
					// Fill the mapping table, allocate tracks according to the stage, and write data
					// Note: Multiply TOP_TRACK_NUM_TOTAL because the number of top and bottom tracks in the zone is equal
					boundary = IMR_BOTTOM_TRACK_SIZE * TOP_TRACK_NUM_TOTAL;
					if(zone_status[zone_idx].z_map_size < boundary){
						// Indicates that the relocated lba should be on the bottom track, in the first stage allocation
						isTopTrack = 0;
						// Judgment should be redirected to the first few bottom tracks
						relocateTrackno = zone_status[zone_idx].z_map_size / IMR_BOTTOM_TRACK_SIZE;
						// Get the pba corresponding to the bio starting lba
						bio->bi_sector = zlba  
							+ (((relocateTrackno+1)*(IMR_TOP_TRACK_SIZE)+relocateTrackno*IMR_BOTTOM_TRACK_SIZE)<<IMR_BLOCK_SIZE_SHIFT) 
							+ ((zone_status[zone_idx].z_map_size % IMR_BOTTOM_TRACK_SIZE) << IMR_BLOCK_SIZE_SHIFT);
						printk(KERN_INFO "imrsim: write_ops(bottom) on zone %u - start LBA is %llu, PBA is %llu\n", zone_idx, lba>>IMR_BLOCK_SIZE_SHIFT, bio->bi_iter.bi_sector>>IMR_BLOCK_SIZE_SHIFT);
						zone_status[zone_idx].z_pba_map[block_offset] = (bio->bi_sector - zlba) >> IMR_BLOCK_SIZE_SHIFT;
						zone_status[zone_idx].z_map_size++;
						lba = bio->bi_sector;
					}else{
						// Indicates that the relocated lba should be on the top track, in a second stage allocation
						isTopTrack = 1;
						relocateTrackno = (zone_status[zone_idx].z_map_size - boundary) / IMR_TOP_TRACK_SIZE;
						bio->bi_sector = zlba
							+ (relocateTrackno*(IMR_TOP_TRACK_SIZE+IMR_BOTTOM_TRACK_SIZE)<<IMR_BLOCK_SIZE_SHIFT)
							+ (((zone_status[zone_idx].z_map_size - boundary) % IMR_TOP_TRACK_SIZE)<<IMR_BLOCK_SIZE_SHIFT);
						printk(KERN_INFO "imrsim: write_ops(top) on zone %u - start LBA is %llu, PBA is %llu\n", zone_idx, lba>>IMR_BLOCK_SIZE_SHIFT, bio->bi_iter.bi_sector>>IMR_BLOCK_SIZE_SHIFT);
						zone_status[zone_idx].z_pba_map[block_offset] = (bio->bi_sector - zlba) >> IMR_BLOCK_SIZE_SHIFT;
						zone_status[zone_idx].z_map_size++;
						lba = bio->bi_sector;
					}
				}else{            // lba is in the mapping table, indicating an update operation
					// Get pba from the mapping table, modify lba in bio
					bio->bi_sector = zlba + (zone_status[zone_idx].z_pba_map[block_offset] << IMR_BLOCK_SIZE_SHIFT);
					printk(KERN_INFO "imrsim: update_ops on zone %u - start LBA is %llu, PBA is %llu\n", zone_idx, lba>>IMR_BLOCK_SIZE_SHIFT, bio->bi_iter.bi_sector>>IMR_BLOCK_SIZE_SHIFT);
					lba = bio->bi_sector;
				}
				break;
			case 3:
				lba = bio->bi_sector;
				lba_offset = bio->bi_sector - zlba;
				block_offset = lba_offset >> IMR_BLOCK_SIZE_SHIFT;
				ret = zone_status[zone_idx].z_pba_map[block_offset]!=-1?1:0;
				if(!ret){         // a new write operation
					boundary = IMR_BOTTOM_TRACK_SIZE * TOP_TRACK_NUM_TOTAL;
					__u32 mapSize = zone_status[zone_idx].z_map_size;
					if(mapSize < boundary){
						// first stage allocation
						isTopTrack = 0;
						relocateTrackno = mapSize / IMR_BOTTOM_TRACK_SIZE;
						bio->bi_sector = zlba  
							+ (((relocateTrackno+1)*(IMR_TOP_TRACK_SIZE)+relocateTrackno*IMR_BOTTOM_TRACK_SIZE)<<IMR_BLOCK_SIZE_SHIFT) 
							+ ((mapSize % IMR_BOTTOM_TRACK_SIZE) << IMR_BLOCK_SIZE_SHIFT);
						printk(KERN_INFO "imrsim: write_ops_3(bottom) - start LBA is %llu, PBA is %llu\n", lba>>IMR_BLOCK_SIZE_SHIFT, bio->bi_iter.bi_sector>>IMR_BLOCK_SIZE_SHIFT);
						zone_status[zone_idx].z_pba_map[block_offset] = (bio->bi_sector - zlba) >> IMR_BLOCK_SIZE_SHIFT;
						zone_status[zone_idx].z_map_size++;
						lba = bio->bi_sector;
					}else if(mapSize >= boundary 
						&& mapSize < boundary + IMR_TOP_TRACK_SIZE*TOP_TRACK_NUM_TOTAL/2){
						// In second stage allocation Top(0,2,4,...)
						isTopTrack = 1;
						relocateTrackno = 2*((mapSize - boundary) / IMR_TOP_TRACK_SIZE);
						bio->bi_sector = zlba
							+ (relocateTrackno*(IMR_TOP_TRACK_SIZE+IMR_BOTTOM_TRACK_SIZE)<<IMR_BLOCK_SIZE_SHIFT)
							+ (((mapSize - boundary) % IMR_TOP_TRACK_SIZE) << IMR_BLOCK_SIZE_SHIFT);
						printk(KERN_INFO "imrsim: write_ops_3(top_1) - start LBA is %llu, PBA is %llu\n", lba>>IMR_BLOCK_SIZE_SHIFT, bio->bi_iter.bi_sector>>IMR_BLOCK_SIZE_SHIFT);
						zone_status[zone_idx].z_pba_map[block_offset] = (bio->bi_sector - zlba) >> IMR_BLOCK_SIZE_SHIFT;
						zone_status[zone_idx].z_map_size++;
						lba = bio->bi_sector;
					}else{
						// In the third stage allocation Top(1,3,5,...)
						isTopTrack = 1;
						relocateTrackno = 2*((mapSize - boundary - IMR_TOP_TRACK_SIZE*TOP_TRACK_NUM_TOTAL/2) 
										/ IMR_TOP_TRACK_SIZE) + 1;
						bio->bi_sector = zlba
							+ (relocateTrackno*(IMR_TOP_TRACK_SIZE+IMR_BOTTOM_TRACK_SIZE)<<IMR_BLOCK_SIZE_SHIFT)
							+ (((mapSize - boundary - IMR_TOP_TRACK_SIZE*TOP_TRACK_NUM_TOTAL/2) % IMR_TOP_TRACK_SIZE)<<IMR_BLOCK_SIZE_SHIFT);
						printk(KERN_INFO "imrsim: write_ops_3(top_2) - start LBA is %llu, PBA is %llu\n", lba>>IMR_BLOCK_SIZE_SHIFT, bio->bi_iter.bi_sector);
						zone_status[zone_idx].z_pba_map[block_offset] = (bio->bi_sector - zlba) >> IMR_BLOCK_SIZE_SHIFT;
						zone_status[zone_idx].z_map_size++;
						lba = bio->bi_sector;
					}
				}else{            // an update operation
					bio->bi_sector = zlba + (zone_status[zone_idx].z_pba_map[block_offset] << IMR_BLOCK_SIZE_SHIFT);
					printk(KERN_INFO "imrsim: update_ops - start lba is %llu, pba is %llu\n", lba, bio->bi_sector);
					lba = bio->bi_sector;
				}
				break;
			default:
				printk(KERN_ERR "imrsim: error: Allocation of more phases is not currently supported!\n");
        }
        #else
        switch(IMR_ALLOCATION_PHASE)
        {
            case 1:
                lba = bio->bi_iter.bi_sector;
                break;
            case 2:
                lba = bio->bi_iter.bi_sector;
                //printk(KERN_INFO "imrsim: request- lba(sectors) is %llu\n", lba);
                lba_offset = bio->bi_iter.bi_sector - zlba;
                block_offset = lba_offset >> IMR_BLOCK_SIZE_SHIFT;
                //Check the mapping table, ret indicates whether the block where lba is located is in the mapping table
                ret = zone_status[zone_idx].z_pba_map[block_offset]!=-1?1:0;
                if(!ret){          // lba is not in the mapping table, indicating a new write operation
                    // Fill the mapping table, allocate tracks according to the stage, and write data
					// Note: Multiply TOP_TRACK_NUM_TOTAL because the number of top and bottom tracks in the zone is equal
                    boundary = IMR_BOTTOM_TRACK_SIZE * TOP_TRACK_NUM_TOTAL;
                    if(zone_status[zone_idx].z_map_size < boundary){
                        // Indicates that the relocated lba should be on the bottom track, in the first stage allocation
                        isTopTrack = 0;
                        // Judgment should be redirected to the first few bottom tracks
                        relocateTrackno = zone_status[zone_idx].z_map_size / IMR_BOTTOM_TRACK_SIZE;
                        // Get the pba corresponding to the bio starting lba
                        bio->bi_iter.bi_sector = zlba  
                            + (((relocateTrackno+1)*(IMR_TOP_TRACK_SIZE)+relocateTrackno*IMR_BOTTOM_TRACK_SIZE)<<IMR_BLOCK_SIZE_SHIFT) 
                            + ((zone_status[zone_idx].z_map_size % IMR_BOTTOM_TRACK_SIZE) << IMR_BLOCK_SIZE_SHIFT);
                        printk(KERN_INFO "imrsim: write_ops(bottom) on zone %u - start LBA is %llu, PBA is %llu\n", zone_idx, lba>>IMR_BLOCK_SIZE_SHIFT, bio->bi_iter.bi_sector>>IMR_BLOCK_SIZE_SHIFT);
                        zone_status[zone_idx].z_pba_map[block_offset] = (bio->bi_iter.bi_sector - zlba) >> IMR_BLOCK_SIZE_SHIFT;
                        zone_status[zone_idx].z_map_size++;
                        lba = bio->bi_iter.bi_sector;
                    }else{
                        // Indicates that the relocated lba should be on the top track, in a second stage allocation
                        isTopTrack = 1;
                        relocateTrackno = (zone_status[zone_idx].z_map_size - boundary) / IMR_TOP_TRACK_SIZE;
                        bio->bi_iter.bi_sector = zlba
                            + (relocateTrackno*(IMR_TOP_TRACK_SIZE+IMR_BOTTOM_TRACK_SIZE)<<IMR_BLOCK_SIZE_SHIFT)
                            + (((zone_status[zone_idx].z_map_size - boundary) % IMR_TOP_TRACK_SIZE)<<IMR_BLOCK_SIZE_SHIFT);
                        printk(KERN_INFO "imrsim: write_ops(top) on zone %u - start LBA is %llu, PBA is %llu\n", zone_idx, lba>>IMR_BLOCK_SIZE_SHIFT, bio->bi_iter.bi_sector>>IMR_BLOCK_SIZE_SHIFT);
                        zone_status[zone_idx].z_pba_map[block_offset] = (bio->bi_iter.bi_sector - zlba) >> IMR_BLOCK_SIZE_SHIFT;
                        zone_status[zone_idx].z_map_size++;
                        lba = bio->bi_iter.bi_sector;
                    }
                }else{            // lba is in the mapping table, indicating an update operation
                    // Get pba from the mapping table, modify lba in bio
                    bio->bi_iter.bi_sector = zlba + (zone_status[zone_idx].z_pba_map[block_offset] << IMR_BLOCK_SIZE_SHIFT);
                    printk(KERN_INFO "imrsim: update_ops on zone %u - start LBA is %llu, PBA is %llu\n", zone_idx, lba>>IMR_BLOCK_SIZE_SHIFT, bio->bi_iter.bi_sector>>IMR_BLOCK_SIZE_SHIFT);
                    lba = bio->bi_iter.bi_sector;
                }
                break;
            case 3:
                lba = bio->bi_iter.bi_sector;
                lba_offset = bio->bi_iter.bi_sector - zlba;
                block_offset = lba_offset >> IMR_BLOCK_SIZE_SHIFT;
                ret = zone_status[zone_idx].z_pba_map[block_offset]!=-1?1:0;  //判断lba请求的block是否在映射表中。
                if(!ret){         // a new write operation
                    // 填充映射表，按照阶段情况分配磁道，写入数据
                    boundary = IMR_BOTTOM_TRACK_SIZE * TOP_TRACK_NUM_TOTAL;
                    __u32 mapSize = zone_status[zone_idx].z_map_size;
                    if(mapSize < boundary){
                        // first stage allocation
                        isTopTrack = 0;
                        // Judgment should be redirected to the first few bottom tracks
                        relocateTrackno = mapSize / IMR_BOTTOM_TRACK_SIZE;
                        // Get the pba corresponding to the bio starting lba
                        bio->bi_iter.bi_sector = zlba  
                            + (((relocateTrackno+1)*(IMR_TOP_TRACK_SIZE)+relocateTrackno*IMR_BOTTOM_TRACK_SIZE)<<IMR_BLOCK_SIZE_SHIFT) 
                            + ((mapSize % IMR_BOTTOM_TRACK_SIZE) << IMR_BLOCK_SIZE_SHIFT);
                        printk(KERN_INFO "imrsim: write_ops_3(bottom) - start LBA is %llu, PBA is %llu\n", lba>>IMR_BLOCK_SIZE_SHIFT, bio->bi_iter.bi_sector>>IMR_BLOCK_SIZE_SHIFT);
                        zone_status[zone_idx].z_pba_map[block_offset] = (bio->bi_iter.bi_sector - zlba) >> IMR_BLOCK_SIZE_SHIFT;
                        zone_status[zone_idx].z_map_size++;
                        lba = bio->bi_iter.bi_sector;
                    }else if(mapSize >= boundary 
                        && mapSize < boundary + IMR_TOP_TRACK_SIZE*TOP_TRACK_NUM_TOTAL/2){
                        // In second stage allocation Top(0,2,4,...)
                        isTopTrack = 1;
                        relocateTrackno = 2*((mapSize - boundary) / IMR_TOP_TRACK_SIZE);
                        bio->bi_iter.bi_sector = zlba
                            + (relocateTrackno*(IMR_TOP_TRACK_SIZE+IMR_BOTTOM_TRACK_SIZE)<<IMR_BLOCK_SIZE_SHIFT)
                            + (((mapSize - boundary) % IMR_TOP_TRACK_SIZE) << IMR_BLOCK_SIZE_SHIFT);
                        printk(KERN_INFO "imrsim: write_ops_3(top_1) - start LBA is %llu, PBA is %llu\n", lba>>IMR_BLOCK_SIZE_SHIFT, bio->bi_iter.bi_sector>>IMR_BLOCK_SIZE_SHIFT);
                        zone_status[zone_idx].z_pba_map[block_offset] = (bio->bi_iter.bi_sector - zlba) >> IMR_BLOCK_SIZE_SHIFT;
                        zone_status[zone_idx].z_map_size++;
                        lba = bio->bi_iter.bi_sector;
                    }else{
                        // In the third stage allocation Top(1,3,5,...)
                        isTopTrack = 1;
                        relocateTrackno = 2*((mapSize - boundary - IMR_TOP_TRACK_SIZE*TOP_TRACK_NUM_TOTAL/2) 
                                        / IMR_TOP_TRACK_SIZE) + 1;
                        bio->bi_iter.bi_sector = zlba
                            + (relocateTrackno*(IMR_TOP_TRACK_SIZE+IMR_BOTTOM_TRACK_SIZE)<<IMR_BLOCK_SIZE_SHIFT)
                            + (((mapSize - boundary - IMR_TOP_TRACK_SIZE*TOP_TRACK_NUM_TOTAL/2) % IMR_TOP_TRACK_SIZE)<<IMR_BLOCK_SIZE_SHIFT);
                        printk(KERN_INFO "imrsim: write_ops_3(top_2) - start LBA is %llu, PBA is %llu\n", lba>>IMR_BLOCK_SIZE_SHIFT, bio->bi_iter.bi_sector);
                        zone_status[zone_idx].z_pba_map[block_offset] = (bio->bi_iter.bi_sector - zlba) >> IMR_BLOCK_SIZE_SHIFT;
                        zone_status[zone_idx].z_map_size++;
                        lba = bio->bi_iter.bi_sector;
                    }
                }else{            // an update operation
                    bio->bi_iter.bi_sector = zlba + (zone_status[zone_idx].z_pba_map[block_offset] << IMR_BLOCK_SIZE_SHIFT);
                    printk(KERN_INFO "imrsim: update_ops - start lba is %llu, pba is %llu\n", lba, bio->bi_iter.bi_sector);
                    lba = bio->bi_iter.bi_sector;
                }
                break;
            default:
                printk(KERN_ERR "imrsim: error: Allocation of more phases is not currently supported!\n");
        }
        #endif
        /* relocate bio end */
    }else{
        #if LINUX_VERSION_CODE < KERNEL_VERSION(3, 14, 0)
        lba = bio->bi_sector;
        #else
        lba = bio->bi_iter.bi_sector;
        #endif
        printk(KERN_INFO "imrsim DIRECT write option.\n");
    }
    
    rv = 0;
    elba = lba + bio_sectors;
    z_size = num_sectors_zone();

    if ((policy_flag == 1) && (zone_status[zone_idx].z_conds == Z_COND_FULL)) {
        zone_status[zone_idx].z_conds = Z_COND_CLOSED;     
    } 
    if (imrsim_dbg_log_enabled && printk_ratelimit()) {
        printk(KERN_INFO "imrsim write PASS\n");
    }
    if (rv && (policy_flag ==1)) {
        printk(KERN_ERR "imrsim: out of policy passed rule violation: %u\n", rv); 
        return IMR_ERR_OUT_OF_POLICY;
    }
    //printk(KERN_INFO "imrsim: %s called! lba: %llu, zlba: %llu ~~\n", __FUNCTION__, lba, zlba);

    trackno = (lba - zlba) / ((IMR_TOP_TRACK_SIZE + IMR_BOTTOM_TRACK_SIZE) <<   //lba在当前zone的第几号磁道组trackno
                IMR_BLOCK_SIZE_SHIFT);   //lba-zlba表示lba在某个zone上的偏移量，而这个偏移量除以一个磁道组的块数就能够得到lba在多少号磁道组上。
    // If it is a new write operation, there is no need to judge isTopTrack
    if(ret){  //ret非0说明是更新操作，需要判断被更新的是顶部磁道还是底部磁道
        isTopTrack = (lba - (zlba + (trackno * (IMR_TOP_TRACK_SIZE + IMR_BOTTOM_TRACK_SIZE) <<
                    IMR_BLOCK_SIZE_SHIFT))) < (IMR_TOP_TRACK_SIZE << IMR_BLOCK_SIZE_SHIFT) ? 1 : 0;
    }
    printk(KERN_INFO "imrsim: %s trackno: %u, isTopTrack: %u.\n",__FUNCTION__, trackno, isTopTrack);

    // record this write operation  记录写操作
    zone_state->stats.zone_stats[zone_idx].z_write_total++;
    zone_state->stats.write_total++;

    // If lba is on the top track, mark the top track with data, and on the bottom track, determine whether to rewrite
    //如果lba(实际是pba)在top track上，则在top track上标记data，在bottom track上，判断是否rewrite
    if(isTopTrack){  //更新顶部磁道
        blockno = (lba - (zlba + (trackno * (IMR_TOP_TRACK_SIZE + IMR_BOTTOM_TRACK_SIZE) <<
                IMR_BLOCK_SIZE_SHIFT))) >> IMR_BLOCK_SIZE_SHIFT;//顶部磁道中需要更新的块
        zone_status[zone_idx].z_tracks[trackno].isUsedBlock[blockno]=1;
        //printk(KERN_INFO "imrsim: SIGN - block is remember\n");
    }else{      //更新底部磁道，对相邻的顶部磁道记录写放大
        wa_penalty=0;
        rewriteSign=0;
        blockno = ((lba - (zlba + (trackno * (IMR_TOP_TRACK_SIZE + IMR_BOTTOM_TRACK_SIZE) <<
                IMR_BLOCK_SIZE_SHIFT))) >> IMR_BLOCK_SIZE_SHIFT) - IMR_TOP_TRACK_SIZE;  //底部磁道需要更新的块号blockno
        trackrate = IMR_BOTTOM_TRACK_SIZE * 10000 / IMR_TOP_TRACK_SIZE; //底部磁道和顶部磁道中块的数量之比
        int wa_pba1=-1,wa_pba2=-1;  //需要在相邻两个磁道上产生的写放大
        imrsim_rmw_task.lba_num=0;   //更新底部磁道需要进行rmw过程
        if(trackno>=0 && zone_status[zone_idx].z_tracks[trackno].isUsedBlock[(__u32)(blockno*10000/trackrate)]==1){ //trackno号磁道有数据
            printk(KERN_INFO "imrsim: write amplification(zone_idx[%u]trackno), block: %u .\n",zone_idx, (__u32)(blockno*10000/trackrate));
            // record write amplification  记录写放大
            zone_state->stats.zone_stats[zone_idx].z_extra_write_total++;
            zone_state->stats.zone_stats[zone_idx].z_write_total++;
            zone_state->stats.extra_write_total++;
            zone_state->stats.write_total++;
            rewriteSign++;
            lba = zlba + (trackno * (IMR_TOP_TRACK_SIZE + IMR_BOTTOM_TRACK_SIZE) <<IMR_BLOCK_SIZE_SHIFT) 
                    + ((__u32)(blockno*10000/trackrate) <<IMR_BLOCK_SIZE_SHIFT);//第一个相邻顶部磁道的位置
            imrsim_rmw_task.lba[imrsim_rmw_task.lba_num] = (sector_t)lba;       //对此位置的块进行rmw，lba强制转换成sector_t
            imrsim_rmw_task.lba_num++;  //imrsim_rmw_task.lba[]数组位置后移一位,以记录下一个rmw
            wa_pba1=lba>>IMR_BLOCK_SIZE_SHIFT;//记录写放大的位置-pba
        }
        if(trackno+1<TOP_TRACK_NUM_TOTAL && zone_status[zone_idx].z_tracks[trackno+1].isUsedBlock[(__u32)(blockno*10000/trackrate)]==1){//trackno+1号磁道有数据
            printk(KERN_INFO "imrsim: write amplification(trackno+1), block: %u .\n", (__u32)(blockno*10000/trackrate));
            zone_state->stats.zone_stats[zone_idx].z_extra_write_total++;
            zone_state->stats.zone_stats[zone_idx].z_write_total++;
            zone_state->stats.extra_write_total++;
            zone_state->stats.write_total++;
            rewriteSign++;
            lba = zlba + ((trackno+1) * (IMR_TOP_TRACK_SIZE + IMR_BOTTOM_TRACK_SIZE) <<IMR_BLOCK_SIZE_SHIFT) 
                    + ((__u32)(blockno*10000/trackrate) <<IMR_BLOCK_SIZE_SHIFT);
            imrsim_rmw_task.lba[imrsim_rmw_task.lba_num] = (sector_t)lba;
            imrsim_rmw_task.lba_num++;
            wa_pba2=lba>>IMR_BLOCK_SIZE_SHIFT;
        }
        if(1 <= rewriteSign){
            printk(KERN_INFO "imrsim: WA, wa_pba_1:%d,wa_pba_2:%d.\n", wa_pba1, wa_pba2);
            return 1;
        }
    }
    return 0;
}

/* Device Read Rules */
int imrsim_read_rule_check(struct bio *bio, __u32 zone_idx,    //lba所在的zone编号zone_idx
                           sector_t bio_sectors, int policy_flag)
{
    __u64 lba;
    __u64 zlba;
    __u64 elba;
    __u32 rv;
    __u8 ret;

    zlba = zone_idx_lba(zone_idx);

    #if LINUX_VERSION_CODE < KERNEL_VERSION(3, 14, 0)
    lba = bio->bi_sector;
    __u32 block_offset = (lba-zlba)>>IMR_BLOCK_SIZE_SHIFT
    ret = zone_status[zone_idx].z_pba_map[block_offset]!=-1?1:0;
    if(ret){
        bio->bi_sector = zlba 
            + (zone_status[zone_idx].z_pba_map[block_offset] << IMR_BLOCK_SIZE_SHIFT);
        printk(KERN_INFO "imrsim: read_ops on zone %u - start lba is %llu, pba is %llu\n", zone_idx, lba, bio->bi_sector); 
        lba = bio->bi_sector;
    }else{
        rv++;
    }
    #else
    lba = bio->bi_iter.bi_sector;
    if(bio->bi_private != &imrsim_completion.read_event)
    {
        __u32 block_offset = (lba-zlba)>>IMR_BLOCK_SIZE_SHIFT;
        ret = zone_status[zone_idx].z_pba_map[block_offset]!=-1?1:0;
        if(ret){   //ret!=-1 说明存在可读数据
            bio->bi_iter.bi_sector = zlba 
                + (zone_status[zone_idx].z_pba_map[block_offset] << IMR_BLOCK_SIZE_SHIFT)
                + (lba-zlba)%(1<<IMR_BLOCK_SIZE_SHIFT);
            printk(KERN_INFO "imrsim: read_ops on zone %u - start LBA is %llu, PBA is %llu\n", zone_idx, lba>>IMR_BLOCK_SIZE_SHIFT, bio->bi_iter.bi_sector>>IMR_BLOCK_SIZE_SHIFT); 
            lba = bio->bi_iter.bi_sector;
        }else{
            rv++;
            //printk(KERN_ERR "imrsim: read none data\n"); 
        }
    }else{
        printk(KERN_INFO "imrsim DIRECT read option.\n");
    }
    
    #endif
    rv = 0;
    elba = lba + bio_sectors;
    
    if(elba > (zlba + num_sectors_zone())){   //跨zone读取
        printk(KERN_ERR "imrsim: error: read across zone: %u.%012llx.%08lx\n",
               zone_idx, lba, bio_sectors);
        rv++;
        zone_state->stats.zone_stats[zone_idx].out_of_policy_read_stats.span_zones_count++;
        imrsim_log_error(bio, IMR_ERR_READ_BORDER);
        if(!policy_flag){
            return IMR_ERR_READ_BORDER;
        }
        printk(KERN_ERR "imrsim:error: out of policy allowed pass\n");
    }
  
    if (imrsim_dbg_log_enabled && printk_ratelimit()) {
        printk(KERN_INFO "imrsim read PASS\n");
    }
    if (rv) {
        printk(KERN_ERR "imrsim: out of policy passed rule violation: %u\n", rv); 
        return IMR_ERR_OUT_OF_POLICY;
    }
    return 0;
}

static bool imrsim_ptask_queue_ok(__u32 idx)
{
   __u32 qidx;
   
    for (qidx = 0; qidx < imrsim_ptask.stu_zone_idx_cnt; qidx++) {
       if (abs(idx - (imrsim_ptask.stu_zone_idx[qidx]))
          <= IMR_PSTORE_PG_EDG) {
          return false;
       }
    }
    return true;
}

static bool imrsim_ptask_gap_ok(__u32 idx)
{
   __u32 qidx;
   
    for (qidx = 0; qidx < imrsim_ptask.stu_zone_idx_cnt; qidx++) {
       if (abs(idx - (imrsim_ptask.stu_zone_idx[qidx]))
          <= IMR_PSTORE_PG_GAP * IMR_PSTORE_PG_EDG) {
          return true;
       }
    }
    return false;
}

/* I/O mapping */
int imrsim_map(struct dm_target *ti, struct bio *bio)  //IO请求映射
{
    struct imrsim_c *c = ti->private;
    int cdir = bio_data_dir(bio);      // Return the data direction, READ or WRITE.
                                       // #define bio_data_dir(bio) \ (op_is_write(bio_op(bio)) ? WRITE : READ) 

    if(bio){
        printk(KERN_INFO "imrsim_map: the bio has %u sectors.\n", bio_sectors(bio));
    }

    sector_t bio_sectors = bio_sectors(bio);
    int policy_rflag = 0;
    int policy_wflag = 0;
    int ret = 0;
    unsigned int penalty;
    __u32 zone_idx;
    __u64 lba;

    mutex_lock(&imrsim_zone_lock);   //锁上互斥锁
    //printk(KERN_INFO "zone_lock.\n");
    #if LINUX_VERSION_CODE < KERNEL_VERSION(3, 14, 0)
    zone_idx = bio->bi_sector >> IMR_BLOCK_SIZE_SHIFT >> IMR_ZONE_SIZE_SHIFT;
    lba = bio->bi_sector;
    #else
    zone_idx = bio->bi_iter.bi_sector >> IMR_BLOCK_SIZE_SHIFT >> IMR_ZONE_SIZE_SHIFT;
    lba = bio->bi_iter.bi_sector;  //bio内的bvec_iter也记录了当前IO请求在磁盘上的起始扇区以及处理进度。
    #endif

    //printk(KERN_INFO "imrsim: map- lba is %llu\n", lba);

    imrsim_dev_idle_update();

    if(IMR_NUMZONES <= zone_idx){
        printk(KERN_ERR "imrsim: lba is out of range. zone_idx: %u\n", zone_idx);
        imrsim_log_error(bio, IMR_ERR_OUT_RANGE);
        goto nomap;
    }
    if(imrsim_dbg_log_enabled){
        printk(KERN_DEBUG "imrsim: %s bio_sectors=%llu\n", __FUNCTION__, 
                (unsigned long long)bio_sectors);
    }
    if((lba + bio_sectors) > (zone_idx_lba(zone_idx) + 2 * num_sectors_zone())){
        printk(KERN_ERR "imrsim: error: %s bio_sectors() is too large\n", __FUNCTION__);
        imrsim_log_error(bio, IMR_ERR_OUT_OF_POLICY);
        goto nomap;
    }
    if(zone_status[zone_idx].z_conds == Z_COND_OFFLINE){
        printk(KERN_ERR "imrsim: error: zone is offline. zone_idx:%u\n", zone_idx);
        imrsim_log_error(bio, IMR_ERR_ZONE_OFFLINE);
        goto nomap;
    }
    bio->bi_bdev = c->dev->bdev;   // struct block_device	*bi_bdev = struct block_device *bdev
    policy_rflag = zone_state->config.dev_config.out_of_policy_read_flag;
    policy_wflag = zone_state->config.dev_config.out_of_policy_write_flag;
    
    // read or write ?
    if(cdir == WRITE){
        if(imrsim_dbg_log_enabled){
            printk(KERN_DEBUG "imrsim: %s WRITE %u.%012llx:%08lx.\n", __FUNCTION__,
                zone_idx, lba, bio_sectors);
        }
        if ((zone_status[zone_idx].z_conds == Z_COND_RO) && !policy_wflag) {  //！polic_wflag代表无法写
            printk(KERN_ERR "imrsim:error: zone is read only. zone_idx: %u\n", zone_idx);  
            imrsim_log_error(bio, IMR_ERR_WRITE_RO);
            goto nomap;
        }
        if ((zone_status[zone_idx].z_conds == Z_COND_FULL) &&
            (lba != zone_idx_lba(zone_idx)) && !policy_wflag) {
            printk(KERN_ERR "imrsim:error: zone is full. zone_idx: %u\n", zone_idx);
            imrsim_log_error(bio, IMR_ERR_WRITE_FULL);
            goto nomap;
        }
        ret = imrsim_write_rule_check(bio, zone_idx, bio_sectors, policy_wflag);  // ret=-242/1/0，1代表发生重写，0代表无重写
        if(ret<0){
            if(policy_wflag == 1 && policy_rflag == 1){
                goto mapped;     //map函数修改了bio的内容，希望DM将bio按照新内容再分发
            }
            penalty = 0;      
            if(policy_wflag == 1){
                penalty = zone_state->config.dev_config.w_time_to_rmw_zone;
                printk(KERN_ERR "imrsim: %s: write error passed: out of policy write flagged on\n", __FUNCTION__);
                udelay(penalty);   //延迟函数
            }else{
                goto nomap;
            }
        }
        if(ret>0){
            goto submitted;   //map函数将bio赋值后又分发出去
        }
        imrsim_ptask.flag |= IMR_STATUS_CHANGE;
        if(imrsim_ptask.stu_zone_idx_cnt == IMR_PSTORE_QDEPTH){
            imrsim_ptask.stu_zone_idx_gap = IMR_PSTORE_PG_GAP;
        }else if(imrsim_ptask_queue_ok(zone_idx)){
            imrsim_ptask.stu_zone_idx[imrsim_ptask.stu_zone_idx_cnt] = zone_idx;
            imrsim_ptask.stu_zone_idx_cnt++;
            if(!imrsim_ptask_gap_ok(zone_idx)){
                imrsim_ptask.stu_zone_idx_gap++;
            }
        }
    }
    else if(cdir == READ){
        if (imrsim_dbg_log_enabled) {
            printk(KERN_DEBUG "imrsim: %s READ %u.%012llx:%08lx.\n", __FUNCTION__,
                    zone_idx, lba, bio_sectors);
        }
        ret = imrsim_read_rule_check(bio, zone_idx, bio_sectors, policy_rflag); //ret=-242或0
        if(ret){  //ret=-242=IMR_ERR_OUT_OF_POLICY
            if(policy_wflag == 1 && policy_rflag == 1){
                printk(KERN_ERR "imrsim: out of policy read passthrough applied\n");
                goto mapped;
            }
            penalty = 0;
            if(policy_rflag == 1){
                penalty = zone_state->config.dev_config.r_time_to_rmw_zone;
                if(printk_ratelimit()){
                    printk(KERN_ERR "imrsim:%s: read error passed: out of policy read flagged on\n", 
                  __FUNCTION__);
                }
                udelay(penalty);
            }else{
                goto nomap;
            }
        }
    }
    mapped:
    if (bio_sectors(bio))   //bio内sector的数量
    #if LINUX_VERSION_CODE < KERNEL_VERSION(3, 14, 0)
        bio->bi_sector =  imrsim_map_sector(ti,bio->bi_sector);
    #else
        bio->bi_iter.bi_sector =  imrsim_map_sector(ti, bio->bi_iter.bi_sector);
    #endif
    mutex_unlock(&imrsim_zone_lock);   //解锁
    //printk(KERN_INFO "zone_unlock.\n");
    return DM_MAPIO_REMAPPED;          //map函数修改了bio的内容，希望DM将bio按照新内容再分发

    submitted:
    printk(KERN_INFO "imrsim_map: submitted and conduct rmw!\n");
    imrsim_rmw_task.bio = bio;//将bio放入rmw的bio中，以进行rmw过程
    imrsim_rmw_thread(ti);
    mutex_unlock(&imrsim_zone_lock);
    printk(KERN_INFO "imrsim_map: end rmw!\n");
    return DM_MAPIO_SUBMITTED;     //已提交bio

    nomap:
    imrsim_ptask.flag |= IMR_STATS_CHANGE;
    imrsim_ptask.sts_zone_idx = zone_idx;
    mutex_unlock(&imrsim_zone_lock);
    //printk(KERN_INFO "zone_unlock.\n");
    return IMR_DM_IO_ERR;
}

/* Device status query */
static void imrsim_status(struct dm_target* ti,   //imrsim_c状态查询
                          status_type_t type,
                          unsigned status_flags, 
                          char* result,
                          unsigned maxlen)
{
   struct imrsim_c* c   = ti->private;

   switch(type)
   {
      case STATUSTYPE_INFO:
         result[0] = '\0';
         break;

      case STATUSTYPE_TABLE:
         snprintf(result, maxlen, "%s %llu", c->dev->name,
	    (unsigned long long)c->start);
         break;
   }
}

/* Present zone status information */
static void imrsim_list_zone_status(struct imrsim_zone_status *ptr, 
                                    __u32 num_zones, int criteria)
{
   __u32 i = 0;
   printk(KERN_DEBUG "\nQuery ceiteria: %d\n", criteria);
   printk(KERN_DEBUG "List zone status of %u zones:\n\n", num_zones);
   for (i = 0; i < num_zones; i++) {
       printk(KERN_DEBUG "zone index        : %lu\n", (long unsigned)ptr[i].z_start);
       printk(KERN_DEBUG "zone length       : %u\n",  ptr[i].z_length);
       printk(KERN_DEBUG "zone type         : 0x%x\n", ptr[i].z_type);
       printk(KERN_DEBUG "zone condition    : 0x%x\n", ptr[i].z_conds);
       printk(KERN_DEBUG "\n");
   }
}

/* Query zone status information and record the result in ptr */
int imrsim_query_zones(sector_t lba, int criteria,
                       __u32 *num_zones, struct imrsim_zone_status *ptr)
{
    int idx32;
    __u32 num32;
    __u32 zone_idx;

    if(!num_zones || !ptr){
        printk(KERN_ERR "imrsim: NULL pointer passed through.\n");
        return -EINVAL;
    }
    mutex_lock(&imrsim_zone_lock);
    zone_idx = lba >> IMR_BLOCK_SIZE_SHIFT >> IMR_ZONE_SIZE_SHIFT;
    if(0 == *num_zones || IMR_NUMZONES < (*num_zones + zone_idx)){
        mutex_unlock(&imrsim_zone_lock);
        printk(KERN_ERR "imrsim: number of zone out of range\n");
        return -EINVAL;
    }
    if (imrsim_dbg_log_enabled) {   
        imrsim_list_zone_status(zone_status, *num_zones, criteria);
    }
    if(criteria > 0){
        idx32 = 0; 
        for (num32 = 0; num32 < *num_zones; num32++) {
            memcpy((ptr + idx32), &zone_status[zone_idx + num32], 
                sizeof(struct imrsim_zone_status));
            idx32++;
        }
        *num_zones = idx32;
        mutex_unlock(&imrsim_zone_lock);
        return 0;  
    }
    switch(criteria){
        case ZONE_MATCH_ALL:
            memcpy(ptr, &zone_status[zone_idx], *num_zones * 
                sizeof(struct imrsim_zone_status));
            break;
        case ZONE_MATCH_FULL:
            idx32 = 0; 
            for (num32 = zone_idx; num32 < IMR_NUMZONES; num32++) {
                if (Z_COND_FULL == zone_status[num32].z_conds) {
                    memcpy((ptr + idx32), &zone_status[num32], 
                            sizeof(struct imrsim_zone_status));
                    idx32++;
                    if (idx32 == *num_zones) {
                        break;
                    }
                }
            }
            *num_zones = idx32;
            break;
        case ZONE_MATCH_NFULL:
            idx32 = 0;
            for (num32 = zone_idx; num32 < IMR_NUMZONES; num32++) {
                memcpy((ptr + idx32), &zone_status[num32], 
                        sizeof(struct imrsim_zone_status));
                idx32++;
                if (idx32 == *num_zones) {
                    break;
                }
            }
            *num_zones = idx32;
            break;
        case ZONE_MATCH_FREE:
            idx32 = 0;
            for (num32 = zone_idx; num32 < IMR_NUMZONES; num32++) {
                if ((Z_COND_EMPTY == zone_status[num32].z_conds)) {
                    memcpy((ptr + idx32), &zone_status[num32], 
                            sizeof(struct imrsim_zone_status));
                    idx32++;
                    if (idx32 == *num_zones) {
                        break;
                    }
                }
            }
            *num_zones = idx32;
            break;
        case ZONE_MATCH_RNLY:
            idx32 = 0;
            for (num32 = zone_idx; num32 < IMR_NUMZONES; num32++) {
                if (Z_COND_RO == zone_status[num32].z_conds) {
                memcpy((ptr + idx32), &zone_status[num32], 
                        sizeof(struct imrsim_zone_status));
                idx32++;
                if (idx32 == *num_zones) {
                    break;
                }
                }
            }
            *num_zones = idx32;
            break;
        case ZONE_MATCH_OFFL:
            idx32 = 0;
            for (num32 = zone_idx; num32 < IMR_NUMZONES; num32++) {
                if (Z_COND_OFFLINE == zone_status[num32].z_conds) {
                memcpy((ptr + idx32), &zone_status[num32], 
                        sizeof(struct imrsim_zone_status));
                idx32++;
                if (idx32 == *num_zones) {
                    break;
                }
                }
            }
            *num_zones = idx32;
            break;
        default:
            printk("imrsim: wrong query parameter\n");
    }
    mutex_unlock(&imrsim_zone_lock);
   return 0;
}
EXPORT_SYMBOL(imrsim_query_zones);//使用EXPORT_SYMBOL可以将一个函数以符号的方式导出给其他模块使用

/* The ioctl interface method implements specific interface functions. */
int imrsim_ioctl(struct dm_target *ti,   //向用户程序暴露的ioctl接口
                 unsigned int cmd,
                 unsigned long arg)
{
    imrsim_zbc_query          *zbc_query;
    struct imrsim_dev_config   pconf;
    //struct imrsim_zone_status  pstatus;
    struct imrsim_stats       *pstats;
    int                        ret = 0;
    __u32                      size  = 0;
    __u64                      num64;
    __u32                      param = IMR_NUMZONES;
    
    imrsim_dev_idle_update();
    mutex_lock(&imrsim_ioctl_lock);
    switch(cmd)
    {
        case IOCTL_IMRSIM_GET_LAST_RERROR:
            if(imrsim_get_last_rd_error(&param)){
                printk(KERN_ERR "imrsim: get last rd error failed\n");
                goto ioerr;
            }
            if((__u64)arg == 0){
                printk(KERN_ERR "imrsim: bad parameter\n");
                goto ioerr;
            }
            if(copy_to_user((__u32 *)arg, &param, sizeof(__u32) )){
                printk(KERN_ERR "imrsim: copy last rd error to user memory failed\n");
                goto ioerr;
            }
            break;
        case IOCTL_IMRSIM_GET_LAST_WERROR:
            if(imrsim_get_last_wd_error(&param)){
                printk(KERN_ERR "imrsim: get last wd error failed\n");
                goto ioerr;
            }
            if((__u64)arg == 0){
                printk(KERN_ERR "imrsim: bad parameter\n");
                goto ioerr;
            }
            if(copy_to_user((__u32 *)arg, &param, sizeof(__u32) )){
                printk(KERN_ERR "imrsim: copy last wd error to user memory failed\n");
                goto ioerr;
            }
            break;
        /* zone ioctl */
        case IOCTL_IMRSIM_SET_LOGENABLE:
            if(imrsim_set_log_enable(1)){
                printk(KERN_ERR "imrsim: enable log failed\n");
                goto ioerr;
            }
            break;
        case IOCTL_IMRSIM_SET_LOGDISABLE:
            if(imrsim_set_log_enable(0)){
                printk(KERN_ERR "imrsim: disable log failed\n");
                goto ioerr;
            }
            break;
        case IOCTL_IMRSIM_GET_NUMZONES:
            if(imrsim_get_num_zones(&param)){
                printk(KERN_ERR "imrsim: get number of zones failed\n");
                goto ioerr;
            }
            if((__u64)arg == 0){
                printk(KERN_ERR "imrsim: bad parameter\n");
                goto ioerr;
            }
            if(copy_to_user((__u32 *)arg, &param, sizeof(__u32) )){
                printk(KERN_ERR "imrsim: copy num of zones to user memory failed\n");
                goto ioerr;
            }
            break;
        case IOCTL_IMRSIM_GET_SIZZONEDEFAULT:
            if(imrsim_get_size_zone_default(&param)){
                printk(KERN_ERR "imrsim: get zone size failed\n");
                goto ioerr;
            }
            if((__u64)arg == 0){
                printk(KERN_ERR "imrsim: bad parameter\n");
                goto ioerr;
            }
            if(copy_to_user((__u32 *)arg, &param, sizeof(__u32) )){
                printk(KERN_ERR "imrsim: copy zone size to user memory failed\n");
                goto ioerr;
            }
            break;
        case IOCTL_IMRSIM_SET_SIZZONEDEFAULT:
            if((__u64)arg == 0){
                printk(KERN_ERR "imrsim: bad parameter\n");
                goto ioerr;
            }
            if(copy_from_user(&param, (__u32 *)arg, sizeof(__u32) )){
                printk(KERN_ERR "imrsim: set zone size copy from user failed\n");
                goto ioerr;
            }
            if(imrsim_set_size_zone_default(param)){
                printk(KERN_ERR "imrsim: set default zone size failed\n");
                goto ioerr;
            }
            imrsim_ptask.flag |= IMR_CONFIG_CHANGE;
            break;
        case IOCTL_IMRSIM_RESET_ZONE:
            if((__u64)arg == 0){
                printk(KERN_ERR "imrsim: bad parameter\n");
                goto ioerr;
            }
            if(copy_from_user(&num64, (__u64 *)arg, sizeof(__u64) )){
                printk(KERN_ERR "imrsim: reset zone write pointer copy from user memory failed\n");
                goto ioerr;
            }
            if(imrsim_blkdev_reset_zone_ptr(num64)){
                printk(KERN_ERR "imrsim: reset zone write pointer failed\n");
                goto ioerr;
            }
            imrsim_ptask.flag |= IMR_CONFIG_CHANGE;
            break;
        case IOCTL_IMRSIM_QUERY:
            zbc_query = kzalloc(sizeof(imrsim_zbc_query), GFP_KERNEL);
            if(!zbc_query){
                printk(KERN_ERR "imrsim: %s no enough memory for zbc query\n", __FUNCTION__);
                goto ioerr;
            }
            if((__u64)arg == 0){
                printk(KERN_ERR "imrsim: bad parameter\n");
                goto zfail;
            }
            ret = copy_from_user(zbc_query, (imrsim_zbc_query *)arg, sizeof(imrsim_zbc_query));
            if(ret){
                printk(KERN_ERR "imrsim: %s copy from user for zbc query failed\n", __FUNCTION__);
                goto zfail;
            }
            if (zbc_query->num_zones == 0 || zbc_query->num_zones > IMR_NUMZONES) {
                printk(KERN_ERR "imrsim: Wrong parameter for the number of zones\n");
                goto zfail;
            }
            size = sizeof(imrsim_zbc_query) + sizeof(struct imrsim_zone_status) *
                  (zbc_query->num_zones - 1);
            zbc_query = krealloc(zbc_query, size, GFP_KERNEL);
            if (!zbc_query) {
                printk(KERN_ERR "imrsim: %s no enough emeory for zbc query\n", __FUNCTION__);
                goto zfail;
            } 
            if (imrsim_query_zones(zbc_query->lba, zbc_query->criteria, 
                &zbc_query->num_zones, zbc_query->ptr)) {
                printk(KERN_ERR "imrsim: %s query zone status failed\n", __FUNCTION__);
                goto zfail;            
            }
            if(copy_to_user((__u32 *)arg, zbc_query, size)){
                    printk(KERN_ERR "imrsim: %s copy to user for zbc query failed\n", __FUNCTION__);
                    goto zfail;
            }
            kfree(zbc_query);
            break;
        zfail:
            kfree(zbc_query);
            break;
        /* IMRSIM stats IOCTLs */
        case IOCTL_IMRSIM_GET_STATS:
            size = imrsim_stats_size();
            pstats = (struct imrsim_stats *)kzalloc(size, GFP_ATOMIC);
            if(!pstats){
                printk(KERN_ERR "imrsim: no enough memory to hold stats\n");
                goto ioerr;
            }
            if(imrsim_get_stats(pstats)){
                printk(KERN_ERR "imrsim: get stats failed\n");
                kfree(pstats);
                goto sfail;
            }
            if(imrsim_dbg_log_enabled){
                imrsim_report_stats(pstats);
            }
            if((__u64)arg == 0){
                printk(KERN_ERR "imrsim: bad parameter\n");
                goto sfail;
            }
            if(copy_to_user((struct imrsim_stats *)arg, pstats, size)){
                printk(KERN_ERR "imrsim: get stats failed as insufficient user memory\n");
                kfree(pstats);
                goto sfail;
            }
            kfree(pstats);
            break;
        sfail:
            kfree(pstats);
            break;
        case IOCTL_IMRSIM_RESET_STATS:
            if(imrsim_reset_stats()){
                printk(KERN_ERR "imrsim: reset stats failed\n");
                goto ioerr;
            }
            imrsim_ptask.flag |= IMR_CONFIG_CHANGE;
            break;
        case IOCTL_IMRSIM_RESET_ZONESTATS:
            if((__u64)arg == 0){
                printk(KERN_ERR "imrsim: bad parameter\n");
                goto ioerr;
            }
            if(copy_from_user(&num64, (__u64 *)arg, sizeof(__u64) )){
                printk(KERN_ERR "imrsim: copy reset zone lba from user memory failed\n");
                goto ioerr;
            }
            if(imrsim_reset_zone_stats(num64)){
                printk(KERN_ERR "imrsim: reset zone stats on lba failed");
                goto ioerr;
            }
            imrsim_ptask.flag |= IMR_CONFIG_CHANGE;
            break;
        /* IMRSIM config IOCTLs */
        case IOCTL_IMRSIM_RESET_DEFAULTCONFIG:
            if(imrsim_reset_default_config()){
                goto ioerr;
            }
            imrsim_ptask.flag |= IMR_CONFIG_CHANGE;
            break;
        case IOCTL_IMRSIM_RESET_ZONECONFIG:
            if(imrsim_reset_default_zone_config()){
                goto ioerr;
            }
            imrsim_ptask.flag |= IMR_CONFIG_CHANGE;
            break;
        case IOCTL_IMRSIM_RESET_DEVCONFIG:
            if(imrsim_reset_default_device_config()){
                goto ioerr;
            }
            imrsim_ptask.flag |= IMR_CONFIG_CHANGE;
            break;
        case IOCTL_IMRSIM_GET_DEVCONFIG:
            if(imrsim_get_device_config(&pconf)){
                goto ioerr;
            }
            if((__u64)arg == 0){
                printk(KERN_ERR "imrsim: bad parameter\n");
                goto ioerr;
            }
            if(copy_to_user((struct imrsim_dev_config*)arg, &pconf, sizeof(struct imrsim_dev_config) )){
                goto ioerr;
            }
            break;
        case IOCTL_IMRSIM_SET_DEVRCONFIG_DELAY:
            if ((__u64)arg == 0) {
                printk(KERN_ERR "imrsim: bad parameter\n");
                goto ioerr; 
            }
            if(copy_from_user(&pconf, (struct imrsim_dev_config *)arg, sizeof(struct imrsim_dev_config) )){
                goto ioerr;
            }
            if(imrsim_set_device_rconfig_delay(&pconf)){
                goto ioerr;
            }
            imrsim_ptask.flag |= IMR_CONFIG_CHANGE;
            break;
        case IOCTL_IMRSIM_SET_DEVWCONFIG_DELAY:
            if ((__u64)arg == 0) {
                printk(KERN_ERR "imrsim: bad parameter\n");
                goto ioerr; 
            }
            if(copy_from_user(&pconf, (struct imrsim_dev_config *)arg, sizeof(struct imrsim_dev_config) )){
                goto ioerr;
            }
            if(imrsim_set_device_wconfig_delay(&pconf)){
                goto ioerr;
            }
            imrsim_ptask.flag |= IMR_CONFIG_CHANGE;
            break;
        default:
            break;
    }
    if(imrsim_ptask.flag & IMR_CONFIG_CHANGE){
        wake_up_process(imrsim_ptask.pstore_thread);
    }
    mutex_unlock(&imrsim_ioctl_lock);
    return 0;
    ioerr:
    mutex_unlock(&imrsim_ioctl_lock);
    return -EFAULT;
}

/* To merge requests. */
static int imrsim_merge(struct dm_target* ti,                //IO请求合并
                        struct bvec_merge_data* bvm,
                        struct bio_vec* biovec, 
                        int max_size)
{
   struct imrsim_c*      c = ti->private;
   struct request_queue* q = bdev_get_queue(c->dev->bdev);

   if (!q->merge_bvec_fn)  //merge_bvec_fn用于确定一个现存的请求是否允许添加更多的数据。
      return max_size;

   bvm->bi_bdev   = c->dev->bdev;
   bvm->bi_sector = imrsim_map_sector(ti, bvm->bi_sector);

   return min(max_size, q->merge_bvec_fn(q, bvm, biovec));
}

/* iterate devices */
static int imrsim_iterate_devices(struct dm_target *ti,     //imrsim_c设备迭代
                                  iterate_devices_callout_fn fn,
                                  void *data)
{
   struct imrsim_c* c = ti->private;

   return fn(ti, c->dev, c->start, ti->len, data);
}

/* Core structure - represents the target-driven plug-in, 
and the structure collects the function entry for the functions implemented by the driver plug-in */
/*将具体函数的地址汇集到target_type结构体中*/
/*对目标设备定义相关的操作，则需要一个target_type结构体，该结构包含对目标设备具体操作函数的入口*/
static struct target_type imrsim_target = 
{
    .name            = "imrsim",
    .version         = {1, 0, 0},
    .module          = THIS_MODULE,
    .ctr             = imrsim_ctr,
    .dtr             = imrsim_dtr,
    .map             = imrsim_map,
    .status          = imrsim_status,
    .ioctl           = imrsim_ioctl,
    .merge           = imrsim_merge,
    .iterate_devices = imrsim_iterate_devices
};

/* init IMRSim module */
static int __init dm_imrsim_init(void)    // 内核模块入口
{
    int ret = 0;

    printk(KERN_INFO "imrsim: %s called.\n", __FUNCTION__);
    ret = dm_register_target(&imrsim_target);
    if(ret < 0){
        printk(KERN_ERR "imrsim: register failed\n");
    }
    return ret;
}

/* kill IMRSim module */
static void dm_imrsim_exit(void)            // 内核模块出口
{
    dm_unregister_target(&imrsim_target);  // 撤销目标设备
}

module_init(dm_imrsim_init);                // 模块入口宏
module_exit(dm_imrsim_exit);                // 模块出口宏

/* Module related signature information */
MODULE_DESCRIPTION(DM_NAME "IMR Simulator");
MODULE_AUTHOR("Zhimin Zeng <im_zzm@126.com>");
MODULE_LICENSE("GPL");
