#include "engine/execution/adapter/LiveMakerExecutionAdapter.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using trading_engine::execution::ILiveOrderSigner;
using trading_engine::execution::ILiveOrderTransport;
using trading_engine::execution::LiveExecutionConfig;
using trading_engine::execution::LiveMakerExecutionAdapter;
using trading_engine::execution::LiveOrderRequest;
using trading_engine::execution::LiveOrderSignResult;
using trading_engine::execution::LiveTransportCancelResult;
using trading_engine::execution::LiveTransportSubmitResult;
using trading_engine::execution::OrderSide;
using trading_engine::execution::QuoteSide;
using trading_engine::execution::SignedLiveOrder;
using trading_engine::risk::ApprovedQuote;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void expect_true(bool value, const std::string& field) {
    if (!value) {
        fail("expected true: " + field);
    }
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

LiveExecutionConfig enabled_config() {
    LiveExecutionConfig config;
    config.enabled = true;
    config.max_child_notional_tick = 1000;
    config.maker_post_only = true;
    config.maker_require_resting_ack = true;
    return config;
}

ApprovedQuote approved_quote(
    std::uint64_t approved_quote_id = 100,
    std::uint64_t idempotency_hash = 200
) {
    ApprovedQuote quote;
    quote.approved_quote_id = approved_quote_id;
    quote.quote_intent_id = 101;
    quote.quote_group_id = 77;
    quote.has_bid = true;
    quote.has_ask = true;
    quote.approved_ts_ns = 1000;
    quote.expires_at_ns = 10'000;
    quote.idempotency_hash = idempotency_hash;
    quote.policy_hash = 300;
    quote.snapshot_version_hash = 400;

    quote.bid.market_id = "market";
    quote.bid.asset_id = "asset";
    quote.bid.side = QuoteSide::Bid;
    quote.bid.price_tick = 45;
    quote.bid.quantity_lots = 10;

    quote.ask.market_id = "market";
    quote.ask.asset_id = "asset";
    quote.ask.side = QuoteSide::Ask;
    quote.ask.price_tick = 55;
    quote.ask.quantity_lots = 10;
    return quote;
}

class FakeSigner final : public ILiveOrderSigner {
public:
    std::vector<LiveOrderRequest> requests;

    [[nodiscard]] LiveOrderSignResult sign_order(
        const LiveOrderRequest& request
    ) override {
        requests.push_back(request);
        return {
            .ok = true,
            .order = SignedLiveOrder{
                .request_body_json = "{\"order\":\"signed\"}",
                .venue_order_id_hint = {}
            }
        };
    }
};

class FakeTransport final : public ILiveOrderTransport {
public:
    std::string submit_status = "live";
    std::vector<std::string> submitted_bodies;
    std::vector<std::string> canceled_order_ids;

    [[nodiscard]] LiveTransportSubmitResult submit_order(
        std::string_view request_body_json
    ) override {
        submitted_bodies.emplace_back(request_body_json);
        const auto id = std::string{"venue-"} +
            std::to_string(submitted_bodies.size());
        return {
            .ok = true,
            .venue_order_id = id,
            .venue_status = submit_status
        };
    }

    [[nodiscard]] LiveTransportCancelResult cancel_order(
        std::string_view venue_order_id
    ) override {
        canceled_order_ids.emplace_back(venue_order_id);
        return {
            .ok = true
        };
    }
};

void LiveMakerExecutionAdapter_DisabledByDefault() {
    LiveMakerExecutionAdapter adapter;

    const auto result = adapter.submit_approved_quote(approved_quote(), 1500);

    expect_true(!result.ok, "disabled");
}

void LiveMakerExecutionAdapter_SubmitsPostOnlyBidAsk() {
    FakeSigner signer;
    FakeTransport transport;
    LiveMakerExecutionAdapter adapter(
        enabled_config(),
        &signer,
        &transport
    );

    const auto result = adapter.submit_approved_quote(approved_quote(), 1500);

    expect_true(result.ok, "maker submit ok");
    expect_equal(signer.requests.size(), static_cast<std::size_t>(2), "signed");
    expect_equal(
        transport.submitted_bodies.size(),
        static_cast<std::size_t>(2),
        "submitted"
    );
    expect_true(signer.requests[0].post_only, "bid post only");
    expect_true(signer.requests[1].post_only, "ask post only");
    expect_equal(
        signer.requests[0].side,
        OrderSide::Buy,
        "bid becomes buy"
    );
    expect_equal(
        signer.requests[1].side,
        OrderSide::Sell,
        "ask becomes sell"
    );
    expect_equal(
        signer.requests[0].order_type,
        std::string{"GTC"},
        "order type"
    );
}

void LiveMakerExecutionAdapter_RejectsNonRestingAck() {
    FakeSigner signer;
    FakeTransport transport;
    transport.submit_status = "matched";
    LiveMakerExecutionAdapter adapter(
        enabled_config(),
        &signer,
        &transport
    );

    const auto result = adapter.submit_approved_quote(approved_quote(), 1500);

    expect_true(!result.ok, "matched rejected");
    expect_equal(
        transport.submitted_bodies.size(),
        static_cast<std::size_t>(1),
        "stops after first non-resting ack"
    );
}

void LiveMakerExecutionAdapter_DuplicateApprovedQuoteIgnored() {
    FakeSigner signer;
    FakeTransport transport;
    LiveMakerExecutionAdapter adapter(
        enabled_config(),
        &signer,
        &transport
    );

    const auto first = adapter.submit_approved_quote(approved_quote(), 1500);
    const auto duplicate = adapter.submit_approved_quote(approved_quote(), 1600);

    expect_true(first.ok, "first ok");
    expect_true(duplicate.ok, "duplicate ok");
    expect_true(duplicate.duplicate_ignored, "duplicate ignored");
    expect_equal(
        transport.submitted_bodies.size(),
        static_cast<std::size_t>(2),
        "no extra venue submit"
    );
}

void LiveMakerExecutionAdapter_ReplacesActiveQuote() {
    FakeSigner signer;
    FakeTransport transport;
    LiveMakerExecutionAdapter adapter(
        enabled_config(),
        &signer,
        &transport
    );

    const auto first = adapter.submit_approved_quote(approved_quote(), 1500);
    auto replacement = approved_quote(101, 201);
    replacement.bid.price_tick = 46;
    replacement.ask.price_tick = 56;
    const auto second = adapter.submit_approved_quote(replacement, 1700);

    expect_true(first.ok, "first ok");
    expect_true(second.ok, "second ok");
    expect_true(second.replaced, "replaced");
    expect_equal(
        transport.canceled_order_ids.size(),
        static_cast<std::size_t>(2),
        "old quote legs canceled"
    );
    expect_equal(
        transport.canceled_order_ids[0],
        std::string{"venue-1"},
        "first venue canceled"
    );
    expect_equal(
        transport.submitted_bodies.size(),
        static_cast<std::size_t>(4),
        "new quote submitted"
    );
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "LiveMakerExecutionAdapter_DisabledByDefault",
            &LiveMakerExecutionAdapter_DisabledByDefault
        },
        {
            "LiveMakerExecutionAdapter_SubmitsPostOnlyBidAsk",
            &LiveMakerExecutionAdapter_SubmitsPostOnlyBidAsk
        },
        {
            "LiveMakerExecutionAdapter_RejectsNonRestingAck",
            &LiveMakerExecutionAdapter_RejectsNonRestingAck
        },
        {
            "LiveMakerExecutionAdapter_DuplicateApprovedQuoteIgnored",
            &LiveMakerExecutionAdapter_DuplicateApprovedQuoteIgnored
        },
        {
            "LiveMakerExecutionAdapter_ReplacesActiveQuote",
            &LiveMakerExecutionAdapter_ReplacesActiveQuote
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
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " <test-name>\n";
        return 2;
    }
    return run_test(argv[1]);
}
