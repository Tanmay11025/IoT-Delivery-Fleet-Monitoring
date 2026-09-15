#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <algorithm>
#include <charconv>
#include <cctype>
#include <limits>

using namespace std;

struct HttpRequest {
    string method;
    string path;
    unordered_map<string, string> headers;
    string body;
};

enum class ParseStatus {
    Incomplete,
    Complete,
    Error
};

class HTTPParser {
public:
    explicit HTTPParser(size_t max_header_bytes = 16 * 1024,
                        size_t max_body_bytes = 1 * 1024 * 1024);

    ParseStatus feed(string_view bytes);
    const HttpRequest& request() const;
    const string& error() const;

private:
    ParseStatus parse_headers(size_t header_end);
    static string trim(string_view value);
    static string lowercase(string_view value);
    ParseStatus fail(string message);

    size_t max_header_bytes;
    size_t max_body_bytes;
    string buffer;
    HttpRequest parsed_request;
    string parse_error;
    size_t content_length = 0;
    bool headers_parsed = false;
    bool complete = false;
};
