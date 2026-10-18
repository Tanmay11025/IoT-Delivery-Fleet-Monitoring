#include "storage/mmap_log.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <unordered_set>
#include <string>

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "usage: storage_recovery_verify DIRECTORY EXPECTED_FILE\n";
        return EXIT_FAILURE;
    }

    const std::string directory = argv[1];
    std::ifstream expected_file(argv[2]);
    if (!expected_file) {
        std::cerr << "unable to open expected-record file\n";
        return EXIT_FAILURE;
    }
    std::unordered_set<std::string> expected;
    std::string payload;
    while (std::getline(expected_file, payload)) expected.insert(payload);
    std::size_t recovered_count = 0;
    MemoryMappedLog log;
    try {
        log.open(directory, 64ULL * 1024ULL * 1024ULL, 1U << 20);
        const auto messages = log.get_recent(1U << 20);
        recovered_count = messages.size();
        if (recovered_count != expected.size()) {
            std::cerr << "expected exactly " << expected.size() << " recovered records, got "
                      << recovered_count << '\n';
            return EXIT_FAILURE;
        }
        std::unordered_set<std::string> recovered;
        for (const auto& message : messages) recovered.insert(message.payload);
        for (const auto& acknowledged : expected) {
            if (recovered.find(acknowledged) == recovered.end()) {
                std::cerr << "acknowledged record missing after recovery: " << acknowledged
                          << '\n';
                return EXIT_FAILURE;
            }
        }
        if (recovered.size() != expected.size()) {
            std::cerr << "recovered log contains unexpected records\n";
            return EXIT_FAILURE;
        }
        log.close();
    } catch (const std::exception& error) {
        std::cerr << "recovery verification failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "acknowledged=" << expected.size() << " records verified; recovered="
              << recovered_count << " total records\n";
    return EXIT_SUCCESS;
}