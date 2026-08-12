#include "app/backend_application.h"
#include "app/backend_config.h"
#include "storage/database.h"

#include <sqlite3.h>

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
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

    [[nodiscard]] std::string scalar_text(const char* sql) {
        sqlite3_stmt* statement = nullptr;
        if (::sqlite3_prepare_v2(
                connection_, sql, -1, &statement, nullptr) != SQLITE_OK) {
            throw std::runtime_error("Cannot prepare test text query");
        }

        const int step_result = ::sqlite3_step(statement);
        std::string value;
        if (step_result == SQLITE_ROW) {
            const auto* text = reinterpret_cast<const char*>(
                ::sqlite3_column_text(statement, 0));
            if (text != nullptr) {
                value = text;
            }
        }
        ::sqlite3_finalize(statement);
        if (step_result != SQLITE_ROW) {
            throw std::runtime_error("Test text query returned no row");
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

personal_cloud::HttpResponse run_application_task(
    personal_cloud::BackendApplication& application,
    personal_cloud::HttpRequest request) {
    std::optional<personal_cloud::ApplicationTask> task =
        application.make_task(request);
    if (!task.has_value()) {
        throw std::runtime_error("Expected an asynchronous application task");
    }
    return (*task)();
}

personal_cloud::HttpRequest json_request(
    std::string path,
    std::string body) {
    return {
        "POST",
        std::move(path),
        "HTTP/1.1",
        {{"content-type", "application/json"}},
        std::move(body),
    };
}

std::optional<std::string> response_header(
    const personal_cloud::HttpResponse& response,
    std::string_view name) {
    for (const auto& [header_name, value] : response.headers) {
        if (header_name == name) {
            return value;
        }
    }
    return std::nullopt;
}

std::string cookie_pair(const personal_cloud::HttpResponse& response) {
    const std::optional<std::string> set_cookie =
        response_header(response, "Set-Cookie");
    if (!set_cookie.has_value()) {
        return {};
    }
    return set_cookie->substr(0, set_cookie->find(';'));
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
        "--worker-count",
        "7",
        "--task-queue-size",
        "99",
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
        expect(result.config->server.application_worker_count == 7,
               "parse the configured application worker count");
        expect(result.config->server.application_queue_size == 99,
               "parse the configured application task queue size");
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
    expect(!parse({"--worker-count", "0"}).config.has_value(),
           "reject a zero application worker count");
    expect(!parse({"--task-queue-size", "-1"}).config.has_value(),
           "reject a negative application task queue size");
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

        std::optional<personal_cloud::ApplicationTask> status_task =
            application.make_task(
                {"GET", "/api/status", "HTTP/1.1", {}, {}});
        expect(status_task.has_value() &&
                   (*status_task)().status_code == 200,
               "create an asynchronous task for a backend status request");
        expect(!application.make_task(
                    {"GET", "/health", "HTTP/1.1", {}, {}})
                    .has_value(),
               "keep fast built-in routes on the epoll thread");
        expect(application.make_task(
                   {"GET", "/", "HTTP/1.1", {}, {}})
                   .has_value(),
               "move static page file reads to an application worker");

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

void test_authentication_lifecycle_and_security() {
    TemporaryDirectory temporary;
    const std::filesystem::path database_path = temporary.path() / "auth.db";

    personal_cloud::BackendConfig config;
    config.database_path = database_path;
    config.storage_root = temporary.path() / "files";

    std::string persisted_cookie;
    std::string first_session_cookie;
    {
        personal_cloud::BackendApplication application(config);

        personal_cloud::HttpResponse response = run_application_task(
            application,
            {"POST",
             "/api/auth/register",
             "HTTP/1.1",
             {},
             "{\"username\":\"Alice\",\"password\":\"secret-pass\"}"});
        expect(response.status_code == 415,
               "require JSON content type for registration");

        response = run_application_task(
            application,
            json_request("/api/auth/register", "not-json"));
        expect(response.status_code == 400,
               "reject malformed authentication JSON");

        response = run_application_task(
            application,
            json_request(
                "/api/auth/register",
                "{\"username\":\"bad space\",\"password\":\"short\"}"));
        expect(response.status_code == 422,
               "validate username and password format");

        response = run_application_task(
            application,
            json_request(
                "/api/auth/register",
                "{\"username\":\"Alice\","
                "\"password\":\"correct-horse-battery\"}"));
        expect(response.status_code == 201 &&
                   response.body.find("\"username\":\"Alice\"") !=
                       std::string::npos,
               "register a user through the asynchronous auth task");

        response = run_application_task(
            application,
            json_request(
                "/api/auth/register",
                "{\"username\":\"alice\","
                "\"password\":\"another-password\"}"));
        expect(response.status_code == 409,
               "enforce case-insensitive username uniqueness");

        const personal_cloud::HttpResponse missing_user =
            run_application_task(
                application,
                json_request(
                    "/api/auth/login",
                    "{\"username\":\"Nobody\","
                    "\"password\":\"wrong-password\"}"));
        const personal_cloud::HttpResponse wrong_password =
            run_application_task(
                application,
                json_request(
                    "/api/auth/login",
                    "{\"username\":\"Alice\","
                    "\"password\":\"wrong-password\"}"));
        expect(missing_user.status_code == 401 &&
                   wrong_password.status_code == 401 &&
                   missing_user.body == wrong_password.body,
               "return the same login error for missing users and bad passwords");

        response = run_application_task(
            application,
            json_request(
                "/api/auth/login",
                "{\"username\":\"aLiCe\","
                "\"password\":\"correct-horse-battery\"}"));
        const std::optional<std::string> set_cookie =
            response_header(response, "Set-Cookie");
        persisted_cookie = cookie_pair(response);
        first_session_cookie = persisted_cookie;
        expect(response.status_code == 200 && !persisted_cookie.empty(),
               "log in with case-insensitive username lookup");
        expect(set_cookie.has_value() &&
                   set_cookie->find("HttpOnly") != std::string::npos &&
                   set_cookie->find("SameSite=Strict") != std::string::npos &&
                   set_cookie->find("Path=/") != std::string::npos,
               "set hardened session cookie attributes");
        expect(response.body.find("pc_session") == std::string::npos &&
                   response.body.find(persisted_cookie) == std::string::npos,
               "keep the plaintext session token out of the response body");

        response = run_application_task(
            application,
            {"GET",
             "/api/auth/me",
             "HTTP/1.1",
             {{"cookie", persisted_cookie}},
             {}});
        expect(response.status_code == 200 &&
                   response.body.find("\"username\":\"Alice\"") !=
                       std::string::npos,
               "resolve the current user from a valid session cookie");

        response = run_application_task(
            application,
            {"POST",
             "/api/auth/logout",
             "HTTP/1.1",
             {{"cookie", persisted_cookie}},
             {}});
        const std::optional<std::string> cleared_cookie =
            response_header(response, "Set-Cookie");
        expect(response.status_code == 200 && cleared_cookie.has_value() &&
                   cleared_cookie->find("Max-Age=0") != std::string::npos,
               "logout deletes the session and clears the cookie");

        response = run_application_task(
            application,
            {"GET",
             "/api/auth/me",
             "HTTP/1.1",
             {{"cookie", persisted_cookie}},
             {}});
        expect(response.status_code == 401,
               "reject a session after logout");

        {
            RawDatabase raw(database_path);
            expect(raw.scalar_int("SELECT COUNT(*) FROM sessions;") == 0,
                   "physically remove a logged-out session");
        }

        response = run_application_task(
            application,
            json_request(
                "/api/auth/login",
                "{\"username\":\"Alice\","
                "\"password\":\"correct-horse-battery\"}"));
        persisted_cookie = cookie_pair(response);
        expect(response.status_code == 200 && !persisted_cookie.empty() &&
                   persisted_cookie != first_session_cookie,
               "issue a fresh random session that survives application restart");

        response = run_application_task(
            application,
            {"POST", "/api/auth/me", "HTTP/1.1", {}, {}});
        expect(response.status_code == 405 &&
                   response_header(response, "Allow") ==
                       std::optional<std::string>{"GET"},
               "return Allow for an unsupported auth endpoint method");
    }

    {
        RawDatabase raw(database_path);
        expect(raw.scalar_text(
                   "SELECT password_hash FROM users WHERE username='Alice';")
                   .starts_with("$argon2id$"),
               "store passwords as libsodium Argon2id hashes");
        expect(raw.scalar_int(
                   "SELECT instr(password_hash, 'correct-horse-battery') "
                   "FROM users WHERE username='Alice';") == 0,
               "never store the plaintext password");
        expect(raw.scalar_int("SELECT COUNT(*) FROM sessions;") == 1,
               "persist only the newly issued active session");
    }

    {
        personal_cloud::BackendApplication reopened(config);
        personal_cloud::HttpResponse response = run_application_task(
            reopened,
            {"GET",
             "/api/auth/me",
             "HTTP/1.1",
             {{"cookie", persisted_cookie}},
             {}});
        expect(response.status_code == 200,
               "preserve a valid session across application restart");

        response = run_application_task(
            reopened,
            json_request(
                "/api/auth/login",
                "{\"username\":\"Alice\","
                "\"password\":\"correct-horse-battery\"}"));
        persisted_cookie = cookie_pair(response);
        expect(response.status_code == 200 && !persisted_cookie.empty(),
               "log in after restarting the application");

        {
            RawDatabase raw(database_path);
            expect(raw.scalar_text(
                       "SELECT typeof(token_hash) || ':' || "
                       "length(token_hash) FROM sessions LIMIT 1;") ==
                       "blob:32",
                   "store only a fixed-size binary session token hash");
            raw.execute("UPDATE sessions SET expires_at=0;");
        }

        response = run_application_task(
            reopened,
            {"GET",
             "/api/auth/me",
             "HTTP/1.1",
             {{"cookie", persisted_cookie}},
             {}});
        expect(response.status_code == 401,
               "reject an expired session");

        for (int attempt = 0; attempt < 4; ++attempt) {
            response = run_application_task(
                reopened,
                json_request(
                    "/api/auth/login",
                    "{\"username\":\"RateLimited\","
                    "\"password\":\"wrong-password\"}"));
        }
        personal_cloud::HttpRequest limited_request = json_request(
            "/api/auth/login",
            "{\"username\":\"ratelimited\","
            "\"password\":\"wrong-password\"}");
        auto first_attempt = std::async(
            std::launch::async,
            [&reopened, limited_request] {
                return run_application_task(reopened, limited_request);
            });
        auto second_attempt = std::async(
            std::launch::async,
            [&reopened, limited_request] {
                return run_application_task(reopened, limited_request);
            });
        const personal_cloud::HttpResponse first_response =
            first_attempt.get();
        const personal_cloud::HttpResponse second_response =
            second_attempt.get();
        const bool one_rate_limited =
            (first_response.status_code == 401 &&
             second_response.status_code == 429) ||
            (first_response.status_code == 429 &&
             second_response.status_code == 401);
        const personal_cloud::HttpResponse& limited_response =
            first_response.status_code == 429
                ? first_response
                : second_response;
        expect(one_rate_limited &&
                   response_header(limited_response, "Retry-After")
                       .has_value(),
               "count concurrent login attempts in the failure limit");
    }
}

void test_authentication_with_shared_memory_database() {
    TemporaryDirectory temporary;
    personal_cloud::BackendConfig config;
    config.database_path = ":memory:";
    config.storage_root = temporary.path() / "files";

    personal_cloud::BackendApplication application(config);
    personal_cloud::HttpResponse response = run_application_task(
        application,
        json_request(
            "/api/auth/register",
            "{\"username\":\"MemoryUser\","
            "\"password\":\"memory-password\"}"));
    expect(response.status_code == 201,
           "share an in-memory database with worker connections");
}

}  // namespace

int main() {
    try {
        test_configuration();
        test_database_migrations_and_persistence();
        test_rejects_newer_schema();
        test_rejects_incomplete_schema();
        test_authentication_lifecycle_and_security();
        test_authentication_with_shared_memory_database();
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
