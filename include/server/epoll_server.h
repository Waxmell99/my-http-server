#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace personal_cloud {

struct EpollServerConfig {
    std::uint16_t port{9000};
    int backlog{1024};
    std::size_t maximum_events{1024};
    std::chrono::seconds idle_timeout{30};
    // 开启后输出每个连接和请求的日志；压测时建议保持关闭。
    bool verbose_logging{false};
};

// 启动单线程 epoll 事件循环。正常情况下会一直运行，初始化或等待事件失败时返回 1。
int run_epoll_server(const EpollServerConfig& config);

}  // namespace personal_cloud
