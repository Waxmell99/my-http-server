#include "server/http_server.h"

#include <cerrno>
#include <cstdio>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace personal_cloud {

int create_listening_socket(std::uint16_t port, int backlog) {
    const int socket_fd = ::socket(AF_INET, SOCK_STREAM, 0);
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
    int client_fd = -1;

    do {
        client_fd = ::accept(listening_fd, nullptr, nullptr);
    } while (client_fd == -1 && errno == EINTR);

    if (client_fd == -1) {
        std::perror("accept");
    }

    return client_fd;
}

ssize_t receive_data(int client_fd, char* buffer, std::size_t buffer_size) {
    ssize_t received = -1;

    do {
        received = ::recv(client_fd, buffer, buffer_size, 0);
    } while (received == -1 && errno == EINTR);

    if (received == -1) {
        std::perror("recv");
    }

    return received;
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
