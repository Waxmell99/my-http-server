#include "common/log.h"
#include "server/client_handler.h"

#include "http/http_request.h"
#include "http/http_response.h"
#include "http/router.h"
#include "server/http_server.h"

#include <array>
#include <cstddef>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace personal_cloud {
namespace {

constexpr std::size_t buffer_size = 4096;
constexpr std::size_t maximum_header_size = 16 * 1024;
constexpr std::size_t maximum_body_size = 64 * 1024;
constexpr std::string_view header_terminator = "\r\n\r\n";

bool send_http_response(int client_fd, const HttpResponse& response) {
    const std::string serialized = serialize_http_response(response);
    return send_all(client_fd, serialized.data(), serialized.size());
}

void send_error_response(int client_fd,
                         int status_code,
                         std::string reason,
                         std::string body) {
    const HttpResponse response {
        status_code,
        std::move(reason),
        "text/plain; charset=utf-8",
        std::move(body),
    };
    send_http_response(client_fd, response);
}

}  // namespace

void handle_client(int client_fd) {
    std::array<char, buffer_size> buffer {};
    std::string request_buffer;

    while (true) {
        const ReceiveResult receive_result = receive_data(
            client_fd, buffer.data(), buffer.size());

        switch (receive_result.status) {
            case ReceiveStatus::data:
                break;

            case ReceiveStatus::peer_closed:
                write_log(
                    std::cout,
                    "Client closed before sending a complete request.\n");
                return;

            case ReceiveStatus::timeout:
                write_log(std::cerr, "Client receive timeout.\n");
                send_error_response(
                    client_fd,
                    408,
                    "Request Timeout",
                    "Request Timeout\n");
                return;

            case ReceiveStatus::error:
                return;
        }

        request_buffer.append(
            buffer.data(), receive_result.bytes_received);

        const std::size_t headers_end =
            request_buffer.find(header_terminator);
        if ((headers_end == std::string::npos &&
             request_buffer.size() > maximum_header_size) ||
            (headers_end != std::string::npos &&
             headers_end + header_terminator.size() > maximum_header_size)) {
            write_log(std::cerr, "HTTP Header is too large.\n");
            send_error_response(
                client_fd,
                431,
                "Request Header Fields Too Large",
                "Request Header Fields Too Large\n");
            return;
        }

        HttpRequest request;
        const HttpParseResult parse_result = parse_http_request(
            request_buffer, request, maximum_body_size);

        switch (parse_result) {
            case HttpParseResult::incomplete:
                continue;

            case HttpParseResult::bad_request:
                write_log(std::cerr, "Malformed HTTP request.\n");
                send_error_response(
                    client_fd, 400, "Bad Request", "Bad Request\n");
                return;

            case HttpParseResult::payload_too_large:
                write_log(std::cerr, "HTTP Body is too large.\n");
                send_error_response(
                    client_fd,
                    413,
                    "Payload Too Large",
                    "Payload Too Large\n");
                return;

            case HttpParseResult::version_not_supported:
                write_log(std::cerr, "HTTP version is not supported.\n");
                send_error_response(
                    client_fd,
                    505,
                    "HTTP Version Not Supported",
                    "HTTP Version Not Supported\n");
                return;

            case HttpParseResult::expectation_failed:
                write_log(std::cerr, "HTTP expectation is not supported.\n");
                send_error_response(
                    client_fd,
                    417,
                    "Expectation Failed",
                    "Expectation Failed\n");
                return;

            case HttpParseResult::complete:
                break;
        }

        write_log(
            std::cout,
            "Parsed HTTP request:\n",
            "  method  = ", request.method, '\n',
            "  path    = ", request.path, '\n',
            "  version = ", request.version, '\n',
            "  headers = ", request.headers.size(), '\n',
            "  body    = ", request.body.size(), " bytes\n");

        const HttpResponse response = route_request(request);
        send_http_response(client_fd, response);
        return;
    }
}

}  // namespace personal_cloud
