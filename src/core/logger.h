#ifndef CORE_LOGGER_H
#define CORE_LOGGER_H

#include <iostream>
#include <chrono>
#include <mutex>
#include <string>

using namespace std;

// Centralized logging utility for consistent, timestamped output across the application.
// All logging calls use this static method to ensure uniform formatting and log levels.
class Logger {
    public:
        // Static method allows calling Logger::info() without instantiating the class.
        // Outputs timestamped INFO-level messages with the format: [timestamp] [INFO] message
        static void info(const string& message) {
            lock_guard<mutex> lock(output_mutex());
            auto now = chrono::system_clock::now();
            auto time = chrono::system_clock::to_time_t(now);
            cout << "[" << ctime(&time) << "] [INFO] " << message << '\n';
        }

    private:
        static mutex& output_mutex() {
            static mutex mutex_instance;
            return mutex_instance;
        }
};

#endif