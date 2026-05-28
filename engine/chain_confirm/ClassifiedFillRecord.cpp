#include "chain_confirm/ClassifiedFillRecord.h"

namespace trading_engine::chain_confirm {

ClassifiedFillRecord classify_confirmed_fill(
    const ConfirmedFill& fill,
    std::uint64_t source_sequence
) {
    ClassifiedFillRecord out;
    out.fill_id = fill.fill_id;
    out.order_hash = fill.order_hash;
    out.market_id = fill.market_id;
    out.asset_id = fill.asset_id;
    out.price_tick = fill.price_tick;
    out.size_lots = fill.size_lots;
    out.direction = fill.direction;
    out.mapping_status = fill.mapping_status;
    out.block_number = fill.block_number;
    out.tx_hash = fill.tx_hash;
    out.log_index = fill.log_index;
    out.chain_seen_monotonic_ns = fill.chain_seen_monotonic_ns;
    out.source_sequence = source_sequence;
    out.removed = fill.removed;

    if (fill.removed) {
        out.classification = FillClassification::ChainRemoved;
    } else if (fill.mapping_status == FillMappingStatus::Mapped) {
        out.classification = FillClassification::ChainConfirmed;
    } else if (fill.mapping_status == FillMappingStatus::AmbiguousFill) {
        out.classification = FillClassification::AmbiguousFill;
    } else {
        out.classification = FillClassification::UnmappedFill;
    }

    return out;
}

std::string to_string(FillClassification classification) {
    switch (classification) {
        case FillClassification::ChainConfirmed:
            return "ChainConfirmed";
        case FillClassification::ChainRemoved:
            return "ChainRemoved";
        case FillClassification::UnmappedFill:
            return "UnmappedFill";
        case FillClassification::AmbiguousFill:
            return "AmbiguousFill";
        case FillClassification::Unknown:
        default:
            return "Unknown";
    }
}

}  // namespace trading_engine::chain_confirm
