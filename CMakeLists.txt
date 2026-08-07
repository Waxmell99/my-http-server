cmake_minimum_required(VERSION 3.20)

# 定义项目名称、版本和使用的语言。
project(personal_cloud_server VERSION 0.1.0 LANGUAGES CXX)

find_package(Threads REQUIRED)

# 生成 compile_commands.json，方便 clangd 等工具理解项目。
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 定义最终生成的可执行程序，以及参与编译的源文件。
add_executable(http_server
    src/main.cpp
    src/epoll_server.cpp
    src/http_server.cpp
    src/http_request.cpp
    src/http_response.cpp
    src/router.cpp
)

# 只对 http_server 目标启用 C++20。
target_compile_features(http_server PRIVATE cxx_std_20)

# 将 include/ 设为头文件根目录，例如可以包含 server/ 和 http/ 下的头文件。
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

# 启用 CTest，并在 BUILD_TESTING=ON 时构建 HTTP 单元测试。
include(CTest)

if(BUILD_TESTING)
    add_executable(http_tests
        tests/http_test.cpp
        src/http_request.cpp
        src/http_response.cpp
        src/router.cpp
    )

    target_compile_features(http_tests PRIVATE cxx_std_20)
    target_include_directories(http_tests
        PRIVATE
            "${PROJECT_SOURCE_DIR}/include"
    )

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(http_tests
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
        )
    endif()

    add_test(NAME http_tests COMMAND http_tests)

    add_executable(thread_pool_tests
        tests/thread_pool_test.cpp
        src/thread_pool.cpp
    )

    target_compile_features(thread_pool_tests PRIVATE cxx_std_20)
    target_include_directories(thread_pool_tests
        PRIVATE
            "${PROJECT_SOURCE_DIR}/include"
    )
    target_link_libraries(thread_pool_tests PRIVATE Threads::Threads)

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(thread_pool_tests
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
        )
    endif()

    add_test(NAME thread_pool_tests COMMAND thread_pool_tests)
endif()
