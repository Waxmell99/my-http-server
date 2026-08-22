#pragma once

#include "app/auth_service.h"
#include "app/backend_config.h"
#include "app/file_service.h"
#include "http/http_request.h"
#include "http/http_response.h"
#include "storage/database.h"
#include "server/epoll_server.h"

#include <filesystem>
#include <memory>
#include <optional>

namespace personal_cloud {

class BackendApplication final {
public:
    explicit BackendApplication(const BackendConfig& config);

    BackendApplication(const BackendApplication&) = delete;
    BackendApplication& operator=(const BackendApplication&) = delete;

    HttpResponse handle_request(const HttpRequest& request);
    [[nodiscard]] std::optional<ApplicationTask> make_task(
        const HttpRequest& request);
    [[nodiscard]] std::optional<UploadPreparationTask> make_upload_task(
        const HttpRequest& request,
        std::uint64_t content_length);
    [[nodiscard]] std::optional<DownloadPreparationTask> make_download_task(
        const HttpRequest& request);

    [[nodiscard]] int schema_version() const;
    [[nodiscard]] const std::filesystem::path& storage_root() const noexcept;

private:
    std::filesystem::path database_path_;
    // 此连接只在应用启动线程中初始化/校验 Schema；worker 不得访问它。后续
    // 业务任务应在执行任务的 worker 内创建并独占自己的 SQLite 连接。
    Database database_;
    std::filesystem::path storage_root_;
    std::shared_ptr<AuthService> auth_service_;
    std::shared_ptr<FileService> file_service_;
    int schema_version_{0};
    bool database_ready_{false};
};

}  // namespace personal_cloud
