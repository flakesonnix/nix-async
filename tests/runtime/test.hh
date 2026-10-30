#pragma once

#include <functional>
#include <future>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

// Unlike assert(), checks remain active in the flake's Release build.
#define CHECK(expression) \
    do { if (!(expression)) throw std::runtime_error(std::string(__FILE__) + ":" + \
        std::to_string(__LINE__) + ": " #expression); } while (false)

template<typename Exception, typename F>
void expect_throw(F && work)
{
    try {
        work();
    } catch (const Exception &) {
        return;
    }
    throw std::runtime_error("expected exception was not thrown");
}

template<typename F>
void expect_future_error(std::future_errc code, F && work)
{
    try {
        work();
    } catch (const std::future_error & error) {
        CHECK(error.code() == std::make_error_code(code));
        return;
    }
    throw std::runtime_error("expected future_error was not thrown");
}

struct TestCase { std::string_view name; void (*run)(); };

inline int run_test(int argc, char ** argv, std::initializer_list<TestCase> tests)
{
    try {
        if (argc == 2)
            for (const auto & test : tests)
                if (test.name == argv[1]) {
                    test.run();
                    return 0;
                }
        throw std::runtime_error("unknown test case");
    } catch (const std::exception & error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
