#include "frame_codec.h"

void put_be32(std::string& output, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        output += static_cast<char>((value >> shift) & 0xffU);
    }
}

void put_be64(std::string& output, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        output += static_cast<char>((value >> shift) & 0xffU);
    }
}

std::uint32_t get_be32(const unsigned char* bytes) {
    std::uint32_t value = 0;
    for (int index = 0; index < 4; ++index) value = (value << 8) | bytes[index];
    return value;
}

std::uint64_t get_be64(const unsigned char* bytes) {
    std::uint64_t value = 0;
    for (int index = 0; index < 8; ++index) value = (value << 8) | bytes[index];
    return value;
}

std::string encode_frame(FrameType type, std::string_view payload) {
    std::string frame;
    frame.reserve(frame_length_prefix + 1 + payload.size());
    put_be32(frame, static_cast<std::uint32_t>(payload.size() + 1));
    frame += static_cast<char>(type);
    frame.append(payload.data(), payload.size());
    return frame;
}

std::string encode_data(std::uint64_t offset, std::uint64_t next_offset,
                        std::uint64_t timestamp_ns, std::string_view payload) {
    std::string frame;
    frame.reserve(frame_length_prefix + 1 + data_header_size + payload.size());
    put_be32(frame, static_cast<std::uint32_t>(1 + data_header_size + payload.size()));
    frame += static_cast<char>(FrameType::Data);
    put_be64(frame, offset);
    put_be64(frame, next_offset);
    put_be64(frame, timestamp_ns);
    frame.append(payload.data(), payload.size());
    return frame;
}

std::string encode_offset_frame(FrameType type, std::uint64_t offset) {
    std::string payload;
    put_be64(payload, offset);
    return encode_frame(type, payload);
}

std::string encode_error(std::string_view message) {
    return encode_frame(FrameType::Error, message);
}

void FrameDecoder::feed(std::string_view bytes) {
    if (failed()) return;
    // Drop already-decoded bytes before growing, so the buffer stays small
    // even for a long-lived connection.
    if (consumed > 0 && consumed == buffer.size()) {
        buffer.clear();
        consumed = 0;
    } else if (consumed > 4096) {
        buffer.erase(0, consumed);
        consumed = 0;
    }
    buffer.append(bytes.data(), bytes.size());
}

std::optional<Frame> FrameDecoder::next() {
    if (failed()) return std::nullopt;
    const std::size_t available = buffer.size() - consumed;
    if (available < frame_length_prefix) return std::nullopt;

    const auto* start = reinterpret_cast<const unsigned char*>(buffer.data() + consumed);
    const std::uint32_t length = get_be32(start);
    if (length == 0) {
        error_message = "frame length must include the type byte";
        return std::nullopt;
    }
    if (length > max_frame_bytes) {
        error_message = "frame exceeds " + std::to_string(max_frame_bytes) + " bytes";
        return std::nullopt;
    }
    if (available < frame_length_prefix + length) return std::nullopt;

    const std::uint8_t raw_type = start[frame_length_prefix];
    if (raw_type < static_cast<std::uint8_t>(FrameType::Subscribe) ||
        raw_type > static_cast<std::uint8_t>(FrameType::Error)) {
        error_message = "unknown frame type " + std::to_string(raw_type);
        return std::nullopt;
    }

    Frame frame{static_cast<FrameType>(raw_type),
                buffer.substr(consumed + frame_length_prefix + 1, length - 1)};
    consumed += frame_length_prefix + length;
    return frame;
}

std::optional<std::uint64_t> read_offset_payload(const Frame& frame) {
    if (frame.payload.size() != 8) return std::nullopt;
    return get_be64(reinterpret_cast<const unsigned char*>(frame.payload.data()));
}
