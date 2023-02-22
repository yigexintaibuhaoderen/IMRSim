#ifndef _IMRSIM_TYPES_H
#define _IMRSIM_TYPES_H

#define IMRSIM_VERSION(a,b,c)  ((a<<16)|(b<<8)|c)
#define TOP_TRACK_NUM_TOTAL 64
#define TOP_TRACK_SIZE 456
#define BOTTOM_TRACK_SIZE 568

// The total number of mapping table entries 
// 映射條目總數
#define TOTAL_ITEMS (TOP_TRACK_SIZE+BOTTOM_TRACK_SIZE)*TOP_TRACK_NUM_TOTAL

enum imrsim_zone_conditions{
    Z_COND_NO_WP      = 0x00,
    Z_COND_EMPTY      = 0x01,
    Z_COND_CLOSED     = 0x02,
    Z_COND_RO         = 0x0D,     /* read only */
    Z_COND_FULL       = 0x0E,
    Z_COND_OFFLINE    = 0x0F
};

enum imrsim_zone_type{
    Z_TYPE_RESERVED     = 0x00,
    Z_TYPE_CONVENTIONAL = 0x01,
    Z_TYPE_SEQUENTIAL   = 0x02,
    Z_TYPE_PREFERRED    = 0x04
};

struct imrsim_zone_track{
    __u8     isUsedBlock[TOP_TRACK_SIZE];
};

struct imrsim_zone_status    //记录每个zone的基本信息
{
    sector_t                     z_start;                /* blocks  */  //zone在磁盘模拟器中的编号（从0开始）
    __u32                        z_length;               /* sectors */  //一个zone以扇区为单位的大小
    __u16                        z_conds;                //zoe的状态（空、满、关闭、只读等）
    __u8                         z_type;                 //zone的类型（这里都实现为传统可随机读写的类型）
    __u8                         z_flag;                 //控制此案的读写许可
	// save the records of all the top tracks in a zone, whether there is data
    struct imrsim_zone_track     z_tracks[TOP_TRACK_NUM_TOTAL];    //记录每个zone中所有磁道组中顶部磁道的信息，
    // mapping table                                               //即顶部磁道的各个块是否存有有效数据
    __u32                        z_map_size;
    int                          z_pba_map[TOTAL_ITEMS];
};

struct imrsim_state_header  //记录基础的头部信息
{
    __u32  magic;     //设备标识
    __u32  length;    //imrsim_state结构体的大小
    __u32  version;   //设备版本号
    __u32  crc32;     //校验和
};

struct imrsim_idle_stats
{
    __u32 dev_idle_time_max;
    __u32 dev_idle_time_min;
};

struct imrsim_dev_stats
{
    struct imrsim_idle_stats idle_stats;
};

struct imrsim_out_of_policy_read_stats
{
    __u32 span_zones_count;
};

struct imrsim_out_of_policy_write_stats
{
    __u32 span_zones_count;
    __u32 unaligned_count;
};

struct imrsim_zone_stats
{
    struct imrsim_out_of_policy_read_stats   out_of_policy_read_stats;
    struct imrsim_out_of_policy_write_stats  out_of_policy_write_stats;
    __u32 z_extra_write_total;      // Record the number of extra writes of a zone
    __u32 z_write_total;            // Record the total number of writes in a zone
};

struct imrsim_stats     //具体记录zone相关的统计信息
{
    struct imrsim_dev_stats  dev_stats;               //设备信息
    __u32                    num_zones;               //zone的总条目，表示磁盘由多少个zone组成
    __u64                    extra_write_total;      // Record the number of imrsim extra writes
    __u64                    write_total;            // Record the total number of imrsim writes
    struct imrsim_zone_stats zone_stats[1];          //每一个zone的统计信息
};

struct imrsim_dev_config
{
    /* flag: 0 to reject with erro, 1 to add latency and satisfy request. */
    __u32 out_of_policy_read_flag;
    __u32 out_of_policy_write_flag;
    __u16 r_time_to_rmw_zone;    /* read time */
    __u16 w_time_to_rmw_zone;    /* write time */
};

struct imrsim_config    //配置信息结构体，主要用来配置读写的延迟时间
{
    struct imrsim_dev_config  dev_config;  
};

struct imrsim_state   //设备统计信息结构体
{
    struct imrsim_state_header header;   //头部
    struct imrsim_config       config;   //配置信息
    struct imrsim_stats        stats;    //设备统计信息
};


typedef struct
{
  __u64                      lba;          /* IN            */
  __u32                      num_zones;    /* IN/OUT, The number of zones to be queried  */
  int                        criteria;     /* IN            */
  struct imrsim_zone_status  ptr[1];       /* OUT           */
} imrsim_zbc_query;


enum imrsim_zbcquery_criteria {
   ZONE_MATCH_ALL  =           0, /* Match all zones                       */
   ZONE_MATCH_FULL =          -1, /* Match all zones full                  */
   ZONE_MATCH_NFULL =         -2, /* Match all zones not full              */
   ZONE_MATCH_FREE =          -3, /* Match all zones free                  */
   ZONE_MATCH_RNLY =          -4, /* Match all zones read-only             */
   ZONE_MATCH_OFFL =          -5, /* Match all zones offline               */
};

#endif