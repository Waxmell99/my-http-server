#include "server/client_handler.h"
#include "server/http_server.h"

#include <cstdint>
#include <iostream>

namespace {

constexpr std::uint16_t port = 9000;
constexpr int backlog = 10;
constexpr int receive_timeout_seconds = 5;
constexpr int send_timeout_seconds = 5;

}  // namespace

int main() {
    std::cout << "Server learning project started.\n";

    const int listening_fd = personal_cloud::create_listening_socket(port, backlog);
    if (listening_fd == -1) {
        return 1;
    }

    std::cout << "Socket listening on port " << port 
              << ", fd = " << listening_fd << '\n';

    while (true) {
        const int client_fd = personal_cloud::accept_client(listening_fd);
        if (client_fd == -1) {
            personal_cloud::close_socket(listening_fd);
            return 1;
        }

        std::cout << "Client connected, fd = " << client_fd << '\n';

        if (!personal_cloud::set_socket_timeouts(
                client_fd,
                receive_timeout_seconds,
                send_timeout_seconds)) {
            personal_cloud::close_socket(client_fd);
            continue;
        }
        personal_cloud::handle_client(client_fd);
        personal_cloud::close_socket(client_fd);
        std::cout << "Client Socket closed.\n";
    }
}
