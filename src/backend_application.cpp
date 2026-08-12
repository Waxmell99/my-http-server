#include "app/backend_application.h"

#include "http/router.h"

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
    if (database_path != ":memory:") {
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

}  // namespace

BackendApplication::BackendApplication(const BackendConfig& config)
    : database_(prepare_database_path(config.database_path)),
      storage_root_(prepare_storage_root(config.storage_root)),
      schema_version_(database_.schema_version()),
      database_ready_(database_.health_check()) {}

HttpResponse BackendApplication::handle_request(const HttpRequest& request) {
    const std::string_view path = path_without_query(request.path);
    if (path != "/api/status") {
        return route_request(request);
    }

    if (request.method != "GET") {
        return json_error(
            405,
            "Method Not Allowed",
            "method_not_allowed",
            "Method not allowed",
            {{"Allow", "GET"}});
    }

    if (!database_ready_) {
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
            std::to_string(schema_version_) + "}\n",
    };
}

int BackendApplication::schema_version() const {
    return schema_version_;
}

const std::filesystem::path& BackendApplication::storage_root() const noexcept {
    return storage_root_;
}

}  // namespace personal_cloud
