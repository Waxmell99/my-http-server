#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

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

private:
    void execute(const char* sql);
    void apply_migrations();
    void verify_schema() const;

    sqlite3* connection_{nullptr};
};

}  // namespace personal_cloud
