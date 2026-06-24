#include "rip.h"
#include <unistd.h>
#include <errno.h>

#define RIP_HEADER_SIZE  4   // 头部：command(1) + version(1) + reserved(2)
#define RIP_ENTRY_SIZE   22  // 每条路由：af(2) + addr(16) + prefix_len(1) + reserved(1) + metric(2)

int rip_send_response(int udp_fd, const struct sockaddr *dst,
                      socklen_t dst_len, route_table *rt,
                      int poison_reverse_idx)
{
    uint8_t buf[RIP_MSG_MAX]; // 定义一个大 buf 来存放 RIP 报文
    int offset = 0;

    // 构造 4 bytes 的报文头，构造完 offset += 4，开始逐条构造路由条目
    rip_header *hdr = (rip_header *)(buf + offset); // 报文头指向 buf[0]，[0] = command, [1] = version, [2-3] = reserved
    hdr->command = RIP_CMD_RESPONSE;
    hdr->version = RIP_VERSION;
    hdr->reserved = 0;
    offset += RIP_HEADER_SIZE;

    pthread_mutex_lock(&rt->lock);
    int count = 0;

    // 遍历路由表，构造每条路由条目，最多 RIP_MAX_ENTRIES = 25 条
    for (int i = 0; i < rt->count && count < RIP_MAX_ENTRIES; i++) {
        const route *e = &rt->routes[i];

        // 每个条目放在从 buf[offset] 开始的 24 bytes 处
        rip_route *rip_rte = (rip_route *)(buf + offset);

        // route.metric 是 int，rip_route.metric 是 uint16_t，且需要网络字节序
        uint16_t metric = (uint16_t)e->metric;

        // 毒性逆转：如果该路由是从正要发送给的邻居学到的，将其度量设为不可达
#if POISON_REVERSE
        if (poison_reverse_idx >= 0 && e->from_neighbor == poison_reverse_idx) {
            metric = RIP_INFINITY;
        }
#endif

        rip_rte->af = htons(e->af == AF_INET6 ? RIP_AFI_IPV6 : RIP_AFI_IPV4);
        addr_copy(e->af, e->dest, rip_rte->addr);
        rip_rte->prefix_len = (uint8_t)e->prefix_len;
        rip_rte->reserved = 0;
        rip_rte->metric = htons(metric);

        offset += RIP_ENTRY_SIZE;
        count++;
    }
    pthread_mutex_unlock(&rt->lock);

    ssize_t sent = sendto(udp_fd, buf, offset, 0, dst, dst_len);
    if (sent < 0) {
        log_printf("sendto failed: %s", strerror(errno));
        return -1;
    }

    char peer[64];
    log_printf("sent %d routes to %s (poison_reverse=%d)",
            count, sockaddr_str(dst, peer, sizeof(peer)), poison_reverse_idx);
    return count;
}

int rip_send_request(int udp_fd, const struct sockaddr *dst, socklen_t dst_len)
{
    uint8_t buf[RIP_HEADER_SIZE];
    rip_header *hdr = (rip_header *)buf;
    hdr->command = RIP_CMD_REQUEST;
    hdr->version = RIP_VERSION;
    hdr->reserved = 0;

    ssize_t sent = sendto(udp_fd, buf, sizeof(buf), 0, dst, dst_len);
    if (sent < 0) {
        log_printf("sendto (request) failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

int rip_recv(int udp_fd, route_table *rt, neighborable *nt)
{
    uint8_t buf[4096];
    // 用 sockaddr_storage 来接收 IPv4 or IPv6 地址
    struct sockaddr_storage from;
    socklen_t from_len = sizeof(from);

    // 阻塞接收 UDP 数据报，接收后 buf 中存放 RIP 报文，from 中存放发送方地址
    ssize_t n = recvfrom(udp_fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);
    if (n < 0) {
        log_printf("ERROR: recvfrom failed: %s", strerror(errno));
        return -1;
    }

    if ((size_t)n < RIP_HEADER_SIZE) {
        log_printf("received too-short RIP message: %zd bytes", n);
        return -1;
    }

    // 解析 RIP 报文头，识别命令类型（REQUEST/RESPONSE），并根据命令类型处理报文
    rip_header *hdr = (rip_header *)buf;

    // 识别发送方邻居
    // nbr_idx 是本路由器的邻居表里，那个发 REQUEST 过来的人的索引。
    // nbr_idx 表明了发请求的那个路由器是本机的哪个邻居
    int nbr_idx = nt_find_by_addr(nt, (const struct sockaddr *)&from);
    // nbr_idx 有 3 个用途
    // 1. 记录路由来源 — 传给 rt_upsert 的 from_neighbor 参数，标记"这条路由是从哪个邻居学来的"。这是 Bellman-Ford
    // 运行的基础：后续收到同一邻居的更新、邻居失效时的批量毒性化，都依赖这个归属关系。
    // 2. 毒性逆转 — 回复 REQUEST 时传进去，避免把从 X 学来的路由再告诉 X。
    // 3. 刷新邻居时间戳 — 收到 X 的包就更新 last_recv，180s 超时检测依赖它。

    char peer[64];

    if (hdr->command == RIP_CMD_REQUEST) {
        // 收到请求，回复当前路由表
        log_printf("received REQUEST from %s",
                sockaddr_str((const struct sockaddr *)&from, peer, sizeof(peer)));
        if (nbr_idx >= 0) {
            nt_update_last_recv(nt, nbr_idx);
            // 回复请求方时对其做毒性逆转，最后一个参是邻居索引
            rip_send_response(udp_fd, (const struct sockaddr *)&from, from_len, rt, nbr_idx);
        }
        return 0;
    }

    if (hdr->command != RIP_CMD_RESPONSE) {
        log_printf("unknown RIP command: %d", hdr->command);
        return -1;
    }

    // hdr->command == RIP_CMD_RESPONSE
    // 处理响应报文
    int offset = RIP_HEADER_SIZE;
    int n_routes = 0;
    int changes = 0;

    // 逐条解析 RIP 路由条目，调用 rt_upsert() 更新路由表
    while (offset + RIP_ENTRY_SIZE <= n) {
        rip_route *rip_rte = (rip_route *)(buf + offset);

        int af;
        uint16_t af_val = ntohs(rip_rte->af);
        if (af_val == RIP_AFI_IPV4) {
            af = AF_INET;
        } else if (af_val == RIP_AFI_IPV6) {
            af = AF_INET6;
        } else {
            // 未知地址族，跳过该条目
            offset += RIP_ENTRY_SIZE;
            continue;
        }

        int prefix_len = rip_rte->prefix_len;
        int metric = ntohs(rip_rte->metric);

        // 更新路由表
        if (nbr_idx >= 0) {
            // Bellman-Ford：经邻居到达目标的度量 = 邻居通告的度量 + 1
            int new_metric = metric + 1;
            if (new_metric > RIP_INFINITY) new_metric = RIP_INFINITY;

            // 下一跳地址就是发送方自己的地址
            uint8_t next_hop[16];
            memset(next_hop, 0, 16);
            if (af == AF_INET) {
                const struct sockaddr_in *sin = (const struct sockaddr_in *)&from;
                memcpy(next_hop, &sin->sin_addr, 4); // memcopy 直接搬二进制
            } else {
                const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)&from;
                memcpy(next_hop, &sin6->sin6_addr, 16);
            }

            int changed = rt_upsert(rt, af, rip_rte->addr, prefix_len,
                                    next_hop, new_metric, nbr_idx);
            if (changed) changes++;
        }

        n_routes++;
        offset += RIP_ENTRY_SIZE;
    }

    log_printf("received RESPONSE from %s: %d routes, %d changes",
            sockaddr_str((const struct sockaddr *)&from, peer, sizeof(peer)),
            n_routes, changes);

    // 更新邻居最后收包时间
    if (nbr_idx >= 0) {
        nt_update_last_recv(nt, nbr_idx);
    }

    return n_routes;
}
