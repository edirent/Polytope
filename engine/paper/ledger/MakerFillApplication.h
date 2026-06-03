#pragma once

#include "engine/execution/public/MakerExecutionTypes.h"
#include "engine/paper/public/PaperFill.h"

#include <string>

namespace trading_engine::paper {

struct MakerFillApplicationResult {
    bool has_fill = false;
    PaperFill fill;
    std::string reason;
};

class MakerFillApplication {
public:
    [[nodiscard]] MakerFillApplicationResult from_report(
        const trading_engine::execution::MakerExecutionReport& report,
        std::int64_t fee_tick = 0
    ) const;
};

[[nodiscard]] std::uint64_t compute_paper_fill_hash(
    const PaperFill& fill
) noexcept;

}  // namespace trading_engine::paper
