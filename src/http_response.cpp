#include "http/http_response.h"

#include <string>

namespace personal_cloud {

std::string serialize_http_response_head(
    const HttpResponse& response,
    std::uint64_t content_length) {
    std::string serialized;
    serialized.reserve(256);

    serialized += "HTTP/1.1 ";
    serialized += std::to_string(response.status_code);
    serialized += ' ';
    serialized += response.reason;
    serialized += "\r\n";

    serialized += "Content-Type: ";
    serialized += response.content_type;
    serialized += "\r\n";

    for (const auto& [name, value] : response.headers) {
        serialized += name;
        serialized += ": ";
        serialized += value;
        serialized += "\r\n";
    }

    serialized += "Content-Length: ";
    serialized += std::to_string(content_length);
    serialized += "\r\n";

    serialized += "Connection: close\r\n";
    serialized += "\r\n";
    return serialized;
}

std::string serialize_http_response(const HttpResponse& response) {
    std::string serialized = serialize_http_response_head(
        response, static_cast<std::uint64_t>(response.body.size()));
    serialized += response.body;

    return serialized;
}

}  // namespace personal_cloud
