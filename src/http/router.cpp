#include "router.h"

void Router::add_route(string method, string path, Handler handler) {
    routes.insert_or_assign(RouteKey{move(method), move(path)}, move(handler));
}

HttpResponse Router::route(const HttpRequest& request) const {
    const auto route = routes.find(RouteKey{request.method, request.path});
    if (route == routes.end()) {
        return HttpResponse{404, "Not Found", {}, "Not Found"};
    }
    return route->second(request);
}

size_t Router::RouteKeyHash::operator()(const RouteKey& key) const {
    const size_t method_hash = hash<string>{}(key.method);
    const size_t path_hash = hash<string>{}(key.path);
    return method_hash ^ (path_hash + 0x9e3779b9U + (method_hash << 6) + (method_hash >> 2));
}
