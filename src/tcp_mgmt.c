#include "tcp_mgmt.h"
#include "rip.h"
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// 与主线程共享的全局指针
static route_table_t   *g_mgmt_rt;
static neighbor_table_t *g_mgmt_nt;
static int              g_mgmt_tcp_fd = -1;
static int              g_mgmt_udp_fd = -1;
static volatile int     g_mgmt_running = 0;
static pthread_t        g_mgmt_thread;

// link down：手动断开与指定邻居的链路
static void cmd_link_down(int fd, const char *neighbor_id)
{
    if (!neighbor_id) {
        dprintf(fd, "ERROR: usage: link down <neighbor_id>\n");
        return;
    }

    int idx = nt_find_by_id(g_mgmt_nt, neighbor_id);
    if (idx < 0) {
        dprintf(fd, "ERROR: neighbor '%s' not found\n", neighbor_id);
        return;
    }

    if (!g_mgmt_nt->entries[idx].active) {
        dprintf(fd, "neighbor '%s' is already DOWN\n", neighbor_id);
        return;
    }

    nt_set_active(g_mgmt_nt, idx, 0);
#if POISON_REVERSE
    int poisoned = rt_poison_from_neighbor(g_mgmt_rt, idx);
#else
    int poisoned = 0;
#endif
    dprintf(fd, "OK: link DOWN to neighbor '%s', %d routes poisoned\n",
            neighbor_id, poisoned);
}

// link up：恢复与指定邻居的链路，并主动发送 RIP Request 拉取路由
static void cmd_link_up(int fd, const char *neighbor_id, int udp_fd)
{
    if (!neighbor_id) {
        dprintf(fd, "ERROR: usage: link up <neighbor_id>\n");
        return;
    }

    int idx = nt_find_by_id(g_mgmt_nt, neighbor_id);
    if (idx < 0) {
        dprintf(fd, "ERROR: neighbor '%s' not found\n", neighbor_id);
        return;
    }

    if (g_mgmt_nt->entries[idx].active) {
        dprintf(fd, "neighbor '%s' is already UP\n", neighbor_id);
        return;
    }

    nt_set_active(g_mgmt_nt, idx, 1);

    if (udp_fd >= 0) {
        rip_send_request(udp_fd,
                        (const struct sockaddr *)&g_mgmt_nt->entries[idx].addr,
                        g_mgmt_nt->entries[idx].addr_len);
    }

    dprintf(fd, "OK: link UP to neighbor '%s', RIP request sent\n", neighbor_id);
}

// 处理客户端的线程
static void *mgmt_client_handler(void *arg)
{
    // arg 是 accept 返回的 client_fd，从调用的线程创建函数那里反向类型转换成 int
    int client_fd = (int)(intptr_t)arg;

    // 服务器端向客户端 fd 中直接写数据，发给 nc 客户端
    dprintf(client_fd, "RIP Router Management Interface\n");
    dprintf(client_fd, "Commands: show route | show neighbors | "
            "link down <id> | link up <id> | quit\n\n> ");

    // 以 FILE * 流的方式读取客户端输入，方便使用 fgets
    FILE *stream = fdopen(client_fd, "r");
    if (!stream) {
        close(client_fd);
        return NULL;
    }

    // 循环读取客户端输入的命令行，解析并执行
    char line[512];
    while (g_mgmt_running && fgets(line, sizeof(line), stream)) {
        size_t len = strlen(line);
        // 去掉行尾的换行符
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
            line[--len] = '\0';
        }

        if (len == 0) continue;

        char cmd[64], arg[256];
        arg[0] = '\0';
        sscanf(line, "%63s %255[^\n]", cmd, arg);

        if (strcmp(cmd, "quit") == 0) {
            dprintf(client_fd, "Goodbye.\n");
            break;
        } else if (strcmp(cmd, "show") == 0) {
            if (strcmp(arg, "route") == 0) {
                rt_show(g_mgmt_rt, client_fd);
            } else if (strcmp(arg, "neighbors") == 0) {
                nt_show(g_mgmt_nt, client_fd);
            } else {
                dprintf(client_fd, "ERROR: usage: show <route|neighbors>\n");
            }
        } else if (strcmp(cmd, "link") == 0) {
            char subcmd[64], nid[256];
            nid[0] = '\0';
            sscanf(arg, "%63s %255s", subcmd, nid);
            if (strcmp(subcmd, "down") == 0) {
                cmd_link_down(client_fd, nid);
            } else if (strcmp(subcmd, "up") == 0) {
                cmd_link_up(client_fd, nid, g_mgmt_udp_fd);
            } else {
                dprintf(client_fd, "ERROR: usage: link <down|up> <neighbor_id>\n");
            }
        } else {
            dprintf(client_fd, "ERROR: unknown command '%s'. "
                    "Try: show route, show neighbors, "
                    "link down <id>, link up <id>, quit\n", cmd);
        }

        dprintf(client_fd, "\n> ");
    }

    fclose(stream);
    return NULL;
}

// 管理服务主线程
static void *mgmt_server_thread(void *arg)
{
    (void)arg;

    // 循环接受客户端连接，每个连接创建一个独立线程处理
    while (g_mgmt_running) {
        struct sockaddr_storage client_addr;
        socklen_t addr_len = sizeof(client_addr);
        // accept 阻塞等待连接
        int client_fd = accept(g_mgmt_tcp_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) { // 如果 accept 出错 检查一下
            if (!g_mgmt_running) break;
            if (errno == EINTR) continue;
            log_printf("ERROR: accept failed: %s", strerror(errno));
            continue;
        }

        // 打印客户端连接信息
        char peer[64];
        log_printf("mgmt connection from %s", sockaddr_str((const struct sockaddr *)&client_addr,
                                                            peer, sizeof(peer)));

        // 创建线程处理客户端连接，函数参数传递 client_fd
        pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        // 需要设置分离态 PTHREAD_CREATE_DETACHED 
        // 因为客户端处理线程在完成后会自动退出，不需要主线程 join
        // 每个客户端连接创建一个线程，处理完 quit 后线程自己 return NULL 结束
        // 不用分离态 且不 join 的话 线程就会变成僵尸线程
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        // 给线程传一个 client_fd 来指定 client 是哪个，用于和 client 交互
        // client_fd 的 int 类型转换成 intptr_t 后直接与指针类型对齐了，所以可以直接转换成 void * 
        if (pthread_create(&tid, &attr, mgmt_client_handler, (void *)(intptr_t)client_fd) != 0) {
            log_printf("ERROR: failed to create client handler thread");
            close(client_fd);
        }
        pthread_attr_destroy(&attr); // 销毁 attr 属性对象
    }

    return NULL;
}

// 启动 TCP 管理监听线程
// 在独立线程中运行，rt 和 nt 与主线程共享（通过 rt->lock 保护）
// 返回 0 成功，-1 失败
int tcp_mgmt_start(int port, route_table_t *rt, neighbor_table_t *nt, int udp_fd)
{
    // 拿到主线程传来的路由表和邻居表指针，以及 UDP socket fd，保存在全局变量中供管理线程使用
    g_mgmt_rt = rt;
    g_mgmt_nt = nt;
    g_mgmt_udp_fd = udp_fd;

    // 创建 TCP 监听 socket，优先尝试 IPv6 双栈，如果失败再尝试 IPv4
    g_mgmt_tcp_fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (g_mgmt_tcp_fd < 0) {
        g_mgmt_tcp_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (g_mgmt_tcp_fd < 0) {
            log_printf("ERROR: mgmt socket failed: %s", strerror(errno));
            return -1;
        }

        int opt = 1;
        setsockopt(g_mgmt_tcp_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(port);

        if (bind(g_mgmt_tcp_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            log_printf("ERROR: mgmt bind failed: %s", strerror(errno));
            close(g_mgmt_tcp_fd);
            g_mgmt_tcp_fd = -1;
            return -1;
        }
    } else { // 成功创建 IPv6 socket，设置为双栈并绑定
        int opt = 1;
        setsockopt(g_mgmt_tcp_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        setsockopt(g_mgmt_tcp_fd, IPPROTO_IPV6, IPV6_V6ONLY, &(int){0}, sizeof(int));

        struct sockaddr_in6 addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin6_family = AF_INET6;
        addr.sin6_addr = in6addr_any;
        addr.sin6_port = htons(port);

        if (bind(g_mgmt_tcp_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            log_printf("ERROR: mgmt bind failed: %s", strerror(errno));
            close(g_mgmt_tcp_fd);
            g_mgmt_tcp_fd = -1;
            return -1;
        }
    }

    if (listen(g_mgmt_tcp_fd, 8) < 0) {
        log_printf("ERROR: mgmt listen failed: %s", strerror(errno));
        close(g_mgmt_tcp_fd);
        g_mgmt_tcp_fd = -1;
        return -1;
    }

    g_mgmt_running = 1;
    if (pthread_create(&g_mgmt_thread, NULL, mgmt_server_thread, NULL) != 0) {
        log_printf("ERROR: failed to create mgmt server thread");
        close(g_mgmt_tcp_fd);
        g_mgmt_tcp_fd = -1;
        g_mgmt_running = 0;
        return -1;
    }

    log_printf("TCP management interface listening on port %d", port);
    return 0;
}

void tcp_mgmt_stop(void)
{
    g_mgmt_running = 0;

    if (g_mgmt_tcp_fd >= 0) {
        shutdown(g_mgmt_tcp_fd, SHUT_RDWR);
        close(g_mgmt_tcp_fd);
        g_mgmt_tcp_fd = -1;
    }

    pthread_join(g_mgmt_thread, NULL);
    log_printf("TCP management interface stopped");
}
