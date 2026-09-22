#include "http/router.h"

#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace personal_cloud {
namespace {

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

bool ascii_case_insensitive_equal(
    std::string_view left,
    std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }

    for (std::size_t index = 0; index < left.size(); ++index) {
        char left_character = left[index];
        char right_character = right[index];
        if (left_character >= 'A' && left_character <= 'Z') {
            left_character = static_cast<char>(
                left_character - 'A' + 'a');
        }
        if (right_character >= 'A' && right_character <= 'Z') {
            right_character = static_cast<char>(
                right_character - 'A' + 'a');
        }
        if (left_character != right_character) {
            return false;
        }
    }

    return true;
}

bool is_text_plain_content_type(std::string_view value) {
    const std::size_t parameters_start = value.find(';');
    const std::string_view media_type = trim_optional_whitespace(
        value.substr(0, parameters_start));
    return ascii_case_insensitive_equal(media_type, "text/plain");
}

std::optional<std::string> read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }

    std::ostringstream content;
    content << file.rdbuf();
    return content.str();
}

}

HttpResponse route_request(const HttpRequest& request) {
    std::string_view path = request.path;
    const std::size_t query_start = path.find('?');
    if (query_start != std::string_view::npos) {
        path = path.substr(0, query_start);
    }

    if (path == "/" || path == "/app") {
        if (request.method != "GET") {
            return {
                405,
                "Method Not Allowed",
                "text/plain; charset=utf-8",
                "Method Not Allowed\n",
                {{"Allow", "GET"}},
            };
        }

        const std::string file_path = path == "/"
            ? "public/index.html"
            : "public/app.html";
        const auto html = read_file(file_path);
        if (!html.has_value()) {
            return {
                500,
                "Internal Server Error",
                "text/plain; charset=utf-8",
                "Cannot open " + file_path + "\n",
            };
        }

        return {
            200,
            "OK",
            "text/html; charset=utf-8",
            *html,
        };

    }

    if (path == "/turntable") {
        if (request.method != "GET") {
            return {
                405,
                "Method Not Allowed",
                "text/plain; charset=utf-8",
                "Method Not Allowed\n",
                {{"Allow", "GET"}},
            };
        }

        const auto html = read_file("public/tt.html");
        if (!html.has_value()) {
            return {
                500,
                "Internal Server Error",
                "text/plain; charset=utf-8",
                "Cannot open public/tt.html\n",
            };
        }

        return {
            200,
            "OK",
            "text/html; charset=utf-8",
            *html,
        };

    }

    if (path == "/hello") {
        if (request.method != "GET") {
            return {
                405,
                "Method Not Allowed",
                "text/plain; charset=utf-8",
                "Method Not Allowed\n",
                {{"Allow", "GET"}},
            };
        }

        return {
            200,
            "OK",
            "text/plain; charset=utf-8",
            "Hello World\n",
        };
    }

    if (path == "/health") {
        if (request.method != "GET") {
            return {
                405,
                "Method Not Allowed",
                "text/plain; charset=utf-8",
                "Method Not Allowed\n",
                {{"Allow", "GET"}},
            };
        }

        return {
            200,
            "OK",
            "text/plain; charset=utf-8",
            "OK\n",
        };
    }

    if (path == "/upload") {
        if (request.method != "POST") {
            return {
                405,
                "Method Not Allowed",
                "text/plain; charset=utf-8",
                "Method Not Allowed\n",
                {{"Allow", "POST"}},
            };
        }

        const auto content_type = request.headers.find("content-type");
        const bool is_text_plain =
            content_type != request.headers.end() &&
            is_text_plain_content_type(content_type->second);
        if (!is_text_plain) {
            return {
                415,
                "Unsupported Media Type",
                "text/plain; charset=utf-8",
                "Content-Type must be text/plain\n",
            };
        }

        return {
            200,
            "OK",
            "text/plain; charset=utf-8",
            "Received text:\n" + request.body,
        };
    }

    return {
        404,
        "Not Found",
        "text/plain; charset=utf-8",
        "Not Found\n",
    };
}  // namespace

}  // namespace personal_cloud
