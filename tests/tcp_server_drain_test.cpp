#include "network/tcp_server.h"

#include <arpa/inet.h>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <mutex>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include "test_runner.h"

int reserve_port() {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd != -1);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    assert(bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    socklen_t length = sizeof(address);
    assert(getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0);
    const int port = ntohs(address.sin_port);
    close(fd);
    return port;
}

std::string send_request(int port, const std::string& path) {
    int client = -1;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (client == -1 && std::chrono::steady_clock::now() < deadline) {
        client = socket(AF_INET, SOCK_STREAM, 0);
        assert(client != -1);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(static_cast<uint16_t>(port));
        if (connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == -1) {
            close(client);
            client = -1;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    assert(client != -1);
    const std::string request = "GET " + path +
                                " HTTP/1.1\r\nHost: localhost\r\n\r\n";
    assert(send(client, request.data(), request.size(), 0) ==
           static_cast<ssize_t>(request.size()));
    timeval timeout{5, 0};
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    std::string response;
    char buffer[1024];
    ssize_t received;
    while ((received = recv(client, buffer, sizeof(buffer), 0)) > 0) {
        response.append(buffer, static_cast<std::size_t>(received));
    }
    close(client);
    return response;
}

void shutdown_drains_an_in_flight_handler() {
    Config config;
    config.port = reserve_port();
    config.epoll_worker_count = 1;
    config.thread_pool_size = 1;
    config.task_queue_capacity = 2;
    config.rate_limit_capacity = 100.0;
    config.storage_segment_size = 256;
    config.storage_directory = (std::filesystem::temp_directory_path() /
                                 ("gateway-drain-test-" + std::to_string(getpid()))).string();
    std::filesystem::remove_all(config.storage_directory);

    TCPServer server(config);
    std::promise<void> handler_started;
    auto started = handler_started.get_future();
    std::mutex mutex;
    std::condition_variable condition;
    bool release_handler = false;
    server.add_route("GET", "/seed-one", [&server](const HttpRequest&) {
        const auto offset = server.append_message("first");
        return HttpResponse{200, "OK", {}, std::to_string(offset)};
    });
    server.add_route("GET", "/seed-two", [&server](const HttpRequest&) {
        const auto offset = server.append_message("second");
        return HttpResponse{200, "OK", {}, std::to_string(offset)};
    });
    server.add_route("GET", "/seed-escaped", [&server](const HttpRequest&) {
        const auto offset = server.append_message("quoted \"value\"\\line\n");
        return HttpResponse{200, "OK", {}, std::to_string(offset)};
    });
    server.add_route("GET", "/recent", [&server](const HttpRequest& request) {
        return server.recent_messages_response(request);
    });
    server.add_route("GET", "/metrics", [&server](const HttpRequest&) {
        return HttpResponse{200, "OK",
                            {{"Content-Type", "text/plain; version=0.0.4; charset=utf-8"}},
                            server.metrics_text()};
    });
    server.add_route("GET", "/blocked", [&](const HttpRequest&) {
        handler_started.set_value();
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [&] { return release_handler; });
        return HttpResponse{200, "OK", {}, "drained"};
    });

    std::atomic<bool> start_result{false};
    std::thread server_thread([&] { start_result.store(server.start()); });
    struct StopServerOnExit {
        std::thread& thread;
        std::mutex& mutex;
        std::condition_variable& condition;
        bool& release_handler;
        ~StopServerOnExit() {
            handle_shutdown_signal(SIGTERM);
            {
                std::lock_guard<std::mutex> lock(mutex);
                release_handler = true;
            }
            condition.notify_all();
            if (thread.joinable()) thread.join();
        }
    } stop_server_on_exit{server_thread, mutex, condition, release_handler};

        assert(send_request(config.port, "/seed-one").find("HTTP/1.1 200 OK") !=
            std::string::npos);
        assert(send_request(config.port, "/seed-two").find("HTTP/1.1 200 OK") !=
            std::string::npos);
        assert(send_request(config.port, "/seed-escaped").find("HTTP/1.1 200 OK") !=
            std::string::npos);
    const std::string recent_response = send_request(config.port, "/recent");
        assert(recent_response.find("Content-Type: application/json") != std::string::npos);
        assert(recent_response.find("\"payload\":\"first\"") != std::string::npos);
        assert(recent_response.find("\"payload\":\"second\"") != std::string::npos);
        assert(recent_response.find("\"payload\":\"quoted \\\"value\\\"\\\\line\\n\"") !=
            std::string::npos);
        assert(recent_response.find("\"payload\":\"first\"") <
               recent_response.find("\"payload\":\"second\""));
        assert(recent_response.find("\"payload\":\"second\"") <
               recent_response.find("\"payload\":\"quoted "));
        const std::string limited_recent = send_request(config.port, "/recent?count=1");
         assert(limited_recent.find("\"payload\":\"quoted ") != std::string::npos);
         assert(limited_recent.find("\"payload\":\"second\"") == std::string::npos);
        assert(limited_recent.find("\"payload\":\"first\"") == std::string::npos);
        const std::string empty_recent = send_request(config.port, "/recent?count=0");
        assert(empty_recent.find("\r\n\r\n[]") != std::string::npos);
        const std::string invalid_recent = send_request(config.port, "/recent?count=1001");
        assert(invalid_recent.find("HTTP/1.1 400 Bad Request") != std::string::npos);
        const std::string malformed_recent = send_request(config.port, "/recent?count=nope");
        assert(malformed_recent.find("HTTP/1.1 400 Bad Request") != std::string::npos);
    const std::string metrics_response = send_request(config.port, "/metrics");
    assert(metrics_response.find("gateway_log_disk_bytes 256\n") != std::string::npos);

    int client = -1;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (client == -1 && std::chrono::steady_clock::now() < deadline) {
        client = socket(AF_INET, SOCK_STREAM, 0);
        assert(client != -1);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(static_cast<uint16_t>(config.port));
        if (connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == -1) {
            close(client);
            client = -1;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    assert(client != -1);
    const char request[] = "GET /blocked HTTP/1.1\r\nHost: localhost\r\n\r\n";
    assert(send(client, request, sizeof(request) - 1, 0) == sizeof(request) - 1);
    assert(started.wait_for(std::chrono::seconds(5)) == std::future_status::ready);

    handle_shutdown_signal(SIGTERM);
    {
        std::lock_guard<std::mutex> lock(mutex);
        release_handler = true;
    }
    condition.notify_one();

    timeval timeout{5, 0};
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    std::string response;
    char buffer[1024];
    ssize_t received;
    while ((received = recv(client, buffer, sizeof(buffer), 0)) > 0) {
        response.append(buffer, static_cast<std::size_t>(received));
    }
    close(client);
    server_thread.join();

    assert(start_result.load());
    assert(response.find("HTTP/1.1 200 OK") != std::string::npos);
    assert(response.find("drained") != std::string::npos);
    const std::string exposition = server.metrics_text();
    assert(exposition.find("gateway_thread_pool_workers 1\n") != std::string::npos);
    std::filesystem::remove_all(config.storage_directory);
}

int main(int argc, char* argv[]) {
    TestRunner runner(argc, argv);
    runner.run("shutdown_drains_an_in_flight_handler", shutdown_drains_an_in_flight_handler);
    return runner.finish();
}