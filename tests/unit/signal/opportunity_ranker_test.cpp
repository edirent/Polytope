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
    IntentStatus status,
    std::int64_t total_edge_tick = 0,
    std::int64_t edge_bps = 0,
    std::int64_t bundle_qty = 0
) {
    OpportunityIntent out;
    out.intent_id = intent_id;
    out.bundle_id = bundle_id;
    out.status = status;
    out.estimated_edge_tick = total_edge_tick;
    out.total_edge_tick = total_edge_tick;
    out.edge_bps = edge_bps;
    out.bundle_qty = bundle_qty;
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
        intent(1, 1, IntentStatus::RejectedLowEdge, 1'000'000, 10, 10),
        intent(2, 2, IntentStatus::PaperOpportunity, 10, 1, 1),
        intent(3, 3, IntentStatus::RejectedBadMarketState, 500'000, 5, 5)
    };

    OpportunityRanker{}.rank(&intents);

    expect_equal(intents[0].intent_id, 2ULL, "paper first");
}

void OpportunityRanker_SortsByTotalEdge() {
    std::vector<OpportunityIntent> intents{
        intent(1, 1, IntentStatus::PaperOpportunity, 10, 100, 100),
        intent(2, 2, IntentStatus::PaperOpportunity, 30, 1, 1),
        intent(3, 3, IntentStatus::PaperOpportunity, 20, 1'000, 1'000)
    };

    OpportunityRanker{}.rank(&intents);

    expect_equal(intent_ids(intents), std::vector<std::uint64_t>{2, 3, 1}, "order");
}

void OpportunityRanker_TieBreaksByEdgeBps() {
    std::vector<OpportunityIntent> intents{
        intent(1, 1, IntentStatus::PaperOpportunity, 100, 10, 100),
        intent(2, 2, IntentStatus::PaperOpportunity, 100, 30, 1),
        intent(3, 3, IntentStatus::PaperOpportunity, 100, 20, 1'000)
    };

    OpportunityRanker{}.rank(&intents);

    expect_equal(intent_ids(intents), std::vector<std::uint64_t>{2, 3, 1}, "order");
}

void OpportunityRanker_TieBreaksByBundleQty() {
    std::vector<OpportunityIntent> intents{
        intent(1, 1, IntentStatus::PaperOpportunity, 100, 10, 10),
        intent(2, 2, IntentStatus::PaperOpportunity, 100, 10, 30),
        intent(3, 3, IntentStatus::PaperOpportunity, 100, 10, 20)
    };

    OpportunityRanker{}.rank(&intents);

    expect_equal(intent_ids(intents), std::vector<std::uint64_t>{2, 3, 1}, "order");
}

void OpportunityRanker_StableFinalTieBreak() {
    std::vector<OpportunityIntent> first{
        intent(4, 2, IntentStatus::RejectedLowEdge, 100, 10, 10),
        intent(3, 1, IntentStatus::PaperOpportunity, 100, 10, 10),
        intent(2, 1, IntentStatus::PaperOpportunity, 100, 10, 10),
        intent(1, 1, IntentStatus::PaperOpportunity, 200, 1, 1)
    };
    std::vector<OpportunityIntent> second{
        intent(1, 1, IntentStatus::PaperOpportunity, 200, 1, 1),
        intent(4, 2, IntentStatus::RejectedLowEdge, 100, 10, 10),
        intent(2, 1, IntentStatus::PaperOpportunity, 100, 10, 10),
        intent(3, 1, IntentStatus::PaperOpportunity, 100, 10, 10)
    };

    OpportunityRanker ranker;
    ranker.rank(&first);
    ranker.rank(&second);

    const std::vector<std::uint64_t> expected{1, 2, 3, 4};
    expect_equal(intent_ids(first), expected, "first order");
    expect_equal(intent_ids(second), expected, "second order");
}

void OpportunityRanker_DoesNotMutateIntentContent() {
    std::vector<OpportunityIntent> intents{
        intent(1, 2, IntentStatus::PaperOpportunity, 100, 20, 10),
        intent(2, 1, IntentStatus::PaperOpportunity, 200, 10, 5)
    };
    intents[0].idempotency_key = "key-1";
    intents[0].proof_ref = "proof-1";
    intents[1].idempotency_key = "key-2";
    intents[1].proof_ref = "proof-2";

    OpportunityRanker{}.rank(&intents);

    expect_equal(intents[0].intent_id, 2ULL, "first id");
    expect_equal(intents[0].idempotency_key, std::string{"key-2"}, "first key");
    expect_equal(intents[0].proof_ref, std::string{"proof-2"}, "first proof");
    expect_equal(intents[1].intent_id, 1ULL, "second id");
    expect_equal(intents[1].idempotency_key, std::string{"key-1"}, "second key");
    expect_equal(intents[1].proof_ref, std::string{"proof-1"}, "second proof");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "OpportunityRanker_SortsPaperOpportunitiesFirst",
            &OpportunityRanker_SortsPaperOpportunitiesFirst
        },
        {
            "OpportunityRanker_SortsByTotalEdge",
            &OpportunityRanker_SortsByTotalEdge
        },
        {
            "OpportunityRanker_TieBreaksByEdgeBps",
            &OpportunityRanker_TieBreaksByEdgeBps
        },
        {
            "OpportunityRanker_TieBreaksByBundleQty",
            &OpportunityRanker_TieBreaksByBundleQty
        },
        {
            "OpportunityRanker_StableFinalTieBreak",
            &OpportunityRanker_StableFinalTieBreak
        },
        {
            "OpportunityRanker_DoesNotMutateIntentContent",
            &OpportunityRanker_DoesNotMutateIntentContent
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
