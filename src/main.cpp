#include "http/http_request.h"
#include "server/http_server.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

int main() {
    std::cout << "Server learning project started.\n";

    constexpr std::uint16_t port = 8080;
    constexpr int backlog = 10;
    constexpr std::size_t buffer_size = 4096;
    constexpr std::size_t maximum_request_size = 16 * 1024;
    constexpr std::string_view headers_end = "\r\n\r\n";

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

        std::array<char, buffer_size> buffer {};
        std::string request_buffer;

        while (true) {
            const ssize_t received = personal_cloud::receive_data(
                client_fd, buffer.data(), buffer.size());

            if (received > 0) {
                request_buffer.append(buffer.data(), static_cast<std::size_t>(received));

                //std::cout << request_buffer; 

                if (request_buffer.size() > maximum_request_size) {
                    std::cerr << "HTTP request is too large.\n";
                    break;
                }

                if (request_buffer.find(headers_end) == std::string::npos) {
                    continue;
                }

                personal_cloud::HttpRequest request;
                if (personal_cloud::parse_http_request(request_buffer, request)) {
                    std::cout << "Parsed HTTP request:\n"
                              << "  method  = " << request.method << '\n'
                              << "  path    = " << request.path << '\n'
                              << "  version = " << request.version << '\n';
                } else {
                    std::cerr << "Malformed HTTP request.\n";
                }

                // 当前阶段只解析一个请求，响应将在下一阶段实现。
                break;
            }

            if (received == 0) {
                std::cout << "Client closed before sending a complete request.\n";
            }
            break;
        }

        personal_cloud::close_socket(client_fd);
        std::cout << "Client Socket closed.\n";
    }
}
