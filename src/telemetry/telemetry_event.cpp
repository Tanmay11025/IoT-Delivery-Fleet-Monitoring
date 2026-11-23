#include "telemetry_event.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <unordered_set>

namespace {

// A tiny cursor-based reader for one flat JSON object. It understands exactly
// the subset of JSON a TelemetryEvent needs: strings, numbers, true/false/null.
class FlatJsonReader {
public:
    explicit FlatJsonReader(std::string_view text) : text(text) {}

    void skip_whitespace() {
        while (position < text.size() &&
               (text[position] == ' ' || text[position] == '\t' ||
                text[position] == '\n' || text[position] == '\r')) {
            ++position;
        }
    }

    bool consume(char expected) {
        skip_whitespace();
        if (position < text.size() && text[position] == expected) {
            ++position;
            return true;
        }
        return false;
    }

    bool at_end() {
        skip_whitespace();
        return position == text.size();
    }

    char peek() {
        skip_whitespace();
        return position < text.size() ? text[position] : '\0';
    }

    // Read a JSON string literal, decoding escapes into UTF-8.
    bool read_string(std::string& output) {
        if (!consume('"')) return false;
        output.clear();
        while (position < text.size()) {
            // Copy the run of ordinary characters in one append; appending one
            // char at a time showed up as ~30% of CPU for large bodies.
            const std::size_t run_start = position;
            while (position < text.size() && text[position] != '"' && text[position] != '\\' &&
                   static_cast<unsigned char>(text[position]) >= 0x20) {
                ++position;
            }
            output.append(text.data() + run_start, position - run_start);
            if (position >= text.size()) return false;
            const char character = text[position++];
            if (character == '"') return true;
            if (character != '\\') return false;  // A raw control character.
            if (position >= text.size()) return false;
            const char escape = text[position++];
            switch (escape) {
            case '"': output += '"'; break;
            case '\\': output += '\\'; break;
            case '/': output += '/'; break;
            case 'b': output += '\b'; break;
            case 'f': output += '\f'; break;
            case 'n': output += '\n'; break;
            case 'r': output += '\r'; break;
            case 't': output += '\t'; break;
            case 'u': {
                unsigned int code_point = 0;
                if (!read_hex4(code_point)) return false;
                // Surrogate pairs are not needed for vehicle ids; reject them
                // instead of producing invalid UTF-8.
                if (code_point >= 0xD800 && code_point <= 0xDFFF) return false;
                append_utf8(output, code_point);
                break;
            }
            default:
                return false;
            }
        }
        return false;
    }

    // Read a JSON number token and convert it to double.
    bool read_number(double& value, bool& is_integer) {
        skip_whitespace();
        const std::size_t start = position;
        if (position < text.size() && text[position] == '-') ++position;
        if (position >= text.size()) return false;
        if (text[position] == '0') {
            ++position;
        } else if (text[position] >= '1' && text[position] <= '9') {
            while (position < text.size() && is_digit(text[position])) ++position;
        } else {
            return false;
        }
        is_integer = true;
        if (position < text.size() && text[position] == '.') {
            is_integer = false;
            ++position;
            if (position >= text.size() || !is_digit(text[position])) return false;
            while (position < text.size() && is_digit(text[position])) ++position;
        }
        if (position < text.size() && (text[position] == 'e' || text[position] == 'E')) {
            is_integer = false;
            ++position;
            if (position < text.size() && (text[position] == '+' || text[position] == '-')) {
                ++position;
            }
            if (position >= text.size() || !is_digit(text[position])) return false;
            while (position < text.size() && is_digit(text[position])) ++position;
        }
        // strtod needs a NUL-terminated buffer; the token is short.
        const std::string token(text.substr(start, position - start));
        errno = 0;
        char* end = nullptr;
        value = std::strtod(token.c_str(), &end);
        return errno == 0 && end == token.c_str() + token.size() && std::isfinite(value);
    }

    bool read_literal(std::string_view literal) {
        skip_whitespace();
        if (text.substr(position, literal.size()) != literal) return false;
        position += literal.size();
        return true;
    }

private:
    static bool is_digit(char character) { return character >= '0' && character <= '9'; }

    bool read_hex4(unsigned int& value) {
        if (position + 4 > text.size()) return false;
        value = 0;
        for (int index = 0; index < 4; ++index) {
            const char digit = text[position++];
            value <<= 4;
            if (digit >= '0' && digit <= '9') value |= static_cast<unsigned int>(digit - '0');
            else if (digit >= 'a' && digit <= 'f') value |= static_cast<unsigned int>(digit - 'a' + 10);
            else if (digit >= 'A' && digit <= 'F') value |= static_cast<unsigned int>(digit - 'A' + 10);
            else return false;
        }
        return true;
    }

    static void append_utf8(std::string& output, unsigned int code_point) {
        if (code_point < 0x80) {
            output += static_cast<char>(code_point);
        } else if (code_point < 0x800) {
            output += static_cast<char>(0xC0 | (code_point >> 6));
            output += static_cast<char>(0x80 | (code_point & 0x3F));
        } else {
            output += static_cast<char>(0xE0 | (code_point >> 12));
            output += static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
            output += static_cast<char>(0x80 | (code_point & 0x3F));
        }
    }

    std::string_view text;
    std::size_t position = 0;
};

TelemetryParseResult failure(std::string message) {
    TelemetryParseResult result;
    result.error = std::move(message);
    return result;
}

bool valid_vehicle_id(const std::string& id) {
    if (id.empty() || id.size() > max_vehicle_id_length) return false;
    for (const char character : id) {
        const bool allowed = (character >= 'a' && character <= 'z') ||
                             (character >= 'A' && character <= 'Z') ||
                             (character >= '0' && character <= '9') ||
                             character == '-' || character == '_' || character == '.';
        if (!allowed) return false;
    }
    return true;
}

}  // namespace

TelemetryParseResult parse_telemetry(std::string_view body) {
    if (body.size() > max_telemetry_body_bytes) {
        return failure("telemetry body exceeds " + std::to_string(max_telemetry_body_bytes) +
                       " bytes");
    }

    FlatJsonReader reader(body);
    if (!reader.consume('{')) return failure("body must be a JSON object");

    TelemetryEvent event;
    std::unordered_set<std::string> seen;
    bool has_vehicle_id = false, has_lat = false, has_lng = false;
    bool has_speed = false, has_status = false, has_timestamp = false;

    if (!reader.consume('}')) {
        while (true) {
            std::string key;
            if (!reader.read_string(key)) return failure("expected a quoted field name");
            if (!seen.insert(key).second) return failure("duplicate field: " + key);
            if (!reader.consume(':')) return failure("expected ':' after field " + key);

            const char next = reader.peek();
            if (next == '"') {
                std::string value;
                if (!reader.read_string(value)) return failure("invalid string for " + key);
                if (key == "vehicle_id") {
                    event.vehicle_id = std::move(value);
                    has_vehicle_id = true;
                } else if (key == "status") {
                    event.status = std::move(value);
                    has_status = true;
                } else if (key == "lat" || key == "lng" || key == "speed" ||
                           key == "timestamp") {
                    return failure(key + " must be a number");
                }
            } else if (next == '-' || (next >= '0' && next <= '9')) {
                double value = 0.0;
                bool is_integer = false;
                if (!reader.read_number(value, is_integer)) {
                    return failure("invalid number for " + key);
                }
                if (key == "lat") {
                    event.lat = value;
                    has_lat = true;
                } else if (key == "lng") {
                    event.lng = value;
                    has_lng = true;
                } else if (key == "speed") {
                    event.speed = value;
                    has_speed = true;
                } else if (key == "timestamp") {
                    if (!is_integer) return failure("timestamp must be an integer");
                    // 2^53 keeps the value exactly representable as a double.
                    if (value < 0 || value > 9007199254740992.0) {
                        return failure("timestamp is out of range");
                    }
                    event.timestamp = static_cast<std::int64_t>(value);
                    has_timestamp = true;
                } else if (key == "vehicle_id" || key == "status") {
                    return failure(key + " must be a string");
                }
            } else if (next == 't' || next == 'f' || next == 'n') {
                if (!reader.read_literal("true") && !reader.read_literal("false") &&
                    !reader.read_literal("null")) {
                    return failure("invalid literal for " + key);
                }
                if (key == "vehicle_id" || key == "status" || key == "lat" ||
                    key == "lng" || key == "speed" || key == "timestamp") {
                    return failure(key + " has the wrong type");
                }
            } else if (next == '{' || next == '[') {
                return failure("nested values are not supported (field " + key + ")");
            } else {
                return failure("expected a value for " + key);
            }

            if (reader.consume(',')) continue;
            if (reader.consume('}')) break;
            return failure("expected ',' or '}' after field " + key);
        }
    }
    if (!reader.at_end()) return failure("unexpected data after the JSON object");

    if (!has_vehicle_id) return failure("missing field: vehicle_id");
    if (!has_lat) return failure("missing field: lat");
    if (!has_lng) return failure("missing field: lng");
    if (!has_speed) return failure("missing field: speed");
    if (!has_status) return failure("missing field: status");
    if (!has_timestamp) return failure("missing field: timestamp");

    if (!valid_vehicle_id(event.vehicle_id)) {
        return failure("vehicle_id must be 1-64 characters of [A-Za-z0-9._-]");
    }
    if (event.lat < -90.0 || event.lat > 90.0) return failure("lat must be within [-90, 90]");
    if (event.lng < -180.0 || event.lng > 180.0) return failure("lng must be within [-180, 180]");
    if (event.speed < 0.0 || event.speed > max_speed_kmh) {
        return failure("speed must be within [0, 400] km/h");
    }
    if (event.status != "moving" && event.status != "idle" && event.status != "offline") {
        return failure("status must be one of moving, idle, offline");
    }

    TelemetryParseResult result;
    result.ok = true;
    result.event = std::move(event);
    return result;
}
