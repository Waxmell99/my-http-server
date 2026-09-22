#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

#include <sys/stat.h>
#include <unistd.h>

namespace {

using Json = nlohmann::json;
namespace fs = std::filesystem;

class SqliteHandle {
public:
    explicit SqliteHandle(sqlite3* handle = nullptr) : handle_(handle) {}
    ~SqliteHandle() {
        if (handle_ != nullptr) {
            sqlite3_close(handle_);
        }
    }
    SqliteHandle(const SqliteHandle&) = delete;
    SqliteHandle& operator=(const SqliteHandle&) = delete;
    sqlite3* get() const { return handle_; }

private:
    sqlite3* handle_;
};

struct Options {
    std::string command;
    fs::path database;
    fs::path storage_root;
    fs::path output;
    std::uint64_t older_than_seconds{24 * 60 * 60};
    bool apply{false};
    bool force{false};
};

[[noreturn]] void usage_error(std::string_view message) {
    throw std::runtime_error(
        std::string(message) +
        "\nUsage:\n"
        "  cloud_admin backup --database PATH --output PATH [--force]\n"
        "  cloud_admin check --database PATH --storage-root PATH\n"
        "  cloud_admin cleanup --storage-root PATH [--older-than SECONDS] [--apply]");
}

std::uint64_t parse_unsigned(std::string_view value) {
    if (value.empty()) {
        usage_error("Missing numeric value");
    }
    std::uint64_t result = 0;
    for (char character : value) {
        if (character < '0' || character > '9') {
            usage_error("Invalid numeric value");
        }
        const std::uint64_t digit = static_cast<unsigned>(character - '0');
        if (result > (UINT64_MAX - digit) / 10) {
            usage_error("Numeric value is too large");
        }
        result = result * 10 + digit;
    }
    return result;
}

Options parse_options(int argc, char** argv) {
    if (argc < 2 || std::string_view(argv[1]) == "--help" ||
        std::string_view(argv[1]) == "-h") {
        usage_error(argc < 2 ? "Missing command" : "Help");
    }
    Options options;
    options.command = argv[1];
    for (int index = 2; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        const auto value = [&](std::string_view name) -> std::string_view {
            if (++index >= argc) {
                usage_error(std::string("Missing value for ") + std::string(name));
            }
            return argv[index];
        };
        if (argument == "--database") {
            options.database = value(argument);
        } else if (argument == "--storage-root") {
            options.storage_root = value(argument);
        } else if (argument == "--output") {
            options.output = value(argument);
        } else if (argument == "--older-than") {
            options.older_than_seconds = parse_unsigned(value(argument));
        } else if (argument == "--apply") {
            options.apply = true;
        } else if (argument == "--force") {
            options.force = true;
        } else {
            usage_error(std::string("Unknown option: ") + std::string(argument));
        }
    }
    return options;
}

SqliteHandle open_database(const fs::path& path, int flags) {
    sqlite3* raw = nullptr;
    const int result = sqlite3_open_v2(path.c_str(), &raw, flags, nullptr);
    if (result != SQLITE_OK) {
        const std::string message = raw == nullptr
            ? "unknown SQLite error"
            : sqlite3_errmsg(raw);
        if (raw != nullptr) {
            sqlite3_close(raw);
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

int backup_database(const Options& options) {
    if (options.database.empty() || options.output.empty()) {
        usage_error("backup requires --database and --output");
    }
    std::error_code error;
    if (fs::exists(options.output, error) && !options.force) {
        throw std::runtime_error("Output exists; pass --force to replace it");
    }
    if (!options.output.parent_path().empty()) {
        fs::create_directories(options.output.parent_path());
    }
    fs::path temporary = options.output;
    temporary += ".tmp." + std::to_string(::getpid());
    if (fs::exists(temporary, error)) {
        throw std::runtime_error("Temporary backup path already exists");
    }

    SqliteHandle source = open_database(
        options.database, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX);
    SqliteHandle destination = open_database(
        temporary, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                       SQLITE_OPEN_EXCLUSIVE | SQLITE_OPEN_FULLMUTEX);
    sqlite3_backup* backup = sqlite3_backup_init(
        destination.get(), "main", source.get(), "main");
    if (backup == nullptr) {
        fs::remove(temporary, error);
        throw std::runtime_error(
            "Cannot initialize backup: " +
            std::string(sqlite3_errmsg(destination.get())));
    }
    int result;
    do {
        result = sqlite3_backup_step(backup, 128);
        if (result == SQLITE_BUSY || result == SQLITE_LOCKED) {
            sqlite3_sleep(25);
        }
    } while (result == SQLITE_OK || result == SQLITE_BUSY ||
             result == SQLITE_LOCKED);
    const int finish_result = sqlite3_backup_finish(backup);
    if (result != SQLITE_DONE || finish_result != SQLITE_OK) {
        fs::remove(temporary, error);
        throw std::runtime_error("SQLite online backup failed");
    }
    ::chmod(temporary.c_str(), S_IRUSR | S_IWUSR);
    fs::rename(temporary, options.output, error);
    if (error) {
        fs::remove(temporary, error);
        throw std::runtime_error("Cannot publish backup: " + error.message());
    }
    std::cout << Json{{"status", "ok"},
                      {"operation", "backup"},
                      {"output", options.output.string()}}
                      .dump()
              << '\n';
    return 0;
}

std::unordered_map<std::string, std::uintmax_t> database_storage_keys(
    sqlite3* database) {
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(
            database, "SELECT storage_key, size FROM files", -1, &statement,
            nullptr) != SQLITE_OK) {
        throw std::runtime_error(
            "Cannot read file metadata: " +
            std::string(sqlite3_errmsg(database)));
    }
    std::unordered_map<std::string, std::uintmax_t> keys;
    int result;
    while ((result = sqlite3_step(statement)) == SQLITE_ROW) {
        const auto* text = sqlite3_column_text(statement, 0);
        if (text != nullptr) {
            const sqlite3_int64 size = sqlite3_column_int64(statement, 1);
            keys.emplace(
                reinterpret_cast<const char*>(text),
                size < 0 ? 0 : static_cast<std::uintmax_t>(size));
        }
    }
    sqlite3_finalize(statement);
    if (result != SQLITE_DONE) {
        throw std::runtime_error("Cannot finish reading file metadata");
    }
    return keys;
}

std::uint64_t directory_entry_count(const fs::path& directory) {
    std::error_code error;
    if (!fs::is_directory(directory, error)) {
        return 0;
    }
    std::uint64_t count = 0;
    for (fs::directory_iterator iterator(directory, error), end;
         !error && iterator != end; iterator.increment(error)) {
        ++count;
    }
    if (error) {
        throw std::runtime_error("Cannot scan " + directory.string() +
                                 ": " + error.message());
    }
    return count;
}

int check_storage(const Options& options) {
    if (options.database.empty() || options.storage_root.empty()) {
        usage_error("check requires --database and --storage-root");
    }
    SqliteHandle database = open_database(
        options.database, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX);
    const auto keys = database_storage_keys(database.get());
    const fs::path objects = options.storage_root / "objects";
    std::uint64_t missing = 0;
    std::uint64_t size_mismatches = 0;
    std::uint64_t invalid_keys = 0;
    for (const auto& [key, expected_size] : keys) {
        if (!valid_storage_key(key)) {
            ++invalid_keys;
            continue;
        }
        std::error_code error;
        const fs::path object = objects / key;
        if (!fs::is_regular_file(object, error)) {
            ++missing;
        } else if (fs::file_size(object, error) != expected_size || error) {
            ++size_mismatches;
        }
    }
    std::uint64_t objects_count = 0;
    std::uint64_t orphans = 0;
    std::error_code error;
    const bool objects_directory_exists = fs::is_directory(objects, error);
    if (error == std::errc::no_such_file_or_directory) {
        error.clear();
    }
    if (objects_directory_exists) {
        for (fs::directory_iterator iterator(objects, error), end;
             !error && iterator != end; iterator.increment(error)) {
            ++objects_count;
            const fs::file_status status = iterator->symlink_status(error);
            if (error) {
                break;
            }
            if (status.type() != fs::file_type::regular ||
                !keys.contains(iterator->path().filename().string())) {
                ++orphans;
            }
        }
    }
    if (error) {
        throw std::runtime_error("Cannot scan objects: " + error.message());
    }
    const Json result{{"status", missing == 0 && size_mismatches == 0 &&
                                    orphans == 0 && invalid_keys == 0
                                ? "ok" : "inconsistent"},
                      {"database_files", keys.size()},
                      {"object_files", objects_count},
                      {"missing_objects", missing},
                      {"size_mismatches", size_mismatches},
                      {"orphan_objects", orphans},
                      {"invalid_storage_keys", invalid_keys},
                      {"temporary_entries",
                       directory_entry_count(options.storage_root / "tmp")},
                      {"trash_entries",
                       directory_entry_count(options.storage_root / "trash")}};
    std::cout << result.dump() << '\n';
    return missing == 0 && size_mismatches == 0 && orphans == 0 &&
                   invalid_keys == 0
        ? 0 : 2;
}

int cleanup_storage(const Options& options) {
    if (options.storage_root.empty()) {
        usage_error("cleanup requires --storage-root");
    }
    const auto cutoff = fs::file_time_type::clock::now() -
        std::chrono::seconds(options.older_than_seconds);
    std::uint64_t candidates = 0;
    std::uint64_t removed = 0;
    std::uint64_t errors = 0;
    for (const char* directory_name : {"tmp", "trash"}) {
        const fs::path directory = options.storage_root / directory_name;
        std::error_code iteration_error;
        if (!fs::is_directory(directory, iteration_error)) {
            continue;
        }
        for (fs::directory_iterator iterator(directory, iteration_error), end;
             !iteration_error && iterator != end;
             iterator.increment(iteration_error)) {
            std::error_code status_error;
            const fs::file_status status = iterator->symlink_status(status_error);
            const auto modified = iterator->last_write_time(status_error);
            if (status_error ||
                status.type() != fs::file_type::regular ||
                modified > cutoff) {
                if (status_error) {
                    ++errors;
                }
                continue;
            }
            ++candidates;
            if (options.apply) {
                std::error_code remove_error;
                if (fs::remove(iterator->path(), remove_error)) {
                    ++removed;
                } else {
                    ++errors;
                }
            }
        }
        if (iteration_error) {
            ++errors;
        }
    }
    std::cout << Json{{"status", errors == 0 ? "ok" : "partial"},
                      {"operation", "cleanup"},
                      {"mode", options.apply ? "apply" : "dry-run"},
                      {"older_than_seconds", options.older_than_seconds},
                      {"candidates", candidates},
                      {"removed", removed},
                      {"errors", errors}}
                      .dump()
              << '\n';
    return errors == 0 ? 0 : 2;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);
        if (options.command == "backup") {
            return backup_database(options);
        }
        if (options.command == "check") {
            return check_storage(options);
        }
        if (options.command == "cleanup") {
            return cleanup_storage(options);
        }
        usage_error("Unknown command: " + options.command);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
