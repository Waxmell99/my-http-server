# Personal Cloud Server

这是一个用于学习 Linux 网络编程和 HTTP 的 C++ 项目。

当前进度：已经完成阻塞式 Socket 层、简单 HTTP Request 解析，以及
`HttpResponse` 的生成、序列化和发送。服务器可以响应 `/hello`、`/health`，
并为未知路径、非 GET 方法和错误请求返回对应状态码。

## 当前结构

```text
.
├── .gitignore      # 不让编译产物和运行数据进入版本库
├── CMakeLists.txt  # 告诉 CMake 如何编译项目
├── README.md       # 当前进度和学习任务
├── guide.md        # 项目整体规划
├── include/
│   ├── http/
│   │   ├── http_request.h  # HTTP 请求类型和解析接口
│   │   └── http_response.h # HTTP 响应类型和序列化接口
│   └── server/
│       └── http_server.h  # Socket 模块对外接口
├── src/
    ├── http_request.cpp    # 简单 HTTP 请求行解析
    ├── http_response.cpp   # GET 处理和响应序列化
    ├── http_server.cpp     # Socket 层系统调用及错误处理
    └── main.cpp            # 接收、累计并解析请求
└── tests/
    └── http_test.cpp        # Request 和 Response 单元测试
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
nc 127.0.0.1 8080
```

可以使用 netcat 发送一条完整的 HTTP 请求：

```bash
printf 'GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n' |
    nc -N 127.0.0.1 8080
```

服务器终端会输出：

```text
Parsed HTTP request:
  method  = GET
  path    = /hello
  version = HTTP/1.1
```

当前采用阻塞式串行模型，并且每个连接只处理一个请求，响应头会明确发送
`Connection: close`。

## 当前模块关系

阅读各文件之间的关系：

1. `http_server.h/.cpp` 提供 Socket 收发能力，不理解 HTTP。
2. `http_request.h/.cpp` 接收一段完整文本，解析 HTTP 请求行。
3. `main.cpp` 累计 Socket 收到的字节，发现 Header 完整后调用解析器。

当前接口包括：

- `create_listening_socket()`：创建、绑定并监听。
- `accept_client()`：接受一个客户端连接。
- `receive_data()`：接收一次数据。
- `send_all()`：处理一次 `send()` 没有发完的情况。
- `close_socket()`：关闭 Socket。

需要能够解释：

- 为什么监听 Socket 和客户端 Socket 是两个不同的文件描述符。
- 为什么 `recv()` 返回值需要区分大于零、等于零和小于零。
- 为什么不能直接使用 `printf("%s", buffer)` 输出网络数据。
- 为什么 `send_all()` 需要循环调用 `send()`。

下一步可以将 `handle_http_request()` 中的路径判断拆成独立 Router，或者先为
Request、Response 和路由行为补充单元测试。

## 学习约定

- 每次只实现一个小目标。
- 代码由学习者自己编写。
- 遇到问题时先分析错误信息，再获取提示。
- 除非明确要求完整实现，否则不直接生成完成代码。
