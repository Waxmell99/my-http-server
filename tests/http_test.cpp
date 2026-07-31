#include "http/http_request.h"
#include "http/http_response.h"
#include "http/router.h"

#include <iostream>
#include <string>
#include <string_view>

namespace {

int failure_count = 0;
constexpr std::size_t maximum_body_size = 1024;

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
    const personal_cloud::HttpParseResult result =
        personal_cloud::parse_http_request(
            "GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n",
            request,
            maximum_body_size);

    expect(result == personal_cloud::HttpParseResult::complete,
           "parse a complete GET request");
    expect(request.method == "GET", "extract request method");
    expect(request.path == "/hello", "extract request path");
    expect(request.version == "HTTP/1.1", "extract HTTP version");
    expect(request.headers.at("host") == "localhost",
           "store a lowercase Header name");
}

void test_invalid_requests() {
    personal_cloud::HttpRequest request;

    expect(personal_cloud::parse_http_request(
               "GET /hello HTTP/1.1\r\nHost: localhost\r\n",
               request,
               maximum_body_size) ==
               personal_cloud::HttpParseResult::incomplete,
           "reject an incomplete HTTP header");

    expect(personal_cloud::parse_http_request(
               "GET  HTTP/1.1\r\n\r\n",
               request,
               maximum_body_size) ==
               personal_cloud::HttpParseResult::bad_request,
           "reject a request without a path");

    expect(personal_cloud::parse_http_request(
               "GET /hello FTP/1.0\r\n\r\n",
               request,
               maximum_body_size) ==
               personal_cloud::HttpParseResult::bad_request,
           "reject a non-HTTP version");

    expect(personal_cloud::parse_http_request(
               "POST /upload HTTP/1.1\r\nContent-Length: abc\r\n\r\n",
               request,
               maximum_body_size) ==
               personal_cloud::HttpParseResult::bad_request,
           "reject an invalid Content-Length");
}

void test_request_body() {
    personal_cloud::HttpRequest request;
    const personal_cloud::HttpParseResult complete =
        personal_cloud::parse_http_request(
            "POST /upload HTTP/1.1\r\n"
            "Content-Type: text/plain; charset=utf-8\r\n"
            "Content-Length: 11\r\n"
            "\r\n"
            "hello world",
            request,
            maximum_body_size);

    expect(complete == personal_cloud::HttpParseResult::complete,
           "parse a complete POST body");
    expect(request.headers.at("content-type") ==
               "text/plain; charset=utf-8",
           "parse and normalize Content-Type");
    expect(request.body == "hello world", "extract request body");

    const personal_cloud::HttpParseResult incomplete =
        personal_cloud::parse_http_request(
            "POST /upload HTTP/1.1\r\n"
            "Content-Length: 5\r\n\r\nabc",
            request,
            maximum_body_size);
    expect(incomplete == personal_cloud::HttpParseResult::incomplete,
           "wait for the complete request body");

    const personal_cloud::HttpParseResult too_large =
        personal_cloud::parse_http_request(
            "POST /upload HTTP/1.1\r\n"
            "Content-Length: 5\r\n\r\n",
            request,
            4);
    expect(too_large == personal_cloud::HttpParseResult::payload_too_large,
           "reject a body larger than the configured limit");
}

void test_routing() {
    personal_cloud::HttpResponse response = personal_cloud::route_request(
        {"GET", "/hello", "HTTP/1.1", {}, {}});
    expect(response.status_code == 200, "GET /hello returns 200");
    expect(response.body == "Hello World\n", "GET /hello returns its body");

    response = personal_cloud::route_request(
        {"GET", "/health", "HTTP/1.1", {}, {}});
    expect(response.status_code == 200, "GET /health returns 200");
    expect(response.body == "OK\n", "GET /health returns its body");

    response = personal_cloud::route_request(
        {"GET", "/missing", "HTTP/1.1", {}, {}});
    expect(response.status_code == 404, "unknown GET path returns 404");

    response = personal_cloud::route_request(
        {"POST", "/hello", "HTTP/1.1", {}, {}});
    expect(response.status_code == 405, "unsupported method returns 405");

    const personal_cloud::HttpRequest upload_request {
        "POST",
        "/upload",
        "HTTP/1.1",
        {{"content-type", "text/plain"}},
        "hello upload",
    };
    response = personal_cloud::route_request(upload_request);
    expect(response.status_code == 200, "POST /upload returns 200");
    expect(response.body == "Received text:\nhello upload",
           "POST /upload returns the received text");

    personal_cloud::HttpRequest unsupported_upload = upload_request;
    unsupported_upload.headers["content-type"] = "application/json";
    response = personal_cloud::route_request(unsupported_upload);
    expect(response.status_code == 415,
           "POST /upload rejects a non-text Content-Type");

    unsupported_upload.headers["content-type"] = "text/plainish";
    response = personal_cloud::route_request(unsupported_upload);
    expect(response.status_code == 415,
           "POST /upload rejects an invalid text/plain prefix");
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
    test_request_body();
    test_routing();
    test_response_serialization();

    if (failure_count != 0) {
        std::cerr << failure_count << " test assertion(s) failed.\n";
        return 1;
    }

    std::cout << "All HTTP tests passed.\n";
    return 0;
}
