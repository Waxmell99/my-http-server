# Personal Cloud Server

这是一个用于学习 Linux 网络编程和 HTTP 的 C++ 项目。

当前进度：已经完成非阻塞 Socket、epoll 事件循环、HTTP Request/Response、
路由、超时清理和单元测试。服务器使用单线程 epoll 管理大量连接，不会让一个
慢客户端阻塞其他客户端。请求解析器支持 Header、`Content-Length` 和 Body，
服务器可以处理 GET，以及在内存中接收简单的 `text/plain` POST 内容。

## 当前结构

```text
.
├── .gitignore      # 不让编译产物和运行数据进入版本库
├── CMakeLists.txt  # 告诉 CMake 如何编译项目
├── README.md       # 当前进度和学习任务
├── guide.md        # 项目整体规划
├── include/
│   ├── concurrency/
│   │   └── thread_pool.h   # 有界线程池接口
│   ├── http/
│   │   ├── http_request.h  # HTTP 请求类型和解析接口
│   │   ├── http_response.h # HTTP 响应类型和序列化接口
│   │   └── router.h        # 路由选择接口
│   └── server/
│       ├── client_handler.h # 旧阻塞模型的单客户端处理接口（学习对照）
│       ├── epoll_server.h   # epoll 服务器配置和启动接口
│       └── http_server.h    # Socket 基础操作接口
├── src/
│   ├── client_handler.cpp  # 旧阻塞式客户端处理流程（学习对照）
│   ├── epoll_server.cpp    # 非阻塞收发、连接状态和 epoll 事件循环
│   ├── http_request.cpp    # 简单 HTTP 请求行解析
│   ├── http_response.cpp   # HTTP 响应序列化
│   ├── http_server.cpp     # Socket 层系统调用及错误处理
│   ├── router.cpp          # method/path 匹配和响应生成
│   ├── thread_pool.cpp     # 旧线程池实现，保留测试和模型对照
│   └── main.cpp            # 设置参数并启动 epoll 服务器
└── tests/
    ├── http_test.cpp        # Request、Router 和 Response 测试
    └── thread_pool_test.cpp # 任务执行、队列容量和停止测试
```

## 构建

```bash
cmake -S . -B build
cmake --build build
```

运行测试：

```bash
ctest --test-dir build --output-on-failure
```

如果希望查看每个检查项的输出，可以直接运行：

```bash
./build/http_tests
./build/thread_pool_tests
```

运行程序：

```bash
./build/http_server
```

## 运行和验证

启动服务器：

```bash
./build/http_server
```

在另一个终端使用 netcat 连接：

```bash
nc 127.0.0.1 9000
```

可以使用 netcat 发送一条完整的 HTTP 请求：

```bash
printf 'GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n' |
    nc -N 127.0.0.1 9000
```

将 `main.cpp` 中的 `verbose_logging` 设为 `true` 后，服务器终端会输出：

```text
Client connected, fd = 5
Parsed request, fd = 5, method = GET, path = /hello, body = 0 bytes
Response sent; closing client fd = 5
```

当前采用单线程 epoll 边缘触发（ET）模型。监听 Socket 和客户端 Socket 都是
非阻塞的；`recv()` 和 `send()` 会一直处理到返回 `EAGAIN`，然后把执行权交还
给事件循环。每个连接只处理一个请求，响应头会发送 `Connection: close`。

发送文本 POST：

```bash
curl -v \
    -H 'Content-Type: text/plain' \
    --data-binary 'hello upload' \
    http://127.0.0.1:9000/upload
```

服务器会返回：

```text
Received text:
hello upload
```

当前 `/upload` 只在内存中接收并回显文本，不会写入磁盘。Header 限制为
16 KiB，Body 限制为 64 KiB。

事件循环记录每个连接最后一次成功收发数据的时间。接收阶段空闲 30 秒时返回
`408 Request Timeout`；发送阶段空闲 30 秒时直接关闭连接。超时检查每秒最多
执行一次，避免高并发时为每一批事件重复扫描连接表。

## 当前模块关系

阅读各文件之间的关系：

1. `http_server.h/.cpp` 创建 Socket，并提供非阻塞模式设置等基础能力。
2. `http_request.h/.cpp` 解析请求行、Header、`Content-Length` 和 Body。
3. `router.h/.cpp` 根据 method 和 path 选择响应。
4. `http_response.h/.cpp` 把响应对象序列化为 HTTP 文本。
5. `epoll_server.h/.cpp` 保存每个连接的接收/发送状态，并调度非阻塞 IO。
6. `main.cpp` 配置端口、backlog、事件数组和空闲超时，然后启动事件循环。
7. `client_handler` 和 `thread_pool` 保留作为旧并发模型的学习对照，不参与
   `http_server` 可执行程序的构建。

当前接口包括：

- `create_listening_socket()`：创建、绑定并监听。
- `accept_client()`：接受一个客户端连接。
- `set_socket_nonblocking()`：使用 `fcntl()` 设置 `O_NONBLOCK`。
- `set_socket_timeouts()`：设置客户端 Socket 的收发超时。
- `receive_data()`：返回数据、对端关闭、超时或普通错误状态。
- `send_all()`：处理一次 `send()` 没有发完的情况。
- `close_socket()`：关闭 Socket。

需要能够解释：

- 为什么监听 Socket 和客户端 Socket 是两个不同的文件描述符。
- 为什么 `recv()` 返回值需要区分大于零、等于零和小于零。
- 为什么不能直接使用 `printf("%s", buffer)` 输出网络数据。
- 为什么 `send_all()` 需要循环调用 `send()`。
- 为什么 ET 模式下必须一直 `accept()`、`recv()` 或 `send()` 到 `EAGAIN`。
- 为什么连接状态中需要分别保存请求缓冲区、响应缓冲区和发送偏移量。

下一步可以补充 SIGINT/SIGTERM 优雅停机、自动化并发压力测试和 HTTP
Keep-Alive。如果路由中出现耗时磁盘或计算任务，再考虑把 epoll 与工作线程池
组合，而不是在事件循环中执行耗时操作。

## 学习约定

- 每次只实现一个小目标。
- 代码由学习者自己编写。
- 遇到问题时先分析错误信息，再获取提示。
- 除非明确要求完整实现，否则不直接生成完成代码。
