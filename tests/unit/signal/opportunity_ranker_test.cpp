#include "engine/signal/rank/OpportunityRanker.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using trading_engine::signal::IntentStatus;
using trading_engine::signal::OpportunityIntent;
using trading_engine::signal::OpportunityRanker;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

template <typename Actual, typename Expected>
void expect_equal(
    const Actual& actual,
    const Expected& expected,
    const std::string& field
) {
    if (!(actual == expected)) {
        fail("mismatch: " + field);
    }
}

OpportunityIntent intent(
    std::uint64_t intent_id,
    std::uint64_t bundle_id,
    std::int64_t edge_tick,
    IntentStatus status
) {
    OpportunityIntent out;
    out.intent_id = intent_id;
    out.bundle_id = bundle_id;
    out.estimated_edge_tick = edge_tick;
    out.status = status;
    return out;
}

std::vector<std::uint64_t> intent_ids(
    const std::vector<OpportunityIntent>& intents
) {
    std::vector<std::uint64_t> out;
    out.reserve(intents.size());
    for (const auto& item : intents) {
        out.push_back(item.intent_id);
    }
    return out;
}

void OpportunityRanker_SortsPaperOpportunitiesFirst() {
    std::vector<OpportunityIntent> intents{
        intent(1, 1, 1'000'000, IntentStatus::RejectedLowEdge),
        intent(2, 2, 10, IntentStatus::PaperOpportunity),
        intent(3, 3, 500'000, IntentStatus::RejectedBadMarketState)
    };

    OpportunityRanker{}.rank(&intents);

    expect_equal(intents[0].intent_id, 2ULL, "paper first");
}

void OpportunityRanker_SortsByEdgeDescending() {
    std::vector<OpportunityIntent> intents{
        intent(1, 1, 10, IntentStatus::PaperOpportunity),
        intent(2, 2, 30, IntentStatus::PaperOpportunity),
        intent(3, 3, 20, IntentStatus::PaperOpportunity)
    };

    OpportunityRanker{}.rank(&intents);

    expect_equal(intent_ids(intents), std::vector<std::uint64_t>{2, 3, 1}, "order");
}

void OpportunityRanker_StableTieBreakByBundleId() {
    std::vector<OpportunityIntent> intents{
        intent(1, 30, 100, IntentStatus::PaperOpportunity),
        intent(2, 10, 100, IntentStatus::PaperOpportunity),
        intent(3, 20, 100, IntentStatus::PaperOpportunity)
    };

    OpportunityRanker{}.rank(&intents);

    expect_equal(intent_ids(intents), std::vector<std::uint64_t>{2, 3, 1}, "order");
}

void OpportunityRanker_DeterministicOrdering() {
    std::vector<OpportunityIntent> first{
        intent(4, 2, 100, IntentStatus::RejectedLowEdge),
        intent(3, 1, 100, IntentStatus::PaperOpportunity),
        intent(2, 1, 100, IntentStatus::PaperOpportunity),
        intent(1, 1, 200, IntentStatus::PaperOpportunity)
    };
    std::vector<OpportunityIntent> second{
        intent(1, 1, 200, IntentStatus::PaperOpportunity),
        intent(4, 2, 100, IntentStatus::RejectedLowEdge),
        intent(2, 1, 100, IntentStatus::PaperOpportunity),
        intent(3, 1, 100, IntentStatus::PaperOpportunity)
    };

    OpportunityRanker ranker;
    ranker.rank(&first);
    ranker.rank(&second);

    const std::vector<std::uint64_t> expected{1, 2, 3, 4};
    expect_equal(intent_ids(first), expected, "first order");
    expect_equal(intent_ids(second), expected, "second order");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "OpportunityRanker_SortsPaperOpportunitiesFirst",
            &OpportunityRanker_SortsPaperOpportunitiesFirst
        },
        {
            "OpportunityRanker_SortsByEdgeDescending",
            &OpportunityRanker_SortsByEdgeDescending
        },
        {
            "OpportunityRanker_StableTieBreakByBundleId",
            &OpportunityRanker_StableTieBreakByBundleId
        },
        {
            "OpportunityRanker_DeterministicOrdering",
            &OpportunityRanker_DeterministicOrdering
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
