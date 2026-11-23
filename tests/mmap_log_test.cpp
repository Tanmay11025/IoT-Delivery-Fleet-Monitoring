#include "storage/mmap_log.h"

#include <array>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <string>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include "test_runner.h"

namespace {
std::filesystem::path test_directory(const std::string& name) {
    auto path = std::filesystem::temp_directory_path() /
                ("gateway-mmap-" + name + "-" + std::to_string(getpid()));
    std::filesystem::remove_all(path);
    return path;
}

std::filesystem::path segment_file(const std::filesystem::path& directory,
                                   unsigned int segment_id) {
    char filename[32];
    std::snprintf(filename, sizeof(filename), "log-%08u.seg", segment_id);
    return directory / filename;
}

void put_u32(std::array<unsigned char, MemoryMappedLog::record_header_size>& header,
             std::size_t offset, std::uint32_t value) {
    for (unsigned int index = 0; index < 4; ++index) {
        header[offset + index] = static_cast<unsigned char>((value >> (index * 8)) & 0xffU);
    }
}

void put_u64(std::array<unsigned char, MemoryMappedLog::record_header_size>& header,
             std::size_t offset, std::uint64_t value) {
    for (unsigned int index = 0; index < 8; ++index) {
        header[offset + index] = static_cast<unsigned char>((value >> (index * 8)) & 0xffU);
    }
}
}

void mmap_log_recovers_records_after_reopen() {
    const auto directory = test_directory("reopen");
    std::uint64_t expected_next_offset = 0;
    {
        MemoryMappedLog log;
        log.open(directory, 512, 8);
        assert(log.append("a") == 0);
        assert(log.append("variable length payload") ==
               MemoryMappedLog::record_header_size + 1);
        expected_next_offset = log.append("");
        assert(expected_next_offset == 32 + 1 + 32 + 23);
        log.flush();
        assert(log.disk_usage_bytes() == 512);
    }
    {
        MemoryMappedLog log;
        log.open(directory, 512, 8);
        const auto messages = log.get_recent(10);
        assert(messages.size() == 3);
        assert(messages[0].offset == 0);
        assert(messages[0].payload == "a");
        assert(messages[1].payload == "variable length payload");
        assert(messages[2].offset == expected_next_offset);
        assert(messages[2].payload.empty());
        assert(log.next_offset() == expected_next_offset + MemoryMappedLog::record_header_size);
        const std::uint64_t append_offset = log.next_offset();
        assert(log.append("after restart") == append_offset);
        log.flush();
    }
    std::filesystem::remove_all(directory);
}

void mmap_log_rolls_segments_and_tracks_global_offsets() {
    const auto directory = test_directory("rollover");
    MemoryMappedLog log;
    log.open(directory, 84, 10);
    assert(log.append("abcdefghijkl") == 0);
    assert(log.append("mnopqrstuvwx") == 44);
    assert(log.append("123456789012") == 88);

    const auto segments = log.segments();
    assert(segments.size() == 3);
    assert(segments[0].segment_id == 1 && segments[0].first_offset == 0);
    assert(segments[0].end_offset == 44);
    assert(segments[1].segment_id == 2 && segments[1].first_offset == 44);
    assert(segments[2].segment_id == 3 && segments[2].first_offset == 88);
    assert(log.disk_usage_bytes() == 84 * 3);
    log.flush();
    log.close();

    MemoryMappedLog recovered;
    recovered.open(directory, 84, 10);
    const auto messages = recovered.get_recent(10);
    assert(messages.size() == 3);
    assert(messages[0].offset == 0 && messages[1].offset == 44 && messages[2].offset == 88);
    recovered.close();
    std::filesystem::remove_all(directory);
}

void mmap_log_truncates_a_torn_active_tail_and_resumes() {
    const auto directory = test_directory("torn");
    const std::uint64_t valid_end = MemoryMappedLog::segment_header_size +
                                    MemoryMappedLog::record_header_size + 4;
    {
        MemoryMappedLog log;
        log.open(directory, 256, 4);
        assert(log.append("good") == 0);
        log.flush();
    }
    const int fd = open(segment_file(directory, 1).c_str(), O_RDWR);
    assert(fd != -1);
    const std::array<unsigned char, 12> partial_record{
        'R', 'E', 'C', '1', 32, 0, 0, 0, 0, 0, 0, 0};
    assert(pwrite(fd, partial_record.data(), partial_record.size(),
                  static_cast<off_t>(valid_end)) ==
           static_cast<ssize_t>(partial_record.size()));
    close(fd);

    MemoryMappedLog recovered;
    recovered.open(directory, 256, 4);
    const auto messages = recovered.get_recent(4);
    assert(messages.size() == 1);
    assert(messages[0].payload == "good");
    assert(recovered.append("next") == MemoryMappedLog::record_header_size + 4);
    recovered.flush();
    recovered.close();

    MemoryMappedLog reopened;
    reopened.open(directory, 256, 4);
    const auto after_restart = reopened.get_recent(4);
    assert(after_restart.size() == 2);
    assert(after_restart[1].payload == "next");
    reopened.close();
    std::filesystem::remove_all(directory);
}

void mmap_log_discards_latest_record_with_bad_checksum() {
    const auto directory = test_directory("checksum");
    {
        MemoryMappedLog log;
        log.open(directory, 256, 4);
        log.append("valid");
        log.append("corrupt");
        log.flush();
    }
    const int fd = open(segment_file(directory, 1).c_str(), O_RDWR);
    assert(fd != -1);
    const off_t second_payload = static_cast<off_t>(MemoryMappedLog::segment_header_size +
                                                    MemoryMappedLog::record_header_size + 5 +
                                                    MemoryMappedLog::record_header_size);
    const unsigned char changed = 'X';
    assert(pwrite(fd, &changed, 1, second_payload) == 1);
    close(fd);

    MemoryMappedLog recovered;
    recovered.open(directory, 256, 4);
    const auto messages = recovered.get_recent(4);
    assert(messages.size() == 1);
    assert(messages[0].payload == "valid");
    assert(recovered.next_offset() == MemoryMappedLog::record_header_size + 5);
    recovered.close();
    std::filesystem::remove_all(directory);
}

void mmap_log_rejects_records_that_cannot_fit() {
    const auto directory = test_directory("oversized");
    MemoryMappedLog log;
    log.open(directory, 84, 4);
    bool rejected = false;
    try {
        log.append("1234567890123");
    } catch (const std::length_error&) {
        rejected = true;
    }
    assert(rejected);
    assert(log.next_offset() == 0);
    assert(log.append("123456789012") == 0);
    log.close();
    std::filesystem::remove_all(directory);
}

void mmap_log_truncates_payload_left_before_record_header() {
    const auto directory = test_directory("payload-before-header");
    const std::uint64_t valid_end = MemoryMappedLog::segment_header_size +
                                    MemoryMappedLog::record_header_size + 4;
    {
        MemoryMappedLog log;
        log.open(directory, 256, 4);
        assert(log.append("good") == 0);
        log.flush();
    }
    const int fd = open(segment_file(directory, 1).c_str(), O_RDWR);
    assert(fd != -1);
    const char partial_payload[] = "torn";
    assert(pwrite(fd, partial_payload, sizeof(partial_payload) - 1,
                  static_cast<off_t>(valid_end + MemoryMappedLog::record_header_size)) ==
           static_cast<ssize_t>(sizeof(partial_payload) - 1));
    close(fd);

    MemoryMappedLog recovered;
    recovered.open(directory, 256, 4);
    assert(recovered.get_recent(4).size() == 1);
    assert(recovered.next_offset() == MemoryMappedLog::record_header_size + 4);
    assert(recovered.append("next") == MemoryMappedLog::record_header_size + 4);
    recovered.close();
    std::filesystem::remove_all(directory);
}

void mmap_log_truncates_a_record_with_a_partial_payload() {
    const auto directory = test_directory("partial-payload");
    const std::uint64_t valid_end = MemoryMappedLog::segment_header_size +
                                    MemoryMappedLog::record_header_size + 4;
    {
        MemoryMappedLog log;
        log.open(directory, 256, 4);
        assert(log.append("good") == 0);
        log.flush();
    }
    const int fd = open(segment_file(directory, 1).c_str(), O_RDWR);
    assert(fd != -1);
    std::array<unsigned char, MemoryMappedLog::record_header_size> header{};
    put_u32(header, 0, 0x31434552U);
    put_u32(header, 4, MemoryMappedLog::record_header_size);
    put_u64(header, 8, MemoryMappedLog::record_header_size + 4);
    put_u32(header, 24, 8);
    assert(pwrite(fd, header.data(), header.size(), static_cast<off_t>(valid_end)) ==
           static_cast<ssize_t>(header.size()));
    const char partial_payload[] = "xy";
    assert(pwrite(fd, partial_payload, sizeof(partial_payload) - 1,
                  static_cast<off_t>(valid_end + header.size())) ==
           static_cast<ssize_t>(sizeof(partial_payload) - 1));
    assert(ftruncate(fd, static_cast<off_t>(valid_end + header.size() + 2)) == 0);
    close(fd);

    MemoryMappedLog recovered;
    recovered.open(directory, 256, 4);
    assert(recovered.get_recent(4).size() == 1);
    assert(recovered.next_offset() == MemoryMappedLog::record_header_size + 4);
    recovered.close();
    std::filesystem::remove_all(directory);
}

void mmap_log_fails_on_corruption_in_a_sealed_segment() {
    const auto directory = test_directory("sealed-corruption");
    {
        MemoryMappedLog log;
        log.open(directory, 84, 4);
        assert(log.append("abcdefghijkl") == 0);
        assert(log.append("mnopqrstuvwx") == 44);
        log.flush();
        log.close();
    }
    const int fd = open(segment_file(directory, 1).c_str(), O_RDWR);
    assert(fd != -1);
    const unsigned char changed = 'X';
    assert(pwrite(fd, &changed, 1,
                  static_cast<off_t>(MemoryMappedLog::segment_header_size +
                                     MemoryMappedLog::record_header_size)) == 1);
    close(fd);

    MemoryMappedLog recovered;
    bool rejected = false;
    try {
        recovered.open(directory, 84, 4);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    assert(rejected);
    assert(!recovered.is_open());
    std::filesystem::remove_all(directory);
}

// Reference bit-at-a-time CRC-32 (IEEE 802.3). The log's own implementation
// may be optimised, but the bytes it writes must never change.
std::uint32_t reference_crc32(const std::string& bytes) {
    std::uint32_t crc = 0xffffffffU;
    for (const unsigned char byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) crc = (crc & 1U) ? (crc >> 1) ^ 0xedb88320U : crc >> 1;
    }
    return crc ^ 0xffffffffU;
}

void mmap_log_writes_standard_crc32_checksums() {
    assert(reference_crc32("123456789") == 0xCBF43926U);  // The standard check value.
    const auto directory = test_directory("crc");
    const std::string payloads[] = {"", "x", std::string(5000, 'q'), "{\"vehicle_id\":\"van-1\"}"};
    {
        MemoryMappedLog log;
        log.open(directory, 1 << 16, 8);
        for (const auto& payload : payloads) log.append(payload);
    }
    std::FILE* file = std::fopen(segment_file(directory, 1).c_str(), "rb");
    assert(file != nullptr);
    long position = MemoryMappedLog::segment_header_size;
    for (const auto& payload : payloads) {
        std::string record(MemoryMappedLog::record_header_size + payload.size(), '\0');
        assert(std::fseek(file, position, SEEK_SET) == 0);
        assert(std::fread(record.data(), 1, record.size(), file) == record.size());
        const auto* bytes = reinterpret_cast<const unsigned char*>(record.data());
        std::uint32_t stored = 0;
        for (int index = 0; index < 4; ++index) stored |= static_cast<std::uint32_t>(bytes[28 + index]) << (index * 8);
        // The checksum covers the first 28 header bytes followed by the payload.
        assert(stored == reference_crc32(record.substr(0, 28) + payload));
        assert(record.substr(MemoryMappedLog::record_header_size) == payload);
        position += static_cast<long>(record.size());
    }
    std::fclose(file);
    std::filesystem::remove_all(directory);
}

void mmap_log_reads_from_an_offset_across_segments() {
    const auto directory = test_directory("read-from");
    std::vector<std::uint64_t> offsets;
    {
        MemoryMappedLog log;
        // 200-byte segments hold four 40-byte records, so 10 records span 3 segments.
        log.open(directory, 200, 4);
        for (int index = 0; index < 10; ++index) {
            offsets.push_back(log.append("record-" + std::to_string(index)));
        }
        assert(log.segments().size() == 3);

        const LogReadResult all = log.read_from(0, 1 << 20);
        assert(all.messages.size() == 10);
        for (int index = 0; index < 10; ++index) {
            assert(all.messages[index].offset == offsets[index]);
            assert(all.messages[index].payload == "record-" + std::to_string(index));
        }
        assert(all.next_offset == log.next_offset());

        // Start in the middle of the second segment.
        const LogReadResult tail = log.read_from(offsets[5], 1 << 20);
        assert(tail.messages.size() == 5);
        assert(tail.messages.front().payload == "record-5");

        // A small byte budget still returns at least one record and a resume point.
        const LogReadResult first = log.read_from(0, 1);
        assert(first.messages.size() == 1);
        assert(first.next_offset == offsets[1]);
        assert(log.read_from(first.next_offset, 1).messages.front().payload == "record-1");

        // Reading at the end returns nothing; past the end or mid-record is an error.
        const LogReadResult end = log.read_from(log.next_offset());
        assert(end.messages.empty());
        assert(end.next_offset == log.next_offset());
        bool threw = false;
        try { (void)log.read_from(log.next_offset() + 1); } catch (const std::out_of_range&) { threw = true; }
        assert(threw);
        threw = false;
        try { (void)log.read_from(offsets[3] + 1); } catch (const std::invalid_argument&) { threw = true; }
        assert(threw);
    }
    {
        // The same reads work after a restart, from sealed segments on disk.
        MemoryMappedLog log;
        log.open(directory, 200, 4);
        const LogReadResult replay = log.read_from(offsets[2], 1 << 20);
        assert(replay.messages.size() == 8);
        assert(replay.messages.front().payload == "record-2");
        assert(replay.messages.back().payload == "record-9");
    }
    std::filesystem::remove_all(directory);
}

int main(int argc, char* argv[]) {
    TestRunner runner(argc, argv);
    runner.run("mmap_log_recovers_records_after_reopen", mmap_log_recovers_records_after_reopen);
    runner.run("mmap_log_rolls_segments_and_tracks_global_offsets",
               mmap_log_rolls_segments_and_tracks_global_offsets);
    runner.run("mmap_log_truncates_a_torn_active_tail_and_resumes",
               mmap_log_truncates_a_torn_active_tail_and_resumes);
    runner.run("mmap_log_discards_latest_record_with_bad_checksum",
               mmap_log_discards_latest_record_with_bad_checksum);
    runner.run("mmap_log_rejects_records_that_cannot_fit",
               mmap_log_rejects_records_that_cannot_fit);
    runner.run("mmap_log_truncates_payload_left_before_record_header",
               mmap_log_truncates_payload_left_before_record_header);
    runner.run("mmap_log_truncates_a_record_with_a_partial_payload",
               mmap_log_truncates_a_record_with_a_partial_payload);
    runner.run("mmap_log_reads_from_an_offset_across_segments",
               mmap_log_reads_from_an_offset_across_segments);
    runner.run("mmap_log_writes_standard_crc32_checksums",
               mmap_log_writes_standard_crc32_checksums);
    runner.run("mmap_log_fails_on_corruption_in_a_sealed_segment",
               mmap_log_fails_on_corruption_in_a_sealed_segment);
    return runner.finish();
}