#include "mmap_log.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
constexpr std::array<unsigned char, 8> segment_magic{'I', 'O', 'T', 'L', 'O', 'G', '0', '1'};
constexpr std::uint32_t segment_version = 1;
constexpr std::uint32_t record_magic = 0x31434552;
constexpr std::uint32_t record_version_size = MemoryMappedLog::record_header_size;
constexpr std::uint32_t maximum_segment_id = 99999999;

void put_u32(unsigned char* destination, std::uint32_t value) {
    for (unsigned int index = 0; index < 4; ++index) {
        destination[index] = static_cast<unsigned char>((value >> (index * 8)) & 0xffU);
    }
}

void put_u64(unsigned char* destination, std::uint64_t value) {
    for (unsigned int index = 0; index < 8; ++index) {
        destination[index] = static_cast<unsigned char>((value >> (index * 8)) & 0xffU);
    }
}

std::uint32_t get_u32(const unsigned char* source) {
    std::uint32_t value = 0;
    for (unsigned int index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(source[index]) << (index * 8);
    }
    return value;
}

std::uint64_t get_u64(const unsigned char* source) {
    std::uint64_t value = 0;
    for (unsigned int index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(source[index]) << (index * 8);
    }
    return value;
}

std::uint32_t crc32_update(std::uint32_t crc, const unsigned char* bytes,
                           std::size_t length) {
    for (std::size_t index = 0; index < length; ++index) {
        crc ^= bytes[index];
        for (int bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1) ^ (0xedb88320U & mask);
        }
    }
    return crc;
}

std::uint32_t record_crc(const unsigned char* header, const std::string& payload) {
    std::uint32_t crc = crc32_update(0xffffffffU, header, 28);
    crc = crc32_update(crc, reinterpret_cast<const unsigned char*>(payload.data()),
                       payload.size());
    return crc ^ 0xffffffffU;
}

std::uint32_t segment_crc(const unsigned char* header) {
    return crc32_update(0xffffffffU, header, 32) ^ 0xffffffffU;
}

bool pread_all(int fd, unsigned char* output, std::size_t length, off_t offset,
               std::size_t& bytes_read) {
    bytes_read = 0;
    while (bytes_read < length) {
        const ssize_t result = pread(fd, output + bytes_read, length - bytes_read,
                                     offset + static_cast<off_t>(bytes_read));
        if (result == 0) return true;
        if (result == -1 && errno == EINTR) continue;
        if (result == -1) return false;
        bytes_read += static_cast<std::size_t>(result);
    }
    return true;
}

std::filesystem::path segment_path(const std::filesystem::path& directory,
                                   std::uint32_t segment_id) {
    char name[32];
    std::snprintf(name, sizeof(name), "log-%08u.seg", segment_id);
    return directory / name;
}

bool parse_segment_id(const std::string& name, std::uint32_t& segment_id) {
    if (name.size() != 16 || name.compare(0, 4, "log-") != 0 ||
        name.compare(12, 4, ".seg") != 0) {
        return false;
    }
    std::uint32_t parsed = 0;
    for (std::size_t index = 4; index < 12; ++index) {
        if (name[index] < '0' || name[index] > '9') return false;
        parsed = parsed * 10 + static_cast<std::uint32_t>(name[index] - '0');
    }
    segment_id = parsed;
    return true;
}

void throw_errno(const std::string& operation, const std::filesystem::path& path) {
    throw std::system_error(errno, std::generic_category(), operation + " " + path.string());
}

bool zero_tail(int fd, std::uint64_t offset, std::uint64_t length,
               const std::filesystem::path& path) {
    std::array<unsigned char, 4096> bytes{};
    while (length != 0) {
        const std::size_t chunk = static_cast<std::size_t>(
            std::min<std::uint64_t>(length, bytes.size()));
        std::size_t bytes_read = 0;
        if (!pread_all(fd, bytes.data(), chunk, static_cast<off_t>(offset), bytes_read)) {
            throw_errno("pread", path);
        }
        if (bytes_read != chunk) {
            throw std::runtime_error("unable to read segment tail: " + path.string());
        }
        if (!std::all_of(bytes.begin(), bytes.begin() + chunk,
                         [](unsigned char byte) { return byte == 0; })) {
            return false;
        }
        offset += chunk;
        length -= chunk;
    }
    return true;
}
}

MemoryMappedLog::~MemoryMappedLog() {
    try {
        close();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "MemoryMappedLog destructor: %s\n", error.what());
    } catch (...) {
        std::fprintf(stderr, "MemoryMappedLog destructor: unknown persistence failure\n");
    }
}

void MemoryMappedLog::open(const std::filesystem::path& log_directory,
                           std::uint64_t initial_size, std::size_t recent_capacity) {
    std::lock_guard<std::mutex> lock(mutex);
    if (opened) throw std::logic_error("memory-mapped log is already open");
    if (initial_size < segment_header_size + record_header_size + 1 ||
        initial_size > std::numeric_limits<std::size_t>::max() ||
        initial_size > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        throw std::invalid_argument("segment size is outside the supported range");
    }
    if (recent_capacity == 0) throw std::invalid_argument("recent capacity must be positive");

    directory = log_directory;
    segment_size = initial_size;
    recent_cache = CircularBuffer<Message>(recent_capacity);
    std::filesystem::create_directories(directory);
    recover_or_create();
    opened = true;
}

std::uint64_t MemoryMappedLog::append(const std::string& payload) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!opened) throw std::logic_error("memory-mapped log is not open");
    if (payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("payload exceeds the on-disk format limit");
    }
    const std::size_t record_size = record_header_size + payload.size();
    if (record_size > active.size - segment_header_size) {
        throw std::length_error("record is too large to fit in an empty segment");
    }
    if (next_message_offset > std::numeric_limits<std::uint64_t>::max() - record_size) {
        throw std::overflow_error("message offset space is exhausted");
    }
    if (active.write_position > active.size - record_size) {
        if (active.segment_id >= maximum_segment_id) {
            throw std::overflow_error("segment id space is exhausted");
        }
        try {
            unmap_active(true);
            create_segment(active.segment_id + 1, next_message_offset);
        } catch (...) {
            opened = false;
            throw;
        }
    }

    const std::uint64_t message_offset = next_message_offset;
    const std::uint64_t timestamp_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    std::array<unsigned char, record_header_size> header{};
    put_u32(header.data(), record_magic);
    put_u32(header.data() + 4, record_version_size);
    put_u64(header.data() + 8, message_offset);
    put_u64(header.data() + 16, timestamp_ns);
    put_u32(header.data() + 24, static_cast<std::uint32_t>(payload.size()));
    put_u32(header.data() + 28, record_crc(header.data(), payload));

    Message cached{message_offset, timestamp_ns,
                   static_cast<std::uint32_t>(payload.size()), payload};
    recent_cache.push(std::move(cached));

    unsigned char* destination = active.data + active.write_position;
    if (!payload.empty()) std::memcpy(destination + record_header_size, payload.data(), payload.size());
    std::memcpy(destination, header.data(), header.size());

    active.write_position += record_size;
    next_message_offset += record_size;
    LogSegmentInfo& info = segment_index.back();
    info.end_offset = next_message_offset;
    info.valid_bytes = active.write_position;
    return message_offset;
}

void MemoryMappedLog::flush() {
    std::lock_guard<std::mutex> lock(mutex);
    if (!opened) return;
    if (msync(active.data, active.size, MS_SYNC) == -1) throw_errno("msync", active.path);
    if (fsync(active.fd) == -1) throw_errno("fsync", active.path);
}

void MemoryMappedLog::close() {
    std::lock_guard<std::mutex> lock(mutex);
    if (!opened && active.fd == -1 && active.data == nullptr) return;
    opened = false;
    if (active.fd != -1) unmap_active(true);
}

std::vector<Message> MemoryMappedLog::get_recent(std::size_t count) const {
    std::lock_guard<std::mutex> lock(mutex);
    return recent_cache.get_recent(count);
}

std::vector<LogSegmentInfo> MemoryMappedLog::segments() const {
    std::lock_guard<std::mutex> lock(mutex);
    return segment_index;
}

std::uint64_t MemoryMappedLog::disk_usage_bytes() const {
    std::lock_guard<std::mutex> lock(mutex);
    std::uint64_t total = 0;
    for (const auto& segment : segment_index) {
        std::error_code error;
        const auto size = std::filesystem::file_size(segment.path, error);
        if (error) throw std::filesystem::filesystem_error(
            "unable to read log segment size", segment.path, error);
        total += size;
    }
    return total;
}

std::uint64_t MemoryMappedLog::next_offset() const {
    std::lock_guard<std::mutex> lock(mutex);
    return next_message_offset;
}

bool MemoryMappedLog::is_open() const {
    std::lock_guard<std::mutex> lock(mutex);
    return opened;
}

void MemoryMappedLog::recover_or_create() {
    std::vector<std::pair<std::uint32_t, std::filesystem::path>> files;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        std::uint32_t id = 0;
        if (entry.is_regular_file() && parse_segment_id(entry.path().filename().string(), id)) {
            files.emplace_back(id, entry.path());
        }
    }
    std::sort(files.begin(), files.end(), [](const auto& left, const auto& right) {
        return left.first < right.first;
    });

    next_message_offset = 0;
    segment_index.clear();
    std::uint64_t expected_offset = 0;
    for (std::size_t index = 0; index < files.size(); ++index) {
        if (files[index].first != index + 1) {
            throw std::runtime_error("log segment sequence has a gap");
        }
        LogSegmentInfo info;
        info.segment_id = files[index].first;
        info.path = files[index].second;
        scan_segment(files[index].second, files[index].first,
                     index + 1 == files.size(), expected_offset, info);
        segment_index.push_back(info);
    }
    next_message_offset = expected_offset;

    if (files.empty()) {
        create_segment(1, 0);
    } else {
        const auto& last = segment_index.back();
        map_active_segment(last.segment_id, last.first_offset,
                           static_cast<std::size_t>(last.valid_bytes));
    }
}

void MemoryMappedLog::create_segment(std::uint32_t segment_id,
                                     std::uint64_t base_offset) {
    const auto path = segment_path(directory, segment_id);
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd == -1) throw_errno("open", path);
    if (ftruncate(fd, static_cast<off_t>(segment_size)) == -1) {
        const int saved_errno = errno;
        ::close(fd);
        std::filesystem::remove(path);
        errno = saved_errno;
        throw_errno("ftruncate", path);
    }
    void* mapped = mmap(nullptr, static_cast<std::size_t>(segment_size),
                        PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapped == MAP_FAILED) {
        const int saved_errno = errno;
        ::close(fd);
        std::filesystem::remove(path);
        errno = saved_errno;
        throw_errno("mmap", path);
    }

    active.fd = fd;
    active.data = static_cast<unsigned char*>(mapped);
    active.size = static_cast<std::size_t>(segment_size);
    active.path = path;
    active.segment_id = segment_id;
    active.base_offset = base_offset;
    active.write_position = segment_header_size;

    std::array<unsigned char, segment_header_size> header{};
    std::copy(segment_magic.begin(), segment_magic.end(), header.begin());
    put_u32(header.data() + 8, segment_version);
    put_u32(header.data() + 12, segment_id);
    put_u64(header.data() + 16, base_offset);
    put_u64(header.data() + 24, segment_size);
    put_u32(header.data() + 32, segment_crc(header.data()));
    std::memcpy(active.data, header.data(), header.size());
    if (msync(active.data, segment_header_size, MS_SYNC) == -1 || fsync(active.fd) == -1) {
        const int saved_errno = errno;
        unmap_active(false);
        std::filesystem::remove(path);
        errno = saved_errno;
        throw_errno("persist segment header", path);
    }
    const int directory_fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory_fd == -1) {
        const int saved_errno = errno;
        unmap_active(false);
        std::filesystem::remove(path);
        errno = saved_errno;
        throw_errno("open directory", directory);
    }
    if (fsync(directory_fd) == -1) {
        const int saved_errno = errno;
        ::close(directory_fd);
        unmap_active(false);
        std::filesystem::remove(path);
        errno = saved_errno;
        throw_errno("fsync directory", directory);
    }
    ::close(directory_fd);

    segment_index.push_back(LogSegmentInfo{segment_id, base_offset, base_offset,
                                          segment_header_size, path});
}

void MemoryMappedLog::map_active_segment(std::uint32_t segment_id,
                                         std::uint64_t base_offset,
                                         std::size_t write_position) {
    const auto path = segment_path(directory, segment_id);
    const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd == -1) throw_errno("open", path);
    if (ftruncate(fd, static_cast<off_t>(segment_size)) == -1) {
        const int saved_errno = errno;
        ::close(fd);
        errno = saved_errno;
        throw_errno("ftruncate", path);
    }
    void* mapped = mmap(nullptr, static_cast<std::size_t>(segment_size),
                        PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapped == MAP_FAILED) {
        const int saved_errno = errno;
        ::close(fd);
        errno = saved_errno;
        throw_errno("mmap", path);
    }
    active.fd = fd;
    active.data = static_cast<unsigned char*>(mapped);
    active.size = static_cast<std::size_t>(segment_size);
    active.path = path;
    active.segment_id = segment_id;
    active.base_offset = base_offset;
    active.write_position = write_position;
}

void MemoryMappedLog::unmap_active(bool sync) {
    const auto path = active.path;
    int first_error = 0;
    if (active.data != nullptr) {
        if (sync && msync(active.data, active.size, MS_SYNC) == -1) first_error = errno;
        if (munmap(active.data, active.size) == -1 && first_error == 0) first_error = errno;
        active.data = nullptr;
    }
    if (active.fd != -1) {
        if (sync && fsync(active.fd) == -1 && first_error == 0) first_error = errno;
        if (::close(active.fd) == -1 && first_error == 0) first_error = errno;
        active.fd = -1;
    }
    active.size = 0;
    if (first_error != 0) {
        throw std::system_error(first_error, std::generic_category(),
                                "flush and close segment " + path.string());
    }
}

void MemoryMappedLog::scan_segment(const std::filesystem::path& path,
                                   std::uint32_t expected_id, bool is_latest,
                                   std::uint64_t& expected_offset,
                                   LogSegmentInfo& info) {
    const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd == -1) throw_errno("open", path);
    struct stat file_status{};
    if (fstat(fd, &file_status) == -1) {
        const int saved_errno = errno;
        ::close(fd);
        errno = saved_errno;
        throw_errno("fstat", path);
    }
    if (file_status.st_size < static_cast<off_t>(segment_header_size)) {
        ::close(fd);
        throw std::runtime_error("segment header is truncated: " + path.string());
    }

    std::array<unsigned char, segment_header_size> segment_header{};
    std::size_t bytes_read = 0;
    if (!pread_all(fd, segment_header.data(), segment_header.size(), 0, bytes_read) ||
        bytes_read != segment_header.size()) {
        ::close(fd);
        throw std::runtime_error("unable to read segment header: " + path.string());
    }
    if (!std::equal(segment_magic.begin(), segment_magic.end(), segment_header.begin()) ||
        get_u32(segment_header.data() + 8) != segment_version ||
        get_u32(segment_header.data() + 12) != expected_id ||
        get_u64(segment_header.data() + 24) != segment_size ||
        get_u32(segment_header.data() + 32) != segment_crc(segment_header.data())) {
        ::close(fd);
        throw std::runtime_error("invalid segment header: " + path.string());
    }

    const std::uint64_t base_offset = get_u64(segment_header.data() + 16);
    if (base_offset != expected_offset) {
        ::close(fd);
        throw std::runtime_error("segment base offset is discontinuous: " + path.string());
    }
    const std::uint64_t actual_size = static_cast<std::uint64_t>(file_status.st_size);
    if (actual_size > segment_size) {
        ::close(fd);
        throw std::runtime_error("segment file exceeds configured size: " + path.string());
    }

    info.first_offset = base_offset;
    std::uint64_t position = segment_header_size;
    bool truncate_tail = actual_size < segment_header_size;
    while (position + record_header_size <= actual_size) {
        std::array<unsigned char, record_header_size> record_header{};
        if (!pread_all(fd, record_header.data(), record_header.size(),
                       static_cast<off_t>(position), bytes_read)) {
            const int saved_errno = errno;
            ::close(fd);
            errno = saved_errno;
            throw_errno("pread", path);
        }
        if (bytes_read == 0) break;
        if (get_u32(record_header.data()) == 0) {
            if (!zero_tail(fd, position, actual_size - position, path)) {
                if (!is_latest) {
                    ::close(fd);
                    throw std::runtime_error("corrupt tail in sealed segment: " + path.string());
                }
                truncate_tail = true;
            }
            break;
        }

        bool valid = bytes_read == record_header.size() &&
                     get_u32(record_header.data()) == record_magic &&
                     get_u32(record_header.data() + 4) == record_version_size &&
                     get_u64(record_header.data() + 8) == expected_offset;
        const std::uint32_t payload_length = valid ? get_u32(record_header.data() + 24) : 0;
        const std::uint64_t total_record_size = record_header_size +
                                                static_cast<std::uint64_t>(payload_length);
        valid = valid && total_record_size <= segment_size - position &&
                position + total_record_size <= actual_size;
        std::string payload;
        if (valid) {
            payload.resize(payload_length);
            bytes_read = 0;
            if (payload_length != 0 &&
                !pread_all(fd, reinterpret_cast<unsigned char*>(payload.data()), payload_length,
                           static_cast<off_t>(position + record_header_size), bytes_read)) {
                const int saved_errno = errno;
                ::close(fd);
                errno = saved_errno;
                throw_errno("pread", path);
            }
            valid = bytes_read == payload_length &&
                    get_u32(record_header.data() + 28) == record_crc(record_header.data(), payload);
        }
        if (!valid) {
            if (!is_latest) {
                ::close(fd);
                throw std::runtime_error("corrupt record in sealed segment: " + path.string());
            }
            truncate_tail = true;
            break;
        }
        if (expected_offset > std::numeric_limits<std::uint64_t>::max() - total_record_size) {
            ::close(fd);
            throw std::runtime_error("record offsets overflow in segment: " + path.string());
        }

        Message message{expected_offset, get_u64(record_header.data() + 16),
                        payload_length, std::move(payload)};
        recent_cache.push(std::move(message));
        position += total_record_size;
        expected_offset += total_record_size;
    }

    if (position < actual_size && position + record_header_size > actual_size) {
        std::array<unsigned char, record_header_size> tail{};
        const std::size_t tail_size = static_cast<std::size_t>(actual_size - position);
        if (!pread_all(fd, tail.data(), tail_size, static_cast<off_t>(position), bytes_read)) {
            const int saved_errno = errno;
            ::close(fd);
            errno = saved_errno;
            throw_errno("pread", path);
        }
        const bool zero_padding = std::all_of(tail.begin(), tail.begin() + tail_size,
                                              [](unsigned char byte) { return byte == 0; });
        if (!zero_padding) {
            if (!is_latest) {
                ::close(fd);
                throw std::runtime_error("truncated record in sealed segment: " + path.string());
            }
            truncate_tail = true;
        }
    }
    if (truncate_tail && ftruncate(fd, static_cast<off_t>(position)) == -1) {
        const int saved_errno = errno;
        ::close(fd);
        errno = saved_errno;
        throw_errno("truncate recovered segment", path);
    }
    if (::close(fd) == -1) throw_errno("close", path);

    info.end_offset = expected_offset;
    info.valid_bytes = position;
}