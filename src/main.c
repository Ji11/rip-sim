#include "ripd.h"
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>

// 全局路由器实例，信号处理器需要访问
static router g_router;

// SIGCHLD 处理器：回收僵尸子进程，防止资源泄漏
static void sigchld_handler(int sig)
{
    (void)sig;
    int saved_errno = errno;
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
    errno = saved_errno;
}

// SIGINT / SIGTERM 处理器：触发优雅关闭
static void sigint_handler(int sig)
{
    (void)sig;
    log_printf("received signal, shutting down...");
    router_shutdown(&g_router);
}

static int setup_signals(void)
{
    struct sigaction sa;

    // SIGPIPE：忽略（写入已关闭的 TCP 连接不应导致进程崩溃）
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &sa, NULL) < 0) {
        perror("sigaction(SIGPIPE)");
        return -1;
    }

    // SIGCHLD：回收子进程
    sa.sa_handler = sigchld_handler;
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGCHLD, &sa, NULL) < 0) {
        perror("sigaction(SIGCHLD)");
        return -1;
    }

    // SIGINT / SIGTERM：优雅退出
    sa.sa_handler = sigint_handler;
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, NULL) < 0) {
        perror("sigaction(SIGINT)");
        return -1;
    }
    if (sigaction(SIGTERM, &sa, NULL) < 0) {
        perror("sigaction(SIGTERM)");
        return -1;
    }

    return 0;
}

static void print_usage(const char *prog)
{
    fprintf(stderr, "Usage: %s <config_file>\n", prog);
    fprintf(stderr, "  config_file  Path to router configuration\n");
    fprintf(stderr, "\nExample:\n");
    fprintf(stderr, "  %s ../config/router1.conf\n", prog);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    const char *config_path = argv[1];

    // 尽早注册信号处理器
    if (setup_signals() < 0) {
        return 1;
    }

    // 初始化路由器
    router_init(&g_router);

    // 加载配置文件
    if (config_load(config_path, &g_router) < 0) {
        fprintf(stderr, "FATAL: failed to load config from %s\n", config_path);
        router_destroy(&g_router);
        return 1;
    }

    // 绑定 socket
    if (router_bind(&g_router) < 0) {
        fprintf(stderr, "FATAL: failed to bind sockets\n");
        router_destroy(&g_router);
        return 1;
    }

    // 打印启动信息
    log_printf("=== RIP Router Simulator ===");
    log_printf("Router ID: %s", g_router.id);
    log_printf("UDP port:  %d", g_router.udp_port);
    log_printf("TCP port:  %d", g_router.tcp_port);
    log_printf("============================");

    // 进入主事件循环（阻塞直到收到关闭信号）
    int ret = router_run(&g_router);

    // 清理资源
    router_destroy(&g_router);

    log_printf("router %s exited with code %d", g_router.id, ret);
    return ret;
}
