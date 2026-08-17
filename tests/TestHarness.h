#pragma once

#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vidscope::test {

struct TestCase final {
    std::string name;
    std::function<void()> function;
};

inline std::vector<TestCase>& registry()
{
    static std::vector<TestCase> tests;
    return tests;
}

class Registrar final {
public:
    Registrar(std::string name, std::function<void()> function)
    {
        registry().push_back({std::move(name), std::move(function)});
    }
};

[[noreturn]] inline void fail(
    std::string_view expression,
    std::string_view file,
    int line,
    std::string_view detail = {})
{
    std::ostringstream message;
    message << file << ':' << line << ": requirement failed: " << expression;
    if (!detail.empty()) {
        message << " (" << detail << ')';
    }
    throw std::runtime_error(message.str());
}

inline int runAll()
{
    int failed = 0;
    for (const auto& test : registry()) {
        try {
            test.function();
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failed;
            std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
        } catch (...) {
            ++failed;
            std::cerr << "[FAIL] " << test.name << ": unknown exception\n";
        }
    }
    std::cout << (registry().size() - static_cast<std::size_t>(failed)) << '/'
              << registry().size() << " tests passed\n";
    return failed == 0 ? 0 : 1;
}

} // namespace vidscope::test

#define VIDSCOPE_TEST(Name)                                                                               \
    static void Name();                                                                             \
    static ::vidscope::test::Registrar Name##_registrar(#Name, &Name);                            \
    static void Name()

#define VIDSCOPE_REQUIRE(Expression)                                                                      \
    do {                                                                                            \
        if (!(Expression)) {                                                                        \
            ::vidscope::test::fail(#Expression, __FILE__, __LINE__);                              \
        }                                                                                           \
    } while (false)

#define VIDSCOPE_REQUIRE_MESSAGE(Expression, Detail)                                                      \
    do {                                                                                            \
        if (!(Expression)) {                                                                        \
            ::vidscope::test::fail(#Expression, __FILE__, __LINE__, Detail);                      \
        }                                                                                           \
    } while (false)
