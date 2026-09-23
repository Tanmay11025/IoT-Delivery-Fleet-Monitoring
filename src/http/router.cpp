#include "router.h"

void Router::add_route(string method, string path, Handler handler) {
    // insert_or_assign makes registering the same method/path replace its handler.
    routes.insert_or_assign(RouteKey{move(method), move(path)}, move(handler));
}

HttpResponse Router::route(const HttpRequest& request) const {
    // Look up the route using the exact method and path from the parsed request.
    const auto route = routes.find(RouteKey{request.method, request.path});
    if (route == routes.end()) {
        // Return a complete response for unknown routes.
        return HttpResponse{404, "Not Found", {}, "Not Found"};
    }
    // Let the matched handler construct the application-specific response.
    return route->second(request);
}

size_t Router::RouteKeyHash::operator()(const RouteKey& key) const {
    // Mix both fields so routes with the same method or path remain distinguishable.
    const size_t method_hash = hash<string>{}(key.method);
    const size_t path_hash = hash<string>{}(key.path);
    return method_hash ^ (path_hash + 0x9e3779b9U + (method_hash << 6) + (method_hash >> 2));
}
