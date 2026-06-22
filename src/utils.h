#ifndef UTILS_H
#define UTILS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// 协议常量
#define RIP_INFINITY       16   // RIP 不可达度量
#define RIP_PERIODIC_SEC   30   // 周期性更新间隔（秒）
#define RIP_GARBAGE_SEC    120  // 垃圾回收超时（秒）
#define RIP_NEIGHBOR_SEC   180  // 邻居失效超时（秒）
#define RIP_AFI_IPV4        2   // RIP 线格式 IPv4 地址族
#define RIP_AFI_IPV6       10   // RIP 线格式 IPv6 地址族
#define RIP_MSG_MAX        604  // 最大 RIP 报文字节数（4 + 25×24）

//  日志
void log_printf(const char *fmt, ...);

//  地址工具
// 将 af 族地址从 src 复制到 16 字节 dst（v4 复制 4 字节，v6 复制 16 字节）
void addr_copy(int af, const uint8_t *src, uint8_t *dst);

// 将 raw uint8_t 地址转为可读字符串
const char *addr_str(int af, const uint8_t *addr, char *buf, size_t len);

// 将 sockaddr 转为可读字符串（支持 v4/v6）
const char *sockaddr_str(const struct sockaddr *sa, char *buf, size_t len);

// 比较两个 sockaddr 是否相等
int sockaddr_eq(const struct sockaddr *a, const struct sockaddr *b);

// 解析 "addr/prefix" 或 "addr" 为 sockaddr_storage + 前缀长度
int parse_cidr(const char *cidr, struct sockaddr_storage *sa,
               int *prefix_len, int *af);

// 比较两条路由键 (af, addr, prefix_len) 是否相同
int route_key_eq(int af1, const void *addr1, int pl1,
                 int af2, const void *addr2, int pl2);

//  时间工具 
const char *time_str(time_t t, char *buf, size_t len);

#endif // UTILS_H
