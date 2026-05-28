#include "chain_confirm/ConfirmedFillStore.h"

namespace trading_engine::chain_confirm {

ConfirmedFillStoreResult ConfirmedFillStore::upsert(
    const ConfirmedFill& fill
) {
    ConfirmedFillStoreResult result;
    result.fill_id = fill.fill_id.empty()
        ? fill_id(fill.tx_hash, fill.log_index)
        : fill.fill_id;

    ConfirmedFill normalized = fill;
    normalized.fill_id = result.fill_id;

    const auto it = fills_.find(result.fill_id);

    if (normalized.removed) {
        if (it == fills_.end()) {
            fills_.emplace(result.fill_id, normalized);
            result.code = ConfirmedFillStoreCode::RemovedUnknown;
            return result;
        }

        it->second.removed = true;
        result.code = ConfirmedFillStoreCode::MarkedRemoved;
        return result;
    }

    if (it == fills_.end()) {
        fills_.emplace(result.fill_id, normalized);
        result.code = ConfirmedFillStoreCode::Inserted;
        return result;
    }

    if (!it->second.removed &&
        it->second.block_number == normalized.block_number &&
        it->second.tx_hash == normalized.tx_hash &&
        it->second.log_index == normalized.log_index &&
        it->second.price_tick == normalized.price_tick &&
        it->second.size_lots == normalized.size_lots &&
        it->second.asset_id == normalized.asset_id &&
        it->second.market_id == normalized.market_id &&
        it->second.direction == normalized.direction) {
        result.code = ConfirmedFillStoreCode::Duplicate;
        return result;
    }

    it->second = normalized;
    result.code = ConfirmedFillStoreCode::Updated;
    return result;
}

bool ConfirmedFillStore::contains(
    const std::string& id
) const noexcept {
    return fills_.find(id) != fills_.end();
}

const ConfirmedFill* ConfirmedFillStore::get(
    const std::string& id
) const noexcept {
    const auto it = fills_.find(id);
    return it == fills_.end() ? nullptr : &it->second;
}

std::size_t ConfirmedFillStore::size() const noexcept {
    return fills_.size();
}

std::size_t ConfirmedFillStore::active_count() const noexcept {
    std::size_t count = 0;
    for (const auto& [_, fill] : fills_) {
        if (!fill.removed) {
            ++count;
        }
    }
    return count;
}

void ConfirmedFillStore::clear() {
    fills_.clear();
}

std::string to_string(ConfirmedFillStoreCode code) {
    switch (code) {
        case ConfirmedFillStoreCode::Inserted:
            return "Inserted";
        case ConfirmedFillStoreCode::Duplicate:
            return "Duplicate";
        case ConfirmedFillStoreCode::Updated:
            return "Updated";
        case ConfirmedFillStoreCode::MarkedRemoved:
            return "MarkedRemoved";
        case ConfirmedFillStoreCode::RemovedUnknown:
            return "RemovedUnknown";
        default:
            return "Unknown";
    }
}

}  // namespace trading_engine::chain_confirm
