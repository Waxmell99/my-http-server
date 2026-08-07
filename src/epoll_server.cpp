#include "server/epoll_server.h"

#include "common/log.h"
#include "http/http_request.h"
#include "http/http_response.h"
#include "http/router.h"
#include "server/http_server.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <sys/epoll.h>
#include <sys/socket.h>

namespace personal_cloud {
namespace {

constexpr std::size_t read_buffer_size = 4096;
constexpr std::size_t maximum_header_size = 16 * 1024;
constexpr std::size_t maximum_body_size = 64 * 1024;
constexpr std::string_view header_terminator = "\r\n\r\n";
constexpr int epoll_wait_timeout_milliseconds = 1000;
constexpr std::chrono::seconds timeout_check_interval{1};

using Clock = std::chrono::steady_clock;

enum class ConnectionState {
    receiving,
    sending,
};

struct ClientConnection {
    std::string request_buffer;
    std::string response_buffer;
    std::size_t sent_size{0};
    Clock::time_point last_activity{Clock::now()};
    ConnectionState state{ConnectionState::receiving};
};

HttpResponse make_error_response(
    int status_code,
    std::string reason,
    std::string body) {
    return {
        status_code,
        std::move(reason),
        "text/plain; charset=utf-8",
        std::move(body),
    };
}

class EpollEventLoop final {
public:
    explicit EpollEventLoop(EpollServerConfig config)
        : config_(std::move(config)) {}

    ~EpollEventLoop() {
        for (const auto& [client_fd, connection] : clients_) {
            static_cast<void>(connection);
            close_socket(client_fd);
        }

        close_socket(listening_fd_);
        close_socket(epoll_fd_);
    }

    EpollEventLoop(const EpollEventLoop&) = delete;
    EpollEventLoop& operator=(const EpollEventLoop&) = delete;

    int run() {
        if (!initialize()) {
            return 1;
        }

        while (true) {
            const int ready_count = ::epoll_wait(
                epoll_fd_,
                events_.data(),
                static_cast<int>(events_.size()),
                epoll_wait_timeout_milliseconds);

            if (ready_count == -1) {
                if (errno == EINTR) {
                    continue;
                }

                std::perror("epoll_wait");
                return 1;
            }

            for (int index = 0; index < ready_count; ++index) {
                const epoll_event event = events_[index];
                const int socket_fd = event.data.fd;

                if (socket_fd == listening_fd_) {
                    accept_ready_clients();
                    continue;
                }

                handle_client_event(socket_fd, event.events);
            }

            remove_idle_clients();
        }
    }

private:
    bool initialize() {
        if (config_.backlog <= 0 || config_.maximum_events == 0 ||
            config_.maximum_events >
                static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
            config_.idle_timeout <= std::chrono::seconds::zero()) {
            write_log(std::cerr, "Invalid epoll server configuration.\n");
            return false;
        }

        listening_fd_ = create_listening_socket(
            config_.port, config_.backlog);
        if (listening_fd_ == -1) {
            return false;
        }

        if (!set_socket_nonblocking(listening_fd_)) {
            return false;
        }

        epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
        if (epoll_fd_ == -1) {
            std::perror("epoll_create1");
            return false;
        }

        epoll_event listening_event {};
        listening_event.events = EPOLLIN | EPOLLET;
        listening_event.data.fd = listening_fd_;
        if (::epoll_ctl(
                epoll_fd_,
                EPOLL_CTL_ADD,
                listening_fd_,
                &listening_event) == -1) {
            std::perror("epoll_ctl add listening Socket");
            return false;
        }

        events_.resize(config_.maximum_events);
        write_log(
            std::cout,
            "Epoll server listening on port ",
            config_.port,
            ", fd = ",
            listening_fd_,
            '\n');
        return true;
    }

    void accept_ready_clients() {
        while (true) {
            const int client_fd = accept_client(listening_fd_);
            if (client_fd == -1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return;
                }

                // accept_client 已经输出了真正的系统错误；本次就绪事件到此结束。
                return;
            }

            if (!set_socket_nonblocking(client_fd)) {
                close_socket(client_fd);
                continue;
            }

            auto [connection, inserted] = clients_.try_emplace(client_fd);
            if (!inserted) {
                close_socket(client_fd);
                continue;
            }

            epoll_event client_event {};
            client_event.events = EPOLLIN | EPOLLRDHUP | EPOLLET;
            client_event.data.fd = client_fd;
            if (::epoll_ctl(
                    epoll_fd_,
                    EPOLL_CTL_ADD,
                    client_fd,
                    &client_event) == -1) {
                std::perror("epoll_ctl add client Socket");
                clients_.erase(connection);
                close_socket(client_fd);
                continue;
            }

            if (config_.verbose_logging) {
                write_log(
                    std::cout, "Client connected, fd = ", client_fd, '\n');
            }
        }
    }

    void handle_client_event(int client_fd, std::uint32_t event_flags) {
        const auto found = clients_.find(client_fd);
        if (found == clients_.end()) {
            return;
        }

        ClientConnection& connection = found->second;
        bool keep_connection = true;

        if ((event_flags & EPOLLERR) != 0U) {
            keep_connection = false;
        } else if (connection.state == ConnectionState::receiving &&
                   (event_flags & (EPOLLIN | EPOLLRDHUP)) != 0U) {
            keep_connection = receive_request(client_fd, connection);
        } else if (connection.state == ConnectionState::sending &&
                   (event_flags & EPOLLOUT) != 0U) {
            keep_connection = send_response(client_fd, connection);
        }

        if (keep_connection && (event_flags & EPOLLHUP) != 0U) {
            keep_connection = false;
        }

        if (!keep_connection) {
            close_client(client_fd);
        }
    }

    bool receive_request(int client_fd, ClientConnection& connection) {
        std::array<char, read_buffer_size> buffer {};

        while (true) {
            const ssize_t received = ::recv(
                client_fd, buffer.data(), buffer.size(), 0);

            if (received > 0) {
                connection.last_activity = Clock::now();
                connection.request_buffer.append(
                    buffer.data(), static_cast<std::size_t>(received));

                if (header_is_too_large(connection.request_buffer)) {
                    write_log(
                        std::cerr,
                        "HTTP Header is too large, fd = ",
                        client_fd,
                        '\n');
                    return queue_response(
                        client_fd,
                        connection,
                        make_error_response(
                            431,
                            "Request Header Fields Too Large",
                            "Request Header Fields Too Large\n"));
                }

                HttpRequest request;
                const HttpParseResult parse_result = parse_http_request(
                    connection.request_buffer,
                    request,
                    maximum_body_size);

                if (parse_result == HttpParseResult::incomplete) {
                    continue;
                }

                if (parse_result == HttpParseResult::bad_request) {
                    write_log(
                        std::cerr,
                        "Malformed HTTP request, fd = ",
                        client_fd,
                        '\n');
                    return queue_response(
                        client_fd,
                        connection,
                        make_error_response(
                            400, "Bad Request", "Bad Request\n"));
                }

                if (parse_result == HttpParseResult::payload_too_large) {
                    write_log(
                        std::cerr,
                        "HTTP Body is too large, fd = ",
                        client_fd,
                        '\n');
                    return queue_response(
                        client_fd,
                        connection,
                        make_error_response(
                            413,
                            "Payload Too Large",
                            "Payload Too Large\n"));
                }

                if (config_.verbose_logging) {
                    write_log(
                        std::cout,
                        "Parsed request, fd = ", client_fd,
                        ", method = ", request.method,
                        ", path = ", request.path,
                        ", body = ", request.body.size(),
                        " bytes\n");
                }
                return queue_response(
                    client_fd, connection, route_request(request));
            }

            if (received == 0) {
                if (config_.verbose_logging) {
                    write_log(
                        std::cout,
                        "Client closed while receiving, fd = ",
                        client_fd,
                        '\n');
                }
                return false;
            }

            if (errno == EINTR) {
                continue;
            }

            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return true;
            }

            std::perror("recv");
            return false;
        }
    }

    bool send_response(int client_fd, ClientConnection& connection) {
        while (connection.sent_size < connection.response_buffer.size()) {
            const ssize_t sent = ::send(
                client_fd,
                connection.response_buffer.data() + connection.sent_size,
                connection.response_buffer.size() - connection.sent_size,
                MSG_NOSIGNAL);

            if (sent > 0) {
                connection.sent_size += static_cast<std::size_t>(sent);
                connection.last_activity = Clock::now();
                continue;
            }

            if (sent == -1 && errno == EINTR) {
                continue;
            }

            if (sent == -1 &&
                (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return true;
            }

            if (sent == -1) {
                std::perror("send");
            }
            return false;
        }

        if (config_.verbose_logging) {
            write_log(
                std::cout,
                "Response sent; closing client fd = ",
                client_fd,
                '\n');
        }
        return false;
    }

    bool queue_response(
        int client_fd,
        ClientConnection& connection,
        const HttpResponse& response) {
        connection.response_buffer = serialize_http_response(response);
        connection.sent_size = 0;
        connection.state = ConnectionState::sending;
        connection.last_activity = Clock::now();

        // 当前服务器每个连接只处理一个请求，响应完成后关闭连接。
        connection.request_buffer.clear();

        epoll_event client_event {};
        client_event.events = EPOLLOUT | EPOLLRDHUP | EPOLLET;
        client_event.data.fd = client_fd;
        if (::epoll_ctl(
                epoll_fd_,
                EPOLL_CTL_MOD,
                client_fd,
                &client_event) == -1) {
            std::perror("epoll_ctl modify client Socket");
            return false;
        }

        return true;
    }

    static bool header_is_too_large(const std::string& request_buffer) {
        const std::size_t headers_end =
            request_buffer.find(header_terminator);
        if (headers_end == std::string::npos) {
            return request_buffer.size() > maximum_header_size;
        }

        return headers_end + header_terminator.size() > maximum_header_size;
    }

    void remove_idle_clients() {
        const Clock::time_point now = Clock::now();
        if (now < next_timeout_check_) {
            return;
        }
        next_timeout_check_ = now + timeout_check_interval;

        std::vector<int> receiving_timeouts;
        std::vector<int> sending_timeouts;

        for (const auto& [client_fd, connection] : clients_) {
            if (now - connection.last_activity < config_.idle_timeout) {
                continue;
            }

            if (connection.state == ConnectionState::receiving) {
                receiving_timeouts.push_back(client_fd);
            } else {
                sending_timeouts.push_back(client_fd);
            }
        }

        for (int client_fd : receiving_timeouts) {
            const auto found = clients_.find(client_fd);
            if (found == clients_.end()) {
                continue;
            }

            write_log(
                std::cerr,
                "Client receive timeout, fd = ",
                client_fd,
                '\n');
            if (!queue_response(
                    client_fd,
                    found->second,
                    make_error_response(
                        408,
                        "Request Timeout",
                        "Request Timeout\n"))) {
                close_client(client_fd);
            }
        }

        for (int client_fd : sending_timeouts) {
            write_log(
                std::cerr,
                "Client send timeout, fd = ",
                client_fd,
                '\n');
            close_client(client_fd);
        }
    }

    void close_client(int client_fd) {
        static_cast<void>(::epoll_ctl(
            epoll_fd_, EPOLL_CTL_DEL, client_fd, nullptr));
        clients_.erase(client_fd);
        close_socket(client_fd);
    }

    EpollServerConfig config_;
    int listening_fd_{-1};
    int epoll_fd_{-1};
    std::vector<epoll_event> events_;
    std::unordered_map<int, ClientConnection> clients_;
    Clock::time_point next_timeout_check_{Clock::now()};
};

}  // namespace

int run_epoll_server(const EpollServerConfig& config) {
    EpollEventLoop event_loop(config);
    return event_loop.run();
}

}  // namespace personal_cloud
