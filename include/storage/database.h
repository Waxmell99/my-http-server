#pragma once

#include <filesystem>

struct sqlite3;

namespace personal_cloud {

class Database final {
public:
    explicit Database(const std::filesystem::path& path);
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    Database(Database&&) = delete;
    Database& operator=(Database&&) = delete;

    [[nodiscard]] int schema_version() const;
    [[nodiscard]] bool health_check() const noexcept;

private:
    void execute(const char* sql);
    void apply_migrations();
    void verify_schema() const;

    sqlite3* connection_{nullptr};
};

}  // namespace personal_cloud
