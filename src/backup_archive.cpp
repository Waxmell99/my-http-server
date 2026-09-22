#include "admin/backup_archive.h"

#include <sodium.h>
#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
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

namespace personal_cloud::admin {
namespace {

namespace fs = std::filesystem;

constexpr std::array<unsigned char, 16> archive_magic{
    'P', 'C', 'L', 'O', 'U', 'D', 'B', 'A',
    'C', 'K', 'U', 'P', '\r', '\n', 0x1a, '\n'};
constexpr std::uint32_t archive_version = 1;
constexpr std::size_t digest_size = crypto_hash_sha256_BYTES;
constexpr std::size_t copy_buffer_size = 64 * 1024;

using Digest = std::array<unsigned char, digest_size>;

class FileDescriptor final {
public:
    explicit FileDescriptor(int value = -1) noexcept : value_(value) {}
    ~FileDescriptor() {
        if (value_ >= 0) {
            ::close(value_);
        }
    }

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    FileDescriptor(FileDescriptor&& other) noexcept
        : value_(std::exchange(other.value_, -1)) {}

    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
        if (this != &other) {
            if (value_ >= 0) {
                ::close(value_);
            }
            value_ = std::exchange(other.value_, -1);
        }
        return *this;
    }

    [[nodiscard]] int get() const noexcept { return value_; }

    void close() {
        if (value_ >= 0 && ::close(std::exchange(value_, -1)) == -1) {
            throw std::runtime_error(
                "Cannot close file: " + std::string(std::strerror(errno)));
        }
    }

private:
    int value_;
};

class SqliteHandle final {
public:
    explicit SqliteHandle(sqlite3* value = nullptr) noexcept : value_(value) {}
    ~SqliteHandle() {
        if (value_ != nullptr) {
            ::sqlite3_close(value_);
        }
    }

    SqliteHandle(const SqliteHandle&) = delete;
    SqliteHandle& operator=(const SqliteHandle&) = delete;

    SqliteHandle(SqliteHandle&& other) noexcept
        : value_(std::exchange(other.value_, nullptr)) {}

    [[nodiscard]] sqlite3* get() const noexcept { return value_; }

private:
    sqlite3* value_;
};

class TemporaryPath final {
public:
    explicit TemporaryPath(fs::path path) : path_(std::move(path)) {}
    ~TemporaryPath() {
        if (!path_.empty()) {
            std::error_code error;
            fs::remove_all(path_, error);
        }
    }

    TemporaryPath(const TemporaryPath&) = delete;
    TemporaryPath& operator=(const TemporaryPath&) = delete;

    [[nodiscard]] const fs::path& get() const noexcept { return path_; }
    void release() noexcept { path_.clear(); }

private:
    fs::path path_;
};

struct StoredObject {
    std::string key;
    std::uint64_t size{0};
    Digest digest{};
};

std::runtime_error system_error(std::string_view operation) {
    return std::runtime_error(
        std::string(operation) + ": " + std::strerror(errno));
}

SqliteHandle open_database(const fs::path& path, int flags) {
    sqlite3* raw = nullptr;
    const int result = ::sqlite3_open_v2(path.c_str(), &raw, flags, nullptr);
    if (result != SQLITE_OK) {
        const std::string message = raw == nullptr
            ? ::sqlite3_errstr(result)
            : ::sqlite3_errmsg(raw);
        if (raw != nullptr) {
            ::sqlite3_close(raw);
        }
        throw std::runtime_error("Cannot open database: " + message);
    }
    return SqliteHandle(raw);
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

std::vector<StoredObject> read_stored_objects(sqlite3* database) {
    sqlite3_stmt* statement = nullptr;
    if (::sqlite3_prepare_v2(
            database,
            "SELECT storage_key, size, sha256 FROM files "
            "ORDER BY storage_key;",
            -1,
            &statement,
            nullptr) != SQLITE_OK) {
        throw std::runtime_error(
            "Cannot read file metadata: " +
            std::string(::sqlite3_errmsg(database)));
    }

    std::vector<StoredObject> objects;
    int result = SQLITE_OK;
    while ((result = ::sqlite3_step(statement)) == SQLITE_ROW) {
        const auto* key = reinterpret_cast<const char*>(
            ::sqlite3_column_text(statement, 0));
        const sqlite3_int64 signed_size = ::sqlite3_column_int64(statement, 1);
        const void* digest = ::sqlite3_column_blob(statement, 2);
        const int bytes = ::sqlite3_column_bytes(statement, 2);
        if (key == nullptr || signed_size < 0 || digest == nullptr ||
            bytes != static_cast<int>(digest_size)) {
            ::sqlite3_finalize(statement);
            throw std::runtime_error("Database contains invalid file metadata");
        }
        StoredObject object;
        object.key = key;
        object.size = static_cast<std::uint64_t>(signed_size);
        std::memcpy(object.digest.data(), digest, digest_size);
        if (!valid_storage_key(object.key)) {
            ::sqlite3_finalize(statement);
            throw std::runtime_error("Database contains an unsafe storage key");
        }
        objects.push_back(std::move(object));
    }
    ::sqlite3_finalize(statement);
    if (result != SQLITE_DONE) {
        throw std::runtime_error("Cannot finish reading file metadata");
    }
    return objects;
}

void require_database_integrity(sqlite3* database) {
    sqlite3_stmt* statement = nullptr;
    if (::sqlite3_prepare_v2(
            database,
            "PRAGMA integrity_check;",
            -1,
            &statement,
            nullptr) != SQLITE_OK) {
        throw std::runtime_error("Cannot start SQLite integrity check");
    }
    const int result = ::sqlite3_step(statement);
    const auto* value = result == SQLITE_ROW
        ? reinterpret_cast<const char*>(::sqlite3_column_text(statement, 0))
        : nullptr;
    const bool valid = value != nullptr && std::string_view(value) == "ok" &&
                       ::sqlite3_step(statement) == SQLITE_DONE;
    ::sqlite3_finalize(statement);
    if (!valid) {
        throw std::runtime_error("SQLite snapshot failed integrity check");
    }
}

void create_sqlite_snapshot(
    const fs::path& source_path,
    const fs::path& destination_path) {
    SqliteHandle source = open_database(
        source_path, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX);
    SqliteHandle destination = open_database(
        destination_path,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_EXCLUSIVE |
            SQLITE_OPEN_FULLMUTEX);
    sqlite3_backup* backup = ::sqlite3_backup_init(
        destination.get(), "main", source.get(), "main");
    if (backup == nullptr) {
        throw std::runtime_error(
            "Cannot initialize SQLite backup: " +
            std::string(::sqlite3_errmsg(destination.get())));
    }
    int result = SQLITE_OK;
    do {
        result = ::sqlite3_backup_step(backup, 128);
        if (result == SQLITE_BUSY || result == SQLITE_LOCKED) {
            ::sqlite3_sleep(25);
        }
    } while (result == SQLITE_OK || result == SQLITE_BUSY ||
             result == SQLITE_LOCKED);
    const int finish_result = ::sqlite3_backup_finish(backup);
    if (result != SQLITE_DONE || finish_result != SQLITE_OK) {
        throw std::runtime_error("SQLite online backup failed");
    }
    if (::chmod(destination_path.c_str(), S_IRUSR | S_IWUSR) == -1) {
        throw system_error("Cannot protect database snapshot");
    }
}

std::uint64_t regular_file_size(int descriptor, std::string_view description) {
    struct stat status {};
    if (::fstat(descriptor, &status) == -1) {
        throw system_error("Cannot inspect " + std::string(description));
    }
    if (!S_ISREG(status.st_mode) || status.st_size < 0) {
        throw std::runtime_error(
            std::string(description) + " is not a regular file");
    }
    return static_cast<std::uint64_t>(status.st_size);
}

FileDescriptor open_readonly_file(
    const fs::path& path,
    std::string_view description) {
    const int descriptor = ::open(
        path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor == -1) {
        throw system_error("Cannot open " + std::string(description));
    }
    return FileDescriptor(descriptor);
}

FileDescriptor create_private_file(const fs::path& path) {
    const int descriptor = ::open(
        path.c_str(),
        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
        S_IRUSR | S_IWUSR);
    if (descriptor == -1) {
        throw system_error("Cannot create " + path.string());
    }
    return FileDescriptor(descriptor);
}

void write_all(int descriptor, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t written = ::write(
            descriptor, bytes + offset, size - offset);
        if (written > 0) {
            offset += static_cast<std::size_t>(written);
        } else if (written == -1 && errno == EINTR) {
            continue;
        } else {
            throw system_error("Cannot write backup archive");
        }
    }
}

void read_exact(int descriptor, void* data, std::size_t size) {
    auto* bytes = static_cast<unsigned char*>(data);
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t received = ::read(
            descriptor, bytes + offset, size - offset);
        if (received > 0) {
            offset += static_cast<std::size_t>(received);
        } else if (received == -1 && errno == EINTR) {
            continue;
        } else if (received == 0) {
            throw std::runtime_error("Backup archive ended unexpectedly");
        } else {
            throw system_error("Cannot read backup archive");
        }
    }
}

template <typename Integer>
void write_integer(int descriptor, Integer value) {
    std::array<unsigned char, sizeof(Integer)> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<unsigned char>(value & 0xffU);
        value >>= 8U;
    }
    write_all(descriptor, bytes.data(), bytes.size());
}

template <typename Integer>
Integer read_integer(int descriptor) {
    std::array<unsigned char, sizeof(Integer)> bytes{};
    read_exact(descriptor, bytes.data(), bytes.size());
    Integer value = 0;
    for (std::size_t index = bytes.size(); index > 0; --index) {
        value = static_cast<Integer>((value << 8U) | bytes[index - 1]);
    }
    return value;
}

Digest copy_and_hash(
    int source,
    int destination,
    std::uint64_t size) {
    crypto_hash_sha256_state hash_state{};
    if (::crypto_hash_sha256_init(&hash_state) != 0) {
        throw std::runtime_error("Cannot initialize SHA-256");
    }
    std::array<unsigned char, copy_buffer_size> buffer{};
    std::uint64_t remaining = size;
    while (remaining > 0) {
        const std::size_t wanted = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, buffer.size()));
        ssize_t received = -1;
        do {
            received = ::read(source, buffer.data(), wanted);
        } while (received == -1 && errno == EINTR);
        if (received <= 0) {
            if (received == 0) {
                throw std::runtime_error("Source file ended unexpectedly");
            }
            throw system_error("Cannot read source file");
        }
        const auto received_size = static_cast<std::size_t>(received);
        if (::crypto_hash_sha256_update(
                &hash_state, buffer.data(), received_size) != 0) {
            throw std::runtime_error("Cannot update SHA-256");
        }
        write_all(destination, buffer.data(), received_size);
        remaining -= received_size;
    }
    Digest digest{};
    if (::crypto_hash_sha256_final(&hash_state, digest.data()) != 0) {
        throw std::runtime_error("Cannot finish SHA-256");
    }
    return digest;
}

void require_archive_end(int descriptor) {
    unsigned char byte = 0;
    ssize_t received = -1;
    do {
        received = ::read(descriptor, &byte, 1);
    } while (received == -1 && errno == EINTR);
    if (received != 0) {
        if (received < 0) {
            throw system_error("Cannot finish reading backup archive");
        }
        throw std::runtime_error("Backup archive has trailing data");
    }
}

fs::path temporary_sibling(const fs::path& path, std::string_view suffix) {
    fs::path result = path;
    result += "." + std::string(suffix) + "." + std::to_string(::getpid());
    return result;
}

void prepare_parent(const fs::path& path) {
    const fs::path parent = path.parent_path();
    if (!parent.empty()) {
        fs::create_directories(parent);
    }
}

void initialize_sodium() {
    if (::sodium_init() < 0) {
        throw std::runtime_error("Cannot initialize libsodium");
    }
}

int rename_without_replacement(
    const fs::path& source,
    const fs::path& destination) {
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
    std::error_code error;
    if (fs::exists(destination, error)) {
        errno = EEXIST;
        return -1;
    }
    if (error) {
        errno = error.value();
        return -1;
    }
    fs::rename(source, destination, error);
    if (error) {
        errno = error.value();
        return -1;
    }
    return 0;
}

}  // namespace

ArchiveSummary create_backup_archive(
    const fs::path& database_path,
    const fs::path& storage_root,
    const fs::path& output_path,
    bool force) {
    initialize_sodium();
    if (database_path.empty() || storage_root.empty() || output_path.empty()) {
        throw std::runtime_error(
            "backup requires database, storage root, and output paths");
    }
    std::error_code error;
    if (fs::exists(output_path, error) && !force) {
        throw std::runtime_error("Output exists; pass --force to replace it");
    }
    prepare_parent(output_path);

    TemporaryPath snapshot(
        temporary_sibling(output_path, "database-snapshot"));
    TemporaryPath archive(
        temporary_sibling(output_path, "archive-tmp"));
    if (fs::exists(snapshot.get()) || fs::exists(archive.get())) {
        throw std::runtime_error("Temporary backup path already exists");
    }
    create_sqlite_snapshot(database_path, snapshot.get());
    SqliteHandle snapshot_database = open_database(
        snapshot.get(), SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX);
    require_database_integrity(snapshot_database.get());
    const std::vector<StoredObject> objects =
        read_stored_objects(snapshot_database.get());

    FileDescriptor output = create_private_file(archive.get());
    write_all(output.get(), archive_magic.data(), archive_magic.size());
    write_integer<std::uint32_t>(output.get(), archive_version);

    FileDescriptor database_file = open_readonly_file(
        snapshot.get(), "database snapshot");
    const std::uint64_t database_size = regular_file_size(
        database_file.get(), "database snapshot");
    write_integer<std::uint64_t>(output.get(), database_size);
    write_integer<std::uint64_t>(
        output.get(), static_cast<std::uint64_t>(objects.size()));
    const Digest database_digest = copy_and_hash(
        database_file.get(), output.get(), database_size);
    write_all(output.get(), database_digest.data(), database_digest.size());

    ArchiveSummary summary;
    summary.object_count = static_cast<std::uint64_t>(objects.size());
    for (const StoredObject& object : objects) {
        if (object.key.size() > std::numeric_limits<std::uint16_t>::max()) {
            throw std::runtime_error("Storage key is too long");
        }
        const fs::path object_path = storage_root / "objects" / object.key;
        FileDescriptor object_file = open_readonly_file(
            object_path, "stored object " + object.key);
        if (regular_file_size(object_file.get(), "stored object") !=
            object.size) {
            throw std::runtime_error(
                "Stored object size does not match database metadata");
        }
        write_integer<std::uint16_t>(
            output.get(), static_cast<std::uint16_t>(object.key.size()));
        write_all(output.get(), object.key.data(), object.key.size());
        write_integer<std::uint64_t>(output.get(), object.size);
        const Digest actual_digest = copy_and_hash(
            object_file.get(), output.get(), object.size);
        if (actual_digest != object.digest) {
            throw std::runtime_error(
                "Stored object SHA-256 does not match database metadata");
        }
        write_all(output.get(), actual_digest.data(), actual_digest.size());
        if (summary.object_bytes >
            std::numeric_limits<std::uint64_t>::max() - object.size) {
            throw std::runtime_error("Stored object byte count overflow");
        }
        summary.object_bytes += object.size;
    }
    if (::fsync(output.get()) == -1) {
        throw system_error("Cannot synchronize backup archive");
    }
    output.close();

    if (force) {
        fs::rename(archive.get(), output_path, error);
    } else if (rename_without_replacement(archive.get(), output_path) == -1) {
        error = std::error_code(errno, std::generic_category());
    }
    if (error) {
        throw std::runtime_error(
            "Cannot publish backup archive: " + error.message());
    }
    archive.release();
    return summary;
}

ArchiveSummary restore_backup_archive(
    const fs::path& input_path,
    const fs::path& database_path,
    const fs::path& storage_root) {
    initialize_sodium();
    if (input_path.empty() || database_path.empty() || storage_root.empty()) {
        throw std::runtime_error(
            "restore requires input, database, and storage root paths");
    }
    if (fs::exists(database_path) || fs::exists(storage_root)) {
        throw std::runtime_error(
            "Restore destination exists; choose empty destination paths");
    }
    prepare_parent(database_path);
    prepare_parent(storage_root);

    TemporaryPath staged_database(
        temporary_sibling(database_path, "restore-tmp"));
    TemporaryPath staged_storage(
        temporary_sibling(storage_root, "restore-tmp"));
    if (fs::exists(staged_database.get()) || fs::exists(staged_storage.get())) {
        throw std::runtime_error("Temporary restore path already exists");
    }
    fs::create_directories(staged_storage.get() / "objects");
    fs::create_directories(staged_storage.get() / "tmp");
    fs::create_directories(staged_storage.get() / "trash");
    static_cast<void>(::chmod(staged_storage.get().c_str(), S_IRWXU));

    FileDescriptor input = open_readonly_file(input_path, "backup archive");
    static_cast<void>(regular_file_size(input.get(), "backup archive"));
    std::array<unsigned char, archive_magic.size()> magic{};
    read_exact(input.get(), magic.data(), magic.size());
    if (magic != archive_magic) {
        throw std::runtime_error("Backup archive magic is invalid");
    }
    if (read_integer<std::uint32_t>(input.get()) != archive_version) {
        throw std::runtime_error("Backup archive version is unsupported");
    }
    const std::uint64_t database_size =
        read_integer<std::uint64_t>(input.get());
    const std::uint64_t object_count =
        read_integer<std::uint64_t>(input.get());

    FileDescriptor database_file = create_private_file(staged_database.get());
    const Digest actual_database_digest = copy_and_hash(
        input.get(), database_file.get(), database_size);
    Digest archived_database_digest{};
    read_exact(
        input.get(),
        archived_database_digest.data(),
        archived_database_digest.size());
    if (actual_database_digest != archived_database_digest) {
        throw std::runtime_error("Database snapshot SHA-256 is invalid");
    }
    if (::fsync(database_file.get()) == -1) {
        throw system_error("Cannot synchronize restored database");
    }
    database_file.close();

    SqliteHandle restored_database = open_database(
        staged_database.get(), SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX);
    require_database_integrity(restored_database.get());
    const std::vector<StoredObject> objects =
        read_stored_objects(restored_database.get());
    if (object_count != static_cast<std::uint64_t>(objects.size())) {
        throw std::runtime_error(
            "Backup object count does not match database metadata");
    }
    std::unordered_map<std::string, StoredObject> expected;
    expected.reserve(objects.size());
    for (const StoredObject& object : objects) {
        expected.emplace(object.key, object);
    }

    ArchiveSummary summary;
    summary.object_count = object_count;
    for (std::uint64_t index = 0; index < object_count; ++index) {
        const std::uint16_t key_size =
            read_integer<std::uint16_t>(input.get());
        if (key_size == 0 || key_size > 255) {
            throw std::runtime_error("Backup storage key length is invalid");
        }
        std::string key(key_size, '\0');
        read_exact(input.get(), key.data(), key.size());
        if (!valid_storage_key(key)) {
            throw std::runtime_error("Backup contains an unsafe storage key");
        }
        const auto found = expected.find(key);
        if (found == expected.end()) {
            throw std::runtime_error(
                "Backup object is duplicated or absent from the database");
        }
        const std::uint64_t size = read_integer<std::uint64_t>(input.get());
        if (size != found->second.size) {
            throw std::runtime_error(
                "Backup object size does not match database metadata");
        }
        FileDescriptor object_file = create_private_file(
            staged_storage.get() / "objects" / key);
        const Digest actual_digest = copy_and_hash(
            input.get(), object_file.get(), size);
        Digest archived_digest{};
        read_exact(input.get(), archived_digest.data(), archived_digest.size());
        if (actual_digest != archived_digest ||
            actual_digest != found->second.digest) {
            throw std::runtime_error(
                "Backup object SHA-256 does not match database metadata");
        }
        if (::fsync(object_file.get()) == -1) {
            throw system_error("Cannot synchronize restored object");
        }
        object_file.close();
        expected.erase(found);
        if (summary.object_bytes >
            std::numeric_limits<std::uint64_t>::max() - size) {
            throw std::runtime_error("Restored object byte count overflow");
        }
        summary.object_bytes += size;
    }
    if (!expected.empty()) {
        throw std::runtime_error("Backup is missing stored objects");
    }
    require_archive_end(input.get());

    std::error_code error;
    if (rename_without_replacement(
            staged_storage.get(), storage_root) == -1) {
        error = std::error_code(errno, std::generic_category());
    }
    if (error) {
        throw std::runtime_error(
            "Cannot publish restored storage: " + error.message());
    }
    if (rename_without_replacement(
            staged_database.get(), database_path) == -1) {
        error = std::error_code(errno, std::generic_category());
    }
    if (error) {
        std::error_code rollback_error;
        fs::rename(storage_root, staged_storage.get(), rollback_error);
        throw std::runtime_error(
            "Cannot publish restored database: " + error.message());
    }
    staged_storage.release();
    staged_database.release();
    return summary;
}

}  // namespace personal_cloud::admin
