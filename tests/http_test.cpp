#include "common/structured_log.h"
#include "http/http_request.h"
#include "http/http_response.h"
#include "http/router.h"

#include <iostream>
#include <sstream>
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

void test_protocol_validation() {
    personal_cloud::HttpRequest request;

    expect(personal_cloud::parse_http_request(
               "GET /hello HTTP/1.1\r\n\r\n",
               request,
               maximum_body_size) ==
               personal_cloud::HttpParseResult::bad_request,
           "require Host for HTTP/1.1");

    expect(personal_cloud::parse_http_request(
               "GET /hello HTTP/1.1\r\nHost:\t \r\n\r\n",
               request,
               maximum_body_size) ==
               personal_cloud::HttpParseResult::bad_request,
           "reject an empty HTTP/1.1 Host");

    expect(personal_cloud::parse_http_request(
               "GET /hello HTTP/1.1\r\nHost: local host\r\n\r\n",
               request,
               maximum_body_size) ==
               personal_cloud::HttpParseResult::bad_request,
           "reject whitespace inside an HTTP/1.1 Host");

    expect(personal_cloud::parse_http_request(
               "GET /hello HTTP/1.0\r\n\r\n",
               request,
               maximum_body_size) ==
               personal_cloud::HttpParseResult::complete,
           "allow HTTP/1.0 without Host");

    expect(personal_cloud::parse_http_request(
               "G@T /hello HTTP/1.1\r\nHost: localhost\r\n\r\n",
               request,
               maximum_body_size) ==
               personal_cloud::HttpParseResult::bad_request,
           "reject a method containing a non-token character");

    expect(personal_cloud::parse_http_request(
               "GET /hello#fragment HTTP/1.1\r\n"
               "Host: localhost\r\n\r\n",
               request,
               maximum_body_size) ==
               personal_cloud::HttpParseResult::bad_request,
           "reject a fragment in the request target");

    expect(personal_cloud::parse_http_request(
               "GET /hello HTTP/1.1\r\n"
               "Host: localhost\r\n"
               "X-Test: ok" "\x01" "bad\r\n\r\n",
               request,
               maximum_body_size) ==
               personal_cloud::HttpParseResult::bad_request,
           "reject a control character in a Header value");

    expect(personal_cloud::parse_http_request(
               "GET /hello HTTP/2.0\r\nHost: localhost\r\n\r\n",
               request,
               maximum_body_size) ==
               personal_cloud::HttpParseResult::version_not_supported,
           "distinguish a valid but unsupported HTTP version");

    expect(personal_cloud::parse_http_request(
               "GET /hello HTTP/1.01\r\nHost: localhost\r\n\r\n",
               request,
               maximum_body_size) ==
               personal_cloud::HttpParseResult::bad_request,
           "reject malformed HTTP version syntax");

    expect(personal_cloud::parse_http_request(
               "POST /upload HTTP/1.1\r\n"
               "Host: localhost\r\n"
               "Content-Length: 5\r\n"
               "Expect: 100-continue\r\n\r\n",
               request,
               maximum_body_size) ==
               personal_cloud::HttpParseResult::expectation_failed,
           "reject unsupported expectations without waiting for the body");

    expect(personal_cloud::parse_http_request(
               "POST /upload HTTP/1.1\r\n"
               "Host: localhost\r\n"
               "Content-Length: 1\r\n"
               "Content-Length: 1\r\n\r\nx",
               request,
               maximum_body_size) ==
               personal_cloud::HttpParseResult::bad_request,
           "reject duplicate Content-Length framing");

    expect(personal_cloud::parse_http_request(
               "POST /upload HTTP/1.1\r\n"
               "Host: localhost\r\n"
               "Transfer-Encoding: chunked\r\n\r\n",
               request,
               maximum_body_size) ==
               personal_cloud::HttpParseResult::bad_request,
           "reject unsupported Transfer-Encoding framing");

    expect(personal_cloud::parse_http_request(
               "POST /upload HTTP/1.1\r\n"
               "Host: localhost\r\n"
               "Content-Length: 999999999999999999999999999999\r\n\r\n",
               request,
               maximum_body_size) ==
               personal_cloud::HttpParseResult::bad_request,
           "reject an overflowing Content-Length");
}

void test_request_body() {
    personal_cloud::HttpRequest request;
    const personal_cloud::HttpParseResult complete =
        personal_cloud::parse_http_request(
            "POST /upload HTTP/1.1\r\n"
            "Host: localhost\r\n"
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
            "Host: localhost\r\n"
            "Content-Length: 5\r\n\r\nabc",
            request,
            maximum_body_size);
    expect(incomplete == personal_cloud::HttpParseResult::incomplete,
           "wait for the complete request body");

    const personal_cloud::HttpParseResult too_large =
        personal_cloud::parse_http_request(
            "POST /upload HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Content-Length: 5\r\n\r\n",
            request,
            4);
    expect(too_large == personal_cloud::HttpParseResult::payload_too_large,
           "reject a body larger than the configured limit");

    std::size_t streamed_content_length = 0;
    std::size_t streamed_body_offset = 0;
    const std::string streamed_head =
        "POST /api/files HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 1000000\r\n\r\npartial";
    const personal_cloud::HttpParseResult head_result =
        personal_cloud::parse_http_request_head(
            streamed_head,
            request,
            streamed_content_length,
            streamed_body_offset);
    expect(head_result == personal_cloud::HttpParseResult::complete &&
               streamed_content_length == 1000000 &&
               streamed_body_offset < streamed_head.size() &&
               request.body.empty(),
           "parse streaming request metadata before the full body arrives");
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
        {"GET", "/health?verbose=true", "HTTP/1.1", {}, {}});
    expect(response.status_code == 200,
           "route using the path portion before a query string");

    response = personal_cloud::route_request(
        {"GET", "/", "HTTP/1.1", {}, {}});
    expect(response.status_code == 200, "GET / returns the personal home page");
    expect(response.body.find("id=\"cloudEntry\"") != std::string::npos &&
               response.body.find("href=\"/app\"") != std::string::npos &&
               response.body.find("fetch('/api/status'") != std::string::npos,
           "home page links to the cloud console and reports live status");

    const personal_cloud::HttpResponse app_alias =
        personal_cloud::route_request(
            {"GET", "/app", "HTTP/1.1", {}, {}});
    expect(app_alias.status_code == 200 &&
               app_alias.body.find("/api/auth/me") != std::string::npos &&
               app_alias.body != response.body,
           "serve the authentication and file console from /app");

    response = personal_cloud::route_request(
        {"GET", "/missing", "HTTP/1.1", {}, {}});
    expect(response.status_code == 404, "unknown GET path returns 404");

    response = personal_cloud::route_request(
        {"POST", "/hello", "HTTP/1.1", {}, {}});
    expect(response.status_code == 405, "unsupported method returns 405");
    expect(response.headers.size() == 1 &&
               response.headers.front() ==
                   std::pair<std::string, std::string>{"Allow", "GET"},
           "405 response identifies the allowed method");

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

    unsupported_upload.headers["content-type"] =
        " Text/Plain \t; charset=utf-8";
    response = personal_cloud::route_request(unsupported_upload);
    expect(response.status_code == 200,
           "POST /upload accepts case-insensitive media types and OWS");
}

void test_response_serialization() {
    const personal_cloud::HttpResponse response {
        200,
        "OK",
        "text/plain; charset=utf-8",
        "abc",
        {{"X-Test", "value"}},
    };

    const std::string serialized =
        personal_cloud::serialize_http_response(response);
    const std::string expected =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "X-Test: value\r\n"
        "Content-Length: 3\r\n"
        "Connection: close\r\n"
        "\r\n"
        "abc";

    expect(serialized == expected,
           "serialize status line, headers, and body");

    const std::string with_transport_header =
        personal_cloud::serialize_http_response(
            response, {{"X-Request-ID", "request-123"}});
    expect(with_transport_header.find(
               "\r\nX-Request-ID: request-123\r\n") != std::string::npos &&
               with_transport_header.ends_with("\r\n\r\nabc"),
           "add transport headers without copying or changing the body");

    const std::string streamed_head =
        personal_cloud::serialize_http_response_head(
            response, 1000000, {{"X-Request-ID", "stream-456"}});
    expect(streamed_head.find("Content-Length: 1000000\r\n") !=
                   std::string::npos &&
               streamed_head.find("X-Request-ID: stream-456\r\n") !=
                   std::string::npos &&
               streamed_head.ends_with("\r\n\r\n") &&
               streamed_head.find("abc") == std::string::npos,
           "serialize a streaming response head without buffering its body");
}

void test_structured_request_log() {
    std::ostringstream output;
    personal_cloud::write_request_log(
        output,
        "request_completed",
        "req-1",
        7,
        "GET",
        "/quoted\"path",
        200);
    const std::string line = output.str();
    expect(line.find("\"event\":\"request_completed\"") !=
                   std::string::npos &&
               line.find("\"request_id\":\"req-1\"") !=
                   std::string::npos &&
               line.find("\"path\":\"/quoted\\\"path\"") !=
                   std::string::npos &&
               line.find("\"status\":200") != std::string::npos &&
               line.ends_with('\n'),
           "emit escaped one-line structured request logs");
}

}  // namespace

int main() {
    test_valid_request();
    test_invalid_requests();
    test_protocol_validation();
    test_request_body();
    test_routing();
    test_response_serialization();
    test_structured_request_log();

    if (failure_count != 0) {
        std::cerr << failure_count << " test assertion(s) failed.\n";
        return 1;
    }

    std::cout << "All HTTP tests passed.\n";
    return 0;
}
