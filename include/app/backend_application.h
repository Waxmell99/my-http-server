#pragma once

#include "app/backend_config.h"
#include "http/http_request.h"
#include "http/http_response.h"
#include "storage/database.h"

#include <filesystem>

namespace personal_cloud {

class BackendApplication final {
public:
    explicit BackendApplication(const BackendConfig& config);

    BackendApplication(const BackendApplication&) = delete;
    BackendApplication& operator=(const BackendApplication&) = delete;

    HttpResponse handle_request(const HttpRequest& request);

    [[nodiscard]] int schema_version() const;
    [[nodiscard]] const std::filesystem::path& storage_root() const noexcept;

private:
    Database database_;
    std::filesystem::path storage_root_;
    int schema_version_{0};
    bool database_ready_{false};
};

}  // namespace personal_cloud
