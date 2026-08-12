#pragma once

#include "http/http_request.h"
#include "http/http_response.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>

namespace personal_cloud {

class AuthService final {
public:
    explicit AuthService(std::filesystem::path database_path);

    AuthService(const AuthService&) = delete;
    AuthService& operator=(const AuthService&) = delete;

    HttpResponse handle_request(const HttpRequest& request);

private:
    struct FailureState {
        std::size_t consecutive_failures{0};
        std::size_t active_attempts{0};
        std::chrono::steady_clock::time_point blocked_until{};
        std::chrono::steady_clock::time_point last_attempt{};
    };

    [[nodiscard]] std::chrono::seconds begin_login_attempt(
        const std::string& normalized_username);
    void finish_login_attempt(
        const std::string& normalized_username,
        bool succeeded);
    void periodically_prune_failures(
        std::chrono::steady_clock::time_point now);

    std::filesystem::path database_path_;
    std::string dummy_password_hash_;
    std::mutex failure_mutex_;
    std::unordered_map<std::string, FailureState> failures_;
    std::size_t limiter_operations_{0};
};

}  // namespace personal_cloud
