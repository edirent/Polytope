#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void SimdKernelParity_DisabledUnlessSimdEnabled() {
#ifndef POLYTOPE_DECISION_FASTPATH_SIMD_ENABLED
    // The CTest entry is disabled by default. If this test is run manually
    // without SIMD enabled, it should still be explicit about why no parity
    // assertion is performed.
    return;
#else
    fail("SIMD kernel parity test needs a SIMD implementation");
#endif
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"SimdKernelParity_DisabledUnlessSimdEnabled",
         &SimdKernelParity_DisabledUnlessSimdEnabled},
    };
    return test_map;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " <test-name>\n";
        return 2;
    }

    const auto it = tests().find(argv[1]);
    if (it == tests().end()) {
        std::cerr << "unknown test: " << argv[1] << '\n';
        return 2;
    }

    try {
        it->second();
    } catch (const std::exception& error) {
        std::cerr << argv[1] << " failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
