#include "neighbor.h"
#include <stdlib.h>

void nt_init(neighbor_table_t *nt)
{
    memset(nt, 0, sizeof(*nt));
}

// 成功返回索引
int nt_add(neighbor_table_t *nt, const char *id, const struct sockaddr *addr, socklen_t addr_len)
{
    if (nt->count >= MAX_NEIGHBORS) {
        log_printf("neighbor table full");
        return -1;
    }

    // 在邻居表中添加新邻居，复制 id，addr，addr_len，设置 active=1，last_recv=当前时间
    neighbor_t *n = &nt->entries[nt->count];
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

int nt_find_by_id(const neighbor_table_t *nt, const char *id)
{
    for (int i = 0; i < nt->count; i++) {
        if (strcmp(nt->entries[i].id, id) == 0) {
            return i;
        }
    }
    return -1;
}

int nt_find_by_addr(const neighbor_table_t *nt, const struct sockaddr *addr)
{
    for (int i = 0; i < nt->count; i++) {
        if (sockaddr_eq((const struct sockaddr *)&nt->entries[i].addr, addr)) {
            return i;
        }
    }
    return -1;
}

void nt_set_active(neighbor_table_t *nt, int idx, int active)
{
    if (idx < 0 || idx >= nt->count) return;

    nt->entries[idx].active = active;
    if (active) {
        // 重新激活时重置收包计时器，避免立即触发超时
        nt->entries[idx].last_recv = time(NULL);
    }
    log_printf("neighbor %s link %s", nt->entries[idx].id, active ? "UP" : "DOWN");
}

void nt_touch(neighbor_table_t *nt, int idx)
{
    if (idx < 0 || idx >= nt->count) return;
    nt->entries[idx].last_recv = time(NULL);
}

int *nt_check_timeouts(const neighbor_table_t *nt, int *count_out)
{
    int *result = NULL;
    int count = 0;
    time_t now = time(NULL);

    for (int i = 0; i < nt->count; i++) {
        if (nt->entries[i].active &&
            (now - nt->entries[i].last_recv) > RIP_NEIGHBOR_SEC) {
            int *tmp = realloc(result, (count + 1) * sizeof(int));
            if (!tmp) {
                free(result);
                *count_out = 0;
                return NULL;
            }
            result = tmp;
            result[count++] = i;
        }
    }

    *count_out = count;
    return result;
}

void nt_show(const neighbor_table_t *nt, int fd)
{
    char addr_buf[64];
    char time_buf[32];

    dprintf(fd, "%-8s %-22s %-6s %s\n",
            "ID", "Address", "State", "Last Recv");
    dprintf(fd, "----------------------------------------------"
               "----------\n");

    for (int i = 0; i < nt->count; i++) {
        const neighbor_t *n = &nt->entries[i];
        dprintf(fd, "%-8s %-22s %-6s %s\n",
                n->id,
                sockaddr_str((const struct sockaddr *)&n->addr,
                             addr_buf, sizeof(addr_buf)),
                n->active ? "UP" : "DOWN",
                time_str(n->last_recv, time_buf, sizeof(time_buf)));
    }

    dprintf(fd, "\nTotal: %d neighbors\n", nt->count);
}
