#pragma once

#include <string>
#include <utility>
#include <vector>

namespace personal_cloud {

// 当前版本需要的最小 HTTP 响应信息。
struct HttpResponse {
    int status_code;
    std::string reason;
    std::string content_type;
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;

    HttpResponse(
        int status_code_value,
        std::string reason_value,
        std::string content_type_value,
        std::string body_value,
        std::vector<std::pair<std::string, std::string>>
            response_headers = {})
        : status_code(status_code_value),
          reason(std::move(reason_value)),
          content_type(std::move(content_type_value)),
          body(std::move(body_value)),
          headers(std::move(response_headers)) {}
};

// 将响应对象序列化为符合 HTTP/1.1 格式的字符串。
std::string serialize_http_response(const HttpResponse& response);

}  // namespace personal_cloud
