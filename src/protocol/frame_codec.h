#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// Consumer wire protocol (see docs/protocol.md).
//
//   +----------------------+------------+------------------+
//   | length: u32, big-end | type: u8   | payload          |
//   +----------------------+------------+------------------+
//
// `length` counts the type byte plus the payload, so it is always >= 1.
// All multi-byte integers are big-endian (network byte order).
enum class FrameType : std::uint8_t {
    Subscribe = 1,  // client -> server, empty payload: stream new records only
    FetchFrom = 2,  // client -> server, u64 offset: replay from offset, then stream
    Data = 3,       // server -> client, u64 offset, u64 next_offset, u64 timestamp_ns, bytes
    Ack = 4,        // client -> server, u64 next_offset the client has fully processed
    Error = 5,      // server -> client, UTF-8 message; the server closes afterwards
};

struct Frame {
    FrameType type;
    std::string payload;
};

constexpr std::size_t frame_length_prefix = 4;
constexpr std::size_t data_header_size = 24;
// Client frames are tiny; a small cap stops a peer from making us buffer
// an arbitrarily large "frame" before we have validated anything.
constexpr std::size_t max_client_frame_bytes = 1024;

void put_be32(std::string& output, std::uint32_t value);
void put_be64(std::string& output, std::uint64_t value);
std::uint32_t get_be32(const unsigned char* bytes);
std::uint64_t get_be64(const unsigned char* bytes);

std::string encode_frame(FrameType type, std::string_view payload);
std::string encode_data(std::uint64_t offset, std::uint64_t next_offset,
                        std::uint64_t timestamp_ns, std::string_view payload);
std::string encode_offset_frame(FrameType type, std::uint64_t offset);
std::string encode_error(std::string_view message);

// Incremental decoder: TCP may deliver half a frame or several frames in one
// read, so bytes are buffered until a whole frame is available.
class FrameDecoder {
public:
    explicit FrameDecoder(std::size_t max_frame_bytes = max_client_frame_bytes)
        : max_frame_bytes(max_frame_bytes) {}

    void feed(std::string_view bytes);

    // Return the next complete frame, or nullopt when more bytes are needed
    // or the stream is invalid (check failed()).
    std::optional<Frame> next();

    bool failed() const { return !error_message.empty(); }
    const std::string& error() const { return error_message; }
    std::size_t buffered_bytes() const { return buffer.size() - consumed; }

private:
    std::size_t max_frame_bytes;
    std::string buffer;
    std::size_t consumed = 0;
    std::string error_message;
};

// Read a u64 payload (FETCH_FROM / ACK). Returns nullopt if the size is wrong.
std::optional<std::uint64_t> read_offset_payload(const Frame& frame);
