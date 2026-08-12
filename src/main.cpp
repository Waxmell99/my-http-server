#include "app/backend_application.h"
#include "app/backend_config.h"
#include "server/epoll_server.h"

#include <exception>
#include <iostream>
#include <string_view>
#include <utility>
#include <vector>

int main(int argc, char* argv[]) {
    std::vector<std::string_view> arguments;
    arguments.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0);
    for (int index = 1; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }

    personal_cloud::ConfigParseResult parsed =
        personal_cloud::parse_backend_config(arguments);
    const std::string_view program_name = argc > 0 ? argv[0] : "http_server";
    if (parsed.show_help) {
        std::cout << personal_cloud::backend_usage(program_name);
        return 0;
    }
    if (!parsed.config.has_value()) {
        std::cerr << "Configuration error: " << parsed.error << "\n\n"
                  << personal_cloud::backend_usage(program_name);
        return 2;
    }

    try {
        personal_cloud::BackendConfig config = std::move(*parsed.config);
        personal_cloud::BackendApplication application(config);
        config.server.request_handler = [&application](
                                            const personal_cloud::HttpRequest&
                                                request) {
            return application.handle_request(request);
        };
        config.server.request_task_factory = [&application](
                                                 const personal_cloud::HttpRequest&
                                                     request) {
            return application.make_task(request);
        };

        return personal_cloud::run_epoll_server(config.server);
    } catch (const std::exception& error) {
        std::cerr << "Server initialization failed: " << error.what() << '\n';
        return 1;
    }
}
