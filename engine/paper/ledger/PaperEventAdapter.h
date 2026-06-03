#pragma once

#include "engine/execution/public/ExecutionReport.h"
#include "engine/execution/public/MakerExecutionTypes.h"
#include "engine/execution/public/OrderPlan.h"
#include "engine/execution/public/ReservationDisposition.h"
#include "engine/paper/ledger/FillApplication.h"
#include "engine/paper/ledger/MakerFillApplication.h"
#include "engine/paper/public/PaperEvent.h"
#include "engine/risk/public/ApprovedIntent.h"
#include "engine/risk/public/ApprovedQuote.h"
#include "engine/risk/public/QuoteRiskDecision.h"
#include "engine/risk/public/RiskDecision.h"
#include "engine/signal/public/OpportunityIntent.h"
#include "engine/state/MarketStateSnapshot.h"
#include "engine/strategy/market_making/public/QuoteIntent.h"

#include <cstdint>
#include <unordered_map>

namespace trading_engine::paper {

struct PaperEventAdapterResult {
    PaperEvent event;

    bool has_fill = false;
    FillApplication fill;

    bool has_paper_fill = false;
    PaperFill paper_fill;
};

class PaperEventAdapter {
public:
    [[nodiscard]] PaperEventAdapterResult observe(
        const trading_engine::signal::OpportunityIntent& intent
    );

    [[nodiscard]] PaperEventAdapterResult observe(
        const trading_engine::risk::RiskDecision& decision
    );

    [[nodiscard]] PaperEventAdapterResult observe(
        const trading_engine::strategy::market_making::QuoteIntent& intent
    );

    [[nodiscard]] PaperEventAdapterResult observe(
        const trading_engine::risk::QuoteRiskDecision& decision
    );

    [[nodiscard]] PaperEventAdapterResult observe(
        const trading_engine::risk::ApprovedIntent& approved
    );

    [[nodiscard]] PaperEventAdapterResult observe(
        const trading_engine::risk::ApprovedQuote& approved
    );

    [[nodiscard]] PaperEventAdapterResult observe(
        const trading_engine::execution::OrderPlan& plan
    );

    [[nodiscard]] PaperEventAdapterResult observe(
        const trading_engine::execution::ExecutionReport& report
    );

    [[nodiscard]] PaperEventAdapterResult observe(
        const trading_engine::execution::MakerExecutionReport& report
    );

    [[nodiscard]] PaperEventAdapterResult observe(
        const trading_engine::execution::ReservationDisposition& disposition
    );

    [[nodiscard]] PaperEventAdapterResult observe_mark_update(
        const trading_engine::state::MarketStateSnapshot& snapshot
    );

private:
    [[nodiscard]] PaperEvent make_event(
        PaperEventType type,
        std::uint64_t ts_ns
    );

    [[nodiscard]] static std::uint64_t child_key(
        std::uint64_t plan_id,
        std::uint64_t child_order_id
    ) noexcept;

    std::uint64_t next_seq_no_ = 1;
    std::unordered_map<std::uint64_t, trading_engine::execution::ChildOrder>
        child_orders_;
};

}  // namespace trading_engine::paper
