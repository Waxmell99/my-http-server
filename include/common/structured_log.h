#pragma once

#include "common/log.h"

#include <chrono>
#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>

namespace personal_cloud {

inline std::string json_escape(std::string_view value) {
    static constexpr std::string_view hex = "0123456789abcdef";
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        switch (character) {
            case '"': escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\b': escaped += "\\b"; break;
            case '\f': escaped += "\\f"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:
                if (byte < 0x20U) {
                    escaped += "\\u00";
                    escaped += hex[byte >> 4U];
                    escaped += hex[byte & 0x0fU];
                } else {
                    escaped += character;
                }
        }
    }
    return escaped;
}

inline void write_request_log(
    std::ostream& output,
    std::string_view event,
    std::string_view request_id,
    int client_fd,
    std::string_view method,
    std::string_view path,
    int status_code) {
    const std::int64_t timestamp_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    write_log(
        output,
        "{\"timestamp_ms\":", timestamp_ms,
        ",\"level\":\"info\",\"event\":\"", json_escape(event),
        "\",\"request_id\":\"", json_escape(request_id),
        "\",\"fd\":", client_fd,
        ",\"method\":\"", json_escape(method),
        "\",\"path\":\"", json_escape(path),
        "\",\"status\":", status_code,
        "}\n");
}

}  // namespace personal_cloud
