#pragma once

#include <string>
#include <string_view>

namespace personal_cloud {

// 当前只保存 HTTP 请求行中的三个部分。
struct HttpRequest {
    std::string method;
    std::string path;
    std::string version;
};

// 解析完整 HTTP Header 中的请求行。
// 成功时填写 request 并返回 true，数据不完整或格式错误时返回 false。
bool parse_http_request(std::string_view raw_request, HttpRequest& request);

}  // namespace personal_cloud
