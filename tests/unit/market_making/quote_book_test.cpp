#include "engine/strategy/market_making/state/QuoteBook.h"
#include "engine/strategy/market_making/state/QuoteStateMachine.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using namespace trading_engine::strategy::market_making;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

template <typename Actual, typename Expected>
void expect_equal(const Actual& actual, const Expected& expected, const std::string& field) {
    if (!(actual == expected)) {
        fail("mismatch: " + field);
    }
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

ActiveQuoteState quote(std::uint32_t asset, std::uint64_t id) {
    ActiveQuoteState state;
    state.asset_index = asset;
    state.quote_intent_id = id;
    state.quote_group_id = id + 100;
    state.status = QuoteStatus::ActivePaper;
    state.idempotency_hash = id + 200;
    return state;
}

void QuoteBook_UpsertsAndFinds() {
    QuoteBook book;
    book.upsert(quote(7, 1));
    const auto* found = book.find(7);
    expect_true(found != nullptr, "found");
    expect_equal(found->quote_intent_id, 1ULL, "id");
}

void QuoteBook_Removes() {
    QuoteBook book;
    book.upsert(quote(7, 1));
    expect_true(book.remove(7), "removed");
    expect_equal(book.find(7), static_cast<const ActiveQuoteState*>(nullptr), "missing");
}

void QuoteBook_HashDeterministic() {
    QuoteBook a;
    QuoteBook b;
    a.upsert(quote(7, 1));
    a.upsert(quote(8, 2));
    b.upsert(quote(8, 2));
    b.upsert(quote(7, 1));
    expect_equal(a.hash(), b.hash(), "hash");
}

void QuoteStateMachine_RejectsTerminalTransition() {
    expect_false(
        can_transition_quote_status(QuoteStatus::Cancelled, QuoteStatus::ActivePaper),
        "terminal"
    );
    expect_true(
        can_transition_quote_status(QuoteStatus::ActivePaper, QuoteStatus::CancelRequested),
        "cancel request"
    );
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> map{
        {"QuoteBook_UpsertsAndFinds", &QuoteBook_UpsertsAndFinds},
        {"QuoteBook_Removes", &QuoteBook_Removes},
        {"QuoteBook_HashDeterministic", &QuoteBook_HashDeterministic},
        {"QuoteStateMachine_RejectsTerminalTransition", &QuoteStateMachine_RejectsTerminalTransition}
    };
    return map;
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
