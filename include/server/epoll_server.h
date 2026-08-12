#pragma once

#include "http/http_request.h"
#include "http/http_response.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <stop_token>

namespace personal_cloud {

using ApplicationTask = std::function<HttpResponse()>;
using ApplicationTaskFactory =
    std::function<std::optional<ApplicationTask>(const HttpRequest&)>;

struct EpollServerConfig {
    std::uint16_t port{9000};
    int backlog{1024};
    std::size_t maximum_events{1024};
    std::size_t maximum_connections{10'000};
    std::chrono::seconds idle_timeout{30};
    // 开启后输出每个连接和请求的日志；压测时建议保持关闭。
    bool verbose_logging{false};
    // 将 SIGINT/SIGTERM 作为 epoll 事件处理并在返回前释放所有 Socket。
    // 多线程嵌入时应由调用方在其他线程屏蔽这些信号，或关闭此选项并使用 stop_token。
    bool handle_termination_signals{true};
    // 为空时使用内置示例路由；应用后端可以注入自己的请求处理入口。
    std::function<HttpResponse(const HttpRequest&)> request_handler;
    // 任务工厂只应进行快速的路由判断和参数复制。返回任务时，任务会进入有界
    // 工作线程池；返回 nullopt 时，请求继续由上面的同步处理器处理。
    ApplicationTaskFactory request_task_factory;
    std::size_t application_worker_count{4};
    std::size_t application_queue_size{256};
};

// 启动单线程 epoll 事件循环。请求停止时返回 0，初始化或等待事件失败时返回 1。
int run_epoll_server(
    const EpollServerConfig& config,
    std::stop_token stop_token = {});

}  // namespace personal_cloud
