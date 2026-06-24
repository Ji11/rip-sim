# 任务书要求 → 代码映射

本文档将《网络编程课设任务书》每一项要求对应到 `rip-sim` 项目的具体源码文件和函数，方便快速定位与验收讲解。

---

## 一、课程目标

| 课程目标 | 实现位置 | 说明 |
|----------|----------|------|
| TCP/UDP 套接字编程 | `src/rip.c` → `rip_send_response()`, `rip_recv()`, `config.c` → `resolve_addr()` | UDP 用于 RIP 协议通信 |
| I/O 复用（select/poll/epoll） | `src/router.c` → `router_run()` | 使用 `select()` 同时监听 UDP 和 TCP socket |
| 多进程/多线程 | `src/tcp_mgmt.c` → `mgmt_server_thread()`, `mgmt_client_handler()` | 每个 TCP 管理连接一个线程（pthread） |
| 地址转换（getaddrinfo） | `src/config.c` → `resolve_addr()` | 统一解析 IP 地址和主机名（如 router2.local） |
| 信号处理（SIGCHLD, SIGPIPE） | `src/main.c` → `setup_signals()`, `sigchld_handler()` | SIGPIPE 忽略, SIGCHLD 回收僵尸进程 |
| 异常处理与资源清理 | 各 .c 文件中 `close()`, `free()`, `router_destroy()` | 所有错误路径均有资源释放 |

---

## 二、功能要求

### 2.2.1 功能 1：路由器初始化

| 任务书要求 | 代码位置 |
|------------|----------|
| 从配置文件读取路由器 ID、UDP 端口、TCP 端口 | `src/config.c` → `config_load()`, 解析 `id`, `udp_port`, `tcp_port` 行 |
| 从配置文件读取邻居信息（地址+端口） | `src/config.c` → `config_load()`, 解析 `neighbor` 行 |
| 邻居地址可以是 IP（127.0.0.1）或主机名（router2.local） | `src/config.c` → `resolve_addr()`, 先用 `AI_NUMERICHOST` 试，失败则用主机名解析 |
| 使用 `getaddrinfo` 统一解析地址 | `src/config.c` → `resolve_addr()`, 调用 `getaddrinfo()` |
| 创建 UDP 套接字，绑定端口，启用 `SO_REUSEADDR` | `src/router.c` → `router_bind()`, `setsockopt(..., SO_REUSEADDR, ...)` |
| 配置文件示例 | `config/router1.conf`, `config/router2.conf`, `config/router3.conf` |

### 2.2.2 功能 2：路由表维护

| 任务书要求 | 代码位置 |
|------------|----------|
| 数据结构：目标网络、下一跳、度量 | `src/ripd.h` → `route` 结构体 |
| 初始时只知道自己的直连网络和邻居 | `src/config.c` → `config_load()`, 解析 `network` 行，调用 `rt_upsert(..., metric=1, from_neighbor=-1)` 添加直连路由 |
| 支持通过 TCP 管理接口查看路由表（功能 6） | `src/route_table.c` → `rt_show()` |
| Bellman-Ford 更新算法 | `src/route_table.c` → `rt_upsert()`, 实现四段决策逻辑：直连路由保护 → 同邻居无条件更新 → 同跳数跳过 → 更优路由替换 |

### 2.2.3 功能 3：RIP 协议实现

| 任务书要求 | 代码位置 |
|------------|----------|
| 周期性发送：每 30 秒向邻居发送路由表 | `src/router.c` → `periodic_update()`, 检查 `now - last_periodic >= 30` |
| 使用 select/poll/epoll 的超时机制实现 30 秒周期发送 | `src/router.c` → `router_run()`, `select(..., tv_sec=1)`, 每秒醒来检查 |
| 接收并处理邻居路由更新 | `src/rip.c` → `rip_recv()`, 解析 RIP 报文，调用 `rt_upsert()` |
| Bellman-Ford 更新路由 | `src/route_table.c` → `rt_upsert()`, 实现标准 Bellman-Ford：新度量 = 收到度量 + 1 |
| 触发更新：路由变化时立即发送，不等定时器 | `src/router.c` → `triggered_update()`, 检测 `rt.changed` 标志，变化时立即发送 |
| RIP 报文格式定义 | `src/ripd.h` → `rip_header`, `rip_route` 结构体（22 字节/条目） |
| 报文构造与发送 | `src/rip.c` → `rip_send_response()` |
| 启动时发送 RIP Request 请求邻居路由 | `src/router.c` → `router_run()`, 调用 `rip_send_request()` |

### 2.2.4 功能 4：邻居失效检测 & 路由超时

| 任务书要求 | 代码位置 |
|------------|----------|
| **方式一：180 秒超时检测（标准 RIP）** | |
| 记录每个邻居最后一次收到更新的时间 | `src/neighbor.c` → `nt_update_last_recv()`, 更新 `last_recv` 字段 |
| 180 秒未收到更新 → 判定失效 | `src/router.c` → `check_neighbor_timeouts()`, 遍历邻居比较 `now - last_recv > 180` |
| 失效后执行毒性逆转（功能 5） | `src/router.c` → `check_neighbor_timeouts()`, 调用 `rt_poison_from_neighbor()` |
| **方式二：手动链路控制（快速测试）** | |
| `link down <neighbor_id>` 命令 | `src/tcp_mgmt.c` → `cmd_link_down()`, 调 `nt_set_active(idx, 0)`, 调 `rt_poison_from_neighbor()` |
| `link up <neighbor_id>` 命令 | `src/tcp_mgmt.c` → `cmd_link_up()`, 调 `nt_set_active(idx, 1)`, 发送 RIP Request |
| **路由超时删除** | |
| 路由标记为不可达（metric=16）后，保留 120 秒 | `src/route_table.c` → `rt_upsert()`, 标记 `metric=RIP_INFINITY` |
| 120 秒后从路由表删除 | `src/route_table.c` → `rt_garbage_collect()`, 在 `router_run()` 中每轮调用 |

### 2.2.5 功能 5：毒性逆转

| 任务书要求 | 代码位置 |
|------------|----------|
| 向邻居 X 发送时，将从 X 学到的路由度量设为 16 | `src/rip.c` → `rip_send_response()`, `if (poison_reverse_idx >= 0 && e->from_neighbor == poison_reverse_idx) metric = 16` |
| 检测到邻居失效时，毒化所有来自该邻居的路由 | `src/route_table.c` → `rt_poison_from_neighbor()`, 将所有 `from_neighbor == nbr_idx` 的路由设为 metric=16 |

### 2.2.6 功能 6：TCP 管理接口

| 任务书要求 | 代码位置 |
|------------|----------|
| TCP 端口监听 | `src/tcp_mgmt.c` → `tcp_mgmt_start()`, 创建 TCP socket, bind, listen |
| `show route` — 显示路由表 | `src/route_table.c` → `rt_show()` |
| `show neighbors` — 显示邻居状态 | `src/neighbor.c` → `nt_show()` |
| `link down <neighbor_id>` — 手动断开链路 | `src/tcp_mgmt.c` → `cmd_link_down()`, 设置邻居 inactive + 毒性逆转 |
| `link up <neighbor_id>` — 恢复链路 | `src/tcp_mgmt.c` → `cmd_link_up()`, 设置邻居 active + 发送 RIP Request |
| `quit` — 断开管理连接 | `src/tcp_mgmt.c` → `mgmt_client_handler()`, 检测 `quit` 命令 break |
| 并发处理多个管理连接 | `src/tcp_mgmt.c` → `mgmt_server_thread()`, `accept()` 后 `pthread_create()` 为每个连接创建线程 |
| 命令解析 | `src/tcp_mgmt.c` → `mgmt_client_handler()`, 用 `sscanf` 解析命令和参数 |

---

## 三、技术要求

| 技术要求 | 实现位置 | 说明 |
|----------|----------|------|
| (1) I/O 复用：使用 select/poll/epoll | `src/router.c` → `router_run()` | 使用 `select()` 同时监听 UDP socket（RIP 数据）和定时器（周期性更新） |
| (2) 使用 select/poll/epoll 的超时机制实现 30 秒周期发送 | `src/router.c` → `router_run()` | `select(..., tv_sec=1)` 超时驱动，每秒醒来检查 `periodic_update()`，到 30 秒即触发 |
| (3) 使用 getaddrinfo 解析地址 | `src/config.c` → `resolve_addr()` | 支持 IP 地址和主机名 |
| (4) TCP 并发连接处理（线程/I/O 复用/多进程选一） | `src/tcp_mgmt.c` → `mgmt_server_thread()` | 选用 pthread 多线程模型，每个 TCP 连接一个线程 |
| (5) 信号处理：SIGPIPE, SIGCHLD | `src/main.c` → `setup_signals()` | SIGPIPE → `SIG_IGN`, SIGCHLD → `sigchld_handler()` 回收子进程 |
| (6) 支持 IPv6（加分项） | `src/config.c` → `resolve_addr()`, `src/router.c` → `router_bind()`, `src/utils.c` → `sockaddr_str()`, `parse_cidr()` | 地址解析用 `AF_UNSPEC`，路由表条目支持 AF_INET6，报文格式支持 16 字节 IPv6 地址 |

---

## 四、测试要求对照

| 测试要求 | 验证命令 |
|----------|----------|
| 3.1 测试环境：3 路由器网状拓扑 | 在 3 个终端分别启动：`./ripd config/router1.conf` / `router2.conf` / `router3.conf` |
| 3.2 基本功能测试 & 路由收敛验证 | `nc 127.0.0.1 8021` → `show route`，应看到全部 3 条路由 |
| 3.3 毒性反转测试 | `nc 127.0.0.1 8021` → `link down 2` → `show route`，10.0.2.0/24 变 metric=16 [GC] |
| 3.4 邻居失效自动检测（180s 超时） | 直接关闭路由器 2 进程，等待 180 秒后路由器 1 的 `show neighbors` 显示 R2 状态为 DOWN |
| 3.5 TCP 管理接口测试 | `nc 127.0.0.1 8021` 依次测试 `show route`, `show neighbors`, `link down`, `link up`, `quit` |

---

## 五、快速导航：验收讲代码时的阅读顺序

1. **入口和整体流程**：`src/main.c` → `src/router.c`（`router_run()` 主循环）
2. **数据结构**：`src/ripd.h`（路由条目 `route`、邻居条目 `neighbor`）
3. **RIP 协议核心**：`src/rip.c`（报文收发）→ `src/route_table.c`（Bellman-Ford 更新）
4. **关键算法**：`src/route_table.c` → `rt_upsert()`（四段决策逻辑）、`src/rip.c` → `rip_send_response()`（毒性逆转）
5. **I/O 复用与定时**：`src/router.c` → `router_run()`（select 驱动的主循环）、`periodic_update()`（30s 周期）
6. **并发处理**：`src/tcp_mgmt.c` → `mgmt_server_thread()`（accept）+ `mgmt_client_handler()`（命令解析）
7. **配置与地址解析**：`src/config.c` → `config_load()` + `resolve_addr()`（getaddrinfo）
8. **信号处理**：`src/main.c` → `setup_signals()`
