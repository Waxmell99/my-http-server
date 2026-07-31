#include "http/router.h"

namespace personal_cloud {

HttpResponse route_request(const HttpRequest& request) {
    if (request.method != "GET") {
        return {
            405,
            "Method Not Allowed",
            "text/plain; charset=utf-8",
            "Method Not Allowed\n",
        };
    }

    if (request.path == "/hello") {
        return {
            200,
            "OK",
            "text/plain; charset=utf-8",
            "Hello World\n",
        };
    }

    if (request.path == "/health") {
        return {
            200,
            "OK",
            "text/plain; charset=utf-8",
            "OK\n",
        };
    }

    return {
        404,
        "Not Found",
        "text/plain; charset=utf-8",
        "Not Found\n",
    };
}

}  // namespace personal_cloud
