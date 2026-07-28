#include "server/http_server.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>

int main() {
    std::cout << "Server learning project started.\n";

    constexpr std::uint16_t port = 8080;
    constexpr int backlog = 10;
    constexpr std::size_t buffer_size = 4096;

    const int listening_fd =
        personal_cloud::create_listening_socket(port, backlog);
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

        std::array<char, buffer_size> buffer {};

        while (true) {
            const ssize_t received = personal_cloud::receive_data(
                client_fd, buffer.data(), buffer.size());

            if (received > 0) {
                std::cout << "Received " << received << " bytes: ";
                std::cout.write(buffer.data(), received);
                std::cout << '\n';

                if (!personal_cloud::send_all(
                        client_fd,
                        buffer.data(),
                        static_cast<std::size_t>(received))) {
                    break;
                }
                continue;
            }

            if (received == 0) {
                std::cout << "Client closed the connection.\n";
            }
            break;
        }

        personal_cloud::close_socket(client_fd);
        std::cout << "Client Socket closed.\n";
    }
}
