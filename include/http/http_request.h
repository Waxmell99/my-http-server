#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

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
    // 由服务器连接层生成；不信任客户端提供的同名 Header。
    std::string request_id;

    HttpRequest() = default;

    HttpRequest(
        std::string method_value,
        std::string path_value,
        std::string version_value,
        std::unordered_map<std::string, std::string> headers_value,
        std::string body_value,
        std::string request_id_value = {})
        : method(std::move(method_value)),
          path(std::move(path_value)),
          version(std::move(version_value)),
          headers(std::move(headers_value)),
          body(std::move(body_value)),
          request_id(std::move(request_id_value)) {}
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
