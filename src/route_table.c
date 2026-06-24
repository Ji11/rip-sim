#include "ripd.h"

void rt_init(route_table *rt)
{
    memset(rt, 0, sizeof(*rt));
    pthread_mutex_init(&rt->lock, NULL);
    rt->changed = 0;
}

void rt_destroy(route_table *rt)
{
    pthread_mutex_destroy(&rt->lock);
}

// 按目标 + 前缀查找路由，返回索引或 -1
int rt_find(const route_table *rt, int af, const uint8_t *dest, int prefix_len)
{
    for (int i = 0; i < rt->count; i++) {
        if (route_key_eq(af, dest, prefix_len,
                         rt->routes[i].af, rt->routes[i].dest,
                         rt->routes[i].prefix_len)) { // route_key_eq() 相同返回 1 不同返回 0
            return i;
        }
    }
    return -1;
}

//  Bellman-Ford 路由更新（核心算法）
//
// 四段决策逻辑：
//   1. 直连路由（from_neighbor==-1）是权威来源，不被更差的邻居路由覆盖
//   2. 来自同一邻居的更新：无条件接受（与 RIP 标准一致）
//   3. 来自不同邻居、度量相同：忽略，只更新时间戳
//   4. 来自不同邻居、度量更优：替换

// 添加或更新路由。返回 1 表示新增/修改，0 表示无变化
// 调用点: config.c 启动注册直连路由, from_neighbor=-1
//         rip.c 收到邻居RESPONSE, from_neighbor=nbr_idx
int rt_upsert(route_table *rt, int af, const uint8_t *dest,
              int prefix_len, const uint8_t *next_hop, int metric,
              int from_neighbor)
{
    // 度量裁剪到 0-16
    if (metric > RIP_INFINITY) metric = RIP_INFINITY;
    if (metric < 0) metric = RIP_INFINITY;

    pthread_mutex_lock(&rt->lock);

    // 按目标 + 前缀查找路由，返回索引或 -1
    int idx = rt_find(rt, af, dest, prefix_len);

    if (idx < 0) {
        // 路由表中没有，就新增路由
        if (rt->count >= MAX_ROUTES) {
            pthread_mutex_unlock(&rt->lock);
            log_printf("route table full, cannot add new route");
            return 0;
        }
        idx = rt->count++;
        route *e = &rt->routes[idx];
        e->af = af;
        addr_copy(af, dest, e->dest);
        e->prefix_len = prefix_len;
        addr_copy(af, next_hop, e->next_hop);
        e->metric = metric;
        e->from_neighbor = from_neighbor;
        e->last_updated = time(NULL);
        rt->changed = 1;
        pthread_mutex_unlock(&rt->lock);
        return 1;
    }

    // idx > 0，路由表中已有路由，则定位这条路由，进行更新决策
    // e 是路由表中已有的条目，from_neighbor 和 metric 是新路由的来源和度量
    route *e = &rt->routes[idx];

    // 规则 1：直连路由保护 —— 不允许被更差或相等的邻居路由覆盖
    // e->from_neighbor == -1 表示已有的路由是直连路由
    // from_neighbor >= 0 表示来自邻居的路由 且合法
    // metric >= e->metric 表示新的邻居路由更差或相等
    if (e->from_neighbor == -1 && from_neighbor >= 0 && metric >= e->metric) {
        pthread_mutex_unlock(&rt->lock);
        return 0;
    }

    // 规则 2：同一下一跳、同一度量、同一来源 只需刷新时间戳
    // 必须验证 from_neighbor 相同，否则多邻居共用同一 IP（如 127.0.0.1）
    // 时，邻居 B 发来的同度量更新会错误重置邻居 A 路由的 GC 计时器
    int same_nh = (memcmp(e->next_hop, next_hop, (af == AF_INET) ? 4 : 16) == 0);
    if (same_nh && e->metric == metric && e->from_neighbor == from_neighbor) {
        e->last_updated = time(NULL);
        pthread_mutex_unlock(&rt->lock);
        return 0;
    }

    // 规则 3：来自同一邻居 无条件更新
    if (e->from_neighbor == from_neighbor) {
        addr_copy(af, next_hop, e->next_hop);
        e->metric = metric;
        e->last_updated = time(NULL);
        rt->changed = 1;
        pthread_mutex_unlock(&rt->lock);
        return 1;
    }

    // 规则 4：来自不同邻居 仅当度量严格更优时更新
    if (metric < e->metric) {
        addr_copy(af, next_hop, e->next_hop);
        e->metric = metric;
        e->from_neighbor = from_neighbor;
        e->last_updated = time(NULL);
        rt->changed = 1;
        pthread_mutex_unlock(&rt->lock);
        return 1;
    }

    // 度量更差的来自不同邻居 忽略
    pthread_mutex_unlock(&rt->lock);
    return 0;
}

// 将从指定邻居学到的所有路由标记为 metric=16，进入垃圾回收
int rt_poison_from_neighbor(route_table *rt, int nbr_idx)
{
    int poisoned = 0; // 统计被毒化的路由条目数

    // 有关 changed 操作，需要加锁，因为：
    // 1. 主线程 rt_upsert() 可能修改路由表，把 rt->changed 置 1
    // 2. TCP 管理线程也会通过 link down → rt_poison_from_neighbor 间接写 rt->changed
    pthread_mutex_lock(&rt->lock);
    for (int i = 0; i < rt->count; i++) {
        if (rt->routes[i].from_neighbor == nbr_idx && rt->routes[i].metric < RIP_INFINITY) {
            rt->routes[i].metric = RIP_INFINITY;
            rt->routes[i].last_updated = time(NULL);
            rt->changed = 1;
            poisoned++;
        }
    }
    pthread_mutex_unlock(&rt->lock);

    return poisoned;
}

// 垃圾回收：删除 metric==16 且超过 120 秒的条目
int rt_garbage_collect(route_table *rt)
{
    int removed = 0;
    time_t now = time(NULL);

    pthread_mutex_lock(&rt->lock);
    // 从后往前遍历，避免删除操作影响后续索引
    for (int i = rt->count - 1; i >= 0; i--) {
        if (rt->routes[i].metric == RIP_INFINITY) {
            if (now - rt->routes[i].last_updated >= RIP_GARBAGE_SEC) {
                // 如果不是最后一条路由，使用 memmove 将后续路由前移覆盖当前路由
                if (i < rt->count - 1) {
                    // 移动 (rt->count - i - 1) 条，即从 i+1 到 count-1 的路由条目，每条 sizeof(route) bytes
                    memmove(&rt->routes[i], &rt->routes[i + 1], (rt->count - i - 1) * sizeof(route));
                }
                rt->count--;
                removed++;
            }
        }
    }
    if (removed > 0) rt->changed = 1;
    pthread_mutex_unlock(&rt->lock);

    return removed;
}

// 显示路由表
// 输出示例：
// Destination              PfxLen Next Hop                 Metric From
// ----------------------------------------------------------------------------
// 10.0.1.0                 24     0.0.0.0                  1       direct
// 10.0.3.0                 24     127.0.0.1                2       neighbor
// 10.0.2.0                 24     127.0.0.1                16      neighbor [GC]
//
// Total: 3 routes
void rt_show(route_table *rt, int fd)
{
    char dest_buf[INET6_ADDRSTRLEN];
    char nh_buf[INET6_ADDRSTRLEN];

    pthread_mutex_lock(&rt->lock);

    dprintf(fd, "%-24s %-6s %-24s %-7s %s\n",
            "Destination", "PfxLen", "Next Hop", "Metric", "From");
    dprintf(fd, "------------------------------------------------------------"
               "--------------\n");

    for (int i = 0; i < rt->count; i++) {
        route *e = &rt->routes[i];

        const char *dest_str = addr_str(e->af, e->dest, dest_buf, sizeof(dest_buf));
        const char *nh_str   = addr_str(e->af, e->next_hop, nh_buf, sizeof(nh_buf));

        const char *from_str = (e->from_neighbor >= 0) ? "neighbor" : "direct";

        dprintf(fd, "%-24s %-6d %-24s %-7d %s%s\n",
                dest_str ? dest_str : "?",
                e->prefix_len,
                nh_str ? nh_str : "?",
                e->metric,
                from_str,
                (e->metric == RIP_INFINITY) ? " [GC]" : "");
    }

    dprintf(fd, "\nTotal: %d routes\n", rt->count);
    pthread_mutex_unlock(&rt->lock);
}
