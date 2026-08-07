#include "server/epoll_server.h"

#include <chrono>

int main() {
    personal_cloud::EpollServerConfig config;
    config.port = 9000;
    config.backlog = 1024;
    config.maximum_events = 1024;
    config.idle_timeout = std::chrono::seconds(30);
    config.verbose_logging = false;

    return personal_cloud::run_epoll_server(config);
}
