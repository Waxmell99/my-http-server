#include "app/backend_config.h"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <system_error>
#include <utility>

#include <arpa/inet.h>

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
        if (option == "--allow-registration") {
            config.allow_registration = true;
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

        if (option == "--bind-address") {
            if (++index >= arguments.size()) {
                return missing_value(option);
            }
            in_addr parsed_address {};
            const std::string address(arguments[index]);
            if (::inet_pton(AF_INET, address.c_str(), &parsed_address) != 1) {
                return {
                    std::nullopt,
                    "Invalid IPv4 bind address: " + address,
                    false,
                };
            }
            config.server.bind_address = address;
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

        if (option == "--worker-count" || option == "--task-queue-size") {
            if (++index >= arguments.size()) {
                return missing_value(option);
            }
            std::size_t value = 0;
            if (!parse_positive_integer(arguments[index], value)) {
                return {
                    std::nullopt,
                    "Invalid value for " + std::string(option) + ": " +
                        std::string(arguments[index]),
                    false,
                };
            }
            if (option == "--worker-count") {
                config.server.application_worker_count = value;
            } else {
                config.server.application_queue_size = value;
            }
            continue;
        }

        if (option == "--max-file-size" || option == "--user-quota") {
            if (++index >= arguments.size()) {
                return missing_value(option);
            }
            std::uint64_t value = 0;
            if (!parse_positive_integer(arguments[index], value)) {
                return {
                    std::nullopt,
                    "Invalid value for " + std::string(option) + ": " +
                        std::string(arguments[index]),
                    false,
                };
            }
            if (option == "--max-file-size") {
                config.maximum_file_size = value;
            } else {
                config.user_quota = value;
            }
            continue;
        }

        if (option == "--max-concurrent-uploads" ||
            option == "--stream-buffer-size") {
            if (++index >= arguments.size()) {
                return missing_value(option);
            }
            std::size_t value = 0;
            if (!parse_positive_integer(arguments[index], value)) {
                return {
                    std::nullopt,
                    "Invalid value for " + std::string(option) + ": " +
                        std::string(arguments[index]),
                    false,
                };
            }
            if (option == "--max-concurrent-uploads") {
                config.maximum_concurrent_uploads = value;
            } else {
                config.server.streaming_chunk_size = value;
            }
            continue;
        }

        return {
            std::nullopt,
            "Unknown option: " + std::string(option),
            false,
        };
    }

    if (config.maximum_file_size > config.user_quota) {
        return {
            std::nullopt,
            "Maximum file size cannot exceed user quota",
            false,
        };
    }
    if (config.server.streaming_chunk_size > 4 * 1024 * 1024) {
        return {
            std::nullopt,
            "Stream buffer size cannot exceed 4194304 bytes",
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
    usage += "  --bind-address <IPv4>     Listening address (default: 127.0.0.1)\n";
    usage += "  --port <1-65535>          Listening port (default: 9000)\n";
    usage += "  --database <path>         SQLite database path\n";
    usage += "  --storage-root <path>     Stored file directory\n";
    usage += "  --max-connections <count> Active connection limit\n";
    usage += "  --idle-timeout <seconds>  Connection idle timeout\n";
    usage += "  --worker-count <count>     Application worker threads (default: 4)\n";
    usage += "  --task-queue-size <count>  Pending task limit (default: 256)\n";
    usage += "  --max-file-size <bytes>    Per-file upload limit (default: 1 GiB)\n";
    usage += "  --user-quota <bytes>       Per-user stored byte limit (default: 10 GiB)\n";
    usage += "  --max-concurrent-uploads <count> Global upload limit (default: 4)\n";
    usage += "  --stream-buffer-size <bytes> Upload/download chunk limit (default: 64 KiB)\n";
    usage += "  --allow-registration       Enable public account registration\n";
    usage += "  --verbose                  Enable per-connection logging\n";
    usage += "  --help, -h                 Show this help\n";
    return usage;
}

}  // namespace personal_cloud
