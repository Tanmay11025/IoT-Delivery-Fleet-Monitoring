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

// Stores the request that was parsed from the client data.
struct HttpRequest {
    string method;                     // Example: GET, POST
    string path;                       // Example: /status
    unordered_map<string, string> headers; // Header names are stored in lowercase.
    string body;                       // Request body, if there is one.
};

// Current state of the parser while reading a request.
enum class ParseStatus {
    Incomplete,
    Complete,
    Error
};

// Reads an HTTP request in parts.
class HTTPParser {
public:
    // Maximum allowed size for headers and body.
    explicit HTTPParser(size_t max_header_bytes = 16 * 1024,    // 16 KB
                        size_t max_body_bytes = 1 * 1024 * 1024);   // 1 MB

    // Add incoming data to the parser.
    ParseStatus feed(string_view bytes);

    // Return the parsed request.
    const HttpRequest& request() const;

    // Return the latest error message.
    const string& error() const;

private:
    // Parse the request line and headers.
    ParseStatus parse_headers(size_t header_end);

    // Remove spaces and tabs from the start and end of a value.
    static string trim(string_view value);

    // Turn a header name into lowercase.
    static string lowercase(string_view value);

    // Save an error and return the error state.
    ParseStatus fail(string message);

    size_t max_header_bytes;      // Max size for headers.
    size_t max_body_bytes;        // Max size for the body.
    string buffer;                // Data received so far.
    HttpRequest parsed_request;   // Final parsed request.
    string parse_error;           // Error text if parsing fails.
    size_t content_length = 0;    // Body length from Content-Length.
    bool headers_parsed = false;  // True after headers are read.
    bool complete = false;        // True after the whole request is done.
};
