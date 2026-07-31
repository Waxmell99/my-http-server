#include "http/http_request.h"
#include "http/http_response.h"

#include <iostream>
#include <string>
#include <string_view>

namespace {

int failure_count = 0;

void expect(bool condition, std::string_view description) {
    if (condition) {
        std::cout << "[PASS] " << description << '\n';
        return;
    }

    std::cerr << "[FAIL] " << description << '\n';
    ++failure_count;
}

void test_valid_request() {
    personal_cloud::HttpRequest request;
    const bool parsed = personal_cloud::parse_http_request(
        "GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n", request);

    expect(parsed, "parse a complete GET request");
    expect(request.method == "GET", "extract request method");
    expect(request.path == "/hello", "extract request path");
    expect(request.version == "HTTP/1.1", "extract HTTP version");
}

void test_invalid_requests() {
    personal_cloud::HttpRequest request;

    expect(!personal_cloud::parse_http_request(
               "GET /hello HTTP/1.1\r\nHost: localhost\r\n", request),
           "reject an incomplete HTTP header");

    expect(!personal_cloud::parse_http_request(
               "GET  HTTP/1.1\r\n\r\n", request),
           "reject a request without a path");

    expect(!personal_cloud::parse_http_request(
               "GET /hello FTP/1.0\r\n\r\n", request),
           "reject a non-HTTP version");
}

void test_request_handling() {
    personal_cloud::HttpResponse response;

    const bool hello_handled = personal_cloud::handle_http_request(
        {"GET", "/hello", "HTTP/1.1"}, response);
    expect(hello_handled, "handle GET /hello");
    expect(response.status_code == 200, "GET /hello returns 200");
    expect(response.body == "Hello World\n", "GET /hello returns its body");

    const bool missing_handled = personal_cloud::handle_http_request(
        {"GET", "/missing", "HTTP/1.1"}, response);
    expect(!missing_handled, "report an unmatched route");
    expect(response.status_code == 404, "unknown GET path returns 404");

    const bool post_handled = personal_cloud::handle_http_request(
        {"POST", "/hello", "HTTP/1.1"}, response);
    expect(!post_handled, "report an unsupported method");
    expect(response.status_code == 405, "unsupported method returns 405");
}

void test_response_serialization() {
    const personal_cloud::HttpResponse response {
        200,
        "OK",
        "text/plain; charset=utf-8",
        "abc",
    };

    const std::string serialized =
        personal_cloud::serialize_http_response(response);
    const std::string expected =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "Content-Length: 3\r\n"
        "Connection: close\r\n"
        "\r\n"
        "abc";

    expect(serialized == expected,
           "serialize status line, headers, and body");
}

}  // namespace

int main() {
    test_valid_request();
    test_invalid_requests();
    test_request_handling();
    test_response_serialization();

    if (failure_count != 0) {
        std::cerr << failure_count << " test assertion(s) failed.\n";
        return 1;
    }

    std::cout << "All HTTP tests passed.\n";
    return 0;
}
