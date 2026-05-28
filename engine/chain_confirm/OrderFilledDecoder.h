#pragma once

#include "chain_confirm/ConfirmedFill.h"
#include "chain_confirm/EthLog.h"
#include "chain_confirm/OrderFilledEvent.h"

#include <cstdint>
#include <string>

namespace trading_engine::chain_confirm {

struct OrderFilledDecodeResult {
    bool ok{false};
    std::string error;
    OrderFilledEvent event;
};

struct ConfirmedFillDecodeResult {
    bool ok{false};
    std::string error;
    ConfirmedFill fill;
};

class OrderFilledDecoder {
public:
    static constexpr const char* kOrderFilledTopic0 =
        "0xd0a08e8c493f9c94f29311604c9de1b4e8c8d4c06bd0c789af57f2d65bfec0f6";

    [[nodiscard]] OrderFilledDecodeResult decode(
        const EthLog& log
    ) const;

    [[nodiscard]] ConfirmedFillDecodeResult decode_confirmed_fill(
        const EthLog& log,
        std::string market_id,
        std::uint64_t chain_seen_monotonic_ns = 0
    ) const;

private:
    [[nodiscard]] ConfirmedFill to_confirmed_fill(
        const OrderFilledEvent& event,
        std::string market_id,
        std::uint64_t chain_seen_monotonic_ns
    ) const;

    [[nodiscard]] ConfirmedDirection direction_from_assets(
        const OrderFilledEvent& event
    ) const noexcept;
};

}  // namespace trading_engine::chain_confirm
