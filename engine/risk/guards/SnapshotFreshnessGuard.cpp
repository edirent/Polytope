#include "engine/risk/guards/SnapshotFreshnessGuard.h"

#include <utility>

namespace trading_engine::risk {

namespace {

[[nodiscard]] GuardResult reject(std::string reason) {
    GuardResult result;
    result.pass = false;
    result.rejection = RiskDecisionType::RejectStaleSnapshot;
    result.reject_flag = kRiskRejectFlagStaleSnapshot;
    result.reason = std::move(reason);
    return result;
}

[[nodiscard]] std::uint64_t evidence_hash(
    const state::MarketStateSnapshot& snapshot
) noexcept {
    return snapshot.snapshot_version_hash != 0
        ? snapshot.snapshot_version_hash
        : snapshot.state_hash;
}

[[nodiscard]] std::uint64_t evidence_hash(
    const state::MarketDepthView& depth_view
) noexcept {
    return depth_view.snapshot_version_hash;
}

}  // namespace

GuardResult SnapshotFreshnessGuard::check(
    const state::MarketStateSnapshot& snapshot,
    const signal::OpportunityIntent& intent,
    const RiskPolicySnapshot& policy,
    std::uint64_t now_ns
) const {
    if (snapshot.last_book_update_ns == 0 ||
        now_ns < snapshot.last_book_update_ns) {
        return reject("invalid snapshot update timestamp");
    }

    const auto age_ns = static_cast<std::int64_t>(
        now_ns - snapshot.last_book_update_ns
    );
    if (policy.max_book_age_ns >= 0 && age_ns > policy.max_book_age_ns) {
        return reject("snapshot too old");
    }

    auto result = pass_guard();
    if (evidence_hash(snapshot) != intent.snapshot_version_hash) {
        const auto version_drift = snapshot.version > intent.snapshot_version
            ? snapshot.version - intent.snapshot_version
            : intent.snapshot_version - snapshot.version;

        if (policy.max_snapshot_skew_ns >= 0 &&
            version_drift >
                static_cast<std::uint64_t>(policy.max_snapshot_skew_ns)) {
            return reject("snapshot version drift exceeds tolerance");
        }

        result.requires_reprice = true;
        result.reason = "snapshot hash changed; reprice required";
    }

    return result;
}

GuardResult SnapshotFreshnessGuard::check(
    const state::MarketDepthView& depth_view,
    const signal::OpportunityIntent& intent,
    const RiskPolicySnapshot& policy,
    std::uint64_t now_ns
) const {
    if (depth_view.last_ws_recv_ns == 0 ||
        now_ns < depth_view.last_ws_recv_ns) {
        return reject("invalid depth view update timestamp");
    }

    const auto age_ns = static_cast<std::int64_t>(
        now_ns - depth_view.last_ws_recv_ns
    );
    if (policy.max_book_age_ns >= 0 && age_ns > policy.max_book_age_ns) {
        return reject("depth view too old");
    }

    auto result = pass_guard();
    if (evidence_hash(depth_view) != intent.snapshot_version_hash) {
        const auto version_drift =
            depth_view.book_version > intent.snapshot_version
                ? depth_view.book_version - intent.snapshot_version
                : intent.snapshot_version - depth_view.book_version;

        if (policy.max_snapshot_skew_ns >= 0 &&
            version_drift >
                static_cast<std::uint64_t>(policy.max_snapshot_skew_ns)) {
            return reject("depth view version drift exceeds tolerance");
        }

        result.requires_reprice = true;
        result.reason = "depth view hash changed; reprice required";
    }

    return result;
}

}  // namespace trading_engine::risk
