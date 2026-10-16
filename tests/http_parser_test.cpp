#include "http/http_parser.h"

#include <cassert>
#include <string>
#include "test_runner.h"

void parses_fragmented_headers_and_body() {
    HTTPParser parser;
    assert(parser.feed("POST /telemetry HTTP/1.1\r\nHo") == ParseStatus::Incomplete);
    assert(parser.feed("st: localhost\r\nContent-Length: 5\r\n\r\nhe") ==
           ParseStatus::Incomplete);
    assert(parser.feed("llo") == ParseStatus::Complete);

    const HttpRequest& request = parser.request();
    assert(request.method == "POST");
    assert(request.path == "/telemetry");
    assert(request.headers.at("host") == "localhost");
    assert(request.body == "hello");
}

void parses_request_without_body() {
    HTTPParser parser;
    assert(parser.feed("GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n") ==
           ParseStatus::Complete);
    assert(parser.request().body.empty());
}

void separates_query_from_request_path() {
    HTTPParser parser;
    assert(parser.feed("GET /recent?count=12 HTTP/1.1\r\nHost: localhost\r\n\r\n") ==
           ParseStatus::Complete);
    assert(parser.request().path == "/recent");
    assert(parser.request().query == "count=12");
}

void rejects_malformed_and_unsupported_requests() {
    HTTPParser malformed;
    assert(malformed.feed("GET /health\r\n\r\n") == ParseStatus::Error);

    HTTPParser conflicting_length;
    assert(conflicting_length.feed(
               "POST / HTTP/1.1\r\nContent-Length: 2\r\nContent-Length: 3\r\n\r\n") ==
           ParseStatus::Error);

    HTTPParser chunked;
    assert(chunked.feed("POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n") ==
           ParseStatus::Error);
}

void enforces_limits() {
    HTTPParser parser(32, 4);
    assert(parser.feed("POST / HTTP/1.1\r\nContent-Length: 5\r\n\r\n") ==
           ParseStatus::Error);

    HTTPParser headers(16, 4);
    assert(headers.feed("GET /a-very-long-path HTTP/1.1\r\n") == ParseStatus::Error);
}

int main(int argc, char* argv[]) {
    TestRunner runner(argc, argv);
    runner.run("parses_fragmented_headers_and_body", parses_fragmented_headers_and_body);
    runner.run("parses_request_without_body", parses_request_without_body);
    runner.run("separates_query_from_request_path", separates_query_from_request_path);
    runner.run("rejects_malformed_and_unsupported_requests",
               rejects_malformed_and_unsupported_requests);
    runner.run("enforces_limits", enforces_limits);
    return runner.finish();
}
