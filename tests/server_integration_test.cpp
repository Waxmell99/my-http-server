#include "app/backend_application.h"
#include "server/epoll_server.h"

#include "http/router.h"
#include "server/http_server.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
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

std::string_view response_body(std::string_view response) {
    const std::size_t separator = response.find("\r\n\r\n");
    return separator == std::string_view::npos
               ? std::string_view{}
               : response.substr(separator + 4);
}

class RunningServer final {
public:
    explicit RunningServer(
        bool handle_termination_signals = true,
        std::size_t maximum_connections = 128,
        std::function<personal_cloud::HttpResponse(
            const personal_cloud::HttpRequest&)> request_handler = {},
        personal_cloud::ApplicationTaskFactory request_task_factory = {},
        std::size_t worker_count = 4,
        std::size_t task_queue_size = 256,
        std::chrono::seconds idle_timeout = 1s,
        personal_cloud::UploadTaskFactory upload_task_factory = {},
        personal_cloud::DownloadTaskFactory download_task_factory = {},
        std::size_t streaming_chunk_size = 64 * 1024)
        : port_(find_available_port()) {
        if (port_ == 0) {
            return;
        }

        personal_cloud::EpollServerConfig config;
        config.port = port_;
        config.maximum_events = 128;
        config.maximum_connections = maximum_connections;
        config.idle_timeout = idle_timeout;
        config.handle_termination_signals = handle_termination_signals;
        config.request_handler = std::move(request_handler);
        config.request_task_factory = std::move(request_task_factory);
        config.upload_task_factory = std::move(upload_task_factory);
        config.download_task_factory = std::move(download_task_factory);
        config.application_worker_count = worker_count;
        config.application_queue_size = task_queue_size;
        config.streaming_chunk_size = streaming_chunk_size;
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

void test_application_handler_injection() {
    RunningServer server(
        true,
        128,
        [](const personal_cloud::HttpRequest& request) {
            if (request.path == "/api/test") {
                return personal_cloud::HttpResponse{
                    200,
                    "OK",
                    "application/json; charset=utf-8",
                    "{\"injected\":true}\n",
                };
            }
            if (request.path == "/api/throw") {
                throw std::runtime_error("intentional test failure");
            }
            return personal_cloud::route_request(request);
        });
    expect(server.started(), "start a server with an application handler");
    if (!server.started()) {
        return;
    }

    ExchangeResult result = exchange(
        server.port(),
        {"GET /api/test HTTP/1.1\r\nHost: localhost\r\n\r\n"});
    expect(has_status(result, "HTTP/1.1 200 OK\r\n") &&
               result.response.ends_with("{\"injected\":true}\n"),
           "dispatch through the injected application handler");

    result = exchange(
        server.port(),
        {"GET /api/throw HTTP/1.1\r\nHost: localhost\r\n\r\n"});
    expect(has_status(result, "HTTP/1.1 500 Internal Server Error\r\n"),
           "contain an application exception and return 500");
    expect(server.stop_with_signal() == 0,
           "stop normally after application handler requests");
}

bool wait_until_true(
    const std::atomic<bool>& value,
    std::chrono::milliseconds timeout = 1s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!value.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    return value.load(std::memory_order_acquire);
}

personal_cloud::HttpResponse async_test_response(std::string body) {
    return {
        200,
        "OK",
        "text/plain; charset=utf-8",
        std::move(body),
    };
}

void test_blocked_application_task_does_not_block_epoll() {
    auto task_started = std::make_shared<std::atomic<bool>>(false);
    auto release_task = std::make_shared<std::atomic<bool>>(false);
    RunningServer server(
        true,
        128,
        {},
        [task_started, release_task](
            const personal_cloud::HttpRequest& request)
            -> std::optional<personal_cloud::ApplicationTask> {
            if (request.path == "/api/factory-throw") {
                throw std::runtime_error("intentional task factory failure");
            }
            if (request.path == "/api/task-throw") {
                return []() -> personal_cloud::HttpResponse {
                    throw std::runtime_error("intentional worker task failure");
                };
            }
            if (request.path != "/api/slow") {
                return std::nullopt;
            }
            return [task_started, release_task] {
                task_started->store(true, std::memory_order_release);
                while (!release_task->load(std::memory_order_acquire)) {
                    std::this_thread::sleep_for(1ms);
                }
                return async_test_response("slow complete\n");
            };
        },
        1,
        4);
    expect(server.started(), "start a server with one application worker");
    if (!server.started()) {
        return;
    }

    FileDescriptor slow_client = connect_to_server(server.port());
    const bool slow_sent = slow_client && send_bytes(
        slow_client.get(),
        "GET /api/slow HTTP/1.1\r\nHost: localhost\r\n\r\n");
    const bool started = slow_sent && wait_until_true(*task_started);

    const auto health_start = std::chrono::steady_clock::now();
    const ExchangeResult health = exchange(
        server.port(),
        {"GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n"});
    const auto health_duration =
        std::chrono::steady_clock::now() - health_start;
    expect(started && has_status(health, "HTTP/1.1 200 OK\r\n") &&
               health_duration < 500ms,
           "keep synchronous health checks responsive while a worker blocks");

    release_task->store(true, std::memory_order_release);
    bool slow_received = false;
    const std::string slow_response = slow_client
                                          ? receive_until_closed(
                                                slow_client.get(),
                                                slow_received)
                                          : std::string{};
    expect(slow_received &&
               slow_response.starts_with("HTTP/1.1 200 OK\r\n") &&
               slow_response.ends_with("slow complete\n"),
           "deliver a worker response through the eventfd completion queue");

    const ExchangeResult task_failure = exchange(
        server.port(),
        {"GET /api/task-throw HTTP/1.1\r\nHost: localhost\r\n\r\n"});
    expect(has_status(
               task_failure,
               "HTTP/1.1 500 Internal Server Error\r\n"),
           "contain a worker task exception and return 500");

    const ExchangeResult factory_failure = exchange(
        server.port(),
        {"GET /api/factory-throw HTTP/1.1\r\nHost: localhost\r\n\r\n"});
    expect(has_status(
               factory_failure,
               "HTTP/1.1 500 Internal Server Error\r\n"),
           "contain a task factory exception and return 500");
    expect(server.stop_with_signal() == 0,
           "stop normally after an asynchronous task");
}

void test_full_application_queue_returns_503() {
    auto first_started = std::make_shared<std::atomic<bool>>(false);
    auto release_tasks = std::make_shared<std::atomic<bool>>(false);
    RunningServer server(
        true,
        128,
        {},
        [first_started, release_tasks](
            const personal_cloud::HttpRequest& request)
            -> std::optional<personal_cloud::ApplicationTask> {
            if (!request.path.starts_with("/api/queued/")) {
                return std::nullopt;
            }
            const std::string path = request.path;
            return [first_started, release_tasks, path] {
                first_started->store(true, std::memory_order_release);
                while (!release_tasks->load(std::memory_order_acquire)) {
                    std::this_thread::sleep_for(1ms);
                }
                return async_test_response(path + "\n");
            };
        },
        1,
        1);
    expect(server.started(), "start a server with a one-entry task queue");
    if (!server.started()) {
        return;
    }

    FileDescriptor active = connect_to_server(server.port());
    const bool active_sent = active && send_bytes(
        active.get(),
        "GET /api/queued/active HTTP/1.1\r\nHost: localhost\r\n\r\n");
    const bool active_started = active_sent && wait_until_true(*first_started);

    FileDescriptor queued = connect_to_server(server.port());
    const bool queued_sent = queued && send_bytes(
        queued.get(),
        "GET /api/queued/waiting HTTP/1.1\r\nHost: localhost\r\n\r\n");
    std::this_thread::sleep_for(20ms);
    const ExchangeResult overflow = exchange(
        server.port(),
        {"GET /api/queued/overflow HTTP/1.1\r\nHost: localhost\r\n\r\n"});
    expect(active_started && queued_sent &&
               has_status(
                   overflow,
                   "HTTP/1.1 503 Service Unavailable\r\n"),
           "return 503 when the bounded application queue is full");

    release_tasks->store(true, std::memory_order_release);
    bool active_received = false;
    bool queued_received = false;
    const std::string active_response = active
                                            ? receive_until_closed(
                                                  active.get(),
                                                  active_received)
                                            : std::string{};
    const std::string queued_response = queued
                                            ? receive_until_closed(
                                                  queued.get(),
                                                  queued_received)
                                            : std::string{};
    expect(active_received && queued_received &&
               active_response.ends_with("/api/queued/active\n") &&
               queued_response.ends_with("/api/queued/waiting\n"),
           "preserve accepted tasks when rejecting queue overflow");
    expect(server.stop_with_signal() == 0,
           "stop normally after task queue saturation");
}

void test_stale_task_completion_is_discarded() {
    auto old_started = std::make_shared<std::atomic<bool>>(false);
    auto release_old = std::make_shared<std::atomic<bool>>(false);
    RunningServer server(
        true,
        128,
        {},
        [old_started, release_old](
            const personal_cloud::HttpRequest& request)
            -> std::optional<personal_cloud::ApplicationTask> {
            if (request.path != "/api/old") {
                return std::nullopt;
            }
            return [old_started, release_old] {
                old_started->store(true, std::memory_order_release);
                while (!release_old->load(std::memory_order_acquire)) {
                    std::this_thread::sleep_for(1ms);
                }
                return async_test_response("OLD RESPONSE MUST NOT LEAK\n");
            };
        },
        1,
        2);
    expect(server.started(), "start a server for stale completion testing");
    if (!server.started()) {
        return;
    }

    FileDescriptor old_client = connect_to_server(server.port());
    const bool old_sent = old_client && send_bytes(
        old_client.get(),
        "GET /api/old HTTP/1.1\r\nHost: localhost\r\n\r\n");
    const bool started = old_sent && wait_until_true(*old_started);
    old_client.reset();

    // processing 连接会按 idle timeout 回收；随后建立的新连接很可能复用同一个
    // 服务端 fd。连接 ID 必须阻止旧任务的完成结果命中新连接。
    std::this_thread::sleep_for(1200ms);
    FileDescriptor replacement = connect_to_server(server.port());
    const bool partial_sent = replacement && send_bytes(
        replacement.get(), "GET /health HTTP/1.1\r\n");
    release_old->store(true, std::memory_order_release);
    std::this_thread::sleep_for(50ms);

    const bool request_completed = partial_sent && send_bytes(
        replacement.get(), "Host: localhost\r\n\r\n");
    bool replacement_received = false;
    const std::string replacement_response = replacement
                                                 ? receive_until_closed(
                                                       replacement.get(),
                                                       replacement_received)
                                                 : std::string{};
    expect(started && request_completed && replacement_received &&
               replacement_response.starts_with("HTTP/1.1 200 OK\r\n") &&
               replacement_response.ends_with("OK\n") &&
               replacement_response.find("OLD RESPONSE") == std::string::npos,
           "discard a late completion after the original connection is gone");
    expect(server.stop_with_signal() == 0,
           "stop normally after discarding a stale completion");
}

std::string response_header_value(
    const ExchangeResult& result,
    std::string_view name) {
    const std::string prefix = "\r\n" + std::string(name) + ": ";
    const std::size_t start = result.response.find(prefix);
    if (start == std::string::npos) {
        return {};
    }
    const std::size_t value_start = start + prefix.size();
    const std::size_t value_end = result.response.find("\r\n", value_start);
    if (value_end == std::string::npos) {
        return {};
    }
    return result.response.substr(value_start, value_end - value_start);
}

ExchangeResult json_exchange(
    std::uint16_t port,
    std::string_view path,
    std::string_view body,
    std::string_view cookie = {}) {
    std::string request = "POST ";
    request += path;
    request += " HTTP/1.1\r\nHost: localhost\r\n";
    request += "Content-Type: application/json\r\nContent-Length: ";
    request += std::to_string(body.size());
    request += "\r\n";
    if (!cookie.empty()) {
        request += "Cookie: ";
        request += cookie;
        request += "\r\n";
    }
    request += "\r\n";
    request += body;
    return exchange(port, {request});
}

void test_live_authentication_api() {
    std::string pattern =
        (std::filesystem::temp_directory_path() /
         "personal-cloud-auth-integration-XXXXXX")
            .string();
    pattern.push_back('\0');
    char* directory = ::mkdtemp(pattern.data());
    expect(directory != nullptr,
           "create a temporary directory for live authentication");
    if (directory == nullptr) {
        return;
    }
    const std::filesystem::path temporary_path(directory);

    personal_cloud::BackendConfig config;
    config.database_path = temporary_path / "auth.db";
    config.storage_root = temporary_path / "files";
    personal_cloud::BackendApplication application(config);
    RunningServer server(
        true,
        128,
        [&application](const personal_cloud::HttpRequest& request) {
            return application.handle_request(request);
        },
        [&application](const personal_cloud::HttpRequest& request) {
            return application.make_task(request);
        },
        2,
        16,
        5s);
    expect(server.started(), "start the real authentication application");
    if (!server.started()) {
        std::error_code error;
        std::filesystem::remove_all(temporary_path, error);
        return;
    }

    ExchangeResult response = json_exchange(
        server.port(),
        "/api/auth/register",
        "{\"username\":\"LiveUser\","
        "\"password\":\"live-user-password\"}");
    expect(has_status(response, "HTTP/1.1 201 Created\r\n"),
           "register through the real HTTP parser and epoll worker bridge");

    response = json_exchange(
        server.port(),
        "/api/auth/login",
        "{\"username\":\"LiveUser\","
        "\"password\":\"live-user-password\"}");
    const std::string set_cookie =
        response_header_value(response, "Set-Cookie");
    const std::string cookie = set_cookie.substr(0, set_cookie.find(';'));
    expect(has_status(response, "HTTP/1.1 200 OK\r\n") &&
               cookie.starts_with("pc_session=") &&
               set_cookie.find("HttpOnly") != std::string::npos,
           "receive a hardened session cookie over real HTTP");

    std::string me_request =
        "GET /api/auth/me HTTP/1.1\r\nHost: localhost\r\nCookie: ";
    me_request += cookie;
    me_request += "\r\n\r\n";
    response = exchange(server.port(), {me_request});
    expect(has_status(response, "HTTP/1.1 200 OK\r\n") &&
               response.response.find("\"username\":\"LiveUser\"") !=
                   std::string::npos,
           "authenticate a real request with the issued cookie");

    response = json_exchange(
        server.port(), "/api/auth/logout", "", cookie);
    expect(has_status(response, "HTTP/1.1 200 OK\r\n") &&
               response_header_value(response, "Set-Cookie")
                       .find("Max-Age=0") != std::string::npos,
           "logout and clear the cookie over real HTTP");

    response = exchange(server.port(), {me_request});
    expect(has_status(response, "HTTP/1.1 401 Unauthorized\r\n"),
           "reject the logged-out cookie over real HTTP");
    expect(server.stop_with_signal() == 0,
           "stop normally after the live authentication flow");

    std::error_code error;
    std::filesystem::remove_all(temporary_path, error);
}

void test_live_streaming_file_api() {
    std::string pattern =
        (std::filesystem::temp_directory_path() /
         "personal-cloud-files-integration-XXXXXX")
            .string();
    pattern.push_back('\0');
    char* directory = ::mkdtemp(pattern.data());
    expect(directory != nullptr,
           "create a temporary directory for streaming file tests");
    if (directory == nullptr) {
        return;
    }
    const std::filesystem::path temporary_path(directory);

    personal_cloud::BackendConfig config;
    config.database_path = temporary_path / "files.db";
    config.storage_root = temporary_path / "storage";
    config.maximum_file_size = 1024 * 1024;
    config.user_quota = 2 * 1024 * 1024;
    personal_cloud::BackendApplication application(config);
    RunningServer server(
        true,
        128,
        [&application](const personal_cloud::HttpRequest& request) {
            return application.handle_request(request);
        },
        [&application](const personal_cloud::HttpRequest& request) {
            return application.make_task(request);
        },
        4,
        32,
        5s,
        [&application](
            const personal_cloud::HttpRequest& request,
            std::uint64_t content_length) {
            return application.make_upload_task(request, content_length);
        },
        [&application](const personal_cloud::HttpRequest& request) {
            return application.make_download_task(request);
        },
        4096);
    expect(server.started(), "start the real streaming file application");
    if (!server.started()) {
        std::error_code error;
        std::filesystem::remove_all(temporary_path, error);
        return;
    }

    ExchangeResult response = json_exchange(
        server.port(),
        "/api/auth/register",
        "{\"username\":\"StreamOwner\","
        "\"password\":\"stream-owner-password\"}");
    response = json_exchange(
        server.port(),
        "/api/auth/login",
        "{\"username\":\"StreamOwner\","
        "\"password\":\"stream-owner-password\"}");
    const std::string owner_cookie =
        response_header_value(response, "Set-Cookie")
            .substr(0, response_header_value(response, "Set-Cookie").find(';'));
    expect(has_status(response, "HTTP/1.1 200 OK\r\n") &&
               owner_cookie.starts_with("pc_session="),
           "log in the streaming file owner");

    response = json_exchange(
        server.port(),
        "/api/auth/register",
        "{\"username\":\"StreamOther\","
        "\"password\":\"stream-other-password\"}");
    response = json_exchange(
        server.port(),
        "/api/auth/login",
        "{\"username\":\"StreamOther\","
        "\"password\":\"stream-other-password\"}");
    const std::string other_set_cookie =
        response_header_value(response, "Set-Cookie");
    const std::string other_cookie =
        other_set_cookie.substr(0, other_set_cookie.find(';'));

    std::string content;
    content.reserve(200 * 1024);
    for (std::size_t index = 0; index < 200 * 1024; ++index) {
        content += static_cast<char>('a' + index % 23);
    }
    std::string upload_head =
        "POST /api/files HTTP/1.1\r\nHost: localhost\r\nCookie: ";
    upload_head += owner_cookie;
    upload_head +=
        "\r\nX-File-Name: streamed.bin\r\n"
        "Content-Type: application/octet-stream\r\nContent-Length: ";
    upload_head += std::to_string(content.size());
    upload_head += "\r\n\r\n";
    std::vector<std::string_view> upload_parts;
    upload_parts.push_back(upload_head);
    for (std::size_t offset = 0; offset < content.size(); offset += 3072) {
        upload_parts.push_back(
            std::string_view(content).substr(offset, 3072));
    }
    response = exchange(server.port(), upload_parts);
    expect(has_status(response, "HTTP/1.1 201 Created\r\n"),
           "upload a body larger than the ordinary 64 KiB request limit");
    std::string file_id;
    if (has_status(response, "HTTP/1.1 201 Created\r\n")) {
        const nlohmann::json body =
            nlohmann::json::parse(response_body(response.response));
        file_id = body["file"]["id"].get<std::string>();
    }

    std::string metadata_request =
        "GET /api/files/" + file_id +
        " HTTP/1.1\r\nHost: localhost\r\nCookie: " + other_cookie +
        "\r\n\r\n";
    response = exchange(server.port(), {metadata_request});
    expect(has_status(response, "HTTP/1.1 404 Not Found\r\n"),
           "deny another user access to guessed file metadata over HTTP");

    std::string denied_download_request =
        "GET /api/files/" + file_id +
        "/content HTTP/1.1\r\nHost: localhost\r\nCookie: " +
        other_cookie + "\r\n\r\n";
    response = exchange(server.port(), {denied_download_request});
    expect(has_status(response, "HTTP/1.1 404 Not Found\r\n"),
           "deny another user access to guessed file content over HTTP");

    std::string download_request =
        "GET /api/files/" + file_id +
        "/content HTTP/1.1\r\nHost: localhost\r\nCookie: " +
        owner_cookie + "\r\n\r\n";
    response = exchange(server.port(), {download_request});
    expect(has_status(response, "HTTP/1.1 200 OK\r\n") &&
               response_body(response.response) == content,
           "download exact large content through bounded worker reads");

    std::string rename_request =
        "PATCH /api/files/" + file_id +
        " HTTP/1.1\r\nHost: localhost\r\nCookie: " + owner_cookie +
        "\r\nContent-Type: application/json\r\nContent-Length: 27\r\n\r\n"
        "{\"name\":\"renamed-live.bin\"}";
    response = exchange(server.port(), {rename_request});
    expect(has_status(response, "HTTP/1.1 200 OK\r\n") &&
               response.response.find("renamed-live.bin") !=
                   std::string::npos,
           "rename an owned file over real HTTP");

    {
        FileDescriptor interrupted = connect_to_server(server.port());
        std::string interrupted_request =
            "POST /api/files HTTP/1.1\r\nHost: localhost\r\nCookie: " +
            owner_cookie +
            "\r\nX-File-Name: interrupted-live.bin\r\n"
            "Content-Type: application/octet-stream\r\n"
            "Content-Length: 50000\r\n\r\npartial";
        expect(interrupted && send_bytes(interrupted.get(), interrupted_request),
               "start an upload that will disconnect early");
    }
    const auto cleanup_deadline = std::chrono::steady_clock::now() + 2s;
    bool temporary_files_cleaned = false;
    while (std::chrono::steady_clock::now() < cleanup_deadline) {
        std::error_code error;
        temporary_files_cleaned =
            std::filesystem::is_empty(config.storage_root / "tmp", error) &&
            !error;
        if (temporary_files_cleaned) {
            break;
        }
        std::this_thread::sleep_for(20ms);
    }
    expect(temporary_files_cleaned,
           "clean a real interrupted upload temporary file");

    std::string list_request =
        "GET /api/files HTTP/1.1\r\nHost: localhost\r\nCookie: " +
        owner_cookie + "\r\n\r\n";
    response = exchange(server.port(), {list_request});
    expect(has_status(response, "HTTP/1.1 200 OK\r\n") &&
               response.response.find("interrupted-live.bin") ==
                   std::string::npos,
           "leave no metadata for a real interrupted upload");

    std::string delete_request =
        "DELETE /api/files/" + file_id +
        " HTTP/1.1\r\nHost: localhost\r\nCookie: " + owner_cookie +
        "\r\n\r\n";
    response = exchange(server.port(), {delete_request});
    expect(has_status(response, "HTTP/1.1 200 OK\r\n"),
           "delete an owned file over real HTTP");

    expect(server.stop_with_signal() == 0,
           "stop normally after the streaming file flow");
    std::error_code error;
    std::filesystem::remove_all(temporary_path, error);
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
    test_application_handler_injection();
    test_blocked_application_task_does_not_block_epoll();
    test_full_application_queue_returns_503();
    test_stale_task_completion_is_discarded();
    test_live_authentication_api();
    test_live_streaming_file_api();

    if (failure_count != 0) {
        std::cerr << failure_count << " integration assertion(s) failed.\n";
        return 1;
    }

    std::cout << "All server integration tests passed.\n";
    return 0;
}
