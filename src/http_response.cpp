#include "http/http_response.h"

#include <string>

namespace personal_cloud {

bool handle_http_request(const HttpRequest& request, HttpResponse& response) {
    if (request.method != "GET") {
        response = {
            405,
            "Method Not Allowed",
            "text/plain; charset=utf-8",
            "Method Not Allowed\n",
        };
        return false;
    }

    if (request.path == "/hello") {
        response = {
            200,
            "OK",
            "text/plain; charset=utf-8",
            "Hello World\n",
        };
        return true;
    }

    if (request.path == "/health") {
        response = {
            200,
            "OK",
            "text/plain; charset=utf-8",
            "OK\n",
        };
        return true;
    }

    response = {
        404,
        "Not Found",
        "text/plain; charset=utf-8",
        "Not Found\n",
    };
    return false;
}

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
