#include "http_parser.h"

HTTPParser::HTTPParser(size_t max_header_bytes, size_t max_body_bytes)
    : max_header_bytes(max_header_bytes), max_body_bytes(max_body_bytes) {}

ParseStatus HTTPParser::feed(string_view bytes) {
    if (complete || !parse_error.empty()) {
        return complete ? ParseStatus::Complete : ParseStatus::Error;
    }

    if (buffer.size() > max_header_bytes + max_body_bytes ||
        bytes.size() > max_header_bytes + max_body_bytes - buffer.size()) {
        return fail("request exceeds configured size limit");
    }
    buffer.append(bytes);

    if (!headers_parsed) {
        const size_t separator = buffer.find("\r\n\r\n");
        if (separator == string::npos) {
            if (buffer.size() > max_header_bytes) {
                return fail("headers exceed configured size limit");
            }
            return ParseStatus::Incomplete;
        }
        if (separator + 4 > max_header_bytes) {
            return fail("headers exceed configured size limit");
        }
        const ParseStatus status = parse_headers(separator);
        if (status == ParseStatus::Error) {
            return status;
        }
        headers_parsed = true;
        buffer.erase(0, separator + 4);
    }

    if (buffer.size() < content_length) {
        return ParseStatus::Incomplete;
    }
    parsed_request.body.assign(buffer.data(), content_length);
    complete = true;
    return ParseStatus::Complete;
}

const HttpRequest& HTTPParser::request() const {
    return parsed_request;
}

const string& HTTPParser::error() const {
    return parse_error;
}

ParseStatus HTTPParser::parse_headers(size_t header_end) {
    const size_t request_line_end = buffer.find("\r\n");
    if (request_line_end == string::npos || request_line_end >= header_end) {
        return fail("missing request line");
    }

    const string_view request_line(buffer.data(), request_line_end);
    const size_t method_end = request_line.find(' ');
    const size_t path_end = method_end == string_view::npos
                                     ? string_view::npos
                                     : request_line.find(' ', method_end + 1);
    if (method_end == string_view::npos || path_end == string_view::npos ||
        method_end == 0 || path_end == method_end + 1) {
        return fail("malformed request line");
    }

    const string_view version = request_line.substr(path_end + 1);
    if (version != "HTTP/1.0" && version != "HTTP/1.1") {
        return fail("unsupported HTTP version");
    }

    parsed_request.method.assign(request_line.substr(0, method_end));
    parsed_request.path.assign(request_line.substr(method_end + 1, path_end - method_end - 1));

    size_t line_start = request_line_end + 2;
    while (line_start < header_end) {
        const size_t line_end = buffer.find("\r\n", line_start);
        if (line_end == string::npos || line_end > header_end) {
            return fail("malformed header line");
        }
        const string_view line(buffer.data() + line_start, line_end - line_start);
        const size_t colon = line.find(':');
        if (colon == string_view::npos || colon == 0) {
            return fail("malformed header");
        }

        const string name = lowercase(line.substr(0, colon));
        const string value = trim(line.substr(colon + 1));
        if (name.empty() || value.find('\r') != string::npos ||
            value.find('\n') != string::npos) {
            return fail("malformed header value");
        }

        auto [header, inserted] = parsed_request.headers.emplace(name, value);
        if (!inserted) {
            if (name == "content-length" && header->second != value) {
                return fail("conflicting content-length headers");
            }
            if (name != "content-length") {
                header->second += "," + value;
            }
        }
        line_start = line_end + 2;
    }

    const auto transfer_encoding = parsed_request.headers.find("transfer-encoding");
    if (transfer_encoding != parsed_request.headers.end()) {
        return fail("transfer-encoding is unsupported");
    }

    const auto length = parsed_request.headers.find("content-length");
    if (length != parsed_request.headers.end()) {
        const string& value = length->second;
        size_t parsed_length = 0;
        const auto result = from_chars(value.data(), value.data() + value.size(), parsed_length);
        if (result.ec != errc{} || result.ptr != value.data() + value.size()) {
            return fail("invalid content-length");
        }
        if (parsed_length > max_body_bytes) {
            return fail("body exceeds configured size limit");
        }
        content_length = parsed_length;
    }

    return ParseStatus::Incomplete;
}

string HTTPParser::trim(string_view value) {
    size_t first = 0;
    while (first < value.size() && (value[first] == ' ' || value[first] == '\t')) {
        ++first;
    }
    size_t last = value.size();
    while (last > first && (value[last - 1] == ' ' || value[last - 1] == '\t')) {
        --last;
    }
    return string(value.substr(first, last - first));
}

string HTTPParser::lowercase(string_view value) {
    string result(value);
    transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
        return static_cast<char>(tolower(character));
    });
    return result;
}

ParseStatus HTTPParser::fail(string message) {
    parse_error = move(message);
    return ParseStatus::Error;
}
