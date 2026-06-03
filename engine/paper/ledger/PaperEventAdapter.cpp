#include "engine/paper/ledger/PaperEventAdapter.h"

#include <algorithm>

namespace trading_engine::paper {
namespace {

[[nodiscard]] bool is_fill_report(
    trading_engine::execution::ChildOrderStatus status
) noexcept {
    using trading_engine::execution::ChildOrderStatus;
    return status == ChildOrderStatus::Filled ||
           status == ChildOrderStatus::PartiallyFilled;
}

}  // namespace

PaperEventAdapterResult PaperEventAdapter::observe(
    const trading_engine::signal::OpportunityIntent& intent
) {
    return {
        .event = make_event(
            PaperEventType::OpportunityIntentObserved,
            intent.created_ts_ns
        )
    };
}

PaperEventAdapterResult PaperEventAdapter::observe(
    const trading_engine::risk::RiskDecision&
) {
    return {
        .event = make_event(PaperEventType::RiskDecisionObserved, 0)
    };
}

PaperEventAdapterResult PaperEventAdapter::observe(
    const trading_engine::strategy::market_making::QuoteIntent& intent
) {
    return {
        .event = make_event(
            PaperEventType::QuoteIntentObserved,
            intent.created_ts_ns
        )
    };
}

PaperEventAdapterResult PaperEventAdapter::observe(
    const trading_engine::risk::QuoteRiskDecision& decision
) {
    return {
        .event = make_event(
            PaperEventType::QuoteRiskDecisionObserved,
            decision.decision_ts_ns
        )
    };
}

PaperEventAdapterResult PaperEventAdapter::observe(
    const trading_engine::risk::ApprovedIntent& approved
) {
    return {
        .event = make_event(
            PaperEventType::ApprovedIntentObserved,
            approved.approved_at_ns
        )
    };
}

PaperEventAdapterResult PaperEventAdapter::observe(
    const trading_engine::risk::ApprovedQuote& approved
) {
    return {
        .event = make_event(
            PaperEventType::ApprovedQuoteObserved,
            approved.approved_ts_ns
        )
    };
}

PaperEventAdapterResult PaperEventAdapter::observe(
    const trading_engine::execution::OrderPlan& plan
) {
    for (std::uint16_t i = 0; i < plan.order_count; ++i) {
        child_orders_.insert_or_assign(
            child_key(plan.plan_id, plan.orders[i].order_id),
            plan.orders[i]
        );
    }

    return {
        .event = make_event(PaperEventType::OrderPlanCreated, plan.created_ts_ns)
    };
}

PaperEventAdapterResult PaperEventAdapter::observe(
    const trading_engine::execution::ExecutionReport& report
) {
    PaperEventAdapterResult result;
    result.event = make_event(
        PaperEventType::ExecutionReportObserved,
        report.event_ts_ns
    );

    if (!is_fill_report(report.status) || report.filled_lots <= 0) {
        return result;
    }

    const auto it = child_orders_.find(
        child_key(report.plan_id, report.child_order_id)
    );
    if (it == child_orders_.end()) {
        return result;
    }

    const auto& order = it->second;
    if (order.asset_id.empty()) {
        return result;
    }

    result.has_fill = true;
    result.fill.report = report;
    result.fill.market_id = order.market_id;
    result.fill.asset_id = order.asset_id;
    result.fill.asset_index = order.asset_index;
    result.fill.side = order.side;
    return result;
}

PaperEventAdapterResult PaperEventAdapter::observe(
    const trading_engine::execution::MakerExecutionReport& report
) {
    PaperEventAdapterResult result;
    result.event = make_event(
        PaperEventType::ExecutionReportObserved,
        report.recv_ts_ns != 0 ? report.recv_ts_ns : report.exchange_ts_ns
    );

    const MakerFillApplication maker_fill_application;
    const auto fill = maker_fill_application.from_report(report);
    if (fill.has_fill) {
        result.has_paper_fill = true;
        result.paper_fill = fill.fill;
    }
    return result;
}

PaperEventAdapterResult PaperEventAdapter::observe(
    const trading_engine::execution::ReservationDisposition&
) {
    return {
        .event = make_event(
            PaperEventType::ReservationDispositionObserved,
            0
        )
    };
}

PaperEventAdapterResult PaperEventAdapter::observe_mark_update(
    const trading_engine::state::MarketStateSnapshot& snapshot
) {
    return {
        .event = make_event(
            PaperEventType::MarkPriceUpdated,
            snapshot.last_book_update_ns
        )
    };
}

PaperEvent PaperEventAdapter::make_event(
    PaperEventType type,
    std::uint64_t ts_ns
) {
    PaperEvent event;
    event.seq_no = next_seq_no_++;
    event.ts_ns = ts_ns;
    event.type = type;
    return event;
}

std::uint64_t PaperEventAdapter::child_key(
    std::uint64_t plan_id,
    std::uint64_t child_order_id
) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    auto mix = [&hash](std::uint64_t value) {
        for (int shift = 0; shift < 64; shift += 8) {
            hash ^= static_cast<std::uint8_t>((value >> shift) & 0xffU);
            hash *= 1099511628211ULL;
        }
    };
    mix(plan_id);
    mix(child_order_id);
    return hash;
}

}  // namespace trading_engine::paper
