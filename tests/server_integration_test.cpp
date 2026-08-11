#include "server/epoll_server.h"

#include "server/http_server.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;

int failure_count = 0;

void expect(bool condition, std::string_view description) {
    if (condition) {
        std::cout << "[PASS] " << description << '\n';
        return;
    }

    std::cerr << "[FAIL] " << description << '\n';
    ++failure_count;
}

class FileDescriptor final {
public:
    FileDescriptor() noexcept = default;
    explicit FileDescriptor(int value) noexcept : value_(value) {}

    ~FileDescriptor() {
        reset();
    }

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    FileDescriptor(FileDescriptor&& other) noexcept
        : value_(std::exchange(other.value_, -1)) {}

    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
        if (this != &other) {
            reset(std::exchange(other.value_, -1));
        }
        return *this;
    }

    [[nodiscard]] int get() const noexcept {
        return value_;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return value_ >= 0;
    }

    void reset(int value = -1) noexcept {
        if (value_ >= 0) {
            ::close(value_);
        }
        value_ = value;
    }

private:
    int value_{-1};
};

std::uint16_t find_available_port() {
    FileDescriptor socket_fd(
        ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (!socket_fd) {
        return 0;
    }

    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(0);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (::bind(
            socket_fd.get(),
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)) == -1) {
        return 0;
    }

    socklen_t address_size = sizeof(address);
    if (::getsockname(
            socket_fd.get(),
            reinterpret_cast<sockaddr*>(&address),
            &address_size) == -1) {
        return 0;
    }

    return ntohs(address.sin_port);
}

FileDescriptor connect_to_server(std::uint16_t port) {
    FileDescriptor socket_fd(
        ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (!socket_fd) {
        return {};
    }

    timeval receive_timeout {};
    receive_timeout.tv_sec = 5;
    if (::setsockopt(
            socket_fd.get(),
            SOL_SOCKET,
            SO_RCVTIMEO,
            &receive_timeout,
            sizeof(receive_timeout)) == -1) {
        return {};
    }

    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (::connect(
            socket_fd.get(),
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)) == -1) {
        return {};
    }

    return socket_fd;
}

bool connect_existing_socket(int socket_fd, std::uint16_t port) {
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return ::connect(
               socket_fd,
               reinterpret_cast<const sockaddr*>(&address),
               sizeof(address)) == 0;
}

bool send_bytes(int socket_fd, std::string_view bytes) {
    std::size_t sent_size = 0;
    while (sent_size < bytes.size()) {
        const ssize_t sent = ::send(
            socket_fd,
            bytes.data() + sent_size,
            bytes.size() - sent_size,
            MSG_NOSIGNAL);
        if (sent > 0) {
            sent_size += static_cast<std::size_t>(sent);
            continue;
        }
        if (sent == -1 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

std::string receive_until_closed(int socket_fd, bool& succeeded) {
    std::array<char, 4096> buffer {};
    std::string response;

    while (true) {
        const ssize_t received =
            ::recv(socket_fd, buffer.data(), buffer.size(), 0);
        if (received > 0) {
            response.append(
                buffer.data(), static_cast<std::size_t>(received));
            continue;
        }
        if (received == 0) {
            succeeded = true;
            return response;
        }
        if (errno == EINTR) {
            continue;
        }
        succeeded = false;
        return response;
    }
}

struct ExchangeResult {
    bool succeeded{false};
    std::string response;
};

ExchangeResult exchange(
    std::uint16_t port,
    const std::vector<std::string_view>& request_parts,
    bool half_close = false) {
    FileDescriptor socket_fd = connect_to_server(port);
    if (!socket_fd) {
        return {};
    }

    for (std::string_view part : request_parts) {
        if (!send_bytes(socket_fd.get(), part)) {
            return {};
        }
        std::this_thread::sleep_for(5ms);
    }

    if (half_close && ::shutdown(socket_fd.get(), SHUT_WR) == -1) {
        return {};
    }

    ExchangeResult result;
    result.response = receive_until_closed(socket_fd.get(), result.succeeded);
    return result;
}

bool has_status(const ExchangeResult& result, std::string_view status) {
    return result.succeeded && result.response.starts_with(status);
}

class RunningServer final {
public:
    explicit RunningServer(
        bool handle_termination_signals = true,
        std::size_t maximum_connections = 128)
        : port_(find_available_port()) {
        if (port_ == 0) {
            return;
        }

        personal_cloud::EpollServerConfig config;
        config.port = port_;
        config.maximum_events = 128;
        config.maximum_connections = maximum_connections;
        config.idle_timeout = 1s;
        config.handle_termination_signals = handle_termination_signals;
        thread_ = std::jthread([this, config](std::stop_token token) {
            result_.store(
                personal_cloud::run_epoll_server(config, token),
                std::memory_order_release);
        });

        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (std::chrono::steady_clock::now() < deadline) {
            FileDescriptor probe = connect_to_server(port_);
            if (probe) {
                started_ = true;
                return;
            }
            if (result_.load(std::memory_order_acquire) != not_finished) {
                return;
            }
            std::this_thread::sleep_for(10ms);
        }
    }

    ~RunningServer() {
        stop();
    }

    RunningServer(const RunningServer&) = delete;
    RunningServer& operator=(const RunningServer&) = delete;

    [[nodiscard]] bool started() const noexcept {
        return started_;
    }

    [[nodiscard]] std::uint16_t port() const noexcept {
        return port_;
    }

    int stop() {
        if (thread_.joinable()) {
            thread_.request_stop();
            thread_.join();
        }
        return result_.load(std::memory_order_acquire);
    }

    int stop_with_signal() {
        if (thread_.joinable()) {
            const int signal_error =
                ::pthread_kill(thread_.native_handle(), SIGTERM);
            if (signal_error != 0) {
                thread_.request_stop();
                thread_.join();
                return -98;
            }
            thread_.join();
        }
        return result_.load(std::memory_order_acquire);
    }

private:
    static constexpr int not_finished = -99;

    std::uint16_t port_{0};
    std::atomic<int> result_{not_finished};
    std::jthread thread_;
    bool started_{false};
};

void test_live_protocol(std::uint16_t port) {
    ExchangeResult result = exchange(
        port,
        {"GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n"},
        true);
    expect(has_status(result, "HTTP/1.1 200 OK\r\n"),
           "serve a request after the client half-closes its write side");
    expect(result.response.ends_with("\r\n\r\nOK\n"),
           "send the complete health response body");

    result = exchange(
        port,
        {
            "POST /upload HTTP/1.1\r\nHost: local",
            "host\r\nContent-Type: text/plain\r\nContent-Length: 11\r\n",
            "\r\nhello ",
            "world",
        });
    expect(has_status(result, "HTTP/1.1 200 OK\r\n"),
           "assemble a request split across multiple reads");
    expect(result.response.ends_with("Received text:\nhello world"),
           "assemble and echo a fragmented request body");

    result = exchange(port, {"GET /hello HTTP/1.1\r\n\r\n"});
    expect(has_status(result, "HTTP/1.1 400 Bad Request\r\n"),
           "reject a live HTTP/1.1 request without Host");

    result = exchange(
        port,
        {"GET /hello HTTP/2.0\r\nHost: localhost\r\n\r\n"});
    expect(has_status(
               result,
               "HTTP/1.1 505 HTTP Version Not Supported\r\n"),
           "return 505 for a syntactically valid unsupported version");

    result = exchange(
        port,
        {"POST /upload HTTP/1.1\r\nHost: localhost\r\n"
         "Content-Length: 65537\r\n\r\n"});
    expect(has_status(result, "HTTP/1.1 413 Payload Too Large\r\n"),
           "reject an oversized declared request body before reading it");

    result = exchange(
        port,
        {"POST /upload HTTP/1.1\r\nHost: localhost\r\n"
         "Content-Length: 5\r\nExpect: 100-continue\r\n\r\n"});
    expect(has_status(result, "HTTP/1.1 417 Expectation Failed\r\n"),
           "reject unsupported Expect without waiting for a request body");

    std::string large_header =
        "GET /hello HTTP/1.1\r\nHost: localhost\r\nX-Large: ";
    large_header.append(17 * 1024, 'a');
    result = exchange(port, {large_header});
    expect(has_status(
               result,
               "HTTP/1.1 431 Request Header Fields Too Large\r\n"),
           "reject an unterminated Header beyond the configured limit");
}

void test_concurrent_connections(std::uint16_t port) {
    constexpr std::size_t client_count = 32;
    std::vector<FileDescriptor> clients;
    clients.reserve(client_count);

    bool all_connected = true;
    for (std::size_t index = 0; index < client_count; ++index) {
        FileDescriptor client = connect_to_server(port);
        all_connected = static_cast<bool>(client) && all_connected;
        clients.push_back(std::move(client));
    }
    expect(all_connected, "accept multiple concurrent client connections");

    const std::string_view request =
        "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n";
    bool all_sent = true;
    for (FileDescriptor& client : clients) {
        all_sent = client && send_bytes(client.get(), request) && all_sent;
    }
    expect(all_sent, "read requests from all concurrent clients");

    bool all_received = true;
    for (FileDescriptor& client : clients) {
        bool received = false;
        const std::string response =
            receive_until_closed(client.get(), received);
        all_received = received &&
                       response.starts_with("HTTP/1.1 200 OK\r\n") &&
                       response.ends_with("\r\n\r\nOK\n") &&
                       all_received;
    }
    expect(all_received, "write complete responses to concurrent clients");
}

void test_idle_timeout(std::uint16_t port) {
    const ExchangeResult result = exchange(port, {"GET /health HTTP/1.1\r\n"});
    expect(has_status(result, "HTTP/1.1 408 Request Timeout\r\n"),
           "return 408 when a partial request becomes idle");
}

void test_descriptor_exhaustion_recovery() {
    rlimit original_limit {};
    if (::getrlimit(RLIMIT_NOFILE, &original_limit) == -1 ||
        original_limit.rlim_cur < 48) {
        expect(true, "skip descriptor exhaustion below a safe fd limit");
        return;
    }

    rlimit test_limit = original_limit;
    test_limit.rlim_cur = 48;
    if (::setrlimit(RLIMIT_NOFILE, &test_limit) == -1) {
        expect(true, "skip descriptor exhaustion when rlimit cannot change");
        return;
    }

    bool recovered = false;
    bool reached_limit = false;
    bool server_stopped = false;
    {
        RunningServer server;
        FileDescriptor pending_client(
            ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
        std::vector<FileDescriptor> fillers;

        if (server.started() && pending_client) {
            while (true) {
                const int filler_fd =
                    ::open("/dev/null", O_RDONLY | O_CLOEXEC);
                if (filler_fd == -1) {
                    reached_limit = errno == EMFILE || errno == ENFILE;
                    break;
                }
                fillers.emplace_back(filler_fd);
            }

            if (reached_limit && connect_existing_socket(
                                     pending_client.get(), server.port())) {
                std::this_thread::sleep_for(100ms);

                const std::size_t descriptors_to_release =
                    std::min<std::size_t>(8, fillers.size());
                for (std::size_t index = 0;
                     index < descriptors_to_release;
                     ++index) {
                    fillers[fillers.size() - 1 - index].reset();
                }

                const ExchangeResult result = exchange(
                    server.port(),
                    {"GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n"});
                recovered = has_status(result, "HTTP/1.1 200 OK\r\n");
            }
        }

        fillers.clear();
        pending_client.reset();
        server_stopped = !server.started() || server.stop_with_signal() == 0;
    }

    const bool limit_restored =
        ::setrlimit(RLIMIT_NOFILE, &original_limit) == 0;
    expect(reached_limit, "reach the temporary process descriptor limit");
    expect(recovered,
           "resume accepts after recovering from descriptor exhaustion");
    expect(server_stopped,
           "stop normally after descriptor exhaustion recovery");
    expect(limit_restored, "restore the original process descriptor limit");
}

void test_stop_token_shutdown() {
    RunningServer server(false);
    expect(server.started(),
           "start an embedded server with signal handling disabled");
    if (server.started()) {
        expect(server.stop() == 0,
               "stop an embedded event loop through its stop token");
    }
}

void test_connection_limit() {
    RunningServer server(true, 2);
    expect(server.started(), "start a server with a small connection limit");
    if (!server.started()) {
        return;
    }

    std::this_thread::sleep_for(50ms);
    FileDescriptor first = connect_to_server(server.port());
    FileDescriptor second = connect_to_server(server.port());
    FileDescriptor excess = connect_to_server(server.port());
    std::this_thread::sleep_for(50ms);

    char byte = '\0';
    const ssize_t excess_read = excess
                                    ? ::recv(
                                          excess.get(),
                                          &byte,
                                          sizeof(byte),
                                          0)
                                    : -1;
    const bool excess_closed =
        excess_read == 0 ||
        (excess_read == -1 &&
         (errno == ECONNRESET || errno == ENOTCONN));
    expect(first && second && excess && excess_closed,
           "reject a connection above the configured limit");

    first.reset();
    std::this_thread::sleep_for(50ms);
    const ExchangeResult after_release = exchange(
        server.port(),
        {"GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n"});
    expect(has_status(after_release, "HTTP/1.1 200 OK\r\n"),
           "accept a new connection after capacity is released");
    expect(server.stop_with_signal() == 0,
           "stop normally after enforcing the connection limit");
}

}  // namespace

int main() {
    RunningServer server;
    expect(server.started(), "start the epoll server on an available port");

    if (server.started()) {
        test_live_protocol(server.port());
        test_concurrent_connections(server.port());
        test_idle_timeout(server.port());

        FileDescriptor active_client = connect_to_server(server.port());
        const bool partial_request_sent = active_client && send_bytes(
            active_client.get(), "GET /health HTTP/1.1\r\n");
        std::this_thread::sleep_for(10ms);

        const std::uint16_t port = server.port();
        expect(server.stop_with_signal() == 0,
               "stop the epoll loop cleanly through SIGTERM");

        char byte = '\0';
        const ssize_t after_shutdown = active_client
                                           ? ::recv(
                                                 active_client.get(),
                                                 &byte,
                                                 sizeof(byte),
                                                 0)
                                           : -1;
        const bool active_client_closed =
            after_shutdown == 0 ||
            (after_shutdown == -1 &&
             (errno == ECONNRESET || errno == ENOTCONN));
        expect(partial_request_sent && active_client_closed,
               "close active client Sockets during graceful shutdown");

        const int listening_fd =
            personal_cloud::create_listening_socket(port, 1);
        expect(listening_fd >= 0,
               "release the listening Socket when the event loop stops");

        const int listening_status_flags =
            listening_fd >= 0 ? ::fcntl(listening_fd, F_GETFL, 0) : -1;
        const int listening_descriptor_flags =
            listening_fd >= 0 ? ::fcntl(listening_fd, F_GETFD, 0) : -1;
        expect(
            listening_status_flags >= 0 &&
                (listening_status_flags & O_NONBLOCK) != 0,
            "create the listening Socket as nonblocking atomically");
        expect(
            listening_descriptor_flags >= 0 &&
                (listening_descriptor_flags & FD_CLOEXEC) != 0,
            "create the listening Socket as close-on-exec atomically");

        FileDescriptor peer = connect_to_server(port);
        const int accepted_fd = listening_fd >= 0
                                    ? personal_cloud::accept_client(listening_fd)
                                    : -1;
        const int accepted_status_flags =
            accepted_fd >= 0 ? ::fcntl(accepted_fd, F_GETFL, 0) : -1;
        const int accepted_descriptor_flags =
            accepted_fd >= 0 ? ::fcntl(accepted_fd, F_GETFD, 0) : -1;
        expect(
            peer && accepted_status_flags >= 0 &&
                (accepted_status_flags & O_NONBLOCK) != 0,
            "accept client Sockets as nonblocking atomically");
        expect(
            accepted_descriptor_flags >= 0 &&
                (accepted_descriptor_flags & FD_CLOEXEC) != 0,
            "accept client Sockets as close-on-exec atomically");
        personal_cloud::close_socket(accepted_fd);
        personal_cloud::close_socket(listening_fd);
    }

    test_descriptor_exhaustion_recovery();
    test_stop_token_shutdown();
    test_connection_limit();

    if (failure_count != 0) {
        std::cerr << failure_count << " integration assertion(s) failed.\n";
        return 1;
    }

    std::cout << "All server integration tests passed.\n";
    return 0;
}
