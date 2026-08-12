#include "app/backend_config.h"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <system_error>
#include <utility>

namespace personal_cloud {
namespace {

template <typename Integer>
bool parse_positive_integer(std::string_view text, Integer& value) {
    if (text.empty()) {
        return false;
    }

    Integer parsed{};
    const auto [end, error] = std::from_chars(
        text.data(), text.data() + text.size(), parsed);
    if (error != std::errc{} || end != text.data() + text.size() ||
        parsed == 0) {
        return false;
    }

    value = parsed;
    return true;
}

ConfigParseResult missing_value(std::string_view option) {
    return {
        std::nullopt,
        "Option requires a value: " + std::string(option),
        false,
    };
}

}  // namespace

ConfigParseResult parse_backend_config(
    std::span<const std::string_view> arguments) {
    BackendConfig config;

    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const std::string_view option = arguments[index];
        if (option == "--help" || option == "-h") {
            return {std::nullopt, {}, true};
        }
        if (option == "--verbose") {
            config.server.verbose_logging = true;
            continue;
        }

        if (option == "--port") {
            if (++index >= arguments.size()) {
                return missing_value(option);
            }
            std::uint16_t port = 0;
            if (!parse_positive_integer(arguments[index], port)) {
                return {
                    std::nullopt,
                    "Invalid port: " + std::string(arguments[index]),
                    false,
                };
            }
            config.server.port = port;
            continue;
        }

        if (option == "--database") {
            if (++index >= arguments.size()) {
                return missing_value(option);
            }
            if (arguments[index].empty()) {
                return {std::nullopt, "Database path cannot be empty", false};
            }
            config.database_path = arguments[index];
            continue;
        }

        if (option == "--storage-root") {
            if (++index >= arguments.size()) {
                return missing_value(option);
            }
            if (arguments[index].empty()) {
                return {std::nullopt, "Storage root cannot be empty", false};
            }
            config.storage_root = arguments[index];
            continue;
        }

        if (option == "--max-connections") {
            if (++index >= arguments.size()) {
                return missing_value(option);
            }
            std::size_t maximum_connections = 0;
            if (!parse_positive_integer(
                    arguments[index], maximum_connections)) {
                return {
                    std::nullopt,
                    "Invalid maximum connection count: " +
                        std::string(arguments[index]),
                    false,
                };
            }
            config.server.maximum_connections = maximum_connections;
            continue;
        }

        if (option == "--idle-timeout") {
            if (++index >= arguments.size()) {
                return missing_value(option);
            }
            std::uint64_t timeout_seconds = 0;
            if (!parse_positive_integer(arguments[index], timeout_seconds) ||
                timeout_seconds > static_cast<std::uint64_t>(
                                      std::numeric_limits<int>::max())) {
                return {
                    std::nullopt,
                    "Invalid idle timeout: " +
                        std::string(arguments[index]),
                    false,
                };
            }
            config.server.idle_timeout =
                std::chrono::seconds(timeout_seconds);
            continue;
        }

        return {
            std::nullopt,
            "Unknown option: " + std::string(option),
            false,
        };
    }

    return {std::move(config), {}, false};
}

std::string backend_usage(std::string_view program_name) {
    std::string usage;
    usage += "Usage: ";
    usage += program_name;
    usage += " [options]\n\n";
    usage += "Options:\n";
    usage += "  --port <1-65535>          Listening port (default: 9000)\n";
    usage += "  --database <path>         SQLite database path\n";
    usage += "  --storage-root <path>     Stored file directory\n";
    usage += "  --max-connections <count> Active connection limit\n";
    usage += "  --idle-timeout <seconds>  Connection idle timeout\n";
    usage += "  --verbose                  Enable per-connection logging\n";
    usage += "  --help, -h                 Show this help\n";
    return usage;
}

}  // namespace personal_cloud
