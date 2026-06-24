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
        const route *e = &rt->entries[i];

        // 每个条目放在从 buf[offset] 开始的 24 bytes 处
        rip_route *entry = (rip_route *)(buf + offset);

        // route.metric 是 int，rip_route.metric 是 uint16_t，且需要网络字节序
        uint16_t metric = (uint16_t)e->metric;

        // 毒性逆转：如果该路由是从正要发送给的邻居学到的，将其度量设为不可达
#if POISON_REVERSE
        if (poison_reverse_idx >= 0 && e->from_neighbor == poison_reverse_idx) {
            metric = RIP_INFINITY;
        }
#endif

        entry->af = htons(e->af == AF_INET6 ? RIP_AFI_IPV6 : RIP_AFI_IPV4);
        addr_copy(e->af, e->dest, entry->addr);
        entry->prefix_len = (uint8_t)e->prefix_len;
        entry->reserved = 0;
        entry->metric = htons(metric);

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
    struct sockaddr_storage from;
    socklen_t from_len = sizeof(from);

    ssize_t n = recvfrom(udp_fd, buf, sizeof(buf), 0,
                         (struct sockaddr *)&from, &from_len);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        log_printf("ERROR: recvfrom failed: %s", strerror(errno));
        return -1;
    }

    if ((size_t)n < RIP_HEADER_SIZE) {
        log_printf("received too-short RIP message: %zd bytes", n);
        return -1;
    }

    rip_header *hdr = (rip_header *)buf;

    if (hdr->version != RIP_VERSION) {
        log_printf("unknown RIP version: %d", hdr->version);
        return -1;
    }

    // 识别发送方邻居
    int nbr_idx = nt_find_by_addr(nt, (const struct sockaddr *)&from);

    char peer[64];

    if (hdr->command == RIP_CMD_REQUEST) {
        // 收到请求：立即回复当前路由表
        log_printf("received REQUEST from %s",
                sockaddr_str((const struct sockaddr *)&from, peer, sizeof(peer)));
        if (nbr_idx >= 0) {
            nt_touch(nt, nbr_idx);
            rip_send_response(udp_fd, (const struct sockaddr *)&from,
                              from_len, rt, nbr_idx);
        }
        return 0;
    }

    if (hdr->command != RIP_CMD_RESPONSE) {
        log_printf("unknown RIP command: %d", hdr->command);
        return -1;
    }

    // 处理响应报文
    int offset = RIP_HEADER_SIZE;
    int nentries = 0;
    int changes = 0;

    while (offset + RIP_ENTRY_SIZE <= n) {
        rip_route *entry = (rip_route *)(buf + offset);

        int af;
        uint16_t af_val = ntohs(entry->af);
        if (af_val == RIP_AFI_IPV4) {
            af = AF_INET;
        } else if (af_val == RIP_AFI_IPV6) {
            af = AF_INET6;
        } else {
            // 未知地址族，跳过该条目
            offset += RIP_ENTRY_SIZE;
            continue;
        }

        int prefix_len = entry->prefix_len;
        int metric = ntohs(entry->metric);

        if (nbr_idx >= 0) {
            // Bellman-Ford：经邻居到达目标的度量 = 邻居通告的度量 + 1
            int new_metric = metric + 1;
            if (new_metric > RIP_INFINITY) new_metric = RIP_INFINITY;

            // 下一跳地址就是发送方自己的地址
            uint8_t next_hop[16];
            memset(next_hop, 0, 16);
            if (af == AF_INET) {
                const struct sockaddr_in *sin = (const struct sockaddr_in *)&from;
                memcpy(next_hop, &sin->sin_addr, 4);
            } else {
                const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)&from;
                memcpy(next_hop, &sin6->sin6_addr, 16);
            }

            int changed = rt_upsert(rt, af, entry->addr, prefix_len,
                                    next_hop, new_metric, nbr_idx);
            if (changed) changes++;
        }

        nentries++;
        offset += RIP_ENTRY_SIZE;
    }

    log_printf("received RESPONSE from %s: %d entries, %d changes",
            sockaddr_str((const struct sockaddr *)&from, peer, sizeof(peer)),
            nentries, changes);

    // 更新邻居最后收包时间
    if (nbr_idx >= 0) {
        nt_touch(nt, nbr_idx);
    }

    return nentries;
}
