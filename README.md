# Personal Cloud Server

这是一个用于学习 Linux 网络编程和 HTTP 的 C++ 项目。

当前进度：已经完成非阻塞 Socket、epoll 事件循环、HTTP Request/Response、
路由、超时清理、优雅停机、SQLite 基础持久化和端到端测试。服务器使用单线程
epoll 管理大量连接，不会让一个慢客户端阻塞其他客户端。请求解析器支持
Header、`Content-Length`
和 Body，并严格检查 HTTP/1.x 请求行、Host、Header 控制字符和消息长度。
服务器可以处理 GET，以及在内存中接收简单的 `text/plain` POST 内容。

## 当前结构

```text
.
├── .gitignore      # 不让编译产物和运行数据进入版本库
├── CMakeLists.txt  # 告诉 CMake 如何编译项目
├── BACKEND_PLAN.md # 个人云后端的分阶段实施计划
├── README.md       # 当前进度和学习任务
├── guide.md        # 项目整体规划
├── include/
│   ├── app/         # 后端配置和应用层请求入口
│   ├── concurrency/
│   │   └── thread_pool.h   # 有界线程池接口
│   ├── http/
│   │   ├── http_request.h  # HTTP 请求类型和解析接口
│   │   ├── http_response.h # HTTP 响应类型和序列化接口
│   │   └── router.h        # 路由选择接口
│   ├── server/
│   │   ├── client_handler.h # 旧阻塞模型的单客户端处理接口（学习对照）
│   │   ├── epoll_server.h   # epoll 服务器配置和启动接口
│   │   └── http_server.h    # Socket 基础操作接口
│   └── storage/
│       └── database.h       # SQLite 生命周期和迁移接口
├── src/
│   ├── backend_application.cpp # 应用路由和 JSON 状态接口
│   ├── backend_config.cpp   # 命令行配置解析
│   ├── client_handler.cpp  # 旧阻塞式客户端处理流程（学习对照）
│   ├── database.cpp        # SQLite 配置和版本化迁移
│   ├── epoll_server.cpp    # 非阻塞收发、连接状态和 epoll 事件循环
│   ├── http_request.cpp    # HTTP/1.x 请求解析和消息边界检查
│   ├── http_response.cpp   # HTTP 响应序列化
│   ├── http_server.cpp     # Socket 层系统调用及错误处理
│   ├── router.cpp          # method/path 匹配和响应生成
│   ├── thread_pool.cpp     # 旧线程池实现，保留测试和模型对照
│   └── main.cpp            # 设置参数并启动 epoll 服务器
└── tests/
    ├── backend_test.cpp     # 配置、迁移、持久化和应用接口测试
    ├── http_test.cpp        # Request、Router 和 Response 测试
    ├── server_integration_test.cpp # 真实 Socket/epoll 端到端测试
    └── thread_pool_test.cpp # 任务执行、队列容量和停止测试
```

## 构建

Ubuntu/Debian 需要先安装 SQLite 开发包：

```bash
sudo apt-get install libsqlite3-dev
```

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
./build/backend_tests
./build/server_integration_tests
./build/thread_pool_tests
```

运行程序：

```bash
./build/http_server
```

常用启动参数：

```bash
./build/http_server \
    --port 9000 \
    --database data/personal_cloud.db \
    --storage-root data/files \
    --max-connections 10000 \
    --idle-timeout 30
```

使用 `./build/http_server --help` 可以查看全部选项。非法配置会在创建监听
Socket 前被拒绝。首次启动时会自动创建数据库目录、文件存储目录和 Schema；
当前第一版迁移包含 `users`、`sessions`、`files` 及必要索引。SQLite 会启用
外键、WAL 和 5 秒 busy timeout。

## 运行和验证

启动服务器：

```bash
./build/http_server
```

页面文件目前按相对路径从 `public/` 读取，因此应从项目根目录运行上述命令。

后端状态接口：

```bash
curl http://127.0.0.1:9000/api/status
```

返回：

```json
{"status":"ok","database":"ok","schema_version":1}
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
非阻塞且带 `close-on-exec` 标志；`recv()` 和 `send()` 会一直处理到返回
`EAGAIN`，然后把执行权交还给事件循环。监听 Socket 还使用 `EPOLLONESHOT`，
每次把 accept 队列排空后显式重新启用，避免文件描述符耗尽时丢失 ET 通知。
每个连接只处理一个请求，响应头会发送 `Connection: close`。

使用 Ctrl-C 或发送 SIGTERM 时，信号会通过 Linux `signalfd` 进入 epoll 事件
循环。服务器正常退出并关闭监听 Socket、epoll fd、信号 fd 和所有客户端
Socket，而不是直接被默认信号动作终止。

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
16 KiB，Body 限制为 64 KiB。媒体类型名称不区分大小写，并允许参数前的可选
空白；例如 `Text/Plain ; charset=utf-8` 可以正常处理。

当前支持 HTTP/1.0 和 HTTP/1.1。HTTP/1.1 必须提供非空 `Host`；不合法请求
返回 `400`，不支持的版本返回 `505`，过大的 Body/Header 分别返回 `413` 和
`431`。服务器还没有实现 `100 Continue` 中间响应，因此带 `Expect` 的
HTTP/1.1 请求会立即返回 `417`，避免客户端和服务器互相等待。已知路由的
`405` 响应会带正确的 `Allow` Header，查询字符串不会影响路由路径匹配。

事件循环记录每个连接最后一次成功收发数据的时间。接收阶段空闲 30 秒时返回
`408 Request Timeout`；发送阶段空闲 30 秒时直接关闭连接。超时检查每秒最多
执行一次，避免高并发时为每一批事件重复扫描连接表。

事件循环保留一个指向 `/dev/null` 的文件描述符。当进程遇到 `EMFILE` 或系统
遇到 `ENFILE` 时，会用该保留槽接受并关闭一个排队连接、暂停监听，然后在
现有客户端关闭或一秒重试点重新启用监听，避免 ET 监听 Socket 永久卡住。
此外，`maximum_connections` 默认把活动连接限制为 10000；达到限制后仍会排空
accept 队列，但会立即关闭超出的连接，防止连接状态无界增长。

## 当前模块关系

阅读各文件之间的关系：

1. `http_server.h/.cpp` 原子创建 nonblocking/close-on-exec Socket，并提供旧
   阻塞模型仍会使用的基础收发能力。
2. `http_request.h/.cpp` 解析请求行、Header、`Content-Length` 和 Body。
3. `router.h/.cpp` 根据 method 和 path 选择响应。
4. `http_response.h/.cpp` 把响应对象序列化为 HTTP 文本。
5. `epoll_server.h/.cpp` 保存每个连接的接收/发送状态，调度非阻塞 IO，处理
   信号停机和描述符耗尽恢复，并调用注入的应用请求处理器。
6. `backend_config` 解析启动配置；`database` 初始化 SQLite 并执行迁移；
   `backend_application` 提供 `/api/status` 并把其他请求交给原有路由。
7. `main.cpp` 组装配置、数据库、应用处理器和事件循环。
8. `client_handler` 和 `thread_pool` 保留作为旧并发模型的学习对照，不参与
   `http_server` 可执行程序的构建。

当前接口包括：

- `create_listening_socket()`：创建、绑定并监听，原子设置 nonblocking 和
  close-on-exec。
- `accept_client()`：通过 `accept4()` 接受客户端并原子设置相同标志。
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
- 为什么 `EMFILE` 时需要保留 fd、暂停监听和显式重新启用 one-shot 事件。
- 为什么用 `signalfd` 可以让信号处理保持在普通同步代码中。

后端后续顺序和各阶段验收标准见 `BACKEND_PLAN.md`。下一步是用有界线程池、
完成队列和 `eventfd` 把数据库/文件任务移出 epoll 线程，然后再实现注册、登录
和 Session；不应直接在当前事件循环里执行密码哈希或耗时数据库操作。

## 学习约定

- 每次只实现一个小目标。
- 代码由学习者自己编写。
- 遇到问题时先分析错误信息，再获取提示。
- 除非明确要求完整实现，否则不直接生成完成代码。
