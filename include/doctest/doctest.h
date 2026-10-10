#pragma once

#include <exception>
#include <functional>
#include <iostream>
#include <utility>
#include <vector>

namespace doctest_compat {
struct Failure : std::exception {
    const char* what() const noexcept override { return "test assertion failed"; }
};
struct Test {
    const char* name;
    std::function<void()> run;
};
inline std::vector<Test>& tests() {
    static std::vector<Test> value;
    return value;
}
struct Registrar {
    Registrar(const char* name, std::function<void()> run) { tests().push_back({name, std::move(run)}); }
};
inline int run() {
    int failures = 0;
    for (const auto& test : tests()) {
        try {
            test.run();
            std::cout << "[pass] " << test.name << "\n";
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << test.name << ": " << error.what() << "\n";
        }
    }
    return failures;
}
template <typename Exception, typename Function>
void checkThrows(Function&& function) {
    try {
        function();
    } catch (const Exception&) {
        return;
    }
    throw Failure();
}
} // namespace doctest_compat

#define DOCTEST_COMPAT_JOIN_IMPL(a, b) a##b
#define DOCTEST_COMPAT_JOIN(a, b) DOCTEST_COMPAT_JOIN_IMPL(a, b)
#define TEST_CASE(name) \
    static void DOCTEST_COMPAT_JOIN(test_, __LINE__)(); \
    static doctest_compat::Registrar DOCTEST_COMPAT_JOIN(registrar_, __LINE__)(name, DOCTEST_COMPAT_JOIN(test_, __LINE__)); \
    static void DOCTEST_COMPAT_JOIN(test_, __LINE__)()
#define CHECK(condition) \
    do { \
        if (!(condition)) throw doctest_compat::Failure(); \
    } while (false)
#define CHECK_THROWS_AS(expression, exception) \
    do { \
        doctest_compat::checkThrows<exception>([&] { expression; }); \
    } while (false)

#ifdef DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
int main() { return doctest_compat::run(); }
#endif
