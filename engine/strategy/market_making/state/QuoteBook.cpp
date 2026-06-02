#include "engine/strategy/market_making/state/QuoteBook.h"

#include <algorithm>

namespace trading_engine::strategy::market_making {

bool QuoteBook::upsert(const ActiveQuoteState& quote) {
    quotes_by_asset_[quote.asset_index] = quote;
    return true;
}

bool QuoteBook::remove(std::uint32_t asset_index) {
    return quotes_by_asset_.erase(asset_index) > 0;
}

const ActiveQuoteState* QuoteBook::find(std::uint32_t asset_index) const {
    const auto it = quotes_by_asset_.find(asset_index);
    return it == quotes_by_asset_.end() ? nullptr : &it->second;
}

ActiveQuoteState* QuoteBook::find(std::uint32_t asset_index) {
    const auto it = quotes_by_asset_.find(asset_index);
    return it == quotes_by_asset_.end() ? nullptr : &it->second;
}

std::vector<ActiveQuoteState> QuoteBook::active_quotes() const {
    std::vector<ActiveQuoteState> quotes;
    quotes.reserve(quotes_by_asset_.size());
    for (const auto& [_, quote] : quotes_by_asset_) {
        quotes.push_back(quote);
    }
    std::sort(quotes.begin(), quotes.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.asset_index < rhs.asset_index;
    });
    return quotes;
}

std::uint64_t QuoteBook::hash() const noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    std::vector<std::uint32_t> keys;
    keys.reserve(quotes_by_asset_.size());
    for (const auto& [key, _] : quotes_by_asset_) {
        keys.push_back(key);
    }
    std::sort(keys.begin(), keys.end());
    for (const auto key : keys) {
        const auto& quote = quotes_by_asset_.at(key);
        hash = fnv1a_mix(hash, key);
        hash = fnv1a_mix(hash, quote.quote_group_id);
        hash = fnv1a_mix(hash, quote.quote_intent_id);
        hash = fnv1a_mix(hash, static_cast<std::uint8_t>(quote.status));
        hash = fnv1a_mix(hash, quote.idempotency_hash);
    }
    return hash;
}

}  // namespace trading_engine::strategy::market_making
