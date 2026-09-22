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

bool is_file_path(std::string_view path) {
    return path == "/api/files" || path.starts_with("/api/files/");
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
    const std::filesystem::path& database_path) {
    if (request.method != "GET") {
        return json_error(
            405,
            "Method Not Allowed",
            "method_not_allowed",
            "Method not allowed",
            {{"Allow", "GET"}});
    }

    try {
        Database database(
            database_path, DatabaseOpenMode::existing_schema);
        if (database.health_check()) {
            return {
                200,
                "OK",
                "application/json; charset=utf-8",
                "{\"status\":\"ok\",\"database\":\"ok\","
                "\"schema_version\":" +
                    std::to_string(database.schema_version()) + "}\n",
            };
        }
    } catch (const std::exception&) {
    }

    return json_error(
        503,
        "Service Unavailable",
        "database_unavailable",
        "Database is unavailable");
}

}  // namespace

BackendApplication::BackendApplication(const BackendConfig& config)
    : database_path_(prepare_database_path(config.database_path)),
      database_(database_path_),
      storage_root_(prepare_storage_root(config.storage_root)),
      auth_service_(std::make_shared<AuthService>(
          database_path_, config.allow_registration)),
      file_service_(std::make_shared<FileService>(
          config, auth_service_, database_path_, storage_root_)),
      schema_version_(database_.schema_version()) {}

HttpResponse BackendApplication::handle_request(const HttpRequest& request) {
    const std::string_view path = path_without_query(request.path);
    if (path != "/api/status") {
        return route_request(request);
    }

    return status_response(request, database_path_);
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

    if (is_file_path(path)) {
        const std::shared_ptr<FileService> service = file_service_;
        return [service, request] {
            return service->handle_request(request);
        };
    }

    if (path == "/" || path == "/turntable" || path == "/app") {
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

    // worker 使用独立连接执行实时检查，不访问启动线程拥有的 SQLite 连接。
    const std::filesystem::path database_path = database_path_;
    return [request, database_path] {
        return status_response(request, database_path);
    };
}

std::optional<UploadPreparationTask> BackendApplication::make_upload_task(
    const HttpRequest& request,
    std::uint64_t content_length) {
    return file_service_->make_upload_task(request, content_length);
}

std::optional<DownloadPreparationTask> BackendApplication::make_download_task(
    const HttpRequest& request) {
    return file_service_->make_download_task(request);
}

int BackendApplication::schema_version() const {
    return schema_version_;
}

const std::filesystem::path& BackendApplication::storage_root() const noexcept {
    return storage_root_;
}

}  // namespace personal_cloud
