#include "chain_confirm/ConfirmedFill.h"

#include <string>

namespace trading_engine::chain_confirm {

std::string fill_id(const std::string& tx_hash, std::uint32_t log_index) {
    return tx_hash + ":" + std::to_string(log_index);
}

std::string to_string(FillMappingStatus status) {
    switch (status) {
        case FillMappingStatus::Mapped:
            return "Mapped";
        case FillMappingStatus::UnmappedFill:
            return "UnmappedFill";
        case FillMappingStatus::AmbiguousFill:
            return "AmbiguousFill";
        default:
            return "Unknown";
    }
}

}  // namespace trading_engine::chain_confirm
