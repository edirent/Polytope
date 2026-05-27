#include "feed/integrity/RecoveryController.h"

namespace trading_engine::feed {

RecoveryAction RecoveryController::decide(
    const ConsistencyResult& consistency
) const noexcept {
    switch (consistency.code) {
        case ConsistencyCode::Ok:
            return RecoveryAction::None;

        case ConsistencyCode::MissingEntityId:
            return RecoveryAction::MarkUnsafe;

        case ConsistencyCode::DeltaBeforeSnapshot:
            return RecoveryAction::RequestSnapshot;

        case ConsistencyCode::InvalidValue:
            return RecoveryAction::MarkUnsafe;

        case ConsistencyCode::CrossedBook:
            return RecoveryAction::RequestSnapshot;

        case ConsistencyCode::ExternalBboDiverged:
            return RecoveryAction::RequestSnapshot;

        case ConsistencyCode::ClosedEntityMutation:
            return RecoveryAction::MarkUnsafe;

        case ConsistencyCode::DecodeError:
            return RecoveryAction::MarkUnsafe;

        case ConsistencyCode::UnknownEvent:
            return RecoveryAction::MarkUnsafe;
    }

    return RecoveryAction::MarkUnsafe;
}

RecoveryAction RecoveryController::decide(
    const StaleResult& stale
) const noexcept {
    switch (stale.level) {
        case StaleLevel::Ok:
            return RecoveryAction::None;

        case StaleLevel::SourceStale:
            return RecoveryAction::ReconnectSource;

        case StaleLevel::EntityStale:
            return RecoveryAction::RequestSnapshot;
    }

    return RecoveryAction::None;
}

RecoveryAction RecoveryController::decide(
    const ConsistencyResult& consistency,
    const StaleResult& stale
) const noexcept {
    const auto stale_action = decide(stale);

    if (stale_action != RecoveryAction::None) {
        return stale_action;
    }

    return decide(consistency);
}

std::string to_string(RecoveryAction action) {
    switch (action) {
        case RecoveryAction::None:
            return "None";
        case RecoveryAction::MarkUnsafe:
            return "MarkUnsafe";
        case RecoveryAction::RequestSnapshot:
            return "RequestSnapshot";
        case RecoveryAction::ReconnectSource:
            return "ReconnectSource";
        case RecoveryAction::ResetEntity:
            return "ResetEntity";
        case RecoveryAction::DisableEntity:
            return "DisableEntity";
        default:
            return "Unknown";
    }
}

}  // namespace trading_engine::feed
