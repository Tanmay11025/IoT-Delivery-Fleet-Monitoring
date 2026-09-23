#pragma once

#include <string>
#include <unordered_map>
#include <string>

using namespace std;

// Represents the HTTP response sent back to a client.
struct HttpResponse {
    int status_code = 200;                  // Numeric HTTP status code.
    string reason = "OK";                   // Text associated with the status code.
    unordered_map<string, string> headers;  // Additional response headers.
    string body;                            // Response payload.

    // Serialize the response into an HTTP/1.1 message.
    string to_string() const;
};
