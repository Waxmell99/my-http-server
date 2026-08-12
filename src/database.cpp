#include "storage/database.h"

#include <sqlite3.h>

#include <filesystem>
#include <stdexcept>
#include <string>

namespace personal_cloud {
namespace {

constexpr int latest_schema_version = 1;

class Statement final {
public:
    Statement(sqlite3* connection, const char* sql) {
        const int result = ::sqlite3_prepare_v2(
            connection, sql, -1, &statement_, nullptr);
        if (result != SQLITE_OK) {
            throw std::runtime_error(
                "SQLite prepare failed: " +
                std::string(::sqlite3_errmsg(connection)));
        }
    }

    ~Statement() {
        if (statement_ != nullptr) {
            ::sqlite3_finalize(statement_);
        }
    }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    [[nodiscard]] sqlite3_stmt* get() const noexcept {
        return statement_;
    }

private:
    sqlite3_stmt* statement_{nullptr};
};

std::runtime_error sqlite_error(sqlite3* connection, const char* operation) {
    return std::runtime_error(
        std::string(operation) + ": " + ::sqlite3_errmsg(connection));
}

}  // namespace

Database::Database(const std::filesystem::path& path) {
    const std::string path_text = path.string();
    const int result = ::sqlite3_open_v2(
        path_text.c_str(),
        &connection_,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        nullptr);
    if (result != SQLITE_OK) {
        const std::string message = connection_ != nullptr
                                        ? ::sqlite3_errmsg(connection_)
                                        : ::sqlite3_errstr(result);
        if (connection_ != nullptr) {
            ::sqlite3_close(connection_);
            connection_ = nullptr;
        }
        throw std::runtime_error("Cannot open SQLite database: " + message);
    }

    try {
        ::sqlite3_extended_result_codes(connection_, 1);
        if (::sqlite3_busy_timeout(connection_, 5000) != SQLITE_OK) {
            throw sqlite_error(
                connection_, "Cannot configure SQLite busy timeout");
        }

        execute("PRAGMA foreign_keys = ON;");
        execute("PRAGMA journal_mode = WAL;");
        execute("PRAGMA synchronous = NORMAL;");
        apply_migrations();
        verify_schema();
    } catch (...) {
        ::sqlite3_close(connection_);
        connection_ = nullptr;
        throw;
    }
}

Database::~Database() {
    if (connection_ != nullptr) {
        ::sqlite3_close(connection_);
    }
}

int Database::schema_version() const {
    Statement statement(
        connection_,
        "SELECT COALESCE(MAX(version), 0) FROM schema_migrations;");
    if (::sqlite3_step(statement.get()) != SQLITE_ROW) {
        throw sqlite_error(connection_, "Cannot read database schema version");
    }
    return ::sqlite3_column_int(statement.get(), 0);
}

bool Database::health_check() const noexcept {
    sqlite3_stmt* statement = nullptr;
    if (::sqlite3_prepare_v2(
            connection_, "SELECT 1;", -1, &statement, nullptr) != SQLITE_OK) {
        return false;
    }

    const bool healthy = ::sqlite3_step(statement) == SQLITE_ROW &&
                         ::sqlite3_column_int(statement, 0) == 1;
    ::sqlite3_finalize(statement);
    return healthy;
}

void Database::execute(const char* sql) {
    char* error_message = nullptr;
    const int result =
        ::sqlite3_exec(connection_, sql, nullptr, nullptr, &error_message);
    if (result == SQLITE_OK) {
        return;
    }

    const std::string message = error_message != nullptr
                                    ? error_message
                                    : ::sqlite3_errmsg(connection_);
    ::sqlite3_free(error_message);
    throw std::runtime_error("SQLite execution failed: " + message);
}

void Database::apply_migrations() {
    execute("BEGIN IMMEDIATE;");
    try {
        execute(
            "CREATE TABLE IF NOT EXISTS schema_migrations ("
            "version INTEGER PRIMARY KEY,"
            "applied_at INTEGER NOT NULL"
            ");");

        const int current_version = schema_version();
        if (current_version > latest_schema_version) {
            throw std::runtime_error(
                "Database schema is newer than this server supports");
        }

        if (current_version < 1) {
            execute(
                "CREATE TABLE users ("
                "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                "username TEXT NOT NULL COLLATE NOCASE UNIQUE,"
                "password_hash TEXT NOT NULL,"
                "created_at INTEGER NOT NULL,"
                "CHECK(length(username) BETWEEN 3 AND 64)"
                ");"
                "CREATE TABLE sessions ("
                "token_hash BLOB PRIMARY KEY,"
                "user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,"
                "expires_at INTEGER NOT NULL,"
                "created_at INTEGER NOT NULL,"
                "CHECK(length(token_hash) = 32)"
                ");"
                "CREATE INDEX sessions_user_id_index ON sessions(user_id);"
                "CREATE INDEX sessions_expires_at_index ON sessions(expires_at);"
                "CREATE TABLE files ("
                "id TEXT PRIMARY KEY,"
                "user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,"
                "original_name TEXT NOT NULL,"
                "storage_key TEXT NOT NULL UNIQUE,"
                "mime_type TEXT NOT NULL,"
                "size INTEGER NOT NULL CHECK(size >= 0),"
                "sha256 BLOB NOT NULL CHECK(length(sha256) = 32),"
                "created_at INTEGER NOT NULL,"
                "updated_at INTEGER NOT NULL,"
                "CHECK(length(original_name) BETWEEN 1 AND 255)"
                ");"
                "CREATE INDEX files_user_created_index "
                "ON files(user_id, created_at DESC);"
                "INSERT INTO schema_migrations(version, applied_at) "
                "VALUES(1, CAST(strftime('%s', 'now') AS INTEGER));");
        }

        execute("COMMIT;");
    } catch (...) {
        try {
            execute("ROLLBACK;");
        } catch (...) {
            // Preserve the original migration error.
        }
        throw;
    }
}

void Database::verify_schema() const {
    Statement statement(
        connection_,
        "SELECT COUNT(*) FROM sqlite_master "
        "WHERE type = 'table' AND name IN "
        "('schema_migrations', 'users', 'sessions', 'files');");
    if (::sqlite3_step(statement.get()) != SQLITE_ROW ||
        ::sqlite3_column_int(statement.get(), 0) != 4) {
        throw std::runtime_error(
            "Database schema is incomplete or inconsistent");
    }
}

}  // namespace personal_cloud
