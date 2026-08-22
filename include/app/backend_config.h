#pragma once

#include "server/epoll_server.h"

#include <filesystem>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace personal_cloud {

struct BackendConfig {
    EpollServerConfig server;
    std::filesystem::path database_path{"data/personal_cloud.db"};
    std::filesystem::path storage_root{"data/files"};
    std::uint64_t maximum_file_size{1024ULL * 1024ULL * 1024ULL};
    std::uint64_t user_quota{10ULL * 1024ULL * 1024ULL * 1024ULL};
    std::size_t maximum_concurrent_uploads{4};
};

struct ConfigParseResult {
    std::optional<BackendConfig> config;
    std::string error;
    bool show_help{false};
};

ConfigParseResult parse_backend_config(
    std::span<const std::string_view> arguments);

std::string backend_usage(std::string_view program_name);

}  // namespace personal_cloud
