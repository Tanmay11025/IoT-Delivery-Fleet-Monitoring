#include "core/config.h"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
#include "test_runner.h"

void config_reads_file_and_cli_overrides() {
    const std::string path = "/tmp/gateway-config-test.conf";
    {
        std::ofstream file(path);
        file << "port=9001\nthread_pool_size=3\nrate_limit_capacity=8\n"
            "storage_directory=temporary-log\nstorage_segment_size=4096\n";
    }
    std::vector<std::string> arguments{
        "gateway", "--config", path, "--port", "9002", "--pool-threads", "5"};
    std::vector<char*> argv;
    for (auto& argument : arguments) argv.push_back(argument.data());

    const Config config = Config::from_args(static_cast<int>(argv.size()), argv.data());
    assert(config.port == 9002);
    assert(config.thread_pool_size == 5);
    assert(config.rate_limit_capacity == 8.0);
    assert(config.storage_directory == "temporary-log");
    assert(config.storage_segment_size == 4096);
    std::remove(path.c_str());
}

void config_rejects_invalid_values_and_unknown_keys() {
    Config config;
    config.port = 0;
    bool invalid_port_rejected = false;
    try {
        config.validate();
    } catch (const std::runtime_error&) {
        invalid_port_rejected = true;
    }
    assert(invalid_port_rejected);

    const std::string path = "/tmp/gateway-config-invalid-test.conf";
    {
        std::ofstream file(path);
        file << "unknown_setting=1\n";
    }
    bool unknown_key_rejected = false;
    try {
        Config::load_file(path);
    } catch (const std::runtime_error&) {
        unknown_key_rejected = true;
    }
    std::remove(path.c_str());
    assert(unknown_key_rejected);
}

int main(int argc, char* argv[]) {
    TestRunner runner(argc, argv);
    runner.run("config_reads_file_and_cli_overrides", config_reads_file_and_cli_overrides);
    runner.run("config_rejects_invalid_values_and_unknown_keys",
               config_rejects_invalid_values_and_unknown_keys);
    return runner.finish();
}