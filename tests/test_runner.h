#pragma once

#include <cstddef>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

class TestRunner {
public:
    TestRunner(int argc, char* argv[]) {
        if (argc == 2) selected_case = argv[1];
        else if (argc > 2) invalid_arguments = true;
    }

    template <typename Test>
    void run(const char* name, Test test) {
        if (!selected_case.empty() && selected_case != name) return;
        if (selected_case == name) selected_case_found = true;
        try {
            test();
            ++passed;
            std::cout << "PASS " << name << std::endl;
        } catch (const std::exception& error) {
            ++failed;
            std::cerr << "FAIL " << name << ": " << error.what() << std::endl;
        } catch (...) {
            ++failed;
            std::cerr << "FAIL " << name << ": unknown exception" << std::endl;
        }
    }

    int finish() const {
        if (invalid_arguments || (!selected_case.empty() && !selected_case_found)) {
            std::cerr << "FAIL invalid test selection";
            if (!selected_case.empty()) std::cerr << ": " << selected_case;
            std::cerr << std::endl;
            return 2;
        }
        std::cout << "RESULT " << passed << " passed, " << failed << " failed"
                  << std::endl;
        return failed == 0 ? 0 : 1;
    }

    static void check(bool condition, const char* expression,
                      const char* file, int line) {
        if (condition) return;
        throw std::runtime_error(std::string(file) + ":" + std::to_string(line) +
                                 ": check failed: " + expression);
    }

private:
    std::size_t passed = 0;
    std::size_t failed = 0;
    std::string selected_case;
    bool selected_case_found = false;
    bool invalid_arguments = false;
};

#ifdef assert
#undef assert
#endif
#define assert(expression) TestRunner::check(static_cast<bool>(expression), #expression, __FILE__, __LINE__)