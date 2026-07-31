#include "http/http_response.h"

#include <string>

namespace personal_cloud {

std::string serialize_http_response(const HttpResponse& response) {
    std::string serialized;
    serialized.reserve(128 + response.body.size());

    serialized += "HTTP/1.1 ";
    serialized += std::to_string(response.status_code);
    serialized += ' ';
    serialized += response.reason;
    serialized += "\r\n";

    serialized += "Content-Type: ";
    serialized += response.content_type;
    serialized += "\r\n";

    serialized += "Content-Length: ";
    serialized += std::to_string(response.body.size());
    serialized += "\r\n";

    serialized += "Connection: close\r\n";
    serialized += "\r\n";
    serialized += response.body;

    return serialized;
}

}  // namespace personal_cloud
