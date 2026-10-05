cmake_minimum_required(VERSION 3.16)

project(message_gateway LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(message_gateway
	src/main.cpp
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
add_test(NAME rate_limiter_tests COMMAND rate_limiter_tests)

add_executable(metrics_tests
	tests/metrics_test.cpp
	src/monitoring/metrics.cpp
)

target_include_directories(metrics_tests PRIVATE src)
target_compile_options(metrics_tests PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME metrics_tests COMMAND metrics_tests)
