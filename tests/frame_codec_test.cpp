#include "protocol/frame_codec.h"

#include <cassert>
#include <string>
#include "test_runner.h"

void frame_codec_encodes_big_endian_frames() {
    const std::string frame = encode_offset_frame(FrameType::FetchFrom, 0x0102030405060708ULL);
    const std::string expected("\x00\x00\x00\x09\x02\x01\x02\x03\x04\x05\x06\x07\x08", 13);
    assert(frame == expected);

    const std::string data = encode_data(64, 100, 7, "abc");
    assert(data.size() == 4 + 1 + 24 + 3);
    const auto* bytes = reinterpret_cast<const unsigned char*>(data.data());
    assert(get_be32(bytes) == 1 + 24 + 3);
    assert(bytes[4] == static_cast<unsigned char>(FrameType::Data));
    assert(get_be64(bytes + 5) == 64);
    assert(get_be64(bytes + 13) == 100);
    assert(get_be64(bytes + 21) == 7);
    assert(data.substr(29) == "abc");
}

void frame_codec_reassembles_fragmented_frames() {
    const std::string frame = encode_offset_frame(FrameType::Ack, 4242);
    FrameDecoder decoder;
    // Deliver one byte at a time, the worst case TCP can produce.
    for (std::size_t index = 0; index + 1 < frame.size(); ++index) {
        decoder.feed(frame.substr(index, 1));
        assert(!decoder.next().has_value());
        assert(!decoder.failed());
    }
    decoder.feed(frame.substr(frame.size() - 1));
    const auto decoded = decoder.next();
    assert(decoded.has_value());
    assert(decoded->type == FrameType::Ack);
    assert(read_offset_payload(*decoded) == 4242U);
    assert(decoder.buffered_bytes() == 0);
}

void frame_codec_splits_several_frames_from_one_read() {
    FrameDecoder decoder;
    decoder.feed(encode_frame(FrameType::Subscribe, "") +
                 encode_offset_frame(FrameType::Ack, 1) +
                 encode_offset_frame(FrameType::Ack, 2).substr(0, 6));
    auto first = decoder.next();
    assert(first && first->type == FrameType::Subscribe && first->payload.empty());
    auto second = decoder.next();
    assert(second && read_offset_payload(*second) == 1U);
    assert(!decoder.next().has_value());
    decoder.feed(encode_offset_frame(FrameType::Ack, 2).substr(6));
    auto third = decoder.next();
    assert(third && read_offset_payload(*third) == 2U);
}

void frame_codec_rejects_invalid_frames() {
    {
        FrameDecoder decoder;
        decoder.feed(std::string("\x00\x00\x00\x00", 4));
        assert(!decoder.next().has_value());
        assert(decoder.failed());
    }
    {
        FrameDecoder decoder(16);
        std::string oversized;
        put_be32(oversized, 17);
        decoder.feed(oversized);
        assert(!decoder.next().has_value());
        assert(decoder.error() == "frame exceeds 16 bytes");
    }
    {
        FrameDecoder decoder;
        decoder.feed(std::string("\x00\x00\x00\x01\x09", 5));
        assert(!decoder.next().has_value());
        assert(decoder.error() == "unknown frame type 9");
        // A failed decoder stays failed.
        decoder.feed(encode_frame(FrameType::Subscribe, ""));
        assert(!decoder.next().has_value());
    }
    {
        const Frame short_ack{FrameType::Ack, "1234"};
        assert(!read_offset_payload(short_ack).has_value());
    }
}

int main(int argc, char* argv[]) {
    TestRunner runner(argc, argv);
    runner.run("frame_codec_encodes_big_endian_frames", frame_codec_encodes_big_endian_frames);
    runner.run("frame_codec_reassembles_fragmented_frames",
               frame_codec_reassembles_fragmented_frames);
    runner.run("frame_codec_splits_several_frames_from_one_read",
               frame_codec_splits_several_frames_from_one_read);
    runner.run("frame_codec_rejects_invalid_frames", frame_codec_rejects_invalid_frames);
    return runner.finish();
}
