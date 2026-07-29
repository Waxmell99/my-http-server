#pragma once

#include "http/http_request.h"

#include <string>

namespace personal_cloud {

// 当前版本需要的最小 HTTP 响应信息。
struct HttpResponse {
    int status_code;
    std::string reason;
    std::string content_type;
    std::string body;
};

// 根据请求生成响应。匹配到有效 GET 路由时返回 true；
// 返回 404 或 405 时返回 false，但 response 仍然是可发送的完整响应。
bool handle_http_request(const HttpRequest& request, HttpResponse& response);

// 将响应对象序列化为符合 HTTP/1.1 格式的字符串。
std::string serialize_http_response(const HttpResponse& response);

}  // namespace personal_cloud
