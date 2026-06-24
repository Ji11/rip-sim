# RIP 路由协议模拟系统

网络编程课程设计 — 基于 RIP（RFC 1058）的简化路由协议模拟器。

## 功能

- **RIP 协议核心**：Bellman-Ford 距离向量算法，30 秒周期性更新，触发更新
- **毒性逆转**：发送给邻居时，将来自该邻居的路由度量设为 16（不可达），防止路由环路
- **邻居失效检测**：180 秒未收到更新即判定邻居失效，自动毒性化相关路由
- **路由超时回收**：度量=16 的路由 120 秒后自动删除
- **TCP 管理接口**：远程查询路由表/邻居状态、手动断开/恢复链路
- **IPv4/IPv6 双栈**：自动检测邻居地址族，支持 IPv6（加分项）
- **信号处理**：SIGPIPE 忽略、SIGCHLD 回收子进程、SIGINT/SIGTERM 优雅退出
- **I/O 复用**：select() 统一管理 UDP 和 TCP socket

## 构建

```bash
# 安装编译依赖（如未安装）
sudo apt install build-essential libconfig-dev

# 编译
cd rip-sim
make
```

## 运行

在三个终端中分别启动：

```bash
./ripd config/router1.conf    # 路由器 1 (UDP 5201, TCP 8021)
./ripd config/router2.conf    # 路由器 2 (UDP 5202, TCP 8022)
./ripd config/router3.conf    # 路由器 3 (UDP 5203, TCP 8023)
```

或使用便捷目标：
```bash
make run1    # 启动路由器 1
make run2    # 启动路由器 2
make run3    # 启动路由器 3
```

## TCP 管理接口

```bash
telnet 127.0.0.1 8021    # 连接到路由器 1
# 或
nc 127.0.0.1 8021
```

支持命令：

| 命令 | 功能 |
|------|------|
| `show route` | 查看路由表 |
| `show neighbors` | 查看邻居状态 |
| `link down <id>` | 断开指定邻居链路 |
| `link up <id>` | 恢复指定邻居链路 |
| `quit` | 退出连接 |

## 配置文件格式（libconfig）

```ini
# 路由器基本信息
id        = "1";
udp_port  = 5201;
tcp_port  = 8021;

# 邻居列表（链状拓扑 R1 只连 R2）
neighbors = (
    { id = "2";  address = "127.0.0.1";  port = 5202; }
);

# 直连网络
networks = (
    { address = "10.0.1.0/24"; }
);
```

支持 IPv4 和 IPv6 地址，邻居 `address` 可以是 IP 或主机名（如 `router2.local`）。

## 测试拓扑

```
    R1 ——— R2 ——— R3
```

链状拓扑，R2 作为中间节点中继 R1 和 R3 之间的路由。

- R1: 直连 10.0.1.0/24，仅连 R2
- R2: 直连 10.0.2.0/24，同时连 R1 和 R3
- R3: 直连 10.0.3.0/24，仅连 R2

## 调试

所有日志输出到 stderr，格式为 `[HH:MM:SS] 消息`，可重定向到文件：

```bash
./ripd config/router1.conf 2>rip.log
```

## 项目结构

```
rip-sim/
├── Makefile
├── README.md
├── config/
│   ├── router1.conf
│   ├── router2.conf
│   └── router3.conf
└── src/
    ├── ripd.h          # 共享头文件（类型、常量、函数声明）
    ├── main.c          # 入口、信号处理
    ├── config.c        # 配置文件解析（getaddrinfo）
    ├── router.c        # 路由器核心、主循环（select）
    ├── route_table.c   # 路由表 CRUD、Bellman-Ford
    ├── neighbor.c      # 邻居状态管理
    ├── rip.c           # RIP 协议报文、收发
    ├── tcp_mgmt.c      # TCP 管理接口（pthread）
    └── utils.c         # 日志、地址工具
```

## 清理

```bash
make clean
```
