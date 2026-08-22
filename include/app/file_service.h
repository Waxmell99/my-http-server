#pragma once

#include "app/auth_service.h"
#include "app/backend_config.h"
#include "http/http_request.h"
#include "http/http_response.h"
#include "server/streaming.h"

#include <filesystem>
#include <memory>
#include <optional>

namespace personal_cloud {

struct FileServiceState;

class FileService final : public std::enable_shared_from_this<FileService> {
public:
    FileService(
        const BackendConfig& config,
        std::shared_ptr<AuthService> auth_service,
        std::filesystem::path database_path,
        std::filesystem::path storage_root);

    FileService(const FileService&) = delete;
    FileService& operator=(const FileService&) = delete;

    HttpResponse handle_request(const HttpRequest& request);
    [[nodiscard]] std::optional<UploadPreparationTask> make_upload_task(
        const HttpRequest& request,
        std::uint64_t content_length);
    [[nodiscard]] std::optional<DownloadPreparationTask> make_download_task(
        const HttpRequest& request);

private:
    UploadPreparationResult prepare_upload(
        const HttpRequest& request,
        std::uint64_t content_length);
    DownloadPreparationResult prepare_download(const HttpRequest& request);

    std::shared_ptr<FileServiceState> state_;
};

}  // namespace personal_cloud
