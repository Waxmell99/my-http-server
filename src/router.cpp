#include "http/router.h"

#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace personal_cloud {
namespace {

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
    if (request.path == "/") {
        if (request.method != "GET") {
            return {
                405,
                "Method Not Allowed",
                "text/plain; charset=utf-8",
                "Method Not Allowed\n",
            };
        }

        const auto html = read_file("public/index_ds.html");
        if (!html.has_value()) {
            return {
                500,
                "Internal Server Error",
                "text/plain; charset=utf-8",
                "Cannot open public/index.html\n",
            };
        }

        return {
            200,
            "OK",
            "text/html; charset=utf-8",
            *html,
        };

    }

    if (request.path == "/turntable") {
        if (request.method != "GET") {
            return {
                405,
                "Method Not Allowed",
                "text/plain; charset=utf-8",
                "Method Not Allowed\n",
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

    if (request.path == "/hello") {
        if (request.method != "GET") {
            return {
                405,
                "Method Not Allowed",
                "text/plain; charset=utf-8",
                "Method Not Allowed\n",
            };
        }

        return {
            200,
            "OK",
            "text/plain; charset=utf-8",
            "Hello World\n",
        };
    }

    if (request.path == "/health") {
        if (request.method != "GET") {
            return {
                405,
                "Method Not Allowed",
                "text/plain; charset=utf-8",
                "Method Not Allowed\n",
            };
        }

        return {
            200,
            "OK",
            "text/plain; charset=utf-8",
            "OK\n",
        };
    }

    if (request.path == "/upload") {
        if (request.method != "POST") {
            return {
                405,
                "Method Not Allowed",
                "text/plain; charset=utf-8",
                "Method Not Allowed\n",
            };
        }

        const auto content_type = request.headers.find("content-type");
        constexpr std::string_view text_plain = "text/plain";
        const bool is_text_plain =
            content_type != request.headers.end() &&
            (content_type->second == text_plain ||
             (content_type->second.size() > text_plain.size() &&
              content_type->second.compare(
                  0, text_plain.size(), text_plain) == 0 &&
              content_type->second[text_plain.size()] == ';'));
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
}

}  // namespace personal_cloud
