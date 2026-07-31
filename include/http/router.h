#pragma once

#include "http/http_request.h"
#include "http/http_response.h"

namespace personal_cloud {

// 根据请求方法和路径选择处理结果。
// 当前支持 GET /hello 和 GET /health。
HttpResponse route_request(const HttpRequest& request);

}  // namespace personal_cloud
