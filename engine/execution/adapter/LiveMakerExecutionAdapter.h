#pragma once

#include "engine/execution/adapter/LiveOrderBridge.h"
#include "engine/execution/public/MakerExecutionTypes.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace trading_engine::execution {

class LiveMakerExecutionAdapter {
public:
    LiveMakerExecutionAdapter() = default;
    LiveMakerExecutionAdapter(
        LiveExecutionConfig config,
        ILiveOrderSigner* signer,
        ILiveOrderTransport* transport
    );

    [[nodiscard]] MakerSubmitResult submit_approved_quote(
        const risk::ApprovedQuote& quote,
        std::uint64_t now_ns
    );

    [[nodiscard]] MakerCancelResult cancel_quote_group(
        std::uint64_t quote_group_id,
        std::uint64_t now_ns
    );

private:
    struct AcceptedMakerOrder {
        std::uint64_t quote_id = 0;
        QuoteSide side = QuoteSide::Bid;
        std::string venue_order_id;
    };

    [[nodiscard]] bool side_allowed(OrderSide side) const noexcept;
    [[nodiscard]] bool notional_allowed(
        std::int64_t quantity_lots,
        std::int64_t price_tick
    ) const noexcept;

    [[nodiscard]] LiveOrderRequest make_request(
        const risk::ApprovedQuote& quote,
        const risk::QuoteLeg& leg,
        QuoteSide side,
        std::uint64_t quote_id
    ) const;

    [[nodiscard]] MakerSubmitResult submit_leg(
        const risk::ApprovedQuote& quote,
        const risk::QuoteLeg& leg,
        QuoteSide side,
        std::uint64_t quote_id,
        std::uint64_t now_ns
    );

    [[nodiscard]] MakerCancelResult cancel_existing_group(
        std::uint64_t quote_group_id
    );

    LiveExecutionConfig config_;
    ILiveOrderSigner* signer_ = nullptr;
    ILiveOrderTransport* transport_ = nullptr;

    std::unordered_map<std::uint64_t, std::vector<AcceptedMakerOrder>>
        accepted_orders_by_group_;
    std::unordered_set<std::uint64_t> submitted_idempotency_hashes_;
};

}  // namespace trading_engine::execution
