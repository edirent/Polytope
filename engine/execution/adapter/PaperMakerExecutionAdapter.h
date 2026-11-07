#pragma once

#include "engine/execution/adapter/PaperMakerFillModel.h"
#include "engine/execution/state/ActivePaperQuoteBook.h"

#include <cstdint>
#include <vector>

namespace trading_engine::execution {

struct PaperMakerExecutionConfig {
    PaperMakerFillMode fill_mode = PaperMakerFillMode::Conservative;
    bool allow_partial_fills = true;
    std::int64_t max_fill_qty_per_trade = 0;
    std::uint64_t queue_min_rest_ns = 250'000'000ULL;
};

class PaperMakerExecutionAdapter {
public:
    explicit PaperMakerExecutionAdapter(PaperMakerFillMode mode);
    explicit PaperMakerExecutionAdapter(PaperMakerExecutionConfig config = {});

    MakerSubmitResult submit_approved_quote(
        const risk::ApprovedQuote& quote,
        std::uint64_t now_ns
    );

    MakerCancelResult cancel_quote_group(
        std::uint64_t quote_group_id,
        std::uint64_t now_ns
    );

    [[nodiscard]] std::vector<MakerExecutionReport> on_market_event(
        const PaperMakerMarketEvent& event
    );

    void expire_old(std::uint64_t now_ns);

    [[nodiscard]] const ActivePaperQuoteBook& quote_book() const noexcept;
    [[nodiscard]] PaperMakerFillMode fill_mode() const noexcept;
    // Central OMS path: pure simulation on an immutable OMS-owned order projection.
    // Does not populate the legacy research quote_book_.
    [[nodiscard]] std::vector<MakerFillResult> evaluate_managed(const PaperMakerQuote& quote,
        const PaperMakerMarketEvent& event,PaperMakerFillMode mode) const {
        return fill_model_.evaluate(quote,event,mode,config_.allow_partial_fills,
            config_.max_fill_qty_per_trade,config_.queue_min_rest_ns);
    }
    [[nodiscard]] bool accept_managed_submission(const risk::ApprovedQuote& quote,std::uint64_t now) const {
        return (quote.has_bid || quote.has_ask) && quote.expires_at_ns>now;
    }
    [[nodiscard]] bool accept_managed_cancel() const {return true;} // Explicit simulated request acceptance, not terminal ACK.

private:
    [[nodiscard]] MakerExecutionReport make_report(
        const MakerFillResult& fill,
        std::int64_t remaining_qty_lots
    ) const;

    PaperMakerExecutionConfig config_;
    ActivePaperQuoteBook quote_book_;
    PaperMakerFillModel fill_model_;
};

}  // namespace trading_engine::execution
