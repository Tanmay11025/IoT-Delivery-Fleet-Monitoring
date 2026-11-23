#pragma once

#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>

// Escape a string so it can be placed inside a JSON string literal.
inline std::string json_escape(std::string_view value) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string escaped;
    escaped.reserve(value.size());
    for (const unsigned char byte : value) {
        switch (byte) {
        case '"': escaped += "\\\""; break;
        case '\\': escaped += "\\\\"; break;
        case '\b': escaped += "\\b"; break;
        case '\f': escaped += "\\f"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default:
            if (byte < 0x20) {
                escaped += "\\u00";
                escaped += hex[byte >> 4];
                escaped += hex[byte & 0x0f];
            } else {
                escaped += static_cast<char>(byte);
            }
        }
    }
    return escaped;
}

// Format a finite double for JSON output without locale or exponent surprises.
inline std::string json_number(double value, int decimals = 6) {
    if (!std::isfinite(value)) return "0";
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
    std::string text(buffer);
    // Trim trailing zeros ("32.500000" -> "32.5") to keep responses compact.
    if (text.find('.') != std::string::npos) {
        while (!text.empty() && text.back() == '0') text.pop_back();
        if (!text.empty() && text.back() == '.') text.pop_back();
    }
    if (text == "-0") text = "0";
    return text;
}
