#pragma once

#include <string>
#include <unordered_map>
#include <string>

using namespace std;

struct HttpResponse {
    int status_code = 200;
    string reason = "OK";
    unordered_map<string, string> headers;
    string body;

    string to_string() const;
};
