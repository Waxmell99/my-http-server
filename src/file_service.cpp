#include "app/file_service.h"

#include "common/log.h"
#include "storage/database.h"

#include <nlohmann/json.hpp>
#include <sodium.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <linux/fs.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace personal_cloud {

struct FileServiceState {
    std::shared_ptr<AuthService> auth_service;
    std::filesystem::path database_path;
    std::filesystem::path objects_directory;
    std::filesystem::path temporary_directory;
    std::filesystem::path trash_directory;
    std::uint64_t maximum_file_size{0};
    std::uint64_t user_quota{0};
    std::size_t maximum_concurrent_uploads{0};

    std::mutex reservation_mutex;
    std::unordered_map<std::int64_t, std::uint64_t> reserved_bytes;
    std::size_t active_uploads{0};
};

namespace {

using Json = nlohmann::json;
using SystemClock = std::chrono::system_clock;

constexpr std::size_t maximum_file_name_size = 255;
constexpr std::size_t maximum_mime_type_size = 255;

std::int64_t unix_time_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               SystemClock::now().time_since_epoch())
        .count();
}

std::string_view path_without_query(std::string_view target) {
    return target.substr(0, target.find('?'));
}

HttpResponse json_response(
    int status_code,
    std::string reason,
    Json body,
    std::vector<std::pair<std::string, std::string>> headers = {}) {
    return {
        status_code,
        std::move(reason),
        "application/json; charset=utf-8",
        body.dump() + "\n",
        std::move(headers),
    };
}

HttpResponse json_error(
    int status_code,
    std::string reason,
    std::string code,
    std::string message,
    std::vector<std::pair<std::string, std::string>> headers = {}) {
    return json_response(
        status_code,
        std::move(reason),
        {{"error", {{"code", std::move(code)},
                    {"message", std::move(message)}}}},
        std::move(headers));
}

HttpResponse unauthorized() {
    return json_error(
        401,
        "Unauthorized",
        "authentication_required",
        "Authentication required");
}

HttpResponse file_not_found() {
    return json_error(404, "Not Found", "file_not_found", "File not found");
}

HttpResponse internal_error() {
    return json_error(
        500,
        "Internal Server Error",
        "internal_error",
        "Internal server error");
}

HttpResponse method_not_allowed(std::string allow) {
    return json_error(
        405,
        "Method Not Allowed",
        "method_not_allowed",
        "Method not allowed",
        {{"Allow", std::move(allow)}});
}

bool valid_file_name(std::string_view name) {
    if (name.empty() || name.size() > maximum_file_name_size) {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](char character) {
        const auto byte = static_cast<unsigned char>(character);
        return byte >= 0x20U && byte != 0x7fU;
    });
}

std::optional<std::string> percent_decode(std::string_view value) {
    const auto hex_value = [](char character) -> int {
        if (character >= '0' && character <= '9') {
            return character - '0';
        }
        if (character >= 'a' && character <= 'f') {
            return character - 'a' + 10;
        }
        if (character >= 'A' && character <= 'F') {
            return character - 'A' + 10;
        }
        return -1;
    };

    std::string decoded;
    decoded.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] != '%') {
            decoded.push_back(value[index]);
            continue;
        }
        if (index + 2 >= value.size()) {
            return std::nullopt;
        }
        const int high = hex_value(value[index + 1]);
        const int low = hex_value(value[index + 2]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        decoded.push_back(static_cast<char>((high << 4) | low));
        index += 2;
    }
    return decoded;
}

bool valid_mime_type(std::string_view mime_type) {
    if (mime_type.empty() || mime_type.size() > maximum_mime_type_size) {
        return false;
    }
    return std::all_of(mime_type.begin(), mime_type.end(), [](char character) {
        const auto byte = static_cast<unsigned char>(character);
        return byte >= 0x20U && byte < 0x7fU;
    });
}

bool ascii_case_insensitive_equal(
    std::string_view left,
    std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        char left_character = left[index];
        char right_character = right[index];
        if (left_character >= 'A' && left_character <= 'Z') {
            left_character = static_cast<char>(left_character - 'A' + 'a');
        }
        if (right_character >= 'A' && right_character <= 'Z') {
            right_character = static_cast<char>(right_character - 'A' + 'a');
        }
        if (left_character != right_character) {
            return false;
        }
    }
    return true;
}

bool has_json_content_type(const HttpRequest& request) {
    const auto found = request.headers.find("content-type");
    if (found == request.headers.end()) {
        return false;
    }
    std::string_view value(found->second);
    const std::size_t parameters = value.find(';');
    value = value.substr(0, parameters);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    return ascii_case_insensitive_equal(value, "application/json");
}

bool valid_storage_key(std::string_view key) {
    return key.size() == 43 &&
           std::all_of(key.begin(), key.end(), [](char character) {
               return (character >= 'a' && character <= 'z') ||
                      (character >= 'A' && character <= 'Z') ||
                      (character >= '0' && character <= '9') ||
                      character == '-' || character == '_';
           });
}

std::string random_identifier(std::size_t random_byte_count) {
    std::vector<unsigned char> random_bytes(random_byte_count);
    ::randombytes_buf(random_bytes.data(), random_bytes.size());
    std::string encoded(
        ::sodium_base64_encoded_len(
            random_bytes.size(), sodium_base64_VARIANT_URLSAFE_NO_PADDING),
        '\0');
    if (::sodium_bin2base64(
            encoded.data(),
            encoded.size(),
            random_bytes.data(),
            random_bytes.size(),
            sodium_base64_VARIANT_URLSAFE_NO_PADDING) == nullptr) {
        throw std::runtime_error("Cannot encode random file identifier");
    }
    encoded.resize(std::strlen(encoded.c_str()));
    ::sodium_memzero(random_bytes.data(), random_bytes.size());
    return encoded;
}

std::string hex_digest(const std::array<unsigned char, 32>& digest) {
    static constexpr std::string_view digits = "0123456789abcdef";
    std::string result;
    result.reserve(digest.size() * 2);
    for (unsigned char byte : digest) {
        result += digits[byte >> 4U];
        result += digits[byte & 0x0fU];
    }
    return result;
}

int rename_without_replacement(
    const std::filesystem::path& source,
    const std::filesystem::path& destination) {
#ifdef SYS_renameat2
    if (::syscall(
            SYS_renameat2,
            AT_FDCWD,
            source.c_str(),
            AT_FDCWD,
            destination.c_str(),
            RENAME_NOREPLACE) == 0) {
        return 0;
    }
    if (errno != ENOSYS && errno != EINVAL) {
        return -1;
    }
#endif
    if (::link(source.c_str(), destination.c_str()) == -1) {
        return -1;
    }
    if (::unlink(source.c_str()) == -1) {
        const int unlink_error = errno;
        static_cast<void>(::unlink(destination.c_str()));
        errno = unlink_error;
        return -1;
    }
    return 0;
}

Json file_json(const StoredFile& file) {
    return {
        {"id", file.id},
        {"name", file.original_name},
        {"mime_type", file.mime_type},
        {"size", file.size},
        {"sha256", hex_digest(file.sha256)},
        {"created_at", file.created_at},
        {"updated_at", file.updated_at},
    };
}

std::optional<std::string_view> metadata_file_id(std::string_view path) {
    constexpr std::string_view prefix = "/api/files/";
    if (!path.starts_with(prefix)) {
        return std::nullopt;
    }
    const std::string_view id = path.substr(prefix.size());
    if (id.empty() || id.find('/') != std::string_view::npos) {
        return std::nullopt;
    }
    return id;
}

std::optional<std::string_view> content_file_id(std::string_view path) {
    constexpr std::string_view prefix = "/api/files/";
    constexpr std::string_view suffix = "/content";
    if (!path.starts_with(prefix) || !path.ends_with(suffix)) {
        return std::nullopt;
    }
    const std::string_view id = path.substr(
        prefix.size(), path.size() - prefix.size() - suffix.size());
    if (id.empty() || id.find('/') != std::string_view::npos) {
        return std::nullopt;
    }
    return id;
}

class UploadReservation final {
public:
    UploadReservation(
        std::shared_ptr<FileServiceState> state,
        std::int64_t user_id,
        std::uint64_t size)
        : state_(std::move(state)), user_id_(user_id), size_(size) {}

    ~UploadReservation() {
        release();
    }

    UploadReservation(const UploadReservation&) = delete;
    UploadReservation& operator=(const UploadReservation&) = delete;

    void release() noexcept {
        if (!state_) {
            return;
        }
        std::lock_guard lock(state_->reservation_mutex);
        auto found = state_->reserved_bytes.find(user_id_);
        if (found != state_->reserved_bytes.end()) {
            if (found->second <= size_) {
                state_->reserved_bytes.erase(found);
            } else {
                found->second -= size_;
            }
        }
        if (state_->active_uploads > 0) {
            --state_->active_uploads;
        }
        state_.reset();
    }

private:
    std::shared_ptr<FileServiceState> state_;
    std::int64_t user_id_{0};
    std::uint64_t size_{0};
};

class FileUploadStream final : public UploadStream {
public:
    FileUploadStream(
        std::shared_ptr<FileServiceState> state,
        std::shared_ptr<UploadReservation> reservation,
        StoredFile file,
        std::filesystem::path temporary_path,
        std::filesystem::path object_path,
        int descriptor)
        : state_(std::move(state)),
          reservation_(std::move(reservation)),
          file_(std::move(file)),
          temporary_path_(std::move(temporary_path)),
          object_path_(std::move(object_path)),
          descriptor_(descriptor) {
        if (::crypto_hash_sha256_init(&hash_state_) != 0) {
            throw std::runtime_error("Cannot initialize upload SHA-256");
        }
    }

    ~FileUploadStream() override {
        close_descriptor();
        if (!committed_) {
            std::error_code error;
            std::filesystem::remove(
                published_ ? object_path_ : temporary_path_, error);
        }
    }

    std::optional<HttpResponse> append(
        std::string_view bytes,
        bool final_chunk) override {
        const std::uint64_t expected_size =
            static_cast<std::uint64_t>(file_.size);
        if (committed_ || descriptor_ < 0 ||
            received_size_ > expected_size ||
            bytes.size() > expected_size - received_size_) {
            throw std::runtime_error("Invalid upload stream state");
        }

        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const ssize_t written = ::write(
                descriptor_, bytes.data() + offset, bytes.size() - offset);
            if (written > 0) {
                offset += static_cast<std::size_t>(written);
                continue;
            }
            if (written == -1 && errno == EINTR) {
                continue;
            }
            throw std::runtime_error(
                "Cannot write upload temporary file: " +
                std::string(std::strerror(errno)));
        }

        if (!bytes.empty() &&
            ::crypto_hash_sha256_update(
                &hash_state_,
                reinterpret_cast<const unsigned char*>(bytes.data()),
                static_cast<unsigned long long>(bytes.size())) != 0) {
            throw std::runtime_error("Cannot update upload SHA-256");
        }
        received_size_ += static_cast<std::uint64_t>(bytes.size());
        if (!final_chunk) {
            return std::nullopt;
        }
        if (received_size_ != static_cast<std::uint64_t>(file_.size)) {
            throw std::runtime_error("Upload ended before Content-Length");
        }

        if (::crypto_hash_sha256_final(&hash_state_, file_.sha256.data()) != 0) {
            throw std::runtime_error("Cannot finish upload SHA-256");
        }
        if (::fdatasync(descriptor_) == -1) {
            throw std::runtime_error(
                "Cannot synchronize upload: " +
                std::string(std::strerror(errno)));
        }
        close_descriptor();

        if (rename_without_replacement(temporary_path_, object_path_) == -1) {
            throw std::runtime_error(
                "Cannot publish uploaded file: " +
                std::string(std::strerror(errno)));
        }
        published_ = true;

        try {
            Database database(
                state_->database_path, DatabaseOpenMode::existing_schema);
            if (!database.create_file(file_)) {
                throw std::runtime_error("Generated file identifier collided");
            }
        } catch (...) {
            std::error_code error;
            std::filesystem::remove(object_path_, error);
            published_ = false;
            throw;
        }

        committed_ = true;
        reservation_->release();
        return json_response(201, "Created", {{"file", file_json(file_)}});
    }

private:
    void close_descriptor() noexcept {
        if (descriptor_ >= 0) {
            static_cast<void>(::close(descriptor_));
            descriptor_ = -1;
        }
    }

    std::shared_ptr<FileServiceState> state_;
    std::shared_ptr<UploadReservation> reservation_;
    StoredFile file_;
    std::filesystem::path temporary_path_;
    std::filesystem::path object_path_;
    int descriptor_{-1};
    crypto_hash_sha256_state hash_state_{};
    std::uint64_t received_size_{0};
    bool published_{false};
    bool committed_{false};
};

class FileDownloadStream final : public DownloadStream {
public:
    FileDownloadStream(StoredFile file, int descriptor)
        : file_(std::move(file)), descriptor_(descriptor) {}

    ~FileDownloadStream() override {
        if (descriptor_ >= 0) {
            static_cast<void>(::close(descriptor_));
        }
    }

    [[nodiscard]] std::uint64_t size() const noexcept override {
        return static_cast<std::uint64_t>(file_.size);
    }

    [[nodiscard]] std::string_view content_type() const noexcept override {
        return file_.mime_type;
    }

    [[nodiscard]] std::string_view original_name() const noexcept override {
        return file_.original_name;
    }

    DownloadChunk read_chunk(std::size_t maximum_size) override {
        if (offset_ >= size()) {
            return {{}, true};
        }
        const std::uint64_t remaining = size() - offset_;
        const std::size_t wanted = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, maximum_size));
        std::string bytes(wanted, '\0');
        while (true) {
            const ssize_t received = ::read(descriptor_, bytes.data(), wanted);
            if (received > 0) {
                bytes.resize(static_cast<std::size_t>(received));
                offset_ += static_cast<std::uint64_t>(received);
                return {std::move(bytes), offset_ == size()};
            }
            if (received == -1 && errno == EINTR) {
                continue;
            }
            if (received == 0) {
                throw std::runtime_error("Stored file ended before metadata size");
            }
            throw std::runtime_error(
                "Cannot read stored file: " +
                std::string(std::strerror(errno)));
        }
    }

private:
    StoredFile file_;
    int descriptor_{-1};
    std::uint64_t offset_{0};
};

}  // namespace

FileService::FileService(
    const BackendConfig& config,
    std::shared_ptr<AuthService> auth_service,
    std::filesystem::path database_path,
    std::filesystem::path storage_root)
    : state_(std::make_shared<FileServiceState>()) {
    if (::sodium_init() < 0) {
        throw std::runtime_error("Cannot initialize libsodium for file service");
    }
    state_->auth_service = std::move(auth_service);
    state_->database_path = std::move(database_path);
    state_->objects_directory = storage_root / "objects";
    state_->temporary_directory = storage_root / "tmp";
    state_->trash_directory = storage_root / "trash";
    state_->maximum_file_size = config.maximum_file_size;
    state_->user_quota = config.user_quota;
    state_->maximum_concurrent_uploads = config.maximum_concurrent_uploads;

    for (const std::filesystem::path* directory : {
             &state_->objects_directory,
             &state_->temporary_directory,
             &state_->trash_directory}) {
        std::error_code error;
        std::filesystem::create_directories(*directory, error);
        if (error) {
            throw std::runtime_error(
                "Cannot create file storage directory: " + error.message());
        }
    }
}

std::optional<UploadPreparationTask> FileService::make_upload_task(
    const HttpRequest& request,
    std::uint64_t content_length) {
    if (request.method != "POST" ||
        path_without_query(request.path) != "/api/files") {
        return std::nullopt;
    }
    const std::shared_ptr<FileService> self = shared_from_this();
    return [self, request, content_length] {
        return self->prepare_upload(request, content_length);
    };
}

UploadPreparationResult FileService::prepare_upload(
    const HttpRequest& request,
    std::uint64_t content_length) {
    try {
        const std::optional<SessionUser> user =
            state_->auth_service->authenticate_request(request);
        if (!user.has_value()) {
            return unauthorized();
        }
        if (content_length > state_->maximum_file_size ||
            content_length > static_cast<std::uint64_t>(
                                 std::numeric_limits<std::int64_t>::max())) {
            return json_error(
                413,
                "Payload Too Large",
                "file_too_large",
                "File exceeds the configured size limit");
        }

        const auto name_header = request.headers.find("x-file-name");
        if (name_header == request.headers.end()) {
            return json_error(
                422,
                "Unprocessable Content",
                "invalid_file_name",
                "X-File-Name must contain a valid 1-255 byte file name");
        }
        std::string original_name = name_header->second;
        const auto encoding_header =
            request.headers.find("x-file-name-encoding");
        if (encoding_header != request.headers.end()) {
            if (!ascii_case_insensitive_equal(
                    encoding_header->second, "percent")) {
                return json_error(
                    422,
                    "Unprocessable Content",
                    "invalid_file_name_encoding",
                    "X-File-Name-Encoding must be percent when provided");
            }
            const std::optional<std::string> decoded =
                percent_decode(original_name);
            if (!decoded.has_value()) {
                return json_error(
                    422,
                    "Unprocessable Content",
                    "invalid_file_name_encoding",
                    "X-File-Name contains invalid percent encoding");
            }
            original_name = *decoded;
        }
        if (!valid_file_name(original_name)) {
            return json_error(
                422,
                "Unprocessable Content",
                "invalid_file_name",
                "X-File-Name must contain a valid 1-255 byte file name");
        }
        std::string mime_type = "application/octet-stream";
        const auto content_type = request.headers.find("content-type");
        if (content_type != request.headers.end()) {
            mime_type = content_type->second;
        }
        if (!valid_mime_type(mime_type)) {
            return json_error(
                422,
                "Unprocessable Content",
                "invalid_mime_type",
                "Content-Type is not a valid MIME value");
        }

        std::shared_ptr<UploadReservation> reservation;
        {
            std::lock_guard lock(state_->reservation_mutex);
            if (state_->active_uploads >=
                state_->maximum_concurrent_uploads) {
                return json_error(
                    503,
                    "Service Unavailable",
                    "upload_limit_reached",
                    "Too many uploads are active");
            }
            Database database(
                state_->database_path, DatabaseOpenMode::existing_schema);
            const std::int64_t committed_size =
                database.total_file_size(user->id);
            const auto reserved_entry = state_->reserved_bytes.find(user->id);
            const std::uint64_t reserved =
                reserved_entry == state_->reserved_bytes.end()
                    ? 0
                    : reserved_entry->second;
            const std::uint64_t committed =
                committed_size < 0
                    ? state_->user_quota
                    : static_cast<std::uint64_t>(committed_size);
            if (committed > state_->user_quota ||
                reserved > state_->user_quota - committed ||
                content_length > state_->user_quota - committed - reserved) {
                return json_error(
                    413,
                    "Payload Too Large",
                    "quota_exceeded",
                    "User storage quota would be exceeded");
            }
            state_->reserved_bytes[user->id] = reserved + content_length;
            ++state_->active_uploads;
            reservation = std::make_shared<UploadReservation>(
                state_, user->id, content_length);
        }

        const std::string id = random_identifier(16);
        const std::string storage_key = random_identifier(32);
        const std::filesystem::path temporary_path =
            state_->temporary_directory / (storage_key + ".upload");
        const std::filesystem::path object_path =
            state_->objects_directory / storage_key;
        const int descriptor = ::open(
            temporary_path.c_str(),
            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
            S_IRUSR | S_IWUSR);
        if (descriptor == -1) {
            throw std::runtime_error(
                "Cannot create upload temporary file: " +
                std::string(std::strerror(errno)));
        }

        const std::int64_t now = unix_time_now();
        StoredFile file{
            id,
            user->id,
            original_name,
            storage_key,
            std::move(mime_type),
            static_cast<std::int64_t>(content_length),
            {},
            now,
            now,
        };
        try {
            return std::static_pointer_cast<UploadStream>(
                std::make_shared<FileUploadStream>(
                    state_,
                    std::move(reservation),
                    std::move(file),
                    temporary_path,
                    object_path,
                    descriptor));
        } catch (...) {
            ::close(descriptor);
            std::error_code error;
            std::filesystem::remove(temporary_path, error);
            throw;
        }
    } catch (const std::exception& error) {
        write_log(std::cerr, "Upload preparation failed: ", error.what(), '\n');
        return internal_error();
    }
}

std::optional<DownloadPreparationTask> FileService::make_download_task(
    const HttpRequest& request) {
    if (request.method != "GET" ||
        !content_file_id(path_without_query(request.path)).has_value()) {
        return std::nullopt;
    }
    const std::shared_ptr<FileService> self = shared_from_this();
    return [self, request] { return self->prepare_download(request); };
}

DownloadPreparationResult FileService::prepare_download(
    const HttpRequest& request) {
    try {
        const std::optional<SessionUser> user =
            state_->auth_service->authenticate_request(request);
        if (!user.has_value()) {
            return unauthorized();
        }
        const std::optional<std::string_view> id =
            content_file_id(path_without_query(request.path));
        if (!id.has_value()) {
            return file_not_found();
        }
        Database database(
            state_->database_path, DatabaseOpenMode::existing_schema);
        std::optional<StoredFile> file = database.find_file(*id, user->id);
        if (!file.has_value()) {
            return file_not_found();
        }
        if (!valid_storage_key(file->storage_key)) {
            throw std::runtime_error("Database contains an unsafe storage key");
        }
        if (!valid_mime_type(file->mime_type)) {
            throw std::runtime_error("Database contains an unsafe MIME type");
        }
        const std::filesystem::path object_path =
            state_->objects_directory / file->storage_key;
        const int descriptor =
            ::open(object_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (descriptor == -1) {
            throw std::runtime_error(
                "Cannot open stored file: " +
                std::string(std::strerror(errno)));
        }
        struct stat status {};
        if (::fstat(descriptor, &status) == -1 || !S_ISREG(status.st_mode) ||
            status.st_size != file->size) {
            ::close(descriptor);
            throw std::runtime_error("Stored file does not match metadata");
        }
        try {
            return std::static_pointer_cast<DownloadStream>(
                std::make_shared<FileDownloadStream>(
                    std::move(*file), descriptor));
        } catch (...) {
            static_cast<void>(::close(descriptor));
            throw;
        }
    } catch (const std::exception& error) {
        write_log(std::cerr, "Download preparation failed: ", error.what(), '\n');
        return internal_error();
    }
}

HttpResponse FileService::handle_request(const HttpRequest& request) {
    try {
        const std::string_view path = path_without_query(request.path);
        if (path == "/api/files") {
            if (request.method == "POST") {
                return json_error(
                    500,
                    "Internal Server Error",
                    "streaming_unavailable",
                    "Streaming upload path is unavailable");
            }
            if (request.method != "GET") {
                return method_not_allowed("GET, POST");
            }
            const std::optional<SessionUser> user =
                state_->auth_service->authenticate_request(request);
            if (!user.has_value()) {
                return unauthorized();
            }
            Database database(
                state_->database_path, DatabaseOpenMode::existing_schema);
            Json files = Json::array();
            const std::vector<StoredFile> listed_files =
                database.list_files(user->id);
            for (const StoredFile& file : listed_files) {
                files.push_back(file_json(file));
            }
            const std::int64_t count = database.file_count(user->id);
            return json_response(
                200,
                "OK",
                {{"files", std::move(files)},
                 {"usage",
                  {{"used_bytes", database.total_file_size(user->id)},
                   {"quota_bytes", state_->user_quota},
                   {"file_count", count},
                   {"returned_count", listed_files.size()},
                   {"truncated", count > static_cast<std::int64_t>(
                                            listed_files.size())}}}});
        }

        const std::optional<std::string_view> id = metadata_file_id(path);
        if (!id.has_value()) {
            if (content_file_id(path).has_value() && request.method != "GET") {
                return method_not_allowed("GET");
            }
            return file_not_found();
        }
        if (request.method != "GET" && request.method != "PATCH" &&
            request.method != "DELETE") {
            return method_not_allowed("GET, PATCH, DELETE");
        }

        const std::optional<SessionUser> user =
            state_->auth_service->authenticate_request(request);
        if (!user.has_value()) {
            return unauthorized();
        }
        Database database(
            state_->database_path, DatabaseOpenMode::existing_schema);
        std::optional<StoredFile> file = database.find_file(*id, user->id);
        if (!file.has_value()) {
            return file_not_found();
        }

        if (request.method == "GET") {
            return json_response(200, "OK", {{"file", file_json(*file)}});
        }

        if (request.method == "PATCH") {
            if (!has_json_content_type(request)) {
                return json_error(
                    415,
                    "Unsupported Media Type",
                    "unsupported_media_type",
                    "Content-Type must be application/json");
            }
            const Json document = Json::parse(request.body, nullptr, false);
            if (document.is_discarded() || !document.is_object()) {
                return json_error(
                    400,
                    "Bad Request",
                    "invalid_json",
                    "Request body must be a JSON object");
            }
            const auto name = document.find("name");
            if (name == document.end() || !name->is_string()) {
                return json_error(
                    422,
                    "Unprocessable Content",
                    "invalid_file_name",
                    "name must be a string");
            }
            const std::string new_name = name->get<std::string>();
            if (!valid_file_name(new_name)) {
                return json_error(
                    422,
                    "Unprocessable Content",
                    "invalid_file_name",
                    "name must contain 1-255 valid bytes");
            }
            const std::int64_t now = unix_time_now();
            if (!database.rename_file(*id, user->id, new_name, now)) {
                return file_not_found();
            }
            file->original_name = new_name;
            file->updated_at = now;
            return json_response(200, "OK", {{"file", file_json(*file)}});
        }

        if (!valid_storage_key(file->storage_key)) {
            throw std::runtime_error("Database contains an unsafe storage key");
        }
        const std::filesystem::path object_path =
            state_->objects_directory / file->storage_key;
        const std::filesystem::path trash_path =
            state_->trash_directory /
            (file->storage_key + "." + random_identifier(8));
        if (rename_without_replacement(object_path, trash_path) == -1) {
            throw std::runtime_error(
                "Cannot isolate file before deletion: " +
                std::string(std::strerror(errno)));
        }
        try {
            if (!database.delete_file(*id, user->id)) {
                static_cast<void>(::rename(
                    trash_path.c_str(), object_path.c_str()));
                return file_not_found();
            }
        } catch (...) {
            if (::rename(trash_path.c_str(), object_path.c_str()) == -1) {
                write_log(
                    std::cerr,
                    "Cannot restore file after database delete failure: ",
                    std::strerror(errno),
                    '\n');
            }
            throw;
        }
        if (::unlink(trash_path.c_str()) == -1) {
            write_log(
                std::cerr,
                "File metadata deleted but trash cleanup failed: ",
                std::strerror(errno),
                '\n');
        }
        return json_response(200, "OK", {{"status", "deleted"}});
    } catch (const std::exception& error) {
        write_log(std::cerr, "File request failed: ", error.what(), '\n');
        return internal_error();
    }
}

}  // namespace personal_cloud
