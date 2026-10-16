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
    runner.run("mmap_log_fails_on_corruption_in_a_sealed_segment",
               mmap_log_fails_on_corruption_in_a_sealed_segment);
    return runner.finish();
}