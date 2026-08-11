#include "http/http_request.h"

#include <charconv>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace personal_cloud {
namespace {

bool is_token_character(char character) {
    if ((character >= 'a' && character <= 'z') ||
        (character >= 'A' && character <= 'Z') ||
        (character >= '0' && character <= '9')) {
        return true;
    }

    constexpr std::string_view symbols = "!#$%&'*+-.^_`|~";
    return symbols.find(character) != std::string_view::npos;
}

bool is_valid_request_target(std::string_view target) {
    for (char character : target) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte <= 0x20U || byte >= 0x7fU || character == '#') {
            return false;
        }
    }

    return true;
}

bool is_valid_header_value(std::string_view value) {
    for (char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (character != '\t' && (byte < 0x20U || byte == 0x7fU)) {
            return false;
        }
    }

    return true;
}

bool has_valid_http_version_syntax(std::string_view version) {
    return version.size() == 8 && version.substr(0, 5) == "HTTP/" &&
           version[5] >= '0' && version[5] <= '9' &&
           version[6] == '.' &&
           version[7] >= '0' && version[7] <= '9';
}

bool is_valid_host(std::string_view value) {
    if (value.empty()) {
        return false;
    }

    for (char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte <= 0x20U || byte >= 0x7fU) {
            return false;
        }
    }
    return true;
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

    for (char character : method) {
        if (!is_token_character(character)) {
            return HttpParseResult::bad_request;
        }
    }

    if (!is_valid_request_target(path) ||
        !has_valid_http_version_syntax(version)) {
        return HttpParseResult::bad_request;
    }

    if (version != "HTTP/1.0" && version != "HTTP/1.1") {
        return HttpParseResult::version_not_supported;
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
            if (!is_token_character(character)) {
                return HttpParseResult::bad_request;
            }
        }

        const std::string_view raw_value = line.substr(colon + 1);
        if (!is_valid_header_value(raw_value)) {
            return HttpParseResult::bad_request;
        }

        std::string name = lowercase_header_name(raw_name);
        const std::string_view value =
            trim_optional_whitespace(raw_value);

        if (parsed_request.headers.contains(name)) {
            return HttpParseResult::bad_request;
        }

        parsed_request.headers.emplace(std::move(name), std::string(value));
        line_start = line_end + 2;
    }

    const auto host_header = parsed_request.headers.find("host");
    if (version == "HTTP/1.1" &&
        (host_header == parsed_request.headers.end() ||
         !is_valid_host(host_header->second))) {
        return HttpParseResult::bad_request;
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

    if (version == "HTTP/1.1" &&
        parsed_request.headers.contains("expect")) {
        // The server does not implement the 100-continue interim-response
        // state, so it must send a final response instead of deadlocking with
        // a client that waits before transmitting its body.
        return HttpParseResult::expectation_failed;
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
