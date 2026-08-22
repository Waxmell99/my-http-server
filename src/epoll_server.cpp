#include "server/epoll_server.h"

#include "common/log.h"
#include "concurrency/thread_pool.h"
#include "http/http_request.h"
#include "http/http_response.h"
#include "http/router.h"
#include "server/http_server.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <deque>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <signal.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
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
constexpr std::uint64_t listening_event_id = 1;
constexpr std::uint64_t signal_event_id = 2;
constexpr std::uint64_t completion_event_id = 3;
constexpr std::uint64_t first_connection_id = 4;

using Clock = std::chrono::steady_clock;

enum class ConnectionState {
    receiving,
    processing,
    sending,
    upload_preparing,
    upload_receiving,
    upload_writing,
    download_preparing,
    download_sending,
    download_reading,
};

enum class CompletionKind {
    response,
    upload_prepared,
    upload_chunk,
    download_prepared,
    download_chunk,
};

enum class SignalReadResult {
    no_signal,
    termination_requested,
    error,
};

struct ClientConnection {
    std::uint64_t id{0};
    std::string request_buffer;
    std::string response_buffer;
    std::size_t sent_size{0};
    Clock::time_point last_activity{Clock::now()};
    ConnectionState state{ConnectionState::receiving};
    std::shared_ptr<UploadStream> upload_stream;
    std::uint64_t upload_size{0};
    std::uint64_t upload_received{0};
    std::string pending_upload_bytes;
    std::shared_ptr<DownloadStream> download_stream;
    bool download_end_of_file{false};
};

struct TaskCompletion {
    int client_fd{-1};
    std::uint64_t connection_id{0};
    CompletionKind kind{CompletionKind::response};
    std::optional<HttpResponse> response;
    std::shared_ptr<UploadStream> upload_stream;
    std::shared_ptr<DownloadStream> download_stream;
    DownloadChunk download_chunk;
    bool failed{false};
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

std::string content_disposition(std::string_view name) {
    std::string result = "attachment; filename=\"";
    for (char character : name) {
        const auto byte = static_cast<unsigned char>(character);
        if (character == '\\' || character == '"') {
            result += '\\';
            result += character;
        } else if (byte >= 0x20U && byte < 0x7fU) {
            result += character;
        } else {
            result += '_';
        }
    }
    result += '"';
    return result;
}

class EpollEventLoop final {
public:
    EpollEventLoop(EpollServerConfig config, std::stop_token stop_token)
        : config_(std::move(config)), stop_token_(stop_token) {}

    ~EpollEventLoop() {
        // worker 可能仍会写完成队列/eventfd，必须先等待它们退出。
        worker_pool_.reset();

        for (const auto& [client_fd, connection] : clients_) {
            static_cast<void>(connection);
            close_socket(client_fd);
        }

        close_socket(listening_fd_);
        close_socket(epoll_fd_);
        close_socket(signal_fd_);
        close_socket(completion_fd_);
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
                const std::uint64_t event_id = event.data.u64;

                if (event_id == listening_event_id) {
                    if (!accept_ready_clients()) {
                        return 1;
                    }
                    continue;
                }

                if (event_id == signal_event_id) {
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

                if (event_id == completion_event_id) {
                    if ((event.events & (EPOLLERR | EPOLLHUP)) != 0U) {
                        write_log(
                            std::cerr,
                            "Application completion descriptor failed.\n");
                        return 1;
                    }
                    if ((event.events & EPOLLIN) != 0U &&
                        !consume_task_completions()) {
                        return 1;
                    }
                    continue;
                }

                const auto client = connection_fds_.find(event_id);
                if (client != connection_fds_.end()) {
                    handle_client_event(client->second, event.events);
                }
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
            config_.idle_timeout <= std::chrono::seconds::zero() ||
            ((config_.request_task_factory || config_.upload_task_factory ||
              config_.download_task_factory) &&
             (config_.application_worker_count == 0 ||
              config_.application_queue_size == 0 ||
              config_.streaming_chunk_size == 0))) {
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
        listening_event.data.u64 = listening_event_id;
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
            signal_event.data.u64 = signal_event_id;
            if (::epoll_ctl(
                    epoll_fd_,
                    EPOLL_CTL_ADD,
                    signal_fd_,
                    &signal_event) == -1) {
                std::perror("epoll_ctl add termination signal descriptor");
                return false;
            }
        }

        if (config_.request_task_factory || config_.upload_task_factory ||
            config_.download_task_factory) {
            completion_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
            if (completion_fd_ == -1) {
                std::perror("eventfd");
                return false;
            }

            epoll_event completion_event {};
            completion_event.events = EPOLLIN;
            completion_event.data.u64 = completion_event_id;
            if (::epoll_ctl(
                    epoll_fd_,
                    EPOLL_CTL_ADD,
                    completion_fd_,
                    &completion_event) == -1) {
                std::perror("epoll_ctl add application completion descriptor");
                return false;
            }

            try {
                worker_pool_ = std::make_unique<ThreadPool>(
                    config_.application_worker_count,
                    config_.application_queue_size);
            } catch (const std::exception& error) {
                write_log(
                    std::cerr,
                    "Cannot create application worker pool: ",
                    error.what(),
                    '\n');
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

            const std::uint64_t connection_id = allocate_connection_id();
            ClientConnection new_connection;
            new_connection.id = connection_id;
            auto [connection, inserted] = clients_.try_emplace(
                client_fd,
                std::move(new_connection));
            if (!inserted) {
                close_socket(client_fd);
                continue;
            }

            epoll_event client_event {};
            client_event.events = EPOLLIN | EPOLLRDHUP | EPOLLET;
            client_event.data.u64 = connection_id;
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
            connection_fds_.emplace(connection_id, client_fd);

            if (config_.verbose_logging) {
                write_log(
                    std::cout, "Client connected, fd = ", client_fd, '\n');
            }
        }
    }

    bool rearm_listening_socket() {
        epoll_event listening_event {};
        listening_event.events = listening_event_flags;
        listening_event.data.u64 = listening_event_id;
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
        } else if (connection.state == ConnectionState::upload_receiving &&
                   (event_flags & (EPOLLIN | EPOLLRDHUP)) != 0U) {
            keep_connection = receive_upload_chunk(client_fd, connection);
        } else if (connection.state == ConnectionState::sending &&
                   (event_flags & EPOLLOUT) != 0U) {
            keep_connection = send_response(client_fd, connection);
        } else if (connection.state == ConnectionState::download_sending &&
                   (event_flags & EPOLLOUT) != 0U) {
            keep_connection = send_download_buffer(client_fd, connection);
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

                if (config_.upload_task_factory &&
                    connection.request_buffer.find(header_terminator) !=
                        std::string::npos) {
                    const std::optional<bool> upload_started =
                        try_begin_upload(client_fd, connection);
                    if (upload_started.has_value()) {
                        return *upload_started;
                    }
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
                    const std::string_view logged_path(request.path);
                    write_log(
                        std::cout,
                        "Parsed request, fd = ", client_fd,
                        ", method = ", request.method,
                        ", path = ", logged_path.substr(
                            0, logged_path.find('?')),
                        ", body = ", request.body.size(),
                        " bytes\n");
                }
                return begin_request_processing(
                    client_fd, connection, request);
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

    std::optional<bool> try_begin_upload(
        int client_fd,
        ClientConnection& connection) {
        HttpRequest request;
        std::size_t content_length = 0;
        std::size_t body_offset = 0;
        const HttpParseResult result = parse_http_request_head(
            connection.request_buffer,
            request,
            content_length,
            body_offset);
        if (result != HttpParseResult::complete) {
            return std::nullopt;
        }

        std::optional<UploadPreparationTask> task;
        try {
            task = config_.upload_task_factory(
                request, static_cast<std::uint64_t>(content_length));
        } catch (const std::exception& error) {
            write_log(
                std::cerr,
                "Upload task factory failed: ",
                error.what(),
                '\n');
            return queue_response(
                client_fd,
                connection,
                make_error_response(
                    500,
                    "Internal Server Error",
                    "Internal Server Error\n"));
        } catch (...) {
            write_log(
                std::cerr,
                "Upload task factory failed with an unknown exception.\n");
            return queue_response(
                client_fd,
                connection,
                make_error_response(
                    500,
                    "Internal Server Error",
                    "Internal Server Error\n"));
        }
        if (!task.has_value()) {
            return std::nullopt;
        }

        const std::size_t buffered_body_size =
            connection.request_buffer.size() - body_offset;
        if (buffered_body_size > content_length) {
            return queue_response(
                client_fd,
                connection,
                make_error_response(
                    400, "Bad Request", "Bad Request\n"));
        }

        const std::uint64_t connection_id = connection.id;
        const bool submitted = worker_pool_ != nullptr &&
            worker_pool_->submit(
                [this,
                 client_fd,
                 connection_id,
                 preparation_task = std::move(*task)]() mutable {
                    TaskCompletion completion;
                    completion.client_fd = client_fd;
                    completion.connection_id = connection_id;
                    completion.kind = CompletionKind::upload_prepared;
                    try {
                        UploadPreparationResult preparation =
                            preparation_task();
                        if (std::holds_alternative<HttpResponse>(preparation)) {
                            completion.response = std::move(
                                std::get<HttpResponse>(preparation));
                        } else {
                            completion.upload_stream = std::move(
                                std::get<std::shared_ptr<UploadStream>>(
                                    preparation));
                        }
                    } catch (const std::exception& error) {
                        write_log(
                            std::cerr,
                            "Upload preparation task failed: ",
                            error.what(),
                            '\n');
                        completion.response = make_error_response(
                            500,
                            "Internal Server Error",
                            "Internal Server Error\n");
                    } catch (...) {
                        write_log(
                            std::cerr,
                            "Upload preparation task failed with an unknown "
                            "exception.\n");
                        completion.response = make_error_response(
                            500,
                            "Internal Server Error",
                            "Internal Server Error\n");
                    }
                    post_task_completion(std::move(completion));
                });
        if (!submitted) {
            return queue_response(
                client_fd,
                connection,
                make_error_response(
                    503,
                    "Service Unavailable",
                    "Application task queue is full\n"));
        }

        connection.pending_upload_bytes.assign(
            connection.request_buffer.data() + body_offset,
            buffered_body_size);
        connection.upload_size = static_cast<std::uint64_t>(content_length);
        connection.upload_received =
            static_cast<std::uint64_t>(buffered_body_size);
        connection.request_buffer.clear();
        connection.state = ConnectionState::upload_preparing;
        connection.last_activity = Clock::now();
        return modify_client_events(
            client_fd,
            connection,
            EPOLLRDHUP | EPOLLET,
            "epoll_ctl pause client Socket for upload preparation");
    }

    bool receive_upload_chunk(
        int client_fd,
        ClientConnection& connection) {
        if (!connection.upload_stream ||
            connection.upload_received >= connection.upload_size) {
            return false;
        }
        const std::uint64_t remaining =
            connection.upload_size - connection.upload_received;
        const std::size_t wanted = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, config_.streaming_chunk_size));
        std::string chunk(wanted, '\0');
        while (true) {
            const ssize_t received =
                ::recv(client_fd, chunk.data(), chunk.size(), 0);
            if (received > 0) {
                chunk.resize(static_cast<std::size_t>(received));
                connection.upload_received +=
                    static_cast<std::uint64_t>(received);
                connection.last_activity = Clock::now();
                const bool final_chunk =
                    connection.upload_received == connection.upload_size;
                return schedule_upload_chunk(
                    client_fd,
                    connection,
                    std::move(chunk),
                    final_chunk);
            }
            if (received == 0) {
                return false;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return true;
            }
            std::perror("recv upload");
            return false;
        }
    }

    bool schedule_upload_chunk(
        int client_fd,
        ClientConnection& connection,
        std::string chunk,
        bool final_chunk) {
        const std::uint64_t connection_id = connection.id;
        const std::shared_ptr<UploadStream> stream = connection.upload_stream;
        const bool submitted = stream && worker_pool_ != nullptr &&
            worker_pool_->submit(
                [this,
                 client_fd,
                 connection_id,
                 stream,
                 chunk = std::move(chunk),
                 final_chunk]() mutable {
                    TaskCompletion completion;
                    completion.client_fd = client_fd;
                    completion.connection_id = connection_id;
                    completion.kind = CompletionKind::upload_chunk;
                    try {
                        completion.response =
                            stream->append(chunk, final_chunk);
                    } catch (const std::exception& error) {
                        write_log(
                            std::cerr,
                            "Upload chunk task failed: ",
                            error.what(),
                            '\n');
                        completion.response = make_error_response(
                            500,
                            "Internal Server Error",
                            "Internal Server Error\n");
                    } catch (...) {
                        write_log(
                            std::cerr,
                            "Upload chunk task failed with an unknown "
                            "exception.\n");
                        completion.response = make_error_response(
                            500,
                            "Internal Server Error",
                            "Internal Server Error\n");
                    }
                    post_task_completion(std::move(completion));
                });
        if (!submitted) {
            connection.upload_stream.reset();
            return queue_response(
                client_fd,
                connection,
                make_error_response(
                    503,
                    "Service Unavailable",
                    "Application task queue is full\n"));
        }
        connection.state = ConnectionState::upload_writing;
        connection.last_activity = Clock::now();
        return modify_client_events(
            client_fd,
            connection,
            EPOLLRDHUP | EPOLLET,
            "epoll_ctl pause client Socket for upload write");
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

    bool begin_download_preparation(
        int client_fd,
        ClientConnection& connection,
        DownloadPreparationTask task) {
        const std::uint64_t connection_id = connection.id;
        const bool submitted = worker_pool_ != nullptr &&
            worker_pool_->submit(
                [this,
                 client_fd,
                 connection_id,
                 preparation_task = std::move(task)]() mutable {
                    TaskCompletion completion;
                    completion.client_fd = client_fd;
                    completion.connection_id = connection_id;
                    completion.kind = CompletionKind::download_prepared;
                    try {
                        DownloadPreparationResult preparation =
                            preparation_task();
                        if (std::holds_alternative<HttpResponse>(preparation)) {
                            completion.response = std::move(
                                std::get<HttpResponse>(preparation));
                        } else {
                            completion.download_stream = std::move(
                                std::get<std::shared_ptr<DownloadStream>>(
                                    preparation));
                        }
                    } catch (const std::exception& error) {
                        write_log(
                            std::cerr,
                            "Download preparation task failed: ",
                            error.what(),
                            '\n');
                        completion.response = make_error_response(
                            500,
                            "Internal Server Error",
                            "Internal Server Error\n");
                    } catch (...) {
                        write_log(
                            std::cerr,
                            "Download preparation task failed with an unknown "
                            "exception.\n");
                        completion.response = make_error_response(
                            500,
                            "Internal Server Error",
                            "Internal Server Error\n");
                    }
                    post_task_completion(std::move(completion));
                });
        if (!submitted) {
            return queue_response(
                client_fd,
                connection,
                make_error_response(
                    503,
                    "Service Unavailable",
                    "Application task queue is full\n"));
        }
        connection.state = ConnectionState::download_preparing;
        connection.last_activity = Clock::now();
        connection.request_buffer.clear();
        return modify_client_events(
            client_fd,
            connection,
            EPOLLRDHUP | EPOLLET,
            "epoll_ctl pause client Socket for download preparation");
    }

    bool send_download_buffer(
        int client_fd,
        ClientConnection& connection) {
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
                std::perror("send download");
            }
            return false;
        }

        connection.response_buffer.clear();
        connection.sent_size = 0;
        if (connection.download_end_of_file) {
            return false;
        }
        return schedule_download_read(client_fd, connection);
    }

    bool schedule_download_read(
        int client_fd,
        ClientConnection& connection) {
        const std::uint64_t connection_id = connection.id;
        const std::shared_ptr<DownloadStream> stream =
            connection.download_stream;
        const std::size_t chunk_size = config_.streaming_chunk_size;
        const bool submitted = stream && worker_pool_ != nullptr &&
            worker_pool_->submit(
                [this,
                 client_fd,
                 connection_id,
                 stream,
                 chunk_size] {
                    TaskCompletion completion;
                    completion.client_fd = client_fd;
                    completion.connection_id = connection_id;
                    completion.kind = CompletionKind::download_chunk;
                    try {
                        completion.download_chunk =
                            stream->read_chunk(chunk_size);
                    } catch (const std::exception& error) {
                        write_log(
                            std::cerr,
                            "Download read task failed: ",
                            error.what(),
                            '\n');
                        completion.failed = true;
                    } catch (...) {
                        write_log(
                            std::cerr,
                            "Download read task failed with an unknown "
                            "exception.\n");
                        completion.failed = true;
                    }
                    post_task_completion(std::move(completion));
                });
        if (!submitted) {
            write_log(
                std::cerr,
                "Cannot queue download read; closing client fd = ",
                client_fd,
                '\n');
            return false;
        }
        connection.state = ConnectionState::download_reading;
        connection.last_activity = Clock::now();
        return modify_client_events(
            client_fd,
            connection,
            EPOLLRDHUP | EPOLLET,
            "epoll_ctl pause client Socket for download read");
    }

    HttpResponse dispatch_request(const HttpRequest& request) {
        try {
            if (config_.request_handler) {
                return config_.request_handler(request);
            }
            return route_request(request);
        } catch (const std::exception& error) {
            write_log(
                std::cerr,
                "Request handler failed: ",
                error.what(),
                '\n');
        } catch (...) {
            write_log(
                std::cerr,
                "Request handler failed with an unknown exception.\n");
        }

        return make_error_response(
            500,
            "Internal Server Error",
            "Internal Server Error\n");
    }

    bool begin_request_processing(
        int client_fd,
        ClientConnection& connection,
        const HttpRequest& request) {
        if (config_.download_task_factory) {
            try {
                std::optional<DownloadPreparationTask> download_task =
                    config_.download_task_factory(request);
                if (download_task.has_value()) {
                    return begin_download_preparation(
                        client_fd, connection, std::move(*download_task));
                }
            } catch (const std::exception& error) {
                write_log(
                    std::cerr,
                    "Download task factory failed: ",
                    error.what(),
                    '\n');
                return queue_response(
                    client_fd,
                    connection,
                    make_error_response(
                        500,
                        "Internal Server Error",
                        "Internal Server Error\n"));
            } catch (...) {
                write_log(
                    std::cerr,
                    "Download task factory failed with an unknown "
                    "exception.\n");
                return queue_response(
                    client_fd,
                    connection,
                    make_error_response(
                        500,
                        "Internal Server Error",
                        "Internal Server Error\n"));
            }
        }

        if (!config_.request_task_factory) {
            return queue_response(
                client_fd, connection, dispatch_request(request));
        }

        std::optional<ApplicationTask> task;
        try {
            task = config_.request_task_factory(request);
        } catch (const std::exception& error) {
            write_log(
                std::cerr,
                "Application task factory failed: ",
                error.what(),
                '\n');
            return queue_response(
                client_fd,
                connection,
                make_error_response(
                    500,
                    "Internal Server Error",
                    "Internal Server Error\n"));
        } catch (...) {
            write_log(
                std::cerr,
                "Application task factory failed with an unknown exception.\n");
            return queue_response(
                client_fd,
                connection,
                make_error_response(
                    500,
                    "Internal Server Error",
                    "Internal Server Error\n"));
        }

        if (!task.has_value()) {
            return queue_response(
                client_fd, connection, dispatch_request(request));
        }

        const std::uint64_t connection_id = connection.id;
        const bool submitted = worker_pool_ != nullptr &&
            worker_pool_->submit(
                [this,
                 client_fd,
                 connection_id,
                 application_task = std::move(*task)]() mutable {
                    HttpResponse response = make_error_response(
                        500,
                        "Internal Server Error",
                        "Internal Server Error\n");
                    try {
                        response = application_task();
                    } catch (const std::exception& error) {
                        write_log(
                            std::cerr,
                            "Application task failed: ",
                            error.what(),
                            '\n');
                        response = make_error_response(
                            500,
                            "Internal Server Error",
                            "Internal Server Error\n");
                    } catch (...) {
                        write_log(
                            std::cerr,
                            "Application task failed with an unknown "
                            "exception.\n");
                        response = make_error_response(
                            500,
                            "Internal Server Error",
                            "Internal Server Error\n");
                    }
                    TaskCompletion completion;
                    completion.client_fd = client_fd;
                    completion.connection_id = connection_id;
                    completion.kind = CompletionKind::response;
                    completion.response = std::move(response);
                    post_task_completion(std::move(completion));
                });

        if (!submitted) {
            return queue_response(
                client_fd,
                connection,
                make_error_response(
                    503,
                    "Service Unavailable",
                    "Application task queue is full\n"));
        }

        connection.state = ConnectionState::processing;
        connection.last_activity = Clock::now();
        connection.request_buffer.clear();

        // 请求处理期间不再读取更多数据；RDHUP/HUP 仍用于观察客户端生命周期。
        epoll_event client_event {};
        client_event.events = EPOLLRDHUP | EPOLLET;
        client_event.data.u64 = connection.id;
        if (::epoll_ctl(
                epoll_fd_,
                EPOLL_CTL_MOD,
                client_fd,
                &client_event) == -1) {
            std::perror("epoll_ctl pause client Socket for application task");
            return false;
        }
        return true;
    }

    void post_task_completion(TaskCompletion completion) {
        {
            std::lock_guard lock(completions_mutex_);
            completions_.push_back(std::move(completion));
        }

        const std::uint64_t increment = 1;
        while (::write(completion_fd_, &increment, sizeof(increment)) == -1) {
            if (errno == EINTR) {
                continue;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                std::perror("write application completion eventfd");
            }
            break;
        }
    }

    bool consume_task_completions() {
        std::uint64_t completed_count = 0;
        while (true) {
            const ssize_t received = ::read(
                completion_fd_, &completed_count, sizeof(completed_count));
            if (received == static_cast<ssize_t>(sizeof(completed_count))) {
                continue;
            }
            if (received == -1 && errno == EINTR) {
                continue;
            }
            if (received == -1 &&
                (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;
            }
            if (received == -1) {
                std::perror("read application completion eventfd");
            } else {
                write_log(
                    std::cerr,
                    "Short read from application completion eventfd.\n");
            }
            return false;
        }

        std::deque<TaskCompletion> ready;
        {
            std::lock_guard lock(completions_mutex_);
            ready.swap(completions_);
        }

        for (TaskCompletion& completion : ready) {
            const auto found = clients_.find(completion.client_fd);
            if (found == clients_.end() ||
                found->second.id != completion.connection_id) {
                continue;
            }
            ClientConnection& connection = found->second;
            bool keep_connection = true;

            if (completion.kind == CompletionKind::response) {
                if (connection.state != ConnectionState::processing ||
                    !completion.response.has_value()) {
                    continue;
                }
                keep_connection = queue_response(
                    completion.client_fd,
                    connection,
                    *completion.response);
            } else if (completion.kind ==
                       CompletionKind::upload_prepared) {
                if (connection.state != ConnectionState::upload_preparing) {
                    continue;
                }
                connection.last_activity = Clock::now();
                if (completion.response.has_value()) {
                    keep_connection = queue_response(
                        completion.client_fd,
                        connection,
                        *completion.response);
                } else if (!completion.upload_stream) {
                    keep_connection = false;
                } else {
                    connection.upload_stream =
                        std::move(completion.upload_stream);
                    std::string buffered =
                        std::move(connection.pending_upload_bytes);
                    connection.pending_upload_bytes.clear();
                    if (!buffered.empty() || connection.upload_size == 0) {
                        const bool final_chunk =
                            connection.upload_received ==
                            connection.upload_size;
                        keep_connection = schedule_upload_chunk(
                            completion.client_fd,
                            connection,
                            std::move(buffered),
                            final_chunk);
                    } else {
                        connection.state = ConnectionState::upload_receiving;
                        keep_connection = modify_client_events(
                            completion.client_fd,
                            connection,
                            EPOLLIN | EPOLLRDHUP | EPOLLET,
                            "epoll_ctl resume upload receive");
                    }
                }
            } else if (completion.kind == CompletionKind::upload_chunk) {
                if (connection.state != ConnectionState::upload_writing) {
                    continue;
                }
                connection.last_activity = Clock::now();
                if (completion.response.has_value()) {
                    connection.upload_stream.reset();
                    keep_connection = queue_response(
                        completion.client_fd,
                        connection,
                        *completion.response);
                } else if (connection.upload_received >=
                           connection.upload_size) {
                    keep_connection = false;
                } else {
                    connection.state = ConnectionState::upload_receiving;
                    keep_connection = modify_client_events(
                        completion.client_fd,
                        connection,
                        EPOLLIN | EPOLLRDHUP | EPOLLET,
                        "epoll_ctl resume upload receive");
                }
            } else if (completion.kind ==
                       CompletionKind::download_prepared) {
                if (connection.state != ConnectionState::download_preparing) {
                    continue;
                }
                connection.last_activity = Clock::now();
                if (completion.response.has_value()) {
                    keep_connection = queue_response(
                        completion.client_fd,
                        connection,
                        *completion.response);
                } else if (!completion.download_stream) {
                    keep_connection = false;
                } else {
                    connection.download_stream =
                        std::move(completion.download_stream);
                    const HttpResponse response_head{
                        200,
                        "OK",
                        std::string(
                            connection.download_stream->content_type()),
                        {},
                        {{"Content-Disposition",
                          content_disposition(
                              connection.download_stream->original_name())}},
                    };
                    connection.response_buffer = serialize_http_response_head(
                        response_head, connection.download_stream->size());
                    connection.sent_size = 0;
                    connection.download_end_of_file =
                        connection.download_stream->size() == 0;
                    connection.state = ConnectionState::download_sending;
                    keep_connection = modify_client_events(
                        completion.client_fd,
                        connection,
                        EPOLLOUT | EPOLLRDHUP | EPOLLET,
                        "epoll_ctl begin streaming download");
                }
            } else {
                if (connection.state != ConnectionState::download_reading) {
                    continue;
                }
                connection.last_activity = Clock::now();
                if (completion.failed ||
                    (completion.download_chunk.bytes.empty() &&
                     !completion.download_chunk.end_of_file)) {
                    keep_connection = false;
                } else if (completion.download_chunk.bytes.empty()) {
                    keep_connection = false;
                } else {
                    connection.response_buffer =
                        std::move(completion.download_chunk.bytes);
                    connection.sent_size = 0;
                    connection.download_end_of_file =
                        completion.download_chunk.end_of_file;
                    connection.state = ConnectionState::download_sending;
                    keep_connection = modify_client_events(
                        completion.client_fd,
                        connection,
                        EPOLLOUT | EPOLLRDHUP | EPOLLET,
                        "epoll_ctl resume streaming download");
                }
            }

            if (!keep_connection) {
                close_client(completion.client_fd);
            }
        }
        return true;
    }

    bool modify_client_events(
        int client_fd,
        const ClientConnection& connection,
        std::uint32_t events,
        const char* error_operation) {
        epoll_event client_event {};
        client_event.events = events;
        client_event.data.u64 = connection.id;
        if (::epoll_ctl(
                epoll_fd_,
                EPOLL_CTL_MOD,
                client_fd,
                &client_event) == -1) {
            std::perror(error_operation);
            return false;
        }
        return true;
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

        return modify_client_events(
            client_fd,
            connection,
            EPOLLOUT | EPOLLRDHUP | EPOLLET,
            "epoll_ctl modify client Socket");
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
        std::vector<int> processing_timeouts;
        std::vector<int> sending_timeouts;

        for (const auto& [client_fd, connection] : clients_) {
            if (now - connection.last_activity < config_.idle_timeout) {
                continue;
            }

            if (connection.state == ConnectionState::receiving ||
                connection.state == ConnectionState::upload_receiving) {
                receiving_timeouts.push_back(client_fd);
            } else if (
                connection.state == ConnectionState::processing ||
                connection.state == ConnectionState::upload_preparing ||
                connection.state == ConnectionState::upload_writing ||
                connection.state == ConnectionState::download_preparing ||
                connection.state == ConnectionState::download_reading) {
                processing_timeouts.push_back(client_fd);
            } else {
                sending_timeouts.push_back(client_fd);
            }
        }

        for (int client_fd : processing_timeouts) {
            write_log(
                std::cerr,
                "Client application task timeout, fd = ",
                client_fd,
                '\n');
            close_client(client_fd);
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
        const auto found = clients_.find(client_fd);
        if (found != clients_.end()) {
            connection_fds_.erase(found->second.id);
        }
        static_cast<void>(::epoll_ctl(
            epoll_fd_, EPOLL_CTL_DEL, client_fd, nullptr));
        clients_.erase(client_fd);
        close_socket(client_fd);
        resume_paused_accepts();
    }

    std::uint64_t allocate_connection_id() {
        const std::uint64_t result = next_connection_id_;
        ++next_connection_id_;
        if (next_connection_id_ < first_connection_id) {
            next_connection_id_ = first_connection_id;
        }
        return result;
    }

    EpollServerConfig config_;
    std::stop_token stop_token_;
    int listening_fd_{-1};
    int epoll_fd_{-1};
    int signal_fd_{-1};
    int completion_fd_{-1};
    int reserve_fd_{-1};
    sigset_t previous_signal_mask_ {};
    bool signal_mask_changed_{false};
    bool accepting_paused_{false};
    std::vector<epoll_event> events_;
    std::unordered_map<int, ClientConnection> clients_;
    std::unordered_map<std::uint64_t, int> connection_fds_;
    std::uint64_t next_connection_id_{first_connection_id};
    std::unique_ptr<ThreadPool> worker_pool_;
    std::mutex completions_mutex_;
    std::deque<TaskCompletion> completions_;
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
