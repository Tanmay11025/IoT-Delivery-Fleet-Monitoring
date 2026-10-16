#include "network/tcp_server.h"

int main(int argc, char* argv[]) {
    Config config;
    try {
        config = Config::from_args(argc, argv);
        Logger::set_file(config.log_path);
    } catch (const exception& error) {
        cerr << error.what() << '\n'
             << "Usage: " << argv[0]
             << " [--config path] [--port n] [--backlog n] [--epoll-workers n]"
                " [--pool-threads n] [--queue-capacity n] [--log-path path]"
                     " [--rate-limit-capacity n] [--rate-limit-refill-rate n]"
                     " [--storage-directory path] [--storage-segment-size bytes]"
                     " [--recent-capacity n]\n";
        return EXIT_FAILURE;
    }

    signal(SIGINT, handle_shutdown_signal);
    signal(SIGTERM, handle_shutdown_signal);

    TCPServer server(config);
    server.add_route("GET", "/health", [](const HttpRequest&) {
        return HttpResponse{200, "OK", {{"Content-Type", "application/json"}},
                            "{\"status\":\"ok\"}"};
    });
    server.add_route("GET", "/connections", [&server](const HttpRequest&) {
        return HttpResponse{200, "OK", {{"Content-Type", "application/json"}},
                            "{\"connections\":" +
                                to_string(server.active_connection_count()) + "}"};
    });
    server.add_route("GET", "/metrics", [&server](const HttpRequest&) {
        return HttpResponse{200, "OK",
                            {{"Content-Type", "text/plain; version=0.0.4; charset=utf-8"}},
                            server.metrics_text()};
    });
    server.add_route("GET", "/recent", [&server](const HttpRequest& request) {
        return server.recent_messages_response(request);
    });
    server.add_route("GET", "/large-response", [](const HttpRequest&) {
        // This deterministic payload exercises queued output and EPOLLOUT tests.
        return HttpResponse{200, "OK", {}, string(512 * 1024, 'x')};
    });

    // start() returns false for operating-system setup failures
    if (!server.start()) {
        return EXIT_FAILURE;
    }

    return 0;
}
