#include "common/log.h"
#include "concurrency/thread_pool.h"
#include "server/client_handler.h"
#include "server/http_server.h"

#include <cstdint>
#include <exception>
#include <iostream>

namespace {

constexpr std::uint16_t port = 9000;
constexpr int backlog = 10;
constexpr int receive_timeout_seconds = 5;
constexpr int send_timeout_seconds = 5;
constexpr std::size_t worker_count = 4;
constexpr std::size_t maximum_queue_size = 128;

void process_client(int client_fd) noexcept {
    try {
        personal_cloud::handle_client(client_fd);
    } catch (const std::exception& error) {
        personal_cloud::write_log(
            std::cerr, "Client handler failed: ", error.what(), '\n');
    } catch (...) {
        personal_cloud::write_log(
            std::cerr,
            "Client handler failed with an unknown exception.\n");
    }

    personal_cloud::close_socket(client_fd);
    personal_cloud::write_log(
        std::cout, "Client Socket closed, fd = ", client_fd, '\n');
}

}  // namespace

int main() {
    std::cout << "Server learning project started.\n";

    const int listening_fd = personal_cloud::create_listening_socket(port, backlog);
    if (listening_fd == -1) {
        return 1;
    }

    std::cout << "Socket listening on port " << port 
              << ", fd = " << listening_fd << '\n';

    personal_cloud::ThreadPool pool(worker_count, maximum_queue_size);

    while (true) {
        const int client_fd = personal_cloud::accept_client(listening_fd);
        if (client_fd == -1) {
            personal_cloud::close_socket(listening_fd);
            return 1;
        }

        personal_cloud::write_log(
            std::cout, "Client connected, fd = ", client_fd, '\n');

        if (!personal_cloud::set_socket_timeouts(
                client_fd,
                receive_timeout_seconds,
                send_timeout_seconds)) {
            personal_cloud::close_socket(client_fd);
            continue;
        }

        const bool submitted = pool.submit([client_fd] {
            process_client(client_fd);
        });

        if (!submitted) {
            personal_cloud::write_log(
                std::cerr,
                "ThreadPool queue is full; closing client fd = ",
                client_fd,
                '\n');
            personal_cloud::close_socket(client_fd);
        }
    }
}
