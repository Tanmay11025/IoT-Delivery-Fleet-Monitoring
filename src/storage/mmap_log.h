#pragma once

#include "circular_buffer.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

struct Message {
    std::uint64_t offset = 0;
    std::uint64_t timestamp_ns = 0;
    std::uint32_t payload_length = 0;
    std::string payload;
};

struct LogSegmentInfo {
    std::uint32_t segment_id = 0;
    std::uint64_t first_offset = 0;
    std::uint64_t end_offset = 0;
    std::uint64_t valid_bytes = 0;
    std::filesystem::path path;
};

class MemoryMappedLog {
public:
    static constexpr std::size_t segment_header_size = 40;
    static constexpr std::size_t record_header_size = 32;

    MemoryMappedLog() = default;
    ~MemoryMappedLog();

    MemoryMappedLog(const MemoryMappedLog&) = delete;
    MemoryMappedLog& operator=(const MemoryMappedLog&) = delete;

    void open(const std::filesystem::path& directory, std::uint64_t initial_size,
              std::size_t recent_capacity = 1024);
    std::uint64_t append(const std::string& payload);
    void flush();
    void close();

    std::vector<Message> get_recent(std::size_t count) const;
    std::vector<LogSegmentInfo> segments() const;
    std::uint64_t disk_usage_bytes() const;
    std::uint64_t next_offset() const;
    bool is_open() const;

private:
    struct Mapping {
        int fd = -1;
        unsigned char* data = nullptr;
        std::size_t size = 0;
        std::filesystem::path path;
        std::uint32_t segment_id = 0;
        std::uint64_t base_offset = 0;
        std::size_t write_position = segment_header_size;
    };

    void recover_or_create();
    void create_segment(std::uint32_t segment_id, std::uint64_t base_offset);
    void map_active_segment(std::uint32_t segment_id, std::uint64_t base_offset,
                            std::size_t write_position);
    void unmap_active(bool sync);
    void scan_segment(const std::filesystem::path& path, std::uint32_t expected_id,
                      bool is_latest, std::uint64_t& expected_offset,
                      LogSegmentInfo& info);

    mutable std::mutex mutex;
    std::filesystem::path directory;
    std::uint64_t segment_size = 0;
    std::uint64_t next_message_offset = 0;
    Mapping active;
    std::vector<LogSegmentInfo> segment_index;
    CircularBuffer<Message> recent_cache{1};
    bool opened = false;
};