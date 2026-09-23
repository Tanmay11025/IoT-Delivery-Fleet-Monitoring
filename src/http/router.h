#pragma once

#include "../http/http_parser.h"
#include "http_response.h"

#include <functional>
#include <string>
#include <functional>
#include <unordered_map>

using namespace std;

// Maps an HTTP method and path to the handler that serves it.
class Router {
public:
    using Handler = function<HttpResponse(const HttpRequest&)>;

    // Register or replace a handler for a method/path pair.
    void add_route(string method, string path, Handler handler);

    // Dispatch a request, returning 404 when no route matches.
    HttpResponse route(const HttpRequest& request) const;

private:
    // The method and path together identify one route.
    struct RouteKey {
        string method;
        string path;

        // Route keys match only when both components are equal.
        bool operator==(const RouteKey& other) const {
            return method == other.method && path == other.path;
        }
    };

    // Combines method and path hashes for the unordered route map.
    struct RouteKeyHash {
        size_t operator()(const RouteKey& key) const;
    };

    unordered_map<RouteKey, Handler, RouteKeyHash> routes;
};
