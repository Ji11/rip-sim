#ifndef RIPD_H
#define RIPD_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

// 协议常量
#define RIP_VERSION 1
#define RIP_CMD_REQUEST 1
#define RIP_CMD_RESPONSE 2
#define RIP_MAX_ENTRIES 25
#define RIP_MSG_MAX 554 // 4 + 25×22

#define RIP_INFINITY 16
#define RIP_PERIODIC_SEC 30
#define RIP_GARBAGE_SEC 120
#define RIP_NEIGHBOR_SEC 180
#define RIP_AFI_IPV4 2
#define RIP_AFI_IPV6 10

#define MAX_ROUTES 256
#define MAX_NEIGHBORS 16

// 路由条目
typedef struct {
	int af;
	uint8_t dest[16];
	int prefix_len;
	uint8_t next_hop[16];
	int metric;
	int from_neighbor;
	time_t last_updated;
} route;

// 路由表
typedef struct {
	route routes[MAX_ROUTES];
	int count;
	int changed;
	pthread_mutex_t lock;
} route_table;

// 邻居条目
typedef struct {
	char id[32];
	struct sockaddr_storage addr;
	socklen_t addr_len;
	int active;
	time_t last_recv;
} neighbor;

// 邻居表
typedef struct {
	neighbor neighbors[MAX_NEIGHBORS];
	int count;
} neighbor_table;

// 路由器
typedef struct {
	char id[32];
	int udp_port;
	int tcp_port;
	int udp_fd;
	route_table rt;
	neighbor_table nt;
	time_t last_periodic;
	volatile int shutdown;
} router;

// RIP 报文头
typedef struct {
	uint8_t command;
	uint8_t version;
	uint16_t reserved;
} rip_header;

// RIP 路由条目（22 bytes）
typedef struct {
	uint16_t af;
	uint8_t addr[16];
	uint8_t prefix_len;
	uint8_t reserved;
	uint16_t metric;
} rip_route;

// 函数声明

// utils.c
void log_printf(const char *fmt, ...);
void addr_copy(int af, const uint8_t *src, uint8_t *dst);
const char *addr_str(int af, const uint8_t *addr, char *buf, size_t len);
const char *sockaddr_str(const struct sockaddr *sa, char *buf, size_t len);
int sockaddr_eq(const struct sockaddr *a, const struct sockaddr *b);
int parse_cidr(const char *cidr, struct sockaddr_storage *sa, int *prefix_len, int *af);
int route_key_eq(int af1, const void *addr1, int pl1, int af2, const void *addr2, int pl2);
const char *time_str(time_t t, char *buf, size_t len);

// config.c
int config_load(const char *path, router *r);

// router.c
void router_init(router *r);
void router_destroy(router *r);
int router_bind(router *r);
int router_run(router *r);
void router_shutdown(router *r);

// route_table.c
void rt_init(route_table *rt);
void rt_destroy(route_table *rt);
int rt_upsert(route_table *rt, int af, const uint8_t *dest, int prefix_len,
	const uint8_t *next_hop, int metric, int from_neighbor);
int rt_find(const route_table *rt, int af, const uint8_t *dest, int prefix_len);
int rt_poison_from_neighbor(route_table *rt, int nbr_idx);
int rt_garbage_collect(route_table *rt);
void rt_show(route_table *rt, int fd);

// neighbor.c
void nt_init(neighbor_table *nt);
int nt_add(neighbor_table *nt, const char *id, const struct sockaddr *addr, socklen_t addr_len);
int nt_find_by_id(const neighbor_table *nt, const char *id);
int nt_find_by_addr(const neighbor_table *nt, const struct sockaddr *addr);
void nt_set_active(neighbor_table *nt, int idx, int active);
void nt_update_last_recv(neighbor_table *nt, int idx);
void nt_show(const neighbor_table *nt, int fd);

// rip.c
int rip_send_response(int udp_fd, const struct sockaddr *dst, socklen_t dst_len,
	route_table *rt, int poison_reverse_idx);
int rip_send_request(int udp_fd, const struct sockaddr *dst, socklen_t dst_len);
int rip_recv(int udp_fd, route_table *rt, neighbor_table *nt);

// tcp_mgmt.c
int tcp_mgmt_start(int port, route_table *rt, neighbor_table *nt, int udp_fd);
void tcp_mgmt_stop(void);

#endif
