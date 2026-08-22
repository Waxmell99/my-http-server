#include "app/auth_service.h"

#include "common/log.h"
#include "storage/database.h"

#include <nlohmann/json.hpp>
#include <sodium.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace personal_cloud {
namespace {

using Json = nlohmann::json;
using SystemClock = std::chrono::system_clock;
using SteadyClock = std::chrono::steady_clock;

constexpr std::size_t minimum_password_size = 8;
constexpr std::size_t maximum_password_size = 1024;
constexpr std::size_t maximum_login_failure_entries = 10'000;
constexpr std::size_t failures_before_block = 5;
constexpr std::chrono::seconds login_block_duration{60};
constexpr std::chrono::minutes failure_retention{15};
constexpr std::chrono::seconds session_lifetime{7 * 24 * 60 * 60};
constexpr std::string_view session_cookie_name = "pc_session";

struct Credentials {
    std::string username;
    std::string normalized_username;
    std::string password;
};

std::string_view path_without_query(std::string_view target) {
    return target.substr(0, target.find('?'));
}

std::string_view trim_optional_whitespace(std::string_view value) {
    while (!value.empty() &&
           (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() &&
           (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    return value;
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
            right_character = static_cast<char>(
                right_character - 'A' + 'a');
        }
        if (left_character != right_character) {
            return false;
        }
    }
    return true;
}

bool has_json_content_type(const HttpRequest& request) {
    const auto content_type = request.headers.find("content-type");
    if (content_type == request.headers.end()) {
        return false;
    }
    const std::size_t parameters_start = content_type->second.find(';');
    return ascii_case_insensitive_equal(
        trim_optional_whitespace(
            std::string_view(content_type->second).substr(
                0, parameters_start)),
        "application/json");
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

HttpResponse method_not_allowed(std::string allow) {
    return json_error(
        405,
        "Method Not Allowed",
        "method_not_allowed",
        "Method not allowed",
        {{"Allow", std::move(allow)}});
}

bool valid_username(std::string_view username) {
    if (username.size() < 3 || username.size() > 64) {
        return false;
    }
    return std::all_of(username.begin(), username.end(), [](char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') ||
               character == '_' || character == '-' || character == '.';
    });
}

std::string normalize_username(std::string_view username) {
    std::string normalized(username);
    for (char& character : normalized) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return normalized;
}

std::optional<Credentials> parse_credentials(
    const HttpRequest& request,
    HttpResponse& error_response) {
    if (!has_json_content_type(request)) {
        error_response = json_error(
            415,
            "Unsupported Media Type",
            "unsupported_media_type",
            "Content-Type must be application/json");
        return std::nullopt;
    }

    Json document = Json::parse(request.body, nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        error_response = json_error(
            400,
            "Bad Request",
            "invalid_json",
            "Request body must be a JSON object");
        return std::nullopt;
    }

    const auto username = document.find("username");
    const auto password = document.find("password");
    if (username == document.end() || password == document.end() ||
        !username->is_string() || !password->is_string()) {
        error_response = json_error(
            422,
            "Unprocessable Content",
            "invalid_credentials_format",
            "Username and password must be strings");
        return std::nullopt;
    }

    Credentials credentials;
    credentials.username = username->get<std::string>();
    credentials.password = password->get<std::string>();
    if (!valid_username(credentials.username) ||
        credentials.password.size() < minimum_password_size ||
        credentials.password.size() > maximum_password_size) {
        error_response = json_error(
            422,
            "Unprocessable Content",
            "invalid_credentials_format",
            "Username or password does not meet the required format");
        return std::nullopt;
    }
    credentials.normalized_username = normalize_username(credentials.username);
    return credentials;
}

std::string hash_password(std::string_view password) {
    std::array<char, crypto_pwhash_STRBYTES> encoded_hash{};
    if (::crypto_pwhash_str_alg(
            encoded_hash.data(),
            password.data(),
            static_cast<unsigned long long>(password.size()),
            crypto_pwhash_OPSLIMIT_INTERACTIVE,
            crypto_pwhash_MEMLIMIT_INTERACTIVE,
            crypto_pwhash_ALG_ARGON2ID13) != 0) {
        throw std::runtime_error("Password hashing failed");
    }
    return encoded_hash.data();
}

bool password_matches(
    std::string_view encoded_hash,
    std::string_view password) {
    const std::string null_terminated_hash(encoded_hash);
    return ::crypto_pwhash_str_verify(
               null_terminated_hash.c_str(),
               password.data(),
               static_cast<unsigned long long>(password.size())) == 0;
}

std::int64_t unix_time_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               SystemClock::now().time_since_epoch())
        .count();
}

Json user_json(
    std::int64_t id,
    const std::string& username,
    std::int64_t created_at) {
    return {
        {"id", id},
        {"username", username},
        {"created_at", created_at},
    };
}

std::string generate_session_token() {
    std::array<unsigned char, 32> random_token{};
    ::randombytes_buf(random_token.data(), random_token.size());

    std::string encoded(
        ::sodium_base64_encoded_len(
            random_token.size(), sodium_base64_VARIANT_URLSAFE_NO_PADDING),
        '\0');
    if (::sodium_bin2base64(
            encoded.data(),
            encoded.size(),
            random_token.data(),
            random_token.size(),
            sodium_base64_VARIANT_URLSAFE_NO_PADDING) == nullptr) {
        throw std::runtime_error("Session token encoding failed");
    }
    encoded.resize(std::strlen(encoded.c_str()));
    ::sodium_memzero(random_token.data(), random_token.size());
    return encoded;
}

SessionTokenHash hash_session_token(std::string_view token) {
    SessionTokenHash result{};
    if (::crypto_generichash(
            result.data(),
            result.size(),
            reinterpret_cast<const unsigned char*>(token.data()),
            static_cast<unsigned long long>(token.size()),
            nullptr,
            0) != 0) {
        throw std::runtime_error("Session token hashing failed");
    }
    return result;
}

bool valid_session_token(std::string_view token) {
    constexpr std::size_t encoded_token_size = 43;
    return token.size() == encoded_token_size &&
           std::all_of(token.begin(), token.end(), [](char character) {
               return (character >= 'a' && character <= 'z') ||
                      (character >= 'A' && character <= 'Z') ||
                      (character >= '0' && character <= '9') ||
                      character == '-' || character == '_';
           });
}

std::optional<std::string> session_token_from_cookie(
    const HttpRequest& request) {
    const auto cookie_header = request.headers.find("cookie");
    if (cookie_header == request.headers.end()) {
        return std::nullopt;
    }

    std::optional<std::string> result;
    std::string_view cookies = cookie_header->second;
    while (!cookies.empty()) {
        const std::size_t separator = cookies.find(';');
        const std::string_view item = trim_optional_whitespace(
            cookies.substr(0, separator));
        const std::size_t equals = item.find('=');
        if (equals != std::string_view::npos &&
            item.substr(0, equals) == session_cookie_name) {
            const std::string_view token = item.substr(equals + 1);
            if (result.has_value() || !valid_session_token(token)) {
                return std::nullopt;
            }
            result = token;
        }
        if (separator == std::string_view::npos) {
            break;
        }
        cookies.remove_prefix(separator + 1);
    }
    return result;
}

std::string session_cookie(const std::string& token) {
    return std::string(session_cookie_name) + "=" + token +
           "; Path=/; HttpOnly; SameSite=Strict; Max-Age=" +
           std::to_string(session_lifetime.count());
}

std::string clear_session_cookie() {
    return std::string(session_cookie_name) +
           "=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0";
}

HttpResponse unauthorized() {
    return json_error(
        401,
        "Unauthorized",
        "invalid_credentials",
        "Invalid username or password");
}

}  // namespace

AuthService::AuthService(std::filesystem::path database_path)
    : database_path_(std::move(database_path)) {
    if (::sodium_init() < 0) {
        throw std::runtime_error("Cannot initialize libsodium");
    }
    dummy_password_hash_ = hash_password("dummy-password-for-timing-only");
}

std::optional<SessionUser> AuthService::authenticate_request(
    const HttpRequest& request) const {
    const std::optional<std::string> token = session_token_from_cookie(request);
    if (!token.has_value()) {
        return std::nullopt;
    }
    Database database(database_path_, DatabaseOpenMode::existing_schema);
    return database.find_session_user(
        hash_session_token(*token), unix_time_now());
}

HttpResponse AuthService::handle_request(const HttpRequest& request) {
    std::optional<std::string> active_login_attempt;
    try {
        const std::string_view path = path_without_query(request.path);

        if (path == "/api/auth/register") {
            if (request.method != "POST") {
                return method_not_allowed("POST");
            }

            HttpResponse parse_error = json_error(
                400, "Bad Request", "invalid_request", "Invalid request");
            std::optional<Credentials> credentials =
                parse_credentials(request, parse_error);
            if (!credentials.has_value()) {
                return parse_error;
            }

            const std::string password_hash =
                hash_password(credentials->password);
            const std::int64_t now = unix_time_now();
            Database database(
                database_path_, DatabaseOpenMode::existing_schema);
            const std::optional<std::int64_t> user_id = database.create_user(
                credentials->username, password_hash, now);
            if (!user_id.has_value()) {
                return json_error(
                    409,
                    "Conflict",
                    "username_taken",
                    "Username is already registered");
            }
            return json_response(
                201,
                "Created",
                {{"user", user_json(
                              *user_id, credentials->username, now)}});
        }

        if (path == "/api/auth/login") {
            if (request.method != "POST") {
                return method_not_allowed("POST");
            }

            HttpResponse parse_error = json_error(
                400, "Bad Request", "invalid_request", "Invalid request");
            std::optional<Credentials> credentials =
                parse_credentials(request, parse_error);
            if (!credentials.has_value()) {
                return parse_error;
            }

            const std::chrono::seconds retry_after = begin_login_attempt(
                credentials->normalized_username);
            if (retry_after > std::chrono::seconds::zero()) {
                return json_error(
                    429,
                    "Too Many Requests",
                    "login_rate_limited",
                    "Too many failed login attempts",
                    {{"Retry-After", std::to_string(retry_after.count())}});
            }
            active_login_attempt = credentials->normalized_username;

            Database database(
                database_path_, DatabaseOpenMode::existing_schema);
            const std::optional<StoredUser> user =
                database.find_user_by_username(credentials->username);
            const std::string_view hash_to_verify = user.has_value()
                                                        ? user->password_hash
                                                        : dummy_password_hash_;
            const bool valid_password = password_matches(
                hash_to_verify, credentials->password);
            if (!user.has_value() || !valid_password) {
                finish_login_attempt(
                    credentials->normalized_username, false);
                active_login_attempt.reset();
                return unauthorized();
            }
            finish_login_attempt(credentials->normalized_username, true);
            active_login_attempt.reset();

            const std::int64_t now = unix_time_now();
            database.delete_expired_sessions(now);
            std::string token;
            bool session_created = false;
            for (int attempt = 0; attempt < 3 && !session_created; ++attempt) {
                token = generate_session_token();
                session_created = database.create_session(
                    hash_session_token(token),
                    user->id,
                    now + session_lifetime.count(),
                    now);
            }
            if (!session_created) {
                throw std::runtime_error(
                    "Cannot allocate a unique session token");
            }

            return json_response(
                200,
                "OK",
                {{"user", user_json(
                              user->id, user->username, user->created_at)}},
                {{"Set-Cookie", session_cookie(token)}});
        }

        if (path == "/api/auth/me") {
            if (request.method != "GET") {
                return method_not_allowed("GET");
            }
            const std::optional<SessionUser> user =
                authenticate_request(request);
            if (!user.has_value()) {
                return json_error(
                    401,
                    "Unauthorized",
                    "authentication_required",
                    "Authentication required");
            }
            return json_response(
                200,
                "OK",
                {{"user", user_json(
                              user->id, user->username, user->created_at)}});
        }

        if (path == "/api/auth/logout") {
            if (request.method != "POST") {
                return method_not_allowed("POST");
            }
            const std::optional<std::string> token =
                session_token_from_cookie(request);
            if (token.has_value()) {
                Database database(
                    database_path_, DatabaseOpenMode::existing_schema);
                static_cast<void>(database.delete_session(
                    hash_session_token(*token)));
            }
            return json_response(
                200,
                "OK",
                {{"status", "logged_out"}},
                {{"Set-Cookie", clear_session_cookie()}});
        }

        return json_error(
            404, "Not Found", "not_found", "Endpoint not found");
    } catch (const std::exception& error) {
        if (active_login_attempt.has_value()) {
            finish_login_attempt(*active_login_attempt, false);
        }
        write_log(
            std::cerr,
            "Authentication request failed: ",
            error.what(),
            '\n');
    } catch (...) {
        if (active_login_attempt.has_value()) {
            finish_login_attempt(*active_login_attempt, false);
        }
        write_log(
            std::cerr,
            "Authentication request failed with an unknown exception.\n");
    }
    return json_error(
        500,
        "Internal Server Error",
        "internal_error",
        "Internal server error");
}

std::chrono::seconds AuthService::begin_login_attempt(
    const std::string& normalized_username) {
    const SteadyClock::time_point now = SteadyClock::now();
    std::lock_guard lock(failure_mutex_);
    periodically_prune_failures(now);

    auto found = failures_.find(normalized_username);
    if (found == failures_.end()) {
        if (failures_.size() >= maximum_login_failure_entries) {
            return login_block_duration;
        }
        found = failures_
                    .emplace(normalized_username, FailureState{})
                    .first;
    }
    FailureState& failure = found->second;
    if (failure.blocked_until > now) {
        return std::max(
            std::chrono::seconds{1},
            std::chrono::duration_cast<std::chrono::seconds>(
                failure.blocked_until - now) +
                std::chrono::seconds{1});
    }
    if (failure.blocked_until != SteadyClock::time_point{}) {
        found->second.blocked_until = {};
    }
    if (failure.consecutive_failures + failure.active_attempts >=
        failures_before_block) {
        return std::chrono::seconds{1};
    }
    failure.last_attempt = now;
    ++failure.active_attempts;
    return std::chrono::seconds::zero();
}

void AuthService::finish_login_attempt(
    const std::string& normalized_username,
    bool succeeded) {
    const SteadyClock::time_point now = SteadyClock::now();
    std::lock_guard lock(failure_mutex_);
    const auto found = failures_.find(normalized_username);
    if (found == failures_.end()) {
        return;
    }
    FailureState& failure = found->second;
    if (failure.active_attempts > 0) {
        --failure.active_attempts;
    }
    failure.last_attempt = now;

    if (succeeded) {
        failure.consecutive_failures = 0;
        failure.blocked_until = {};
        if (failure.active_attempts == 0) {
            failures_.erase(found);
        }
        return;
    }

    ++failure.consecutive_failures;
    if (failure.consecutive_failures >= failures_before_block) {
        failure.consecutive_failures = 0;
        failure.blocked_until = now + login_block_duration;
    }
}

void AuthService::periodically_prune_failures(
    SteadyClock::time_point now) {
    ++limiter_operations_;
    if (limiter_operations_ % 128 != 0) {
        return;
    }
    std::erase_if(failures_, [now](const auto& item) {
        const FailureState& state = item.second;
        return state.active_attempts == 0 && state.blocked_until <= now &&
               now - state.last_attempt >= failure_retention;
    });
}

}  // namespace personal_cloud
