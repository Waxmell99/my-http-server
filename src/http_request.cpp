#include "http/http_request.h"

#include <charconv>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace personal_cloud {
namespace {

bool is_header_name_character(char character) {
    if ((character >= 'a' && character <= 'z') ||
        (character >= 'A' && character <= 'Z') ||
        (character >= '0' && character <= '9')) {
        return true;
    }

    constexpr std::string_view symbols = "!#$%&'*+-.^_`|~";
    return symbols.find(character) != std::string_view::npos;
}

std::string lowercase_header_name(std::string_view name) {
    std::string lowercase;
    lowercase.reserve(name.size());

    for (char character : name) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
        lowercase += character;
    }

    return lowercase;
}

std::string_view trim_optional_whitespace(std::string_view value) {
    while (!value.empty() &&
           (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }

    while (!value.empty() &&
           (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }

    return value;
}

}  // namespace

HttpParseResult parse_http_request(
    std::string_view raw_request,
    HttpRequest& request,
    std::size_t maximum_body_size) {
    const std::size_t headers_end = raw_request.find("\r\n\r\n");
    if (headers_end == std::string_view::npos) {
        return HttpParseResult::incomplete;
    }

    const std::size_t request_line_end = raw_request.find("\r\n");
    if (request_line_end == std::string_view::npos ||
        request_line_end > headers_end) {
        return HttpParseResult::bad_request;
    }

    const std::string_view request_line =
        raw_request.substr(0, request_line_end);

    const std::size_t first_space = request_line.find(' ');
    if (first_space == std::string_view::npos || first_space == 0) {
        return HttpParseResult::bad_request;
    }

    const std::size_t second_space = request_line.find(' ', first_space + 1);
    if (second_space == std::string_view::npos ||
        second_space == first_space + 1) {
        return HttpParseResult::bad_request;
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
        return HttpParseResult::bad_request;
    }

    HttpRequest parsed_request;
    parsed_request.method = method;
    parsed_request.path = path;
    parsed_request.version = version;

    std::size_t line_start = request_line_end + 2;
    while (line_start < headers_end) {
        const std::size_t line_end = raw_request.find("\r\n", line_start);
        if (line_end == std::string_view::npos || line_end > headers_end) {
            return HttpParseResult::bad_request;
        }

        const std::string_view line =
            raw_request.substr(line_start, line_end - line_start);
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0) {
            return HttpParseResult::bad_request;
        }

        const std::string_view raw_name = line.substr(0, colon);
        for (char character : raw_name) {
            if (!is_header_name_character(character)) {
                return HttpParseResult::bad_request;
            }
        }

        std::string name = lowercase_header_name(raw_name);
        const std::string_view value =
            trim_optional_whitespace(line.substr(colon + 1));

        if (parsed_request.headers.contains(name)) {
            return HttpParseResult::bad_request;
        }

        parsed_request.headers.emplace(std::move(name), std::string(value));
        line_start = line_end + 2;
    }

    if (parsed_request.headers.contains("transfer-encoding")) {
        // 当前版本不支持 chunked request body。
        return HttpParseResult::bad_request;
    }

    std::size_t content_length = 0;
    const auto content_length_header =
        parsed_request.headers.find("content-length");
    if (content_length_header != parsed_request.headers.end()) {
        const std::string& value = content_length_header->second;
        if (value.empty()) {
            return HttpParseResult::bad_request;
        }

        const auto [end, error] = std::from_chars(
            value.data(), value.data() + value.size(), content_length);
        if (error != std::errc{} || end != value.data() + value.size()) {
            return HttpParseResult::bad_request;
        }
    }

    if (content_length > maximum_body_size) {
        return HttpParseResult::payload_too_large;
    }

    const std::size_t body_start = headers_end + 4;
    const std::size_t received_body_size = raw_request.size() - body_start;
    if (received_body_size < content_length) {
        return HttpParseResult::incomplete;
    }

    parsed_request.body.assign(
        raw_request.substr(body_start, content_length));
    request = std::move(parsed_request);
    return HttpParseResult::complete;
}

}  // namespace personal_cloud
