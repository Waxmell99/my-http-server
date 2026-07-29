#include "http/http_request.h"
#include "http/http_response.h"
#include "server/http_server.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

namespace {

constexpr std::uint16_t port = 8080;
constexpr int backlog = 10;
constexpr std::size_t buffer_size = 4096;
constexpr std::size_t maximum_request_size = 16 * 1024;
constexpr std::string_view headers_end = "\r\n\r\n";

bool send_http_response(int client_fd, const personal_cloud::HttpResponse& response) {
    const std::string serialized = personal_cloud::serialize_http_response(response);
    return personal_cloud::send_all(client_fd, serialized.data(), serialized.size());
}

void handle_client(int client_fd) {
    std::array<char, buffer_size> buffer {};
    std::string request_buffer;

    while (true) {
        const ssize_t received = personal_cloud::receive_data(
            client_fd, buffer.data(), buffer.size());

        if (received == 0) {
            std::cout << "Client closed before sending a complete request.\n";
            return;
        }

        if (received == -1) {
            return;
        }

        request_buffer.append(
            buffer.data(), static_cast<std::size_t>(received));

        if (request_buffer.size() > maximum_request_size) {
            std::cerr << "HTTP request is too large.\n";
            return;
        }

        if (request_buffer.find(headers_end) == std::string::npos) {
            continue;
        }

        personal_cloud::HttpRequest request;
        if (!personal_cloud::parse_http_request(request_buffer, request)) {
            std::cerr << "Malformed HTTP request.\n";

            const personal_cloud::HttpResponse response {
                400,
                "Bad Request",
                "text/plain; charset=utf-8",
                "Bad Request\n",
            };
            send_http_response(client_fd, response);
            return;
        }

        std::cout << "Parsed HTTP request:\n"
                  << "  method  = " << request.method << '\n'
                  << "  path    = " << request.path << '\n'
                  << "  version = " << request.version << '\n';

        personal_cloud::HttpResponse response;

        // 即使路由不存在或方法不支持，response 中也包含 404/405 响应。
        personal_cloud::handle_http_request(request, response);
        send_http_response(client_fd, response);
        return;
    }
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

    while (true) {
        const int client_fd = personal_cloud::accept_client(listening_fd);
        if (client_fd == -1) {
            personal_cloud::close_socket(listening_fd);
            return 1;
        }

        std::cout << "Client connected, fd = " << client_fd << '\n';

        handle_client(client_fd);
        personal_cloud::close_socket(client_fd);
        std::cout << "Client Socket closed.\n";
    }
}
