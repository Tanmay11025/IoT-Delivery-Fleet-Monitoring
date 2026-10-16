cmake_minimum_required(VERSION 3.16)

project(message_gateway LANGUAGES CXX)

find_package(Threads REQUIRED)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(message_gateway
	src/main.cpp
	src/core/config.cpp
	src/http/http_parser.cpp
	src/http/http_response.cpp
	src/http/router.cpp
	src/security/rate_limiter.cpp
	src/monitoring/metrics.cpp
	src/network/connection_manager.cpp
	src/network/epoll_loop.cpp
	src/network/tcp_server.cpp
)

target_include_directories(message_gateway PRIVATE src)
target_compile_options(message_gateway PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(message_gateway PRIVATE Threads::Threads)

enable_testing()

add_executable(http_parser_tests
	tests/http_parser_test.cpp
	src/http/http_parser.cpp
)

target_include_directories(http_parser_tests PRIVATE src)
target_compile_options(http_parser_tests PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME http_parser_tests COMMAND http_parser_tests)

add_executable(rate_limiter_tests
	tests/rate_limiter_test.cpp
	src/http/http_parser.cpp
	src/http/http_response.cpp
	src/http/router.cpp
	src/security/rate_limiter.cpp
)

target_include_directories(rate_limiter_tests PRIVATE src)
target_compile_options(rate_limiter_tests PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(rate_limiter_tests PRIVATE Threads::Threads)
add_test(NAME rate_limiter_tests COMMAND rate_limiter_tests)

add_executable(metrics_tests
	tests/metrics_test.cpp
	src/monitoring/metrics.cpp
)

target_include_directories(metrics_tests PRIVATE src)
target_compile_options(metrics_tests PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME metrics_tests COMMAND metrics_tests)

add_executable(config_tests
	tests/config_test.cpp
	src/core/config.cpp
)
target_include_directories(config_tests PRIVATE src)
target_compile_options(config_tests PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME config_tests COMMAND config_tests)

add_executable(thread_pool_tests tests/thread_pool_test.cpp)
target_include_directories(thread_pool_tests PRIVATE src)
target_compile_options(thread_pool_tests PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(thread_pool_tests PRIVATE Threads::Threads)
add_test(NAME thread_pool_tests COMMAND thread_pool_tests)

add_executable(tcp_server_drain_tests
	tests/tcp_server_drain_test.cpp
	src/core/config.cpp
	src/http/http_parser.cpp
	src/http/http_response.cpp
	src/http/router.cpp
	src/security/rate_limiter.cpp
	src/monitoring/metrics.cpp
	src/network/connection_manager.cpp
	src/network/epoll_loop.cpp
	src/network/tcp_server.cpp
)
target_include_directories(tcp_server_drain_tests PRIVATE src)
target_compile_options(tcp_server_drain_tests PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(tcp_server_drain_tests PRIVATE Threads::Threads)
add_test(NAME tcp_server_drain_tests COMMAND tcp_server_drain_tests)
