
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace personal_cloud {

enum class ReceiveStatus {
    data,
    peer_closed,
    timeout,
    error,
};

struct ReceiveResult {
    ReceiveStatus status;
    std::size_t bytes_received;
};

// 创建、绑定并开始监听一个 nonblocking、close-on-exec TCP Socket。
// 成功时返回 Socket 文件描述符，失败时返回 -1。
int create_listening_socket(
    std::string_view bind_address,
    std::uint16_t port,
    int backlog);

// 接受一个 nonblocking、close-on-exec 客户端连接。
// 成功时返回客户端 Socket 文件描述符，失败时返回 -1。
int accept_client(int listening_fd);

// 将 Socket 设置为非阻塞模式，供 epoll 事件循环使用。
bool set_socket_nonblocking(int socket_fd);

// 从客户端接收一次数据，并区分数据、关闭、超时和错误。
ReceiveResult receive_data(
    int client_fd,
    char* buffer,
    std::size_t buffer_size);

// 将指定长度的数据完整发送给客户端。
// 全部发送成功时返回 true，发生错误时返回 false。
bool send_all(int client_fd, const char* data, std::size_t data_size);

// 关闭一个有效的 Socket 文件描述符。
void close_socket(int socket_fd) noexcept;

// 设置 Socket 的收发超时；0 表示禁用对应超时。
bool set_socket_timeouts(
    int socket_fd,
    int receive_timeout_seconds,
    int send_timeout_seconds);

}  // namespace personal_cloud
