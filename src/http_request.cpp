#include "http/http_request.h"

namespace personal_cloud {

bool parse_http_request(std::string_view raw_request, HttpRequest& request) {
    const std::size_t headers_end = raw_request.find("\r\n\r\n");
    if (headers_end == std::string_view::npos) {
        return false;
    }

    const std::size_t request_line_end = raw_request.find("\r\n");
    if (request_line_end == std::string_view::npos) {
        return false;
    }

    const std::string_view request_line =
        raw_request.substr(0, request_line_end);

    const std::size_t first_space = request_line.find(' ');
    if (first_space == std::string_view::npos || first_space == 0) {
        return false;
    }

    const std::size_t second_space = request_line.find(' ', first_space + 1);
    if (second_space == std::string_view::npos ||
        second_space == first_space + 1) {
        return false;
    }

    const std::string_view method = request_line.substr(0, first_space);
    const std::string_view path =
        request_line.substr(first_space + 1,
                            second_space - first_space - 1);
    const std::string_view version = request_line.substr(second_space + 1);

    constexpr std::string_view http_prefix = "HTTP/";
    if (version.size() < http_prefix.size() ||
        version.find(' ') != std::string_view::npos ||
        version.substr(0, http_prefix.size()) != http_prefix) {
        return false;
    }

    request.method = method;
    request.path = path;
    request.version = version;
    return true;
}

}  // namespace personal_cloud
