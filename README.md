# Personal Cloud Server

这是一个用于学习 Linux 网络编程和 HTTP 的 C++ 项目。

当前进度：已经完成阻塞式 Socket 层，包括 `socket()`、`setsockopt()`、
`bind()`、`listen()`、`accept()`、`recv()`、`send()` 和 `close()`。
程序会依次处理客户端，把收到的数据原样发送回去。

## 当前结构

```text
.
├── .gitignore      # 不让编译产物和运行数据进入版本库
├── CMakeLists.txt  # 告诉 CMake 如何编译项目
├── README.md       # 当前进度和学习任务
├── guide.md        # 项目整体规划
├── include/
│   └── server/
│       └── http_server.h  # 网络模块对外接口
└── src/
    ├── http_server.cpp    # Socket 层系统调用及错误处理
    └── main.cpp           # 程序入口和调用流程
```

## 构建

当前环境还没有安装 CMake，可以先直接使用 g++：

```bash
mkdir -p build
g++ -std=c++20 -Wall -Wextra -Wpedantic \
    -Iinclude src/main.cpp src/http_server.cpp \
    -o build/http_server
```

运行程序：

```bash
./build/http_server
```

安装 CMake 后，也可以使用标准构建方式：

```bash
cmake -S . -B build
cmake --build build
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

输入任意内容并回车，服务器会把收到的数据原样发送回来。服务器会持续读取当前
连接，直到客户端断开。当前采用阻塞式串行模型：一个客户端断开后，服务器才会
接受下一个客户端。

## 当前 Socket 层接口

阅读三个文件之间的关系：

1. `http_server.h` 声明其他代码可以调用什么。
2. `http_server.cpp` 实现 Socket 系统调用和错误处理。
3. `main.cpp` 组织“接受连接、收取数据、回送数据”的流程。

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

下一步是实现 HTTP 请求解析。在此之前，Socket 层只负责字节传输，不理解
GET、路径、Header 等 HTTP 概念。

## 学习约定

- 每次只实现一个小目标。
- 代码由学习者自己编写。
- 遇到问题时先分析错误信息，再获取提示。
- 除非明确要求完整实现，否则不直接生成完成代码。
