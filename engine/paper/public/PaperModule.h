#pragma once

#include "engine/execution/public/ExecutionReport.h"
#include "engine/execution/public/ReservationDisposition.h"
#include "engine/paper/public/PaperAccount.h"
#include "engine/paper/public/PaperEvent.h"
#include "engine/paper/public/PaperPnL.h"
#include "engine/paper/public/PaperSnapshot.h"
#include "engine/paper/public/PaperTypes.h"
#include "engine/paper/public/PerformanceSnapshot.h"
#include "engine/risk/public/ApprovedIntent.h"
#include "engine/risk/public/RiskDecision.h"
#include "engine/signal/public/OpportunityIntent.h"
#include "state/MarketStateSnapshot.h"

namespace trading_engine::paper {

[[nodiscard]] PaperModuleStatus paper_module_status() noexcept;

[[nodiscard]] bool public_contracts_available() noexcept;

}  // namespace trading_engine::paper
