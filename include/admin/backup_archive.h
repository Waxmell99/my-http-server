#pragma once

#include <cstdint>
#include <filesystem>

namespace personal_cloud::admin {

struct ArchiveSummary {
    std::uint64_t object_count{0};
    std::uint64_t object_bytes{0};
};

ArchiveSummary create_backup_archive(
    const std::filesystem::path& database_path,
    const std::filesystem::path& storage_root,
    const std::filesystem::path& output_path,
    bool force);

ArchiveSummary restore_backup_archive(
    const std::filesystem::path& input_path,
    const std::filesystem::path& database_path,
    const std::filesystem::path& storage_root);

}  // namespace personal_cloud::admin
