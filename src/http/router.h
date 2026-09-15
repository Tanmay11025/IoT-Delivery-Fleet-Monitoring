#pragma once

#include "../http/http_parser.h"
#include "http_response.h"

#include <functional>
#include <string>
#include <functional>
#include <unordered_map>

using namespace std;

class Router {
public:
    using Handler = function<HttpResponse(const HttpRequest&)>;

    void add_route(string method, string path, Handler handler);
    HttpResponse route(const HttpRequest& request) const;

private:
    struct RouteKey {
        string method;
        string path;

        bool operator==(const RouteKey& other) const {
            return method == other.method && path == other.path;
        }
    };

    struct RouteKeyHash {
        size_t operator()(const RouteKey& key) const;
    };

    unordered_map<RouteKey, Handler, RouteKeyHash> routes;
};
