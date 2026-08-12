#include "app/backend_application.h"

#include "http/router.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace personal_cloud {
namespace {

std::filesystem::path prepare_database_path(
    const std::filesystem::path& database_path) {
    if (database_path == ":memory:") {
        static std::atomic<std::uint64_t> memory_database_id{0};
        return "file:personal_cloud_memory_" +
               std::to_string(memory_database_id.fetch_add(1)) +
               "?mode=memory&cache=shared";
    }

    {
        const std::filesystem::path parent = database_path.parent_path();
        if (!parent.empty()) {
            std::error_code error;
            std::filesystem::create_directories(parent, error);
            if (error) {
                throw std::runtime_error(
                    "Cannot create database directory: " + error.message());
            }
        }
    }
    return database_path;
}

bool is_authentication_path(std::string_view path) {
    return path == "/api/auth/register" || path == "/api/auth/login" ||
           path == "/api/auth/logout" || path == "/api/auth/me";
}

std::filesystem::path prepare_storage_root(
    const std::filesystem::path& storage_root) {
    std::error_code error;
    std::filesystem::create_directories(storage_root, error);
    if (error) {
        throw std::runtime_error(
            "Cannot create storage directory: " + error.message());
    }
    return storage_root;
}

std::string_view path_without_query(std::string_view target) {
    const std::size_t query_start = target.find('?');
    return target.substr(0, query_start);
}

HttpResponse json_error(
    int status_code,
    std::string reason,
    std::string code,
    std::string message,
    std::vector<std::pair<std::string, std::string>> headers = {}) {
    return {
        status_code,
        std::move(reason),
        "application/json; charset=utf-8",
        "{\"error\":{\"code\":\"" + code +
            "\",\"message\":\"" + message + "\"}}\n",
        std::move(headers),
    };
}

HttpResponse status_response(
    const HttpRequest& request,
    bool database_ready,
    int schema_version) {
    if (request.method != "GET") {
        return json_error(
            405,
            "Method Not Allowed",
            "method_not_allowed",
            "Method not allowed",
            {{"Allow", "GET"}});
    }

    if (!database_ready) {
        return json_error(
            503,
            "Service Unavailable",
            "database_unavailable",
            "Database is unavailable");
    }

    return {
        200,
        "OK",
        "application/json; charset=utf-8",
        "{\"status\":\"ok\",\"database\":\"ok\",\"schema_version\":" +
            std::to_string(schema_version) + "}\n",
    };
}

}  // namespace

BackendApplication::BackendApplication(const BackendConfig& config)
    : database_path_(prepare_database_path(config.database_path)),
      database_(database_path_),
      storage_root_(prepare_storage_root(config.storage_root)),
      auth_service_(std::make_shared<AuthService>(database_path_)),
      schema_version_(database_.schema_version()),
      database_ready_(database_.health_check()) {}

HttpResponse BackendApplication::handle_request(const HttpRequest& request) {
    const std::string_view path = path_without_query(request.path);
    if (path != "/api/status") {
        return route_request(request);
    }

    return status_response(request, database_ready_, schema_version_);
}

std::optional<ApplicationTask> BackendApplication::make_task(
    const HttpRequest& request) {
    const std::string_view path = path_without_query(request.path);
    if (is_authentication_path(path)) {
        const std::shared_ptr<AuthService> service = auth_service_;
        return [service, request] {
            return service->handle_request(request);
        };
    }

    if (path == "/" || path == "/turntable") {
        if (request.method != "GET") {
            return std::nullopt;
        }

        // 示例页面仍通过文件系统读取，必须和后续文件 API 一样离开 epoll 线程。
        return [request] {
            return route_request(request);
        };
    }

    if (path != "/api/status") {
        return std::nullopt;
    }

    // 只捕获不可变快照，既不引用 epoll 解析缓冲区，也不让 worker 访问启动线程
    // 拥有的 SQLite 连接。后续 DB 任务必须在 worker 内创建独占连接。
    const bool database_ready = database_ready_;
    const int schema_version = schema_version_;
    return [request, database_ready, schema_version] {
        return status_response(request, database_ready, schema_version);
    };
}

int BackendApplication::schema_version() const {
    return schema_version_;
}

const std::filesystem::path& BackendApplication::storage_root() const noexcept {
    return storage_root_;
}

}  // namespace personal_cloud
