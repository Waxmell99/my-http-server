cmake_minimum_required(VERSION 3.20)

# 定义项目名称、版本和使用的语言。
project(personal_cloud_server VERSION 0.1.0 LANGUAGES CXX)

# 生成 compile_commands.json，方便 clangd 等工具理解项目。
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 定义最终生成的可执行程序，以及参与编译的源文件。
add_executable(http_server
    src/main.cpp
    src/http_server.cpp
)

# 只对 http_server 目标启用 C++20。
target_compile_features(http_server PRIVATE cxx_std_20)

# 允许源码使用 #include "server/http_server.h"。
target_include_directories(http_server
    PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
)

# GCC 和 Clang 使用的常见警告选项。
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(http_server
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
    )
endif()
