#ifndef CORE_LOGGER_H
#define CORE_LOGGER_H

#include <iostream>
#include <chrono>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>

using namespace std;

// Centralized logging utility for consistent, timestamped output across the application.
// All logging calls use this static method to ensure uniform formatting and log levels.
class Logger {
    public:
        static void set_file(const string& path) {
            lock_guard<mutex> lock(output_mutex());
            if (path.empty()) {
                log_file().close();
                return;
            }
            log_file().open(path, ios::out | ios::app);
            if (!log_file()) {
                throw runtime_error("Unable to open log file: " + path);
            }
        }

        // Static method allows calling Logger::info() without instantiating the class.
        // Outputs timestamped INFO-level messages with the format: [timestamp] [INFO] message
        static void info(const string& message) {
            lock_guard<mutex> lock(output_mutex());
            auto now = chrono::system_clock::now();
            auto time = chrono::system_clock::to_time_t(now);
            ostream& output = log_file().is_open() ? static_cast<ostream&>(log_file()) : cout;
            output << "[" << ctime(&time) << "] [INFO] " << message << '\n';
            output.flush();
        }

    private:
        static mutex& output_mutex() {
            static mutex mutex_instance;
            return mutex_instance;
        }

        static ofstream& log_file() {
            static ofstream file;
            return file;
        }
};

#endif