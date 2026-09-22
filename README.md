# Personal Cloud Server

这是一个用于学习 Linux 网络编程和 HTTP 的 C++ 项目。

当前进度：已经完成非阻塞 Socket、epoll 事件循环、HTTP Request/Response、
路由、超时清理、优雅停机、SQLite 持久化、异步任务桥、用户/Session 认证、
流式文件上传/下载、文件管理、亮色 Web 控制台、请求 ID、运维工具和端到端测试。
服务器使用单线程 epoll 管理大量连接，不会让一个慢客户端阻塞其他客户端。
请求解析器支持
Header、`Content-Length`
和 Body，并严格检查 HTTP/1.x 请求行、Host、Header 控制字符和消息长度。
服务器可以处理 GET、认证/文件 JSON API、普通小 Body 请求，以及不进入普通
Body 缓冲的大文件上传和下载。

## 当前结构

```text
.
├── .gitignore      # 不让编译产物和运行数据进入版本库
├── CMakeLists.txt  # 告诉 CMake 如何编译项目
├── deploy/         # Nginx HTTPS 与 systemd 部署示例
├── BACKEND_PLAN.md # 个人云后端的分阶段实施计划
├── README.md       # 当前进度和学习任务
├── guide.md        # 项目整体规划
├── include/
│   ├── admin/       # 完整备份归档与恢复接口
│   ├── app/         # 后端配置、认证/文件服务和应用层请求入口
│   ├── concurrency/
│   │   └── thread_pool.h   # 有界线程池接口
│   ├── http/
│   │   ├── http_request.h  # HTTP 请求类型和解析接口
│   │   ├── http_response.h # HTTP 响应类型和序列化接口
│   │   └── router.h        # 路由选择接口
│   ├── server/
│   │   ├── client_handler.h # 旧阻塞模型的单客户端处理接口（学习对照）
│   │   ├── epoll_server.h   # epoll 服务器配置和启动接口
│   │   ├── streaming.h      # worker 与 epoll 间的流式文件接口
│   │   └── http_server.h    # Socket 基础操作接口
│   └── storage/
│       └── database.h       # SQLite 生命周期和迁移接口
├── src/
│   ├── backup_archive.cpp # 流式完整备份、校验和恢复
│   ├── backend_application.cpp # 应用路由和 JSON 状态接口
│   ├── backend_config.cpp   # 命令行配置解析
│   ├── auth_service.cpp     # 注册、登录、Session 和限速
│   ├── file_service.cpp     # 文件元数据、上传、下载、重命名和删除
│   ├── client_handler.cpp  # 旧阻塞式客户端处理流程（学习对照）
│   ├── database.cpp        # SQLite 配置和版本化迁移
│   ├── epoll_server.cpp    # 非阻塞收发、连接状态和 epoll 事件循环
│   ├── http_request.cpp    # HTTP/1.x 请求解析和消息边界检查
│   ├── http_response.cpp   # HTTP 响应序列化
│   ├── http_server.cpp     # Socket 层系统调用及错误处理
│   ├── router.cpp          # method/path 匹配和响应生成
│   ├── thread_pool.cpp     # 异步应用任务使用的有界工作线程池
│   ├── cloud_admin.cpp     # 备份、清理和存储一致性检查工具
│   └── main.cpp            # 设置参数并启动 epoll 服务器
├── tools/
│   └── benchmark.py        # 可重复的并发 HTTP 压测
└── tests/
    ├── backend_test.cpp     # 配置、迁移、持久化和应用接口测试
    ├── http_test.cpp        # Request、Router 和 Response 测试
    ├── server_integration_test.cpp # 真实 Socket/epoll 端到端测试
    └── thread_pool_test.cpp # 任务执行、队列容量和停止测试
```

## 构建

Ubuntu/Debian 需要先安装 SQLite、libsodium 和 nlohmann JSON 开发包：

```bash
sudo apt-get install libsqlite3-dev libsodium-dev nlohmann-json3-dev pkg-config
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
    --bind-address 127.0.0.1 \
    --port 9000 \
    --database data/personal_cloud.db \
    --storage-root data/files \
    --max-connections 10000 \
    --idle-timeout 30 \
    --worker-count 4 \
    --task-queue-size 256 \
    --max-file-size 1073741824 \
    --user-quota 10737418240 \
    --max-concurrent-uploads 4 \
    --stream-buffer-size 65536 \
    --allow-registration
```

使用 `./build/http_server --help` 可以查看全部选项。非法配置会在创建监听
Socket 前被拒绝。首次启动时会自动创建数据库目录、文件存储目录和 Schema；
当前第一版迁移包含 `users`、`sessions`、`files` 及必要索引。存储根下会创建
`objects/`、`tmp/` 和 `trash/`。SQLite 会启用外键、WAL 和 5 秒 busy timeout。

可能访问数据库或文件系统的请求通过有界应用工作线程池执行。等待队列满时服务
返回 `503 Service Unavailable`，不会继续无界积累任务。`/health` 等纯内存快速
路由仍由 epoll 线程直接处理，因此某个 worker 阻塞不会阻塞新连接的健康检查。

## 运行和验证

启动服务器：

```bash
./build/http_server
```

页面文件目前按相对路径从 `public/` 读取，因此应从项目根目录运行上述命令。访问
`http://127.0.0.1:9000/app` 可打开亮色控制台，完成注册、登录和文件管理；页面
支持拖放、多文件顺序上传、实时进度、名称搜索和窄屏布局。

后端状态接口：

```bash
curl http://127.0.0.1:9000/api/status
```

返回：

```json
{"status":"ok","database":"ok","schema_version":1}
```

`/api/status` 已接入异步任务桥，并在每次请求时使用独立 SQLite 连接执行实时健康
检查。worker 通过完成队列和 Linux `eventfd` 把响应交还给 epoll 线程；worker
不直接操作 Socket。每个连接使用独立的 64 位连接 ID，避免 fd 复用后把旧任务响应
发送给新客户端。

## 用户认证 API

注册默认关闭，避免公网服务被任意创建账号。需要创建账号时，以
`--allow-registration` 启动服务；创建完成后移除该参数并重启。注册请求：

```bash
curl -i \
    -H 'Content-Type: application/json' \
    --data-binary '{"username":"alice","password":"example-password"}' \
    http://127.0.0.1:9000/api/auth/register
```

登录并把 Session Cookie 保存到临时 Cookie Jar：

```bash
curl -i -c /tmp/personal-cloud-cookie.txt \
    -H 'Content-Type: application/json' \
    --data-binary '{"username":"alice","password":"example-password"}' \
    http://127.0.0.1:9000/api/auth/login
```

查询当前用户、退出并验证 Session 已失效：

```bash
curl -i -b /tmp/personal-cloud-cookie.txt \
    http://127.0.0.1:9000/api/auth/me

curl -i -b /tmp/personal-cloud-cookie.txt -X POST \
    http://127.0.0.1:9000/api/auth/logout

curl -i -b /tmp/personal-cloud-cookie.txt \
    http://127.0.0.1:9000/api/auth/me
```

用户名只允许 3–64 个 ASCII 字母、数字、点、下划线和连字符；密码长度为
8–1024 字节。密码通过 libsodium Argon2id 存储，明文不会写入数据库。Session
Token 使用 32 字节安全随机数，只通过 `Set-Cookie` 发给客户端；数据库仅保存其
32 字节 BLAKE2b 哈希。Cookie 带 `HttpOnly`、`SameSite=Strict`、`Path=/` 和
7 天 `Max-Age`。

本地服务器使用明文 HTTP，因此应用不直接添加 `Secure`。生产环境必须使用 HTTPS
前置代理，并为 `pc_session` Cookie 添加 `Secure`；例如支持
`proxy_cookie_flags pc_session secure` 的 Nginx 配置。登录对不存在用户和密码错误
返回相同响应，并对连续失败进行有界内存限速。

## 文件 API

文件接口使用登录得到的 `pc_session` Cookie。上传 Body 是文件原始字节，展示名称
通过 `X-File-Name` 传入，`Content-Type` 会保存为文件 MIME。浏览器等客户端若要
传输 UTF-8 或空格文件名，可同时发送 `X-File-Name-Encoding: percent`，此时名称
必须使用严格百分号编码：

```bash
curl -i -b /tmp/personal-cloud-cookie.txt \
    -H 'X-File-Name: example.pdf' \
    -H 'Content-Type: application/pdf' \
    --data-binary @example.pdf \
    http://127.0.0.1:9000/api/files
```

响应中的 `id` 是对外文件 ID。列出文件和读取单条元数据：

```bash
curl -b /tmp/personal-cloud-cookie.txt \
    http://127.0.0.1:9000/api/files

curl -b /tmp/personal-cloud-cookie.txt \
    http://127.0.0.1:9000/api/files/FILE_ID
```

流式下载、修改展示名称和删除：

```bash
curl -b /tmp/personal-cloud-cookie.txt -o downloaded.bin \
    http://127.0.0.1:9000/api/files/FILE_ID/content

curl -i -b /tmp/personal-cloud-cookie.txt \
    -X PATCH -H 'Content-Type: application/json' \
    --data-binary '{"name":"renamed.pdf"}' \
    http://127.0.0.1:9000/api/files/FILE_ID

curl -i -b /tmp/personal-cloud-cookie.txt -X DELETE \
    http://127.0.0.1:9000/api/files/FILE_ID
```

默认单文件上限为 1 GiB、单用户容量为 10 GiB、全局同时上传数为 4，上传和下载
块为 64 KiB。上传块写盘期间连接会暂停读取，下载也只在上一块完整发送后读取
下一块，因此慢磁盘或慢客户端不会造成文件内容在内存中无界累积。上传完成前只写
`tmp/`，成功后原子发布到 `objects/` 并提交 SQLite 元数据；断连和失败会清理临时
文件。服务端生成的存储键不会暴露给 API，所有文件查询同时校验当前 Session 和
所有者。

`GET /api/files` 还会返回 `usage`，包括已用字节数、总容量、文件总数、本次返回
数和列表是否截断。所有网络响应都有 `X-Request-ID`；协议层与 API 错误使用统一
格式，前端会显示缩短后的请求 ID，便于定位日志：

```json
{"error":{"code":"bad_request","message":"Bad request"}}
```

使用 `--verbose` 启动时，请求接收与完成日志为单行 JSON，包含时间戳、请求 ID、
fd、方法、路径和状态码，不包含 Cookie、请求 Body 或密码。

## 运维工具

构建后会生成 `build/cloud_admin`：

```bash
./build/cloud_admin backup \
    --database data/personal_cloud.db \
    --storage-root data/files \
    --output backups/cloud.pcbackup

# 恢复目标必须不存在；恢复期间不要启动服务器
./build/cloud_admin restore \
    --input backups/cloud.pcbackup \
    --database restored/personal_cloud.db \
    --storage-root restored/files

./build/cloud_admin check \
    --database data/personal_cloud.db --storage-root data/files

# 默认只预演；确认输出后才显式执行
./build/cloud_admin cleanup --storage-root data/files --older-than 86400
./build/cloud_admin cleanup --storage-root data/files \
    --older-than 86400 --apply
```

备份是包含 SQLite 一致性快照和全部已提交对象的单文件流式归档，默认拒绝覆盖已有
目标。创建归档时会核对数据库完整性、对象大小和 SHA-256；如果并发删除等操作导致
快照与文件不一致，命令会失败且不会发布残缺归档。恢复同样校验数据库、归档和对象
哈希，并拒绝覆盖已有数据库或存储目录。恢复前应停止服务器，完成后先执行 `check`
再切换服务路径。`cleanup` 只扫描 `tmp/` 和 `trash/` 下超过指定秒数的普通文件，
从不删除 `objects/`。

## 并发压测

脚本只依赖 Python 标准库：

```bash
python3 tools/benchmark.py http://127.0.0.1:9000 \
    --path /health --requests 5000 --concurrency 64 \
    --server-pid SERVER_PID --output benchmark.json
```

JSON 报告包含吞吐、平均延迟、p50/p95/p99/max、HTTP 状态/失败分布，以及可选的
Linux 进程 RSS 和打开 fd 前后值。对比优化前后结果时应固定硬件、构建类型、请求
数和并发数，并先进行一轮预热。

## 生产部署

`deploy/nginx.conf.example` 提供 HTTPS 终止、1 GiB Body 上限、关闭请求/响应代理
缓冲和为 Session Cookie 添加 `Secure` 的示例；请替换域名与证书路径。
`deploy/personal-cloud.service` 使用 `DynamicUser`、`StateDirectory` 和 systemd
沙箱限制，假设二进制及 `public/` 安装在 `/opt/personal-cloud`。

```bash
sudo cp deploy/personal-cloud.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now personal-cloud
sudo journalctl -u personal-cloud -f
```

Nginx 配置需按实际域名修改，并经 `nginx -t` 验证后启用。程序默认只监听
`127.0.0.1`；只有明确需要直接接受其他主机连接时，才传入
`--bind-address 0.0.0.0`。生产主机仍应通过防火墙只开放 Nginx 的 80/443 端口。

在另一个终端使用 netcat 连接：

```bash
nc 127.0.0.1 9000
```

可以使用 netcat 发送一条完整的 HTTP 请求：

```bash
printf 'GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n' |
    nc -N 127.0.0.1 9000
```

传入 `--verbose` 后，服务器终端会输出结构化请求日志：

```text
{"timestamp_ms":0,"level":"info","event":"request_completed","request_id":"...","fd":5,"method":"GET","path":"/hello","status":200}
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

事件循环记录每个连接最后一次活动时间。接收阶段空闲 30 秒时返回
`408 Request Timeout`；应用处理或发送阶段超时则直接关闭连接，迟到的 worker
结果会被丢弃。超时检查每秒最多执行一次，避免高并发时为每一批事件重复扫描
连接表。

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
5. `epoll_server.h/.cpp` 保存普通请求以及流式上传/下载的连接状态，调度非阻塞
   IO，处理信号停机和描述符耗尽恢复，并通过有界线程池、完成队列和 `eventfd`
   调度可能阻塞的数据库及文件任务。
6. `backend_config` 解析启动配置；`database` 初始化 SQLite、执行迁移并提供全部
   绑定参数查询；`auth_service` 负责 Argon2id、Session 和失败限速；
   `file_service` 负责文件所有权、配额、元数据和磁盘一致性；
   `backend_application` 把这些操作封装为普通或流式 worker 任务。
7. `main.cpp` 组装配置、数据库、应用处理器和事件循环。
8. `thread_pool` 执行异步应用任务；`client_handler` 只作为旧阻塞模型的学习
   对照，不参与 `http_server` 可执行程序的构建。

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
- 为什么 worker 只能通过完成队列和 `eventfd` 通知 epoll，不能直接操作 Socket。
- 为什么任务结果除 fd 外还必须携带连接 ID，防止 fd 复用造成串响应。
- 为什么 `EMFILE` 时需要保留 fd、暂停监听和显式重新启用 one-shot 事件。
- 为什么用 `signalfd` 可以让信号处理保持在普通同步代码中。

后端顺序和各阶段验收标准见 `BACKEND_PLAN.md`。阶段 1–6 已完成，当前具备用户与
文件最小闭环、亮色控制台、请求关联日志、备份/检查/清理工具、压测脚本和生产
部署基线。下一步可从 cursor 分页、Range 下载、浏览器自动化测试和备份恢复演练
中选择高价值项目继续推进。

## 学习约定

- 每次只实现一个小目标。
- 代码由学习者自己编写。
- 遇到问题时先分析错误信息，再获取提示。
- 除非明确要求完整实现，否则不直接生成完成代码。
