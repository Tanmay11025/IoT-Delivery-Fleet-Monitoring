#include "http_response.h"



string HttpResponse::to_string() const {
    string serialized = "HTTP/1.1 " + std::to_string(status_code) + " " + reason + "\r\n";
    bool has_length = false;
    for (const auto& [name, value] : headers) {
        serialized += name + ": " + value + "\r\n";
        has_length = has_length || name == "Content-Length" || name == "content-length";
    }
    if (!has_length) {
        serialized += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    serialized += "Connection: close\r\n\r\n";
    serialized += body;
    return serialized;
}
