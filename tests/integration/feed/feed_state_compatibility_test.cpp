#include "feed/state/EntityStateStore.h"
#include "feed/state/StateHasher.h"
#include "state/EntityStateStore.h"
#include "state/StateHasher.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>

namespace {

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void FeedStateCompatibility_OldPathAliasesStateTypes() {
    static_assert(
        std::is_same_v<
            trading_engine::feed::EntityStateStore,
            trading_engine::state::EntityStateStore
        >
    );
    static_assert(
        std::is_same_v<
            trading_engine::feed::EntityState,
            trading_engine::state::EntityState
        >
    );
    static_assert(
        std::is_same_v<
            trading_engine::feed::OrderBookState,
            trading_engine::state::OrderBookState
        >
    );
    static_assert(
        std::is_same_v<
            trading_engine::feed::StateHasher,
            trading_engine::state::StateHasher
        >
    );

    trading_engine::feed::EntityStateStore store;
    if (store.entity_count() != 0) {
        fail("compatibility store should default to empty");
    }
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "FeedStateCompatibility_OldPathAliasesStateTypes",
            &FeedStateCompatibility_OldPathAliasesStateTypes
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

    std::cout << name << " passed\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2) {
        return run_test(argv[1]);
    }

    int failures = 0;
    for (const auto& [name, _] : tests()) {
        failures += run_test(name) == 0 ? 0 : 1;
    }

    return failures == 0 ? 0 : 1;
}
