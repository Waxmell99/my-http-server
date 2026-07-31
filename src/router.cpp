#include "http/router.h"

#include <string_view>

namespace personal_cloud {

HttpResponse route_request(const HttpRequest& request) {
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
