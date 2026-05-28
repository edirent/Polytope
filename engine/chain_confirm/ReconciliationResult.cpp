#include "chain_confirm/ReconciliationResult.h"

namespace trading_engine::chain_confirm {

std::string to_string(ReconciliationStatus status) {
    switch (status) {
        case ReconciliationStatus::ConfirmedOneToOne:
            return "ConfirmedOneToOne";
        case ReconciliationStatus::ConfirmedOneToMany:
            return "ConfirmedOneToMany";
        case ReconciliationStatus::ConfirmedManyToOne:
            return "ConfirmedManyToOne";
        case ReconciliationStatus::Ambiguous:
            return "Ambiguous";
        case ReconciliationStatus::UnmatchedHint:
            return "UnmatchedHint";
        case ReconciliationStatus::UnmatchedFill:
            return "UnmatchedFill";
        case ReconciliationStatus::ExpiredHint:
            return "ExpiredHint";
        case ReconciliationStatus::RemovedByReorg:
            return "RemovedByReorg";
        default:
            return "Unknown";
    }
}

}  // namespace trading_engine::chain_confirm
