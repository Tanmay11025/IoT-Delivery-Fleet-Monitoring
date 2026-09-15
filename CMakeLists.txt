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
