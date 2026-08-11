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
#include <pthread.h>
#include <signal.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <unistd.h>

namespace personal_cloud {
namespace {

constexpr std::size_t read_buffer_size = 4096;
constexpr std::size_t maximum_header_size = 16 * 1024;
constexpr std::size_t maximum_body_size = 64 * 1024;
constexpr std::string_view header_terminator = "\r\n\r\n";
constexpr int epoll_wait_timeout_milliseconds = 1000;
constexpr std::chrono::seconds timeout_check_interval{1};
constexpr std::chrono::seconds accept_retry_interval{1};
constexpr std::uint32_t listening_event_flags =
    EPOLLIN | EPOLLET | EPOLLONESHOT;

using Clock = std::chrono::steady_clock;

enum class ConnectionState {
    receiving,
    sending,
};

enum class SignalReadResult {
    no_signal,
    termination_requested,
    error,
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
    EpollEventLoop(EpollServerConfig config, std::stop_token stop_token)
        : config_(std::move(config)), stop_token_(stop_token) {}

    ~EpollEventLoop() {
        for (const auto& [client_fd, connection] : clients_) {
            static_cast<void>(connection);
            close_socket(client_fd);
        }

        close_socket(listening_fd_);
        close_socket(epoll_fd_);
        close_socket(signal_fd_);
        close_socket(reserve_fd_);

        if (signal_mask_changed_) {
            const int mask_error = ::pthread_sigmask(
                SIG_SETMASK, &previous_signal_mask_, nullptr);
            if (mask_error != 0) {
                errno = mask_error;
                std::perror("restore signal mask");
            }
        }
    }

    EpollEventLoop(const EpollEventLoop&) = delete;
    EpollEventLoop& operator=(const EpollEventLoop&) = delete;

    int run() {
        if (!initialize()) {
            return 1;
        }

        while (!stop_token_.stop_requested()) {
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
                    if (!accept_ready_clients()) {
                        return 1;
                    }
                    continue;
                }

                if (socket_fd == signal_fd_) {
                    if ((event.events & (EPOLLERR | EPOLLHUP)) != 0U) {
                        write_log(
                            std::cerr,
                            "Termination signal descriptor failed.\n");
                        return 1;
                    }
                    if ((event.events & EPOLLIN) != 0U) {
                        const SignalReadResult signal_result =
                            consume_termination_signal();
                        if (signal_result ==
                            SignalReadResult::termination_requested) {
                            return 0;
                        }
                        if (signal_result == SignalReadResult::error) {
                            return 1;
                        }
                    }
                    continue;
                }

                handle_client_event(socket_fd, event.events);
            }

            remove_idle_clients();
            retry_paused_accepts();
        }

        return 0;
    }

private:
    bool initialize() {
        if (config_.backlog <= 0 || config_.maximum_events == 0 ||
            config_.maximum_connections == 0 ||
            config_.maximum_events >
                static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
            config_.idle_timeout <= std::chrono::seconds::zero()) {
            write_log(std::cerr, "Invalid epoll server configuration.\n");
            return false;
        }

        if (!setup_termination_signals()) {
            return false;
        }

        reserve_fd_ = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (reserve_fd_ == -1) {
            std::perror("open reserve descriptor");
            return false;
        }

        listening_fd_ = create_listening_socket(
            config_.port, config_.backlog);
        if (listening_fd_ == -1) {
            return false;
        }

        epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
        if (epoll_fd_ == -1) {
            std::perror("epoll_create1");
            return false;
        }

        epoll_event listening_event {};
        listening_event.events = listening_event_flags;
        listening_event.data.fd = listening_fd_;
        if (::epoll_ctl(
                epoll_fd_,
                EPOLL_CTL_ADD,
                listening_fd_,
                &listening_event) == -1) {
            std::perror("epoll_ctl add listening Socket");
            return false;
        }

        if (signal_fd_ >= 0) {
            epoll_event signal_event {};
            signal_event.events = EPOLLIN;
            signal_event.data.fd = signal_fd_;
            if (::epoll_ctl(
                    epoll_fd_,
                    EPOLL_CTL_ADD,
                    signal_fd_,
                    &signal_event) == -1) {
                std::perror("epoll_ctl add termination signal descriptor");
                return false;
            }
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

    bool setup_termination_signals() {
        if (!config_.handle_termination_signals) {
            return true;
        }

        sigset_t termination_signals;
        if (::sigemptyset(&termination_signals) == -1 ||
            ::sigaddset(&termination_signals, SIGINT) == -1 ||
            ::sigaddset(&termination_signals, SIGTERM) == -1) {
            std::perror("configure termination signal mask");
            return false;
        }

        const int mask_error = ::pthread_sigmask(
            SIG_BLOCK,
            &termination_signals,
            &previous_signal_mask_);
        if (mask_error != 0) {
            errno = mask_error;
            std::perror("pthread_sigmask");
            return false;
        }
        signal_mask_changed_ = true;

        signal_fd_ = ::signalfd(
            -1,
            &termination_signals,
            SFD_NONBLOCK | SFD_CLOEXEC);
        if (signal_fd_ == -1) {
            std::perror("signalfd");
            return false;
        }
        return true;
    }

    SignalReadResult consume_termination_signal() {
        while (true) {
            signalfd_siginfo signal_info {};
            const ssize_t received = ::read(
                signal_fd_, &signal_info, sizeof(signal_info));
            if (received == static_cast<ssize_t>(sizeof(signal_info))) {
                if (signal_info.ssi_signo == SIGINT ||
                    signal_info.ssi_signo == SIGTERM) {
                    return SignalReadResult::termination_requested;
                }
                continue;
            }

            if (received == -1 && errno == EINTR) {
                continue;
            }
            if (received == -1 &&
                (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return SignalReadResult::no_signal;
            }

            if (received == -1) {
                std::perror("read signalfd");
            } else {
                write_log(
                    std::cerr,
                    "Short read from termination signal descriptor.\n");
            }
            return SignalReadResult::error;
        }
    }

    bool accept_ready_clients() {
        while (true) {
            const int client_fd = accept_client(listening_fd_);
            if (client_fd == -1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return rearm_listening_socket();
                }

                if (errno == EMFILE || errno == ENFILE) {
                    recover_from_descriptor_exhaustion();
                    accepting_paused_ = true;
                    next_accept_retry_ = Clock::now() + accept_retry_interval;
                    return true;
                }

                // accept_client 已经输出了真正的系统错误。
                return false;
            }

            if (clients_.size() >= config_.maximum_connections) {
                if (config_.verbose_logging) {
                    write_log(
                        std::cerr,
                        "Connection limit reached; rejecting fd = ",
                        client_fd,
                        '\n');
                }
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

    bool rearm_listening_socket() {
        epoll_event listening_event {};
        listening_event.events = listening_event_flags;
        listening_event.data.fd = listening_fd_;
        if (::epoll_ctl(
                epoll_fd_,
                EPOLL_CTL_MOD,
                listening_fd_,
                &listening_event) == -1) {
            std::perror("epoll_ctl rearm listening Socket");
            return false;
        }
        return true;
    }

    void recover_from_descriptor_exhaustion() {
        write_log(
            std::cerr,
            "File descriptor limit reached; pausing accepts.\n");

        close_socket(reserve_fd_);
        reserve_fd_ = -1;

        const int rejected_fd = accept_client(listening_fd_);
        close_socket(rejected_fd);

        reserve_fd_ = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (reserve_fd_ == -1) {
            std::perror("reopen reserve descriptor");
        }
    }

    void retry_paused_accepts() {
        if (!accepting_paused_ || Clock::now() < next_accept_retry_) {
            return;
        }

        if (rearm_listening_socket()) {
            accepting_paused_ = false;
        } else {
            next_accept_retry_ = Clock::now() + accept_retry_interval;
        }
    }

    void resume_paused_accepts() {
        if (!accepting_paused_) {
            return;
        }

        if (rearm_listening_socket()) {
            accepting_paused_ = false;
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

                if (parse_result ==
                    HttpParseResult::version_not_supported) {
                    write_log(
                        std::cerr,
                        "HTTP version is not supported, fd = ",
                        client_fd,
                        '\n');
                    return queue_response(
                        client_fd,
                        connection,
                        make_error_response(
                            505,
                            "HTTP Version Not Supported",
                            "HTTP Version Not Supported\n"));
                }

                if (parse_result == HttpParseResult::expectation_failed) {
                    write_log(
                        std::cerr,
                        "HTTP expectation is not supported, fd = ",
                        client_fd,
                        '\n');
                    return queue_response(
                        client_fd,
                        connection,
                        make_error_response(
                            417,
                            "Expectation Failed",
                            "Expectation Failed\n"));
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
        resume_paused_accepts();
    }

    EpollServerConfig config_;
    std::stop_token stop_token_;
    int listening_fd_{-1};
    int epoll_fd_{-1};
    int signal_fd_{-1};
    int reserve_fd_{-1};
    sigset_t previous_signal_mask_ {};
    bool signal_mask_changed_{false};
    bool accepting_paused_{false};
    std::vector<epoll_event> events_;
    std::unordered_map<int, ClientConnection> clients_;
    Clock::time_point next_timeout_check_{Clock::now()};
    Clock::time_point next_accept_retry_{Clock::now()};
};

}  // namespace

int run_epoll_server(
    const EpollServerConfig& config,
    std::stop_token stop_token) {
    EpollEventLoop event_loop(config, stop_token);
    return event_loop.run();
}

}  // namespace personal_cloud
