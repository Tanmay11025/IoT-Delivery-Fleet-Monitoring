#include "config.h"

#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace {
std::string trim(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

template <typename T>
T parse_number(const std::string& value, const std::string& key) {
    std::istringstream input(value);
    T parsed{};
    char extra = '\0';
    if (!(input >> parsed) || (input >> extra)) {
        throw std::runtime_error("Invalid value for " + key + ": " + value);
    }
    return parsed;
}

void apply(Config& config, const std::string& key, const std::string& value) {
    if (key == "port") config.port = parse_number<int>(value, key);
    else if (key == "backlog") config.backlog = parse_number<int>(value, key);
    else if (key == "epoll_worker_count") config.epoll_worker_count = parse_number<unsigned int>(value, key);
    else if (key == "thread_pool_size") config.thread_pool_size = parse_number<unsigned int>(value, key);
    else if (key == "task_queue_capacity") config.task_queue_capacity = parse_number<std::size_t>(value, key);
    else if (key == "log_path") config.log_path = value;
    else if (key == "rate_limit_capacity") config.rate_limit_capacity = parse_number<double>(value, key);
    else if (key == "rate_limit_refill_rate") config.rate_limit_refill_rate = parse_number<double>(value, key);
    else throw std::runtime_error("Unknown configuration key: " + key);
}

const std::unordered_map<std::string, std::string> options{
    {"--port", "port"},
    {"--backlog", "backlog"},
    {"--epoll-workers", "epoll_worker_count"},
    {"--pool-threads", "thread_pool_size"},
    {"--queue-capacity", "task_queue_capacity"},
    {"--log-path", "log_path"},
    {"--rate-limit-capacity", "rate_limit_capacity"},
    {"--rate-limit-refill-rate", "rate_limit_refill_rate"},
};
}

Config Config::load_file(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("Unable to open config file: " + path);
    }

    Config config;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(file, line)) {
        ++line_number;
        const auto comment = line.find('#');
        if (comment != std::string::npos) line.erase(comment);
        line = trim(line);
        if (line.empty()) continue;
        const auto separator = line.find('=');
        if (separator == std::string::npos) {
            throw std::runtime_error("Expected key=value at " + path + ":" +
                                     std::to_string(line_number));
        }
        const auto key = trim(line.substr(0, separator));
        const auto value = trim(line.substr(separator + 1));
        if (key.empty() || value.empty()) {
            throw std::runtime_error("Empty key or value at " + path + ":" +
                                     std::to_string(line_number));
        }
        apply(config, key, value);
    }
    config.validate();
    return config;
}

Config Config::from_args(int argc, char* argv[]) {
    std::string config_path;
    for (int index = 1; index < argc; ++index) {
        const std::string option(argv[index]);
        if (option == "--config") {
            if (++index >= argc) throw std::runtime_error("--config requires a path");
            config_path = argv[index];
        }
    }

    Config config = config_path.empty() ? Config{} : load_file(config_path);
    for (int index = 1; index < argc; ++index) {
        const std::string option(argv[index]);
        if (option == "--config") {
            ++index;
            continue;
        }
        const auto found = options.find(option);
        if (found == options.end()) {
            throw std::runtime_error("Unknown argument: " + option);
        }
        if (++index >= argc) throw std::runtime_error(option + " requires a value");
        apply(config, found->second, argv[index]);
    }
    config.validate();
    return config;
}

void Config::validate() const {
    if (port < 1 || port > 65535) throw std::runtime_error("port must be between 1 and 65535");
    if (backlog < 1) throw std::runtime_error("backlog must be positive");
    if (epoll_worker_count > 256) throw std::runtime_error("epoll_worker_count must be at most 256");
    if (thread_pool_size < 1 || thread_pool_size > 256) {
        throw std::runtime_error("thread_pool_size must be between 1 and 256");
    }
    if (task_queue_capacity < 1 || task_queue_capacity > (1U << 20)) {
        throw std::runtime_error("task_queue_capacity must be between 1 and 1048576");
    }
    if (!std::isfinite(rate_limit_capacity) || rate_limit_capacity < 1.0) {
        throw std::runtime_error("rate_limit_capacity must be finite and at least 1");
    }
    if (!std::isfinite(rate_limit_refill_rate) || rate_limit_refill_rate < 0.0) {
        throw std::runtime_error("rate_limit_refill_rate must be finite and non-negative");
    }
}