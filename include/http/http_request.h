#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>

namespace personal_cloud {

enum class HttpParseResult {
    incomplete,
    complete,
    bad_request,
    payload_too_large,
    version_not_supported,
    expectation_failed,
};

// HTTP Header 名会在解析时统一转换为小写。
struct HttpRequest {
    std::string method;
    std::string path;
    std::string version;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
};

// 解析请求行、Header 和 Content-Length 指定的 Body。
// 只有返回 complete 时才会填写 request。
HttpParseResult parse_http_request_head(
    std::string_view raw_request,
    HttpRequest& request,
    std::size_t& content_length,
    std::size_t& body_offset);

HttpParseResult parse_http_request(
    std::string_view raw_request,
    HttpRequest& request,
    std::size_t maximum_body_size);

}  // namespace personal_cloud
