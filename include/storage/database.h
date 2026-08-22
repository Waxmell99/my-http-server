#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct sqlite3;

namespace personal_cloud {

enum class DatabaseOpenMode {
    initialize_schema,
    existing_schema,
};

using SessionTokenHash = std::array<unsigned char, 32>;

struct StoredUser {
    std::int64_t id{0};
    std::string username;
    std::string password_hash;
    std::int64_t created_at{0};
};

struct SessionUser {
    std::int64_t id{0};
    std::string username;
    std::int64_t created_at{0};
};

struct StoredFile {
    std::string id;
    std::int64_t user_id{0};
    std::string original_name;
    std::string storage_key;
    std::string mime_type;
    std::int64_t size{0};
    std::array<unsigned char, 32> sha256{};
    std::int64_t created_at{0};
    std::int64_t updated_at{0};
};

class Database final {
public:
    explicit Database(
        const std::filesystem::path& path,
        DatabaseOpenMode mode = DatabaseOpenMode::initialize_schema);
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    Database(Database&&) = delete;
    Database& operator=(Database&&) = delete;

    [[nodiscard]] int schema_version() const;
    [[nodiscard]] bool health_check() const noexcept;

    [[nodiscard]] std::optional<std::int64_t> create_user(
        std::string_view username,
        std::string_view password_hash,
        std::int64_t created_at);
    [[nodiscard]] std::optional<StoredUser> find_user_by_username(
        std::string_view username) const;
    [[nodiscard]] bool create_session(
        const SessionTokenHash& token_hash,
        std::int64_t user_id,
        std::int64_t expires_at,
        std::int64_t created_at);
    [[nodiscard]] std::optional<SessionUser> find_session_user(
        const SessionTokenHash& token_hash,
        std::int64_t now) const;
    bool delete_session(const SessionTokenHash& token_hash);
    void delete_expired_sessions(std::int64_t now);

    [[nodiscard]] std::int64_t total_file_size(std::int64_t user_id) const;
    [[nodiscard]] bool create_file(const StoredFile& file);
    [[nodiscard]] std::vector<StoredFile> list_files(
        std::int64_t user_id,
        std::size_t limit = 1000) const;
    [[nodiscard]] std::optional<StoredFile> find_file(
        std::string_view id,
        std::int64_t user_id) const;
    [[nodiscard]] bool rename_file(
        std::string_view id,
        std::int64_t user_id,
        std::string_view original_name,
        std::int64_t updated_at);
    [[nodiscard]] bool delete_file(
        std::string_view id,
        std::int64_t user_id);

private:
    void execute(const char* sql);
    void apply_migrations();
    void verify_schema() const;

    sqlite3* connection_{nullptr};
};

}  // namespace personal_cloud
