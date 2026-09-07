cmake_minimum_required(VERSION 3.16)

project(message_gateway LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(message_gateway
	src/main.cpp
	src/epoll_loop/epoll_loop.cpp
	src/tcp_server/tcp_server.cpp
)

target_include_directories(message_gateway PRIVATE src)
target_compile_options(message_gateway PRIVATE -Wall -Wextra -Wpedantic)
