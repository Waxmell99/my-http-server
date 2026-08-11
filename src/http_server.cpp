#include "server/http_server.h"

#include <cerrno>
#include <cstdio>

#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace personal_cloud {
namespace {

bool is_transient_accept_error(int error) noexcept {
    switch (error) {
        case ECONNABORTED:
        case ENETDOWN:
        case EPROTO:
        case ENOPROTOOPT:
        case EHOSTDOWN:
        case ENONET:
        case EHOSTUNREACH:
        case EOPNOTSUPP:
        case ENETUNREACH:
            return true;

        default:
            return false;
    }
}

}  // namespace

int create_listening_socket(std::uint16_t port, int backlog) {
    const int socket_fd = ::socket(
        AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (socket_fd == -1) {
        std::perror("socket");
        return -1;
    }

    int reuse_address = 1;
    if (::setsockopt(socket_fd,
                     SOL_SOCKET,
                     SO_REUSEADDR,
                     &reuse_address,
                     sizeof(reuse_address)) == -1) {
        std::perror("setsockopt");
        close_socket(socket_fd);
        return -1;
    }

    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);

    if (::bind(socket_fd,
               reinterpret_cast<const sockaddr*>(&address),
               sizeof(address)) == -1) {
        std::perror("bind");
        close_socket(socket_fd);
        return -1;
    }

    if (::listen(socket_fd, backlog) == -1) {
        std::perror("listen");
        close_socket(socket_fd);
        return -1;
    }

    return socket_fd;
}

int accept_client(int listening_fd) {
    while (true) {
        const int client_fd = ::accept4(
            listening_fd,
            nullptr,
            nullptr,
            SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (client_fd >= 0) {
            return client_fd;
        }

        if (errno == EINTR || is_transient_accept_error(errno)) {
            continue;
        }

        if (errno != EAGAIN && errno != EWOULDBLOCK &&
            errno != EMFILE && errno != ENFILE) {
            std::perror("accept4");
        }
        return -1;
    }
}

bool set_socket_nonblocking(int socket_fd) {
    const int current_flags = ::fcntl(socket_fd, F_GETFL, 0);
    if (current_flags == -1) {
        std::perror("fcntl F_GETFL");
        return false;
    }

    if (::fcntl(socket_fd, F_SETFL, current_flags | O_NONBLOCK) == -1) {
        std::perror("fcntl F_SETFL O_NONBLOCK");
        return false;
    }

    return true;
}

bool set_socket_timeouts(int socket_fd, int receive_timeout_seconds, int send_timeout_seconds) {
    if (receive_timeout_seconds < 0 || send_timeout_seconds < 0) {
        errno = EINVAL;
        std::perror("invalid Socket timeout");
        return false;
    }

    timeval receive_timeout {};
    receive_timeout.tv_sec = receive_timeout_seconds;

    if (::setsockopt(socket_fd,
                    SOL_SOCKET,
                    SO_RCVTIMEO,
                    &receive_timeout,
                    sizeof(receive_timeout)) == -1) {
        std::perror("setsockopt SO_RCVTIMEO");
        return false;
    }

    timeval send_timeout {};
    send_timeout.tv_sec = send_timeout_seconds;

    if (::setsockopt(socket_fd,
                    SOL_SOCKET,
                    SO_SNDTIMEO,
                    &send_timeout,
                    sizeof(send_timeout)) == -1) {
        std::perror("setsockopt SO_SNDTIMEO");
        return false;
    }

    return true;
}

ReceiveResult receive_data(
    int client_fd,
    char* buffer,
    std::size_t buffer_size) {
    ssize_t received = -1;

    do {
        received = ::recv(client_fd, buffer, buffer_size, 0);
    } while (received == -1 && errno == EINTR);

    if (received > 0) {
        return {
            ReceiveStatus::data,
            static_cast<std::size_t>(received),
        };
    }

    if (received == 0) {
        return {ReceiveStatus::peer_closed, 0};
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return {ReceiveStatus::timeout, 0};
    }

    std::perror("recv");
    return {ReceiveStatus::error, 0};
}

bool send_all(int client_fd, const char* data, std::size_t data_size) {
    std::size_t total_sent = 0;

    while (total_sent < data_size) {
        const ssize_t sent =
            ::send(client_fd,
                   data + total_sent,
                   data_size - total_sent,
                   MSG_NOSIGNAL);

        if (sent > 0) {
            total_sent += static_cast<std::size_t>(sent);
            continue;
        }

        if (sent == -1 && errno == EINTR) {
            continue;
        }

        if (sent == -1) {
            std::perror("send");
        }
        return false;
    }

    return true;
}

void close_socket(int socket_fd) noexcept {
    if (socket_fd >= 0) {
        ::close(socket_fd);
    }
}

}  // namespace personal_cloud
