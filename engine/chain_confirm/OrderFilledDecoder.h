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
    /**
     * Polymarket CTF Exchange V2:
     *
     *   OrderFilled(
     *       bytes32 indexed orderHash,
     *       address indexed maker,
     *       address indexed taker,
     *       uint8 side,
     *       uint256 tokenId,
     *       uint256 makerAmountFilled,
     *       uint256 takerAmountFilled,
     *       uint256 fee,
     *       bytes32 builder,
     *       bytes32 metadata
     *   )
     */
    static constexpr const char* kOrderFilledTopic0 =
        "0xd543adfd945773f1a62f74f0ee55a5e3b9b1a28262980ba90b1a89f2ea84d8ee";

    static constexpr const char* kLegacyOrderFilledTopic0 =
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
