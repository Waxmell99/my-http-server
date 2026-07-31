#include "http/http_request.h"
#include "http/http_response.h"
#include "http/router.h"
#include "server/http_server.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace {

constexpr std::uint16_t port = 8080;
constexpr int backlog = 10;
constexpr std::size_t buffer_size = 4096;
constexpr std::size_t maximum_header_size = 16 * 1024;
constexpr std::size_t maximum_body_size = 64 * 1024;
constexpr std::string_view header_terminator = "\r\n\r\n";

bool send_http_response(int client_fd, const personal_cloud::HttpResponse& response) {
    const std::string serialized = personal_cloud::serialize_http_response(response);
    return personal_cloud::send_all(client_fd, serialized.data(), serialized.size());
}

void send_error_response(int client_fd,
                         int status_code,
                         std::string reason,
                         std::string body) {
    const personal_cloud::HttpResponse response {
        status_code,
        std::move(reason),
        "text/plain; charset=utf-8",
        std::move(body),
    };
    send_http_response(client_fd, response);
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

        const std::size_t headers_end =
            request_buffer.find(header_terminator);
        if ((headers_end == std::string::npos &&
             request_buffer.size() > maximum_header_size) ||
            (headers_end != std::string::npos &&
             headers_end + header_terminator.size() > maximum_header_size)) {
            std::cerr << "HTTP Header is too large.\n";
            send_error_response(
                client_fd,
                431,
                "Request Header Fields Too Large",
                "Request Header Fields Too Large\n");
            return;
        }

        personal_cloud::HttpRequest request;
        const personal_cloud::HttpParseResult parse_result =
            personal_cloud::parse_http_request(
                request_buffer, request, maximum_body_size);

        switch (parse_result) {
            case personal_cloud::HttpParseResult::incomplete:
                continue;

            case personal_cloud::HttpParseResult::bad_request:
                std::cerr << "Malformed HTTP request.\n";
                send_error_response(
                    client_fd, 400, "Bad Request", "Bad Request\n");
                return;

            case personal_cloud::HttpParseResult::payload_too_large:
                std::cerr << "HTTP Body is too large.\n";
                send_error_response(
                    client_fd,
                    413,
                    "Payload Too Large",
                    "Payload Too Large\n");
                return;

            case personal_cloud::HttpParseResult::complete:
                break;
        }

        std::cout << "Parsed HTTP request:\n"
                  << "  method  = " << request.method << '\n'
                  << "  path    = " << request.path << '\n'
                  << "  version = " << request.version << '\n'
                  << "  headers = " << request.headers.size() << '\n'
                  << "  body    = " << request.body.size() << " bytes\n";

        const personal_cloud::HttpResponse response =
            personal_cloud::route_request(request);
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
