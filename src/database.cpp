#include "storage/database.h"

#include <sqlite3.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string_view>
#include <stdexcept>
#include <string>

namespace personal_cloud {
namespace {

constexpr int latest_schema_version = 1;

std::runtime_error sqlite_error(sqlite3* connection, const char* operation);

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

    void bind_text(int index, std::string_view value) {
        if (::sqlite3_bind_text(
                statement_,
                index,
                value.data(),
                static_cast<int>(value.size()),
                SQLITE_TRANSIENT) != SQLITE_OK) {
            throw sqlite_error(
                ::sqlite3_db_handle(statement_), "Cannot bind SQLite text");
        }
    }

    void bind_int64(int index, std::int64_t value) {
        if (::sqlite3_bind_int64(statement_, index, value) != SQLITE_OK) {
            throw sqlite_error(
                ::sqlite3_db_handle(statement_), "Cannot bind SQLite integer");
        }
    }

    void bind_blob(int index, const SessionTokenHash& value) {
        if (::sqlite3_bind_blob(
                statement_,
                index,
                value.data(),
                static_cast<int>(value.size()),
                SQLITE_TRANSIENT) != SQLITE_OK) {
            throw sqlite_error(
                ::sqlite3_db_handle(statement_), "Cannot bind SQLite blob");
        }
    }

private:
    sqlite3_stmt* statement_{nullptr};
};

std::runtime_error sqlite_error(sqlite3* connection, const char* operation) {
    return std::runtime_error(
        std::string(operation) + ": " + ::sqlite3_errmsg(connection));
}

StoredFile stored_file_from_row(sqlite3_stmt* statement) {
    const auto text_column = [statement](int index) -> std::string {
        const auto* value = reinterpret_cast<const char*>(
            ::sqlite3_column_text(statement, index));
        if (value == nullptr) {
            throw std::runtime_error("Stored file contains a null text field");
        }
        return value;
    };

    StoredFile file;
    file.id = text_column(0);
    file.user_id = ::sqlite3_column_int64(statement, 1);
    file.original_name = text_column(2);
    file.storage_key = text_column(3);
    file.mime_type = text_column(4);
    file.size = ::sqlite3_column_int64(statement, 5);
    const void* digest = ::sqlite3_column_blob(statement, 6);
    const int digest_size = ::sqlite3_column_bytes(statement, 6);
    if (digest == nullptr || digest_size != static_cast<int>(file.sha256.size())) {
        throw std::runtime_error("Stored file contains an invalid SHA-256");
    }
    std::memcpy(file.sha256.data(), digest, file.sha256.size());
    file.created_at = ::sqlite3_column_int64(statement, 7);
    file.updated_at = ::sqlite3_column_int64(statement, 8);
    return file;
}

}  // namespace

Database::Database(
    const std::filesystem::path& path,
    DatabaseOpenMode mode) {
    const std::string path_text = path.string();
    int open_flags =
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_URI;
    if (mode == DatabaseOpenMode::initialize_schema) {
        open_flags |= SQLITE_OPEN_CREATE;
    }
    const int result = ::sqlite3_open_v2(
        path_text.c_str(),
        &connection_,
        open_flags,
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
        execute("PRAGMA synchronous = NORMAL;");
        if (mode == DatabaseOpenMode::initialize_schema) {
            execute("PRAGMA journal_mode = WAL;");
            apply_migrations();
            verify_schema();
        }
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

std::optional<std::int64_t> Database::create_user(
    std::string_view username,
    std::string_view password_hash,
    std::int64_t created_at) {
    Statement statement(
        connection_,
        "INSERT INTO users(username, password_hash, created_at) "
        "VALUES(?1, ?2, ?3);");
    statement.bind_text(1, username);
    statement.bind_text(2, password_hash);
    statement.bind_int64(3, created_at);

    const int result = ::sqlite3_step(statement.get());
    if (result == SQLITE_DONE) {
        return ::sqlite3_last_insert_rowid(connection_);
    }

    const int extended_error = ::sqlite3_extended_errcode(connection_);
    if (extended_error == SQLITE_CONSTRAINT_UNIQUE ||
        extended_error == SQLITE_CONSTRAINT_PRIMARYKEY) {
        return std::nullopt;
    }
    throw sqlite_error(connection_, "Cannot create user");
}

std::optional<StoredUser> Database::find_user_by_username(
    std::string_view username) const {
    Statement statement(
        connection_,
        "SELECT id, username, password_hash, created_at "
        "FROM users WHERE username = ?1;");
    statement.bind_text(1, username);

    const int result = ::sqlite3_step(statement.get());
    if (result == SQLITE_DONE) {
        return std::nullopt;
    }
    if (result != SQLITE_ROW) {
        throw sqlite_error(connection_, "Cannot find user");
    }

    const auto* username_text = reinterpret_cast<const char*>(
        ::sqlite3_column_text(statement.get(), 1));
    const auto* password_hash_text = reinterpret_cast<const char*>(
        ::sqlite3_column_text(statement.get(), 2));
    if (username_text == nullptr || password_hash_text == nullptr) {
        throw std::runtime_error("Stored user contains invalid null fields");
    }

    return StoredUser{
        ::sqlite3_column_int64(statement.get(), 0),
        username_text,
        password_hash_text,
        ::sqlite3_column_int64(statement.get(), 3),
    };
}

bool Database::create_session(
    const SessionTokenHash& token_hash,
    std::int64_t user_id,
    std::int64_t expires_at,
    std::int64_t created_at) {
    Statement statement(
        connection_,
        "INSERT INTO sessions(token_hash, user_id, expires_at, created_at) "
        "VALUES(?1, ?2, ?3, ?4);");
    statement.bind_blob(1, token_hash);
    statement.bind_int64(2, user_id);
    statement.bind_int64(3, expires_at);
    statement.bind_int64(4, created_at);

    const int result = ::sqlite3_step(statement.get());
    if (result == SQLITE_DONE) {
        return true;
    }
    const int extended_error = ::sqlite3_extended_errcode(connection_);
    if (extended_error == SQLITE_CONSTRAINT_UNIQUE ||
        extended_error == SQLITE_CONSTRAINT_PRIMARYKEY) {
        return false;
    }
    throw sqlite_error(connection_, "Cannot create session");
}

std::optional<SessionUser> Database::find_session_user(
    const SessionTokenHash& token_hash,
    std::int64_t now) const {
    Statement statement(
        connection_,
        "SELECT users.id, users.username, users.created_at "
        "FROM sessions JOIN users ON users.id = sessions.user_id "
        "WHERE sessions.token_hash = ?1 AND sessions.expires_at > ?2;");
    statement.bind_blob(1, token_hash);
    statement.bind_int64(2, now);

    const int result = ::sqlite3_step(statement.get());
    if (result == SQLITE_DONE) {
        return std::nullopt;
    }
    if (result != SQLITE_ROW) {
        throw sqlite_error(connection_, "Cannot find session");
    }

    const auto* username_text = reinterpret_cast<const char*>(
        ::sqlite3_column_text(statement.get(), 1));
    if (username_text == nullptr) {
        throw std::runtime_error("Stored session user has a null username");
    }
    return SessionUser{
        ::sqlite3_column_int64(statement.get(), 0),
        username_text,
        ::sqlite3_column_int64(statement.get(), 2),
    };
}

bool Database::delete_session(const SessionTokenHash& token_hash) {
    Statement statement(
        connection_, "DELETE FROM sessions WHERE token_hash = ?1;");
    statement.bind_blob(1, token_hash);
    if (::sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw sqlite_error(connection_, "Cannot delete session");
    }
    return ::sqlite3_changes(connection_) != 0;
}

void Database::delete_expired_sessions(std::int64_t now) {
    Statement statement(
        connection_, "DELETE FROM sessions WHERE expires_at <= ?1;");
    statement.bind_int64(1, now);
    if (::sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw sqlite_error(connection_, "Cannot delete expired sessions");
    }
}

std::int64_t Database::total_file_size(std::int64_t user_id) const {
    Statement statement(
        connection_,
        "SELECT COALESCE(SUM(size), 0) FROM files WHERE user_id = ?1;");
    statement.bind_int64(1, user_id);
    if (::sqlite3_step(statement.get()) != SQLITE_ROW) {
        throw sqlite_error(connection_, "Cannot calculate file usage");
    }
    return ::sqlite3_column_int64(statement.get(), 0);
}

bool Database::create_file(const StoredFile& file) {
    execute("BEGIN IMMEDIATE;");
    try {
        Statement statement(
            connection_,
            "INSERT INTO files(id, user_id, original_name, storage_key, "
            "mime_type, size, sha256, created_at, updated_at) "
            "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9);");
        statement.bind_text(1, file.id);
        statement.bind_int64(2, file.user_id);
        statement.bind_text(3, file.original_name);
        statement.bind_text(4, file.storage_key);
        statement.bind_text(5, file.mime_type);
        statement.bind_int64(6, file.size);
        statement.bind_blob(7, file.sha256);
        statement.bind_int64(8, file.created_at);
        statement.bind_int64(9, file.updated_at);

        const int result = ::sqlite3_step(statement.get());
        if (result != SQLITE_DONE) {
            const int extended_error = ::sqlite3_extended_errcode(connection_);
            if (extended_error == SQLITE_CONSTRAINT_UNIQUE ||
                extended_error == SQLITE_CONSTRAINT_PRIMARYKEY) {
                execute("ROLLBACK;");
                return false;
            }
            throw sqlite_error(connection_, "Cannot create file metadata");
        }
        execute("COMMIT;");
        return true;
    } catch (...) {
        try {
            execute("ROLLBACK;");
        } catch (...) {
        }
        throw;
    }
}

std::vector<StoredFile> Database::list_files(
    std::int64_t user_id,
    std::size_t limit) const {
    const std::size_t bounded_limit = std::min<std::size_t>(limit, 1000);
    Statement statement(
        connection_,
        "SELECT id, user_id, original_name, storage_key, mime_type, size, "
        "sha256, created_at, updated_at FROM files WHERE user_id = ?1 "
        "ORDER BY created_at DESC, id DESC LIMIT ?2;");
    statement.bind_int64(1, user_id);
    statement.bind_int64(2, static_cast<std::int64_t>(bounded_limit));

    std::vector<StoredFile> files;
    files.reserve(bounded_limit);
    while (true) {
        const int result = ::sqlite3_step(statement.get());
        if (result == SQLITE_DONE) {
            return files;
        }
        if (result != SQLITE_ROW) {
            throw sqlite_error(connection_, "Cannot list files");
        }
        files.push_back(stored_file_from_row(statement.get()));
    }
}

std::optional<StoredFile> Database::find_file(
    std::string_view id,
    std::int64_t user_id) const {
    Statement statement(
        connection_,
        "SELECT id, user_id, original_name, storage_key, mime_type, size, "
        "sha256, created_at, updated_at FROM files "
        "WHERE id = ?1 AND user_id = ?2;");
    statement.bind_text(1, id);
    statement.bind_int64(2, user_id);
    const int result = ::sqlite3_step(statement.get());
    if (result == SQLITE_DONE) {
        return std::nullopt;
    }
    if (result != SQLITE_ROW) {
        throw sqlite_error(connection_, "Cannot find file");
    }
    return stored_file_from_row(statement.get());
}

bool Database::rename_file(
    std::string_view id,
    std::int64_t user_id,
    std::string_view original_name,
    std::int64_t updated_at) {
    Statement statement(
        connection_,
        "UPDATE files SET original_name = ?1, updated_at = ?2 "
        "WHERE id = ?3 AND user_id = ?4;");
    statement.bind_text(1, original_name);
    statement.bind_int64(2, updated_at);
    statement.bind_text(3, id);
    statement.bind_int64(4, user_id);
    if (::sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw sqlite_error(connection_, "Cannot rename file");
    }
    return ::sqlite3_changes(connection_) != 0;
}

bool Database::delete_file(std::string_view id, std::int64_t user_id) {
    Statement statement(
        connection_, "DELETE FROM files WHERE id = ?1 AND user_id = ?2;");
    statement.bind_text(1, id);
    statement.bind_int64(2, user_id);
    if (::sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw sqlite_error(connection_, "Cannot delete file metadata");
    }
    return ::sqlite3_changes(connection_) != 0;
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
