#include "chain_confirm/OrderFilledDecoder.h"

#include "chain_confirm/EthLogDecoder.h"

#include <limits>
#include <utility>

namespace trading_engine::chain_confirm {

namespace {

constexpr std::int64_t kPriceScale = 1'000'000;

enum class CtfExchangeSide : std::uint8_t {
    Buy = 0,
    Sell = 1
};

bool is_zero_asset(const std::string& asset_id) {
    return asset_id == "0";
}

std::int64_t checked_u64_to_i64(std::uint64_t value) {
    if (value > static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max()
        )) {
        return 0;
    }

    return static_cast<std::int64_t>(value);
}

std::int64_t price_tick_from_ratio(
    std::uint64_t cash_amount,
    std::uint64_t asset_amount
) {
    if (asset_amount == 0) {
        return 0;
    }

    const auto scaled =
        static_cast<long double>(cash_amount) *
        static_cast<long double>(kPriceScale) /
        static_cast<long double>(asset_amount);

    if (scaled < 0.0L ||
        scaled > static_cast<long double>(
            std::numeric_limits<std::int64_t>::max()
        )) {
        return 0;
    }

    return static_cast<std::int64_t>(scaled + 0.5L);
}

void fill_log_metadata(OrderFilledEvent& event, const EthLog& log) {
    using namespace eth_log_decoder;

    event.block_number = log.block_number;
    event.tx_hash = normalize_hex(log.tx_hash);
    event.log_index = log.log_index;
    event.removed = log.removed;
}

OrderFilledDecodeResult decode_ctf_exchange_v2(const EthLog& log) {
    using namespace eth_log_decoder;

    OrderFilledDecodeResult result;

    if (log.topics.size() != 4) {
        result.error = "CTF Exchange V2 OrderFilled log must have 4 topics";
        return result;
    }

    if (!is_hex_32_word(log.topics[1]) ||
        !is_hex_32_word(log.topics[2]) ||
        !is_hex_32_word(log.topics[3])) {
        result.error = "CTF Exchange V2 OrderFilled indexed topics are malformed";
        return result;
    }

    const auto words = split_data_words(log.data);
    if (words.size() != 7) {
        result.error =
            "CTF Exchange V2 OrderFilled data must contain 7 ABI words";
        return result;
    }

    const auto side = uint256_to_u64(words[0]);
    const auto maker_amount = uint256_to_u64(words[2]);
    const auto taker_amount = uint256_to_u64(words[3]);
    const auto fee = uint256_to_u64(words[4]);

    if (!side || *side > 255 || !maker_amount || !taker_amount || !fee) {
        result.error = "CTF Exchange V2 OrderFilled numeric field overflow";
        return result;
    }

    const std::string token_id = uint256_to_decimal_string(words[1]);
    if (token_id.empty()) {
        result.error = "CTF Exchange V2 OrderFilled missing token id";
        return result;
    }

    result.event.order_hash = normalize_hex(log.topics[1]);
    result.event.maker = topic_to_address(log.topics[2]);
    result.event.taker = topic_to_address(log.topics[3]);
    result.event.side = static_cast<std::uint8_t>(*side);
    result.event.token_id = token_id;
    result.event.maker_amount_filled = *maker_amount;
    result.event.taker_amount_filled = *taker_amount;
    result.event.fee = *fee;
    result.event.builder = normalize_hex(words[5]);
    result.event.metadata = normalize_hex(words[6]);
    fill_log_metadata(result.event, log);

    if (result.event.side == static_cast<std::uint8_t>(CtfExchangeSide::Buy)) {
        result.event.maker_asset_id = "0";
        result.event.taker_asset_id = token_id;
    } else if (
        result.event.side == static_cast<std::uint8_t>(CtfExchangeSide::Sell)) {
        result.event.maker_asset_id = token_id;
        result.event.taker_asset_id = "0";
    }

    if (result.event.maker.empty() || result.event.taker.empty()) {
        result.error = "CTF Exchange V2 OrderFilled log contains malformed addresses";
        result.event = {};
        return result;
    }

    result.ok = true;
    return result;
}

OrderFilledDecodeResult decode_legacy_order_filled(const EthLog& log) {
    using namespace eth_log_decoder;

    OrderFilledDecodeResult result;

    if (log.topics.size() != 4) {
        result.error = "legacy OrderFilled log must have 4 topics";
        return result;
    }

    if (!is_hex_32_word(log.topics[1]) ||
        !is_hex_32_word(log.topics[2]) ||
        !is_hex_32_word(log.topics[3])) {
        result.error = "legacy OrderFilled indexed topics are malformed";
        return result;
    }

    const auto words = split_data_words(log.data);
    if (words.size() != 5) {
        result.error = "legacy OrderFilled data must contain 5 uint256 words";
        return result;
    }

    auto maker_amount = uint256_to_u64(words[2]);
    auto taker_amount = uint256_to_u64(words[3]);
    auto fee = uint256_to_u64(words[4]);
    if (!maker_amount || !taker_amount || !fee) {
        result.error = "legacy OrderFilled amount exceeds uint64";
        return result;
    }

    result.event.order_hash = normalize_hex(log.topics[1]);
    result.event.maker = topic_to_address(log.topics[2]);
    result.event.taker = topic_to_address(log.topics[3]);
    result.event.maker_asset_id = uint256_to_decimal_string(words[0]);
    result.event.taker_asset_id = uint256_to_decimal_string(words[1]);
    result.event.token_id = !is_zero_asset(result.event.maker_asset_id)
        ? result.event.maker_asset_id
        : result.event.taker_asset_id;
    result.event.maker_amount_filled = *maker_amount;
    result.event.taker_amount_filled = *taker_amount;
    result.event.fee = *fee;
    fill_log_metadata(result.event, log);

    if (result.event.maker.empty() || result.event.taker.empty() ||
        result.event.maker_asset_id.empty() ||
        result.event.taker_asset_id.empty()) {
        result.error = "legacy OrderFilled log contains malformed fields";
        result.event = {};
        return result;
    }

    result.ok = true;
    return result;
}

}  // namespace

OrderFilledDecodeResult OrderFilledDecoder::decode(const EthLog& log) const {
    using namespace eth_log_decoder;

    OrderFilledDecodeResult result;

    if (log.topics.empty()) {
        result.error = "wrong OrderFilled topic";
        return result;
    }

    const std::string topic0 = normalize_hex(log.topics[0]);

    if (topic0 == normalize_hex(kOrderFilledTopic0)) {
        return decode_ctf_exchange_v2(log);
    }

    if (topic0 == normalize_hex(kLegacyOrderFilledTopic0)) {
        return decode_legacy_order_filled(log);
    }

    result.error = "wrong OrderFilled topic";
    return result;
}

ConfirmedFillDecodeResult OrderFilledDecoder::decode_confirmed_fill(
    const EthLog& log,
    std::string market_id,
    std::uint64_t chain_seen_monotonic_ns
) const {
    ConfirmedFillDecodeResult result;
    const auto decoded = decode(log);

    if (!decoded.ok) {
        result.error = decoded.error;
        return result;
    }

    result.fill = to_confirmed_fill(
        decoded.event,
        std::move(market_id),
        chain_seen_monotonic_ns
    );
    result.ok = result.fill.mapping_status == FillMappingStatus::Mapped ||
        result.fill.removed;

    if (!result.ok) {
        result.error = to_string(result.fill.mapping_status);
    }

    return result;
}

ConfirmedFill OrderFilledDecoder::to_confirmed_fill(
    const OrderFilledEvent& event,
    std::string market_id,
    std::uint64_t chain_seen_monotonic_ns
) const {
    ConfirmedFill fill;
    fill.fill_id = fill_id(event.tx_hash, event.log_index);
    fill.order_hash = event.order_hash;
    fill.market_id = std::move(market_id);
    fill.direction = direction_from_assets(event);
    fill.block_number = event.block_number;
    fill.tx_hash = event.tx_hash;
    fill.log_index = event.log_index;
    fill.chain_seen_monotonic_ns = chain_seen_monotonic_ns;
    fill.removed = event.removed;

    if (fill.direction == ConfirmedDirection::BuyAggressor &&
        !is_zero_asset(event.taker_asset_id)) {
        fill.asset_id = event.taker_asset_id;
        fill.size_lots = checked_u64_to_i64(event.taker_amount_filled);
        fill.price_tick = price_tick_from_ratio(
            event.maker_amount_filled,
            event.taker_amount_filled
        );
    } else if (fill.direction == ConfirmedDirection::SellAggressor &&
               !is_zero_asset(event.maker_asset_id)) {
        fill.asset_id = event.maker_asset_id;
        fill.size_lots = checked_u64_to_i64(event.maker_amount_filled);
        fill.price_tick = price_tick_from_ratio(
            event.taker_amount_filled,
            event.maker_amount_filled
        );
    }

    const bool mapped =
        !fill.market_id.empty() &&
        !fill.asset_id.empty() &&
        fill.price_tick > 0 &&
        fill.size_lots > 0 &&
        fill.direction != ConfirmedDirection::Unknown;

    fill.mapping_status = mapped
        ? FillMappingStatus::Mapped
        : FillMappingStatus::UnmappedFill;

    return fill;
}

ConfirmedDirection OrderFilledDecoder::direction_from_assets(
    const OrderFilledEvent& event
) const noexcept {
    const bool maker_zero = is_zero_asset(event.maker_asset_id);
    const bool taker_zero = is_zero_asset(event.taker_asset_id);

    if (maker_zero && !taker_zero) {
        return ConfirmedDirection::BuyAggressor;
    }

    if (taker_zero && !maker_zero) {
        return ConfirmedDirection::SellAggressor;
    }

    return ConfirmedDirection::Unknown;
}

}  // namespace trading_engine::chain_confirm
