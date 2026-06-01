#pragma once

#include "engine/paper/public/PaperAccount.h"
#include "engine/paper/public/PaperPnL.h"
#include "engine/paper/public/PerformanceSnapshot.h"

#include <cstdint>

namespace trading_engine::paper {

struct PaperSnapshot {
    std::uint64_t snapshot_id = 0;
    std::uint64_t ts_ns = 0;
    std::uint64_t source_seq_no = 0;

    PaperAccount account;
    PaperPnL pnl;
    PerformanceSnapshot performance;
};

}  // namespace trading_engine::paper
