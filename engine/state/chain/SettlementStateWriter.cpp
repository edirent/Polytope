#include "state/chain/SettlementStateWriter.h"

#include <utility>

namespace trading_engine::state {

SettlementApplyResult SettlementStateWriter::mark_open(
    const std::string& market_id,
    std::uint64_t block_number
) {
    return apply_status(market_id, SettlementStatus::Open, {}, block_number);
}

SettlementApplyResult SettlementStateWriter::mark_closed(
    const std::string& market_id,
    std::uint64_t block_number
) {
    return apply_status(market_id, SettlementStatus::Closed, {}, block_number);
}

SettlementApplyResult SettlementStateWriter::mark_resolved(
    const std::string& market_id,
    const std::string& winning_asset_id,
    std::uint64_t block_number
) {
    return apply_status(
        market_id,
        SettlementStatus::Resolved,
        winning_asset_id,
        block_number
    );
}

const SettlementState* SettlementStateWriter::get(
    const std::string& market_id
) const noexcept {
    const auto it = states_.find(market_id);
    if (it == states_.end()) {
        return nullptr;
    }
    return &it->second;
}

bool SettlementStateWriter::contains(
    const std::string& market_id
) const noexcept {
    return states_.find(market_id) != states_.end();
}

SettlementApplyResult SettlementStateWriter::apply_status(
    const std::string& market_id,
    SettlementStatus status,
    std::string winning_asset_id,
    std::uint64_t block_number
) {
    SettlementApplyResult result;
    result.market_id = market_id;
    result.status = status;

    if (market_id.empty()) {
        result.code = SettlementApplyCode::MissingMarketId;
        result.message = "settlement update missing market_id";
        return result;
    }

    SettlementState& state = states_[market_id];
    state.status = status;
    state.resolved = status == SettlementStatus::Resolved;
    state.winning_asset_id = std::move(winning_asset_id);
    state.last_update_block = block_number;
    ++state.version;

    result.code = SettlementApplyCode::Applied;
    result.state_changed = true;
    result.message = "settlement state updated";
    return result;
}

const char* to_string(SettlementApplyCode code) noexcept {
    switch (code) {
        case SettlementApplyCode::Applied:
            return "Applied";
        case SettlementApplyCode::MissingMarketId:
        default:
            return "MissingMarketId";
    }
}

}  // namespace trading_engine::state
