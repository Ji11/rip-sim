#ifndef ROUTE_TABLE_H
#define ROUTE_TABLE_H

#include <stdint.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include "utils.h"

// 路由表最大条目数
#define MAX_ROUTES 256

// 路由条目
typedef struct {
    int         af;              // AF_INET 或 AF_INET6
    // uint8_t：强制用 8 位无符号整数，这样 dest 和 next_hop 的大小固定为 16 bytes，跟ipv6长度一样
    uint8_t     dest[16];        // 目标网络地址（v4 只用前 4 bytes）
    int         prefix_len;      // 前缀长度：v4 0-32, v6 0-128
    uint8_t     next_hop[16];    // 下一跳地址
    int         metric;          // 度量：1-15 可达, 16 不可达
    int         from_neighbor;   // 来自哪个邻居（邻居表索引），-1 表示直连路由
    time_t      last_updated;    // 最近一次更新时间
} route_entry_t;

// 路由表（线程安全，带互斥锁）
typedef struct {
    route_entry_t entries[MAX_ROUTES]; // 路由条目数组 每个路由器最多 256 条路由
    int           count;
    int           changed;       // 路由表变更标志，触发更新用
    pthread_mutex_t lock;
} route_table_t;

//  生命周期 
void rt_init(route_table_t *rt);
void rt_destroy(route_table_t *rt);

//  CRUD 操作 
// 添加或更新路由。返回 1 表示新增/修改，0 表示无变化
int  rt_upsert(route_table_t *rt, int af, const uint8_t *dest,
               int prefix_len, const uint8_t *next_hop, int metric,
               int from_neighbor);

// 按目标 + 前缀查找路由，返回索引或 -1
int  rt_find(const route_table_t *rt, int af, const uint8_t *dest,
             int prefix_len);

// 将从指定邻居学到的所有路由标记为 metric=16，进入垃圾回收
int  rt_poison_from_neighbor(route_table_t *rt, int nbr_idx);

// 垃圾回收：删除 metric==16 且超过 120 秒的条目
int  rt_garbage_collect(route_table_t *rt);

// 显示路由表（直接写入文件描述符）
void rt_show(route_table_t *rt, int fd);

#endif // ROUTE_TABLE_H
