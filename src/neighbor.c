#include "ripd.h"

void nt_init(neighbor_table *nt)
{
    memset(nt, 0, sizeof(*nt));
}

int nt_add(neighbor_table *nt, const char *id, const struct sockaddr *addr, socklen_t addr_len)
{
    if (nt->count >= MAX_NEIGHBORS) {
        log_printf("neighbor table full");
        return -1;
    }

    // 在邻居表中添加新邻居，复制 id，addr，addr_len，设置 active=1，last_recv=当前时间
    neighbor *n = &nt->neighbors[nt->count];
    strncpy(n->id, id, sizeof(n->id) - 1);
    n->id[sizeof(n->id) - 1] = '\0';
    memcpy(&n->addr, addr, addr_len);
    n->addr_len = addr_len;
    n->active = 1;
    n->last_recv = time(NULL);

    char buf[64];
    log_printf("neighbor %s added: %s", id,
            sockaddr_str((const struct sockaddr *)&n->addr, buf, sizeof(buf)));

    return nt->count++;
}

int nt_find_by_id(const neighbor_table *nt, const char *id)
{
    for (int i = 0; i < nt->count; i++) {
        if (strcmp(nt->neighbors[i].id, id) == 0) {
            return i;
        }
    }
    return -1;
}

// 根据 sockaddr 查找邻居索引，返回索引或-1
int nt_find_by_addr(const neighbor_table *nt, const struct sockaddr *addr)
{
    for (int i = 0; i < nt->count; i++) {
        if (sockaddr_eq((const struct sockaddr *)&nt->neighbors[i].addr, addr)) {
            return i;
        }
    }
    return -1;
}

// 设置链路状态
void nt_set_active(neighbor_table *nt, int idx, int active)
{
    if (idx < 0 || idx >= nt->count) return;

    nt->neighbors[idx].active = active;
    if (active) {
        // 重新激活时重置收包计时器，避免立即触发超时
        nt->neighbors[idx].last_recv = time(NULL);
    }
    log_printf("neighbor %s link %s", nt->neighbors[idx].id, active ? "UP" : "DOWN");
}

// 更新最近收包时间戳
void nt_update_last_recv(neighbor_table *nt, int idx)
{
    if (idx < 0 || idx >= nt->count) return;
    nt->neighbors[idx].last_recv = time(NULL);
}

void nt_show(const neighbor_table *nt, int fd)
{
    char addr_buf[64];
    char time_buf[32];

    // 输出示例：
    // ID       Address                State  Last Recv
    // --------------------------------------------------------
    // 2        127.0.0.1:5202         UP     20:54:56
    // 3        127.0.0.1:5203         UP     20:54:56
    //
    // Total: 2 neighbors
    dprintf(fd, "%-8s %-22s %-6s %s\n",
            "ID", "Address", "State", "Last Recv");
    dprintf(fd, "----------------------------------------------"
               "----------\n");

    for (int i = 0; i < nt->count; i++) {
        const neighbor *n = &nt->neighbors[i];
        dprintf(fd, "%-8s %-22s %-6s %s\n",
                n->id,
                sockaddr_str((const struct sockaddr *)&n->addr,
                             addr_buf, sizeof(addr_buf)),
                n->active ? "UP" : "DOWN",
                time_str(n->last_recv, time_buf, sizeof(time_buf)));
    }

    dprintf(fd, "\nTotal: %d neighbors\n", nt->count);
}
