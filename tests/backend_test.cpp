#include "app/backend_application.h"
#include "app/backend_config.h"
#include "storage/database.h"

#include <sqlite3.h>

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

int failure_count = 0;

void expect(bool condition, std::string_view description) {
    if (condition) {
        std::cout << "[PASS] " << description << '\n';
        return;
    }

    std::cerr << "[FAIL] " << description << '\n';
    ++failure_count;
}

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        std::string pattern =
            (std::filesystem::temp_directory_path() /
             "personal-cloud-backend-test-XXXXXX")
                .string();
        pattern.push_back('\0');
        char* created = ::mkdtemp(pattern.data());
        if (created == nullptr) {
            throw std::runtime_error(
                "mkdtemp failed with errno " + std::to_string(errno));
        }
        path_ = created;
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

class RawDatabase final {
public:
    explicit RawDatabase(const std::filesystem::path& path) {
        if (::sqlite3_open(path.c_str(), &connection_) != SQLITE_OK) {
            const std::string message = connection_ != nullptr
                                            ? ::sqlite3_errmsg(connection_)
                                            : "unknown SQLite error";
            if (connection_ != nullptr) {
                ::sqlite3_close(connection_);
                connection_ = nullptr;
            }
            throw std::runtime_error("Cannot open test database: " + message);
        }
    }

    ~RawDatabase() {
        if (connection_ != nullptr) {
            ::sqlite3_close(connection_);
        }
    }

    RawDatabase(const RawDatabase&) = delete;
    RawDatabase& operator=(const RawDatabase&) = delete;

    void execute(const char* sql) {
        char* error_message = nullptr;
        const int result = ::sqlite3_exec(
            connection_, sql, nullptr, nullptr, &error_message);
        if (result == SQLITE_OK) {
            return;
        }
        const std::string message = error_message != nullptr
                                        ? error_message
                                        : ::sqlite3_errmsg(connection_);
        ::sqlite3_free(error_message);
        throw std::runtime_error("Test SQL failed: " + message);
    }

    [[nodiscard]] int scalar_int(const char* sql) {
        sqlite3_stmt* statement = nullptr;
        if (::sqlite3_prepare_v2(
                connection_, sql, -1, &statement, nullptr) != SQLITE_OK) {
            throw std::runtime_error("Cannot prepare test query");
        }

        const int step_result = ::sqlite3_step(statement);
        const int value = step_result == SQLITE_ROW
                              ? ::sqlite3_column_int(statement, 0)
                              : -1;
        ::sqlite3_finalize(statement);
        if (step_result != SQLITE_ROW) {
            throw std::runtime_error("Test query returned no row");
        }
        return value;
    }

private:
    sqlite3* connection_{nullptr};
};

personal_cloud::ConfigParseResult parse(
    std::initializer_list<std::string_view> arguments) {
    const std::vector<std::string_view> values(arguments);
    return personal_cloud::parse_backend_config(values);
}

void test_configuration() {
    personal_cloud::ConfigParseResult result = parse({});
    expect(result.config.has_value(), "accept default backend configuration");
    if (result.config.has_value()) {
        expect(result.config->server.port == 9000,
               "use the documented default port");
        expect(result.config->database_path == "data/personal_cloud.db",
               "use the documented default database path");
        expect(result.config->storage_root == "data/files",
               "use the documented default storage directory");
    }

    result = parse({
        "--port",
        "18080",
        "--database",
        "/tmp/cloud.db",
        "--storage-root",
        "/tmp/cloud-files",
        "--max-connections",
        "321",
        "--idle-timeout",
        "17",
        "--verbose",
    });
    expect(result.config.has_value(), "parse all supported backend options");
    if (result.config.has_value()) {
        expect(result.config->server.port == 18080,
               "parse the configured port");
        expect(result.config->server.maximum_connections == 321,
               "parse the configured connection limit");
        expect(result.config->server.idle_timeout.count() == 17,
               "parse the configured idle timeout");
        expect(result.config->server.verbose_logging,
               "parse verbose logging flag");
    }

    expect(!parse({"--port", "0"}).config.has_value(),
           "reject port zero");
    expect(!parse({"--port", "65536"}).config.has_value(),
           "reject a port above uint16 range");
    expect(!parse({"--idle-timeout", "-1"}).config.has_value(),
           "reject a negative idle timeout");
    expect(!parse({"--database"}).config.has_value(),
           "reject a missing option value");
    expect(!parse({"--unknown"}).config.has_value(),
           "reject an unknown option");
    expect(parse({"--help"}).show_help, "recognize the help option");
}

void test_database_migrations_and_persistence() {
    TemporaryDirectory temporary;
    const std::filesystem::path database_path = temporary.path() / "db/cloud.db";
    const std::filesystem::path storage_root = temporary.path() / "files";

    personal_cloud::BackendConfig config;
    config.database_path = database_path;
    config.storage_root = storage_root;

    {
        personal_cloud::BackendApplication application(config);
        expect(application.schema_version() == 1,
               "apply the first database migration");
        expect(std::filesystem::is_directory(storage_root),
               "create the configured storage directory");

        const personal_cloud::HttpResponse status =
            application.handle_request(
                {"GET", "/api/status", "HTTP/1.1", {}, {}});
        expect(status.status_code == 200,
               "return 200 from the backend status endpoint");
        expect(status.content_type == "application/json; charset=utf-8",
               "return JSON from the backend status endpoint");
        expect(status.body ==
                   "{\"status\":\"ok\",\"database\":\"ok\","
                   "\"schema_version\":1}\n",
               "report the database schema version as JSON");

        const personal_cloud::HttpResponse wrong_method =
            application.handle_request(
                {"POST", "/api/status", "HTTP/1.1", {}, {}});
        expect(wrong_method.status_code == 405,
               "reject an unsupported status method");
        expect(!wrong_method.headers.empty() &&
                   wrong_method.headers.front() ==
                       std::pair<std::string, std::string>{"Allow", "GET"},
               "include Allow in a JSON 405 response");

        const personal_cloud::HttpResponse existing_route =
            application.handle_request(
                {"GET", "/health", "HTTP/1.1", {}, {}});
        expect(existing_route.status_code == 200 &&
                   existing_route.body == "OK\n",
               "preserve existing routes behind the application handler");
    }

    {
        RawDatabase raw(database_path);
        expect(raw.scalar_int(
                   "SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
                   "AND name IN ('users','sessions','files');") == 3,
               "create all core backend tables");
        raw.execute(
            "INSERT INTO users(username, password_hash, created_at) "
            "VALUES('persisted-user', 'test-only-hash', 1);");
    }

    {
        personal_cloud::BackendApplication reopened(config);
        expect(reopened.schema_version() == 1,
               "reopen an already migrated database idempotently");
    }

    {
        RawDatabase raw(database_path);
        expect(raw.scalar_int(
                   "SELECT COUNT(*) FROM users "
                   "WHERE username='persisted-user';") == 1,
               "preserve existing data across application restarts");
        expect(raw.scalar_int(
                   "SELECT COUNT(*) FROM schema_migrations "
                   "WHERE version=1;") == 1,
               "record each migration exactly once");
    }
}

void test_rejects_newer_schema() {
    TemporaryDirectory temporary;
    const std::filesystem::path database_path = temporary.path() / "future.db";
    {
        RawDatabase raw(database_path);
        raw.execute(
            "CREATE TABLE schema_migrations ("
            "version INTEGER PRIMARY KEY, applied_at INTEGER NOT NULL);"
            "INSERT INTO schema_migrations VALUES(99, 1);");
    }

    bool rejected = false;
    try {
        personal_cloud::Database database(database_path);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    expect(rejected, "reject a database created by a newer server version");
}

void test_rejects_incomplete_schema() {
    TemporaryDirectory temporary;
    const std::filesystem::path database_path =
        temporary.path() / "incomplete.db";
    {
        RawDatabase raw(database_path);
        raw.execute(
            "CREATE TABLE schema_migrations ("
            "version INTEGER PRIMARY KEY, applied_at INTEGER NOT NULL);"
            "INSERT INTO schema_migrations VALUES(1, 1);");
    }

    bool rejected = false;
    try {
        personal_cloud::Database database(database_path);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    expect(rejected, "reject an incomplete database at a recorded version");
}

}  // namespace

int main() {
    try {
        test_configuration();
        test_database_migrations_and_persistence();
        test_rejects_newer_schema();
        test_rejects_incomplete_schema();
    } catch (const std::exception& error) {
        std::cerr << "Unexpected backend test exception: "
                  << error.what() << '\n';
        return 1;
    }

    if (failure_count != 0) {
        std::cerr << failure_count << " backend assertion(s) failed.\n";
        return 1;
    }

    std::cout << "All backend tests passed.\n";
    return 0;
}
