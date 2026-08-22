#pragma once

#include "http/http_request.h"
#include "http/http_response.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace personal_cloud {

class UploadStream {
public:
    virtual ~UploadStream() = default;

    // 非最后一块成功时返回 nullopt；最后一块返回最终 HTTP 响应。
    virtual std::optional<HttpResponse> append(
        std::string_view bytes,
        bool final_chunk) = 0;
};

struct DownloadChunk {
    std::string bytes;
    bool end_of_file{false};
};

class DownloadStream {
public:
    virtual ~DownloadStream() = default;

    [[nodiscard]] virtual std::uint64_t size() const noexcept = 0;
    [[nodiscard]] virtual std::string_view content_type() const noexcept = 0;
    [[nodiscard]] virtual std::string_view original_name() const noexcept = 0;
    virtual DownloadChunk read_chunk(std::size_t maximum_size) = 0;
};

using UploadPreparationResult =
    std::variant<HttpResponse, std::shared_ptr<UploadStream>>;
using UploadPreparationTask = std::function<UploadPreparationResult()>;
using UploadTaskFactory = std::function<std::optional<UploadPreparationTask>(
    const HttpRequest&, std::uint64_t)>;

using DownloadPreparationResult =
    std::variant<HttpResponse, std::shared_ptr<DownloadStream>>;
using DownloadPreparationTask = std::function<DownloadPreparationResult()>;
using DownloadTaskFactory = std::function<std::optional<DownloadPreparationTask>(
    const HttpRequest&)>;

}  // namespace personal_cloud
