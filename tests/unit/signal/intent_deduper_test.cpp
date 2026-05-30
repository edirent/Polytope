#include "engine/signal/publish/IntentDeduper.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::signal::IntentDeduper;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void expect_true(bool value, const std::string& field) {
    if (!value) {
        fail("expected true: " + field);
    }
}

void expect_false(bool value, const std::string& field) {
    if (value) {
        fail("expected false: " + field);
    }
}

void IntentDeduper_AllowsFirstIntent() {
    IntentDeduper deduper(100);

    expect_false(deduper.seen_recently("key", 10), "first intent");
}

void IntentDeduper_RejectsDuplicateWithinTtl() {
    IntentDeduper deduper(100);
    deduper.mark_seen("key", 10);

    expect_true(deduper.seen_recently("key", 99), "duplicate");
}

void IntentDeduper_AllowsAfterTtl() {
    IntentDeduper deduper(100);
    deduper.mark_seen("key", 10);

    expect_false(deduper.seen_recently("key", 110), "expired");
}

void IntentDeduper_DifferentKeyAllowed() {
    IntentDeduper deduper(100);
    deduper.mark_seen("key_a", 10);

    expect_false(deduper.seen_recently("key_b", 20), "different key");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "IntentDeduper_AllowsFirstIntent",
            &IntentDeduper_AllowsFirstIntent
        },
        {
            "IntentDeduper_RejectsDuplicateWithinTtl",
            &IntentDeduper_RejectsDuplicateWithinTtl
        },
        {
            "IntentDeduper_AllowsAfterTtl",
            &IntentDeduper_AllowsAfterTtl
        },
        {
            "IntentDeduper_DifferentKeyAllowed",
            &IntentDeduper_DifferentKeyAllowed
        }
    };
    return test_map;
}

int run_test(const std::string& name) {
    const auto it = tests().find(name);
    if (it == tests().end()) {
        std::cerr << "unknown test: " << name << '\n';
        return 2;
    }

    try {
        it->second();
    } catch (const std::exception& error) {
        std::cerr << name << " failed: " << error.what() << '\n';
        return 1;
    }

    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " <test-name>\n";
        return 2;
    }
    return run_test(argv[1]);
}
