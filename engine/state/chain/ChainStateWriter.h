#pragma once

#include "state/chain/ConfirmedTradeState.h"
#include "state/chain/ConfirmedTradeWindow.h"
#include "state/core/MarketStateEvent.h"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace trading_engine::state {

enum class ChainApplyCode : std::uint8_t {
    Applied = 0,
    IgnoredNonChainEvent,
    MissingEntityId,
    UnknownDirection,
    AmbiguousFill,
    RemovedFill,
    DuplicateFill
};

struct ChainApplyResult {
    ChainApplyCode code{ChainApplyCode::IgnoredNonChainEvent};
    std::string entity_id;
    std::string fill_id;
    bool state_changed{false};
    std::string message;

    [[nodiscard]] bool ok() const noexcept {
        return code == ChainApplyCode::Applied ||
               code == ChainApplyCode::IgnoredNonChainEvent ||
               code == ChainApplyCode::UnknownDirection ||
               code == ChainApplyCode::AmbiguousFill ||
               code == ChainApplyCode::RemovedFill ||
               code == ChainApplyCode::DuplicateFill;
    }
};

class ChainStateWriter {
public:
    ChainApplyResult apply(const MarketStateEvent& event);

    [[nodiscard]] const ConfirmedTradeState* get(
        const std::string& entity_id
    ) const noexcept;

    [[nodiscard]] bool contains(const std::string& entity_id) const noexcept;
    [[nodiscard]] std::uint64_t version(
        const std::string& entity_id
    ) const noexcept;

private:
    struct AppliedFill {
        std::string entity_id;
        AggressorSide side{AggressorSide::Unknown};
        std::int64_t size_lots{0};
        std::uint64_t chain_seen_ns{0};
        bool counted_volume{false};
        bool removed{false};
    };

    [[nodiscard]] ChainApplyResult apply_confirmed(
        const MarketStateEvent& event
    );

    [[nodiscard]] ChainApplyResult apply_removed(
        const MarketStateEvent& event
    );

    [[nodiscard]] std::string resolve_entity_id(
        const MarketStateEvent& event
    ) const;

    void refresh_windows(
        const std::string& entity_id,
        std::uint64_t now_ns
    );

private:
    std::unordered_map<std::string, ConfirmedTradeState> states_;
    std::unordered_map<std::string, ConfirmedTradeWindow> windows_;
    std::unordered_map<std::string, AppliedFill> fills_;
};

[[nodiscard]] const char* to_string(ChainApplyCode code) noexcept;

}  // namespace trading_engine::state
