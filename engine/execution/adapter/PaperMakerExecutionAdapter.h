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
