# Personal Cloud Server

这是一个用于学习 Linux 网络编程和 HTTP 的 C++ 项目。

当前进度：已经完成阻塞式 Socket 层、HTTP Request/Response、路由、单元测试和
固定线程池。服务器使用 4 个工作线程，最多保存 128 个等待任务。
请求解析器支持 Header、`Content-Length` 和 Body，服务器可以处理 GET，
以及在内存中接收简单的 `text/plain` POST 内容。

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
│       ├── client_handler.h # 单个客户端请求处理接口
│       └── http_server.h    # Socket 模块对外接口
├── src/
│   ├── client_handler.cpp  # 接收、解析并响应客户端
│   ├── http_request.cpp    # 简单 HTTP 请求行解析
│   ├── http_response.cpp   # HTTP 响应序列化
│   ├── http_server.cpp     # Socket 层系统调用及错误处理
│   ├── router.cpp          # method/path 匹配和响应生成
│   ├── thread_pool.cpp     # 工作线程和任务队列
│   └── main.cpp            # 接受连接并向线程池提交任务
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

服务器终端会输出：

```text
Parsed HTTP request:
  method  = GET
  path    = /hello
  version = HTTP/1.1
```

当前采用固定线程池模型：4 个工作线程使用阻塞式 Socket 处理请求，最多有
128 个任务在队列中等待。每个连接只处理一个请求，响应头会发送
`Connection: close`。队列已满时，主线程直接关闭新客户端连接。

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

每个客户端 Socket 当前设置了 5 秒收发超时。请求在 5 秒内没有新数据时，
`receive_data()` 返回 `ReceiveStatus::timeout`，ClientHandler 尝试返回
`408 Request Timeout` 并关闭连接。

## 当前模块关系

阅读各文件之间的关系：

1. `http_server.h/.cpp` 提供 Socket 收发能力，不理解 HTTP。
2. `http_request.h/.cpp` 解析请求行、Header、`Content-Length` 和 Body。
3. `router.h/.cpp` 根据 method 和 path 选择响应。
4. `http_response.h/.cpp` 把响应对象序列化为 HTTP 文本。
5. `client_handler.h/.cpp` 组合收发、解析、路由和响应流程。
6. `thread_pool.h/.cpp` 管理 4 个工作线程和有界任务队列。
7. `main.cpp` 负责监听、接受连接和提交任务；工作任务负责关闭 Socket。

当前接口包括：

- `create_listening_socket()`：创建、绑定并监听。
- `accept_client()`：接受一个客户端连接。
- `set_socket_timeouts()`：设置客户端 Socket 的收发超时。
- `receive_data()`：返回数据、对端关闭、超时或普通错误状态。
- `send_all()`：处理一次 `send()` 没有发完的情况。
- `close_socket()`：关闭 Socket。

需要能够解释：

- 为什么监听 Socket 和客户端 Socket 是两个不同的文件描述符。
- 为什么 `recv()` 返回值需要区分大于零、等于零和小于零。
- 为什么不能直接使用 `printf("%s", buffer)` 输出网络数据。
- 为什么 `send_all()` 需要循环调用 `send()`。

下一步可以先补充优雅停机和并发压力测试，再将 Socket 改为非阻塞模式并引入
epoll。

## 学习约定

- 每次只实现一个小目标。
- 代码由学习者自己编写。
- 遇到问题时先分析错误信息，再获取提示。
- 除非明确要求完整实现，否则不直接生成完成代码。
