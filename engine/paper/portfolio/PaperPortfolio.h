#pragma once

#include "engine/paper/ledger/FillApplication.h"
#include "engine/paper/ledger/PositionLedger.h"
#include "engine/paper/portfolio/ExposureView.h"

#include <cstdint>
#include <string>

namespace trading_engine::paper {

class PaperPortfolio {
public:
    [[nodiscard]] PositionLedgerApplyResult apply_fill(const FillApplication& fill);

    void mark_mid(const std::string& asset_id, std::int64_t mid_tick);
    void mark_liquidation(const std::string& asset_id, std::int64_t liquidation_tick);

    [[nodiscard]] const PositionLedger& positions() const noexcept;
    [[nodiscard]] ExposureView exposure() const;

private:
    PositionLedger positions_;
};

}  // namespace trading_engine::paper
