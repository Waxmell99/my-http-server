#pragma once

namespace personal_cloud {

// 接收、解析并响应一个 HTTP 请求。
// 该函数不关闭 client_fd，Socket 的拥有者负责关闭。
void handle_client(int client_fd);

}  // namespace personal_cloud
