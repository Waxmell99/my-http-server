#pragma once

#include <string>

namespace personal_cloud {

// 当前版本需要的最小 HTTP 响应信息。
struct HttpResponse {
    int status_code;
    std::string reason;
    std::string content_type;
    std::string body;
};

// 将响应对象序列化为符合 HTTP/1.1 格式的字符串。
std::string serialize_http_response(const HttpResponse& response);

}  // namespace personal_cloud
