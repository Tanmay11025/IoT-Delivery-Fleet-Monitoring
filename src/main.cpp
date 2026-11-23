#include "network/tcp_server.h"
#include "http/static_files.h"

#include <chrono>
#include <cstdlib>
#include <thread>

namespace {
void print_usage(ostream& output, const char* program) {
    output << "Usage: " << program
           << " [--config path] [--port n] [--backlog n] [--epoll-workers n]"
              " [--pool-threads n] [--queue-capacity n] [--log-path path]"
              " [--rate-limit-capacity n] [--rate-limit-refill-rate n]"
              " [--storage-directory path] [--storage-segment-size bytes]"
              " [--recent-capacity n] [--consumer-port n] [--max-consumers n]"
              " [--dashboard-directory path] [--vehicle-offline-seconds n]\n";
}
}

int main(int argc, char* argv[]) {
    for (int index = 1; index < argc; ++index) {
        const string argument(argv[index]);
        if (argument == "--help" || argument == "-h") {
            print_usage(cout, argv[0]);
            return EXIT_SUCCESS;
        }
    }

    Config config;
    try {
        config = Config::from_args(argc, argv);
        Logger::set_file(config.log_path);
    } catch (const exception& error) {
        cerr << error.what() << '\n';
        print_usage(cerr, argv[0]);
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
    server.add_route("POST", "/publish", [&server](const HttpRequest& request) {
        return server.publish_telemetry(request);
    });
    server.add_route("GET", "/vehicles", [&server](const HttpRequest&) {
        return HttpResponse{200, "OK", {{"Content-Type", "application/json"}},
                            server.vehicles_json()};
    });
    server.add_route("GET", "/consumers", [&server](const HttpRequest&) {
        return HttpResponse{200, "OK", {{"Content-Type", "application/json"}},
                            server.consumers_json()};
    });

    std::vector<std::string> missing_assets;
    const auto dashboard_files = load_dashboard_files(config.dashboard_directory, missing_assets);
    for (const StaticFile& file : dashboard_files) {
        server.add_route("GET", file.url_path, [file](const HttpRequest&) {
            return HttpResponse{200, "OK",
                                {{"Content-Type", file.content_type},
                                 {"Cache-Control", "no-cache"}},
                                file.body};
        });
    }
    if (!missing_assets.empty()) {
        Logger::warn("Dashboard directory '" + config.dashboard_directory + "' is missing " +
                     std::to_string(missing_assets.size()) + " file(s); the dashboard may be "
                     "incomplete. Set dashboard_directory or --dashboard-directory.");
    }
    server.add_route("GET", "/large-response", [](const HttpRequest&) {
        // This deterministic payload exercises queued output and EPOLLOUT tests.
        return HttpResponse{200, "OK", {}, string(512 * 1024, 'x')};
    });
    if (std::getenv("GATEWAY_TEST_MODE") != nullptr) {
        server.add_route("GET", "/test/hold", [](const HttpRequest&) {
            std::this_thread::sleep_for(std::chrono::seconds(15));
            return HttpResponse{200, "OK", {}, "held"};
        });
    }

    // start() returns false for operating-system setup failures
    if (!server.start()) {
        return EXIT_FAILURE;
    }

    return 0;
}
