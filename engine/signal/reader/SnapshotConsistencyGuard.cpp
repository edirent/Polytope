#include "engine/signal/reader/SnapshotConsistencyGuard.h"

#include "engine/signal/reader/LOBStalenessChecker.h"
#include "state/quality/BookQualityState.h"

#include <algorithm>
#include <limits>

namespace trading_engine::signal {

namespace {

constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

void mix_u64(std::uint64_t value, std::uint64_t* hash) noexcept {
    for (std::uint8_t i = 0; i < 8; ++i) {
        *hash ^= static_cast<std::uint8_t>((value >> (i * 8U)) & 0xffU);
        *hash *= kFnvPrime;
    }
}

void mix_string(const std::string& value, std::uint64_t* hash) noexcept {
    for (const unsigned char c : value) {
        *hash ^= c;
        *hash *= kFnvPrime;
    }
}

SnapshotConsistencyResult reject(
    IntentStatus status,
    std::string error,
    SnapshotVersion version = {}
) {
    SnapshotConsistencyResult result;
    result.ok = false;
    result.rejection_status = status;
    result.error = std::move(error);
    result.version = version;
    return result;
}

bool quality_rejects(
    const trading_engine::state::MarketStateSnapshot& snapshot
) noexcept {
    using trading_engine::state::BookQuality;
    return snapshot.quality == BookQuality::Stale ||
           snapshot.quality == BookQuality::Recovering ||
           snapshot.quality == BookQuality::Crossed ||
           snapshot.quality == BookQuality::Closed ||
           snapshot.quality == BookQuality::Resolved;
}

SnapshotVersion make_version(
    std::span<const trading_engine::state::MarketStateSnapshot> snapshots,
    std::uint64_t now_ns
) noexcept {
    SnapshotVersion version;
    version.read_ts_ns = now_ns;
    version.min_book_version = std::numeric_limits<std::uint64_t>::max();
    version.max_book_version = 0;

    std::uint64_t hash = kFnvOffset;
    for (const auto& snapshot : snapshots) {
        version.min_book_version =
            std::min(version.min_book_version, snapshot.version);
        version.max_book_version =
            std::max(version.max_book_version, snapshot.version);
        mix_string(snapshot.entity_id, &hash);
        mix_u64(snapshot.version, &hash);
        mix_u64(snapshot.state_hash, &hash);
        mix_u64(snapshot.last_book_update_ns, &hash);
    }

    if (snapshots.empty()) {
        version.min_book_version = 0;
    }
    version.combined_hash = hash;
    return version;
}

}  // namespace

SnapshotConsistencyResult SnapshotConsistencyGuard::check(
    std::span<const trading_engine::state::MarketStateSnapshot> snapshots,
    std::uint64_t now_ns,
    const SignalConfig& config
) const {
    const auto version = make_version(snapshots, now_ns);

    if (snapshots.empty()) {
        return reject(
            IntentStatus::RejectedMissingSnapshot,
            "missing snapshot",
            version
        );
    }

    LOBStalenessChecker staleness;
    for (const auto& snapshot : snapshots) {
        if (snapshot.entity_id.empty()) {
            return reject(
                IntentStatus::RejectedMissingSnapshot,
                "snapshot missing entity id",
                version
            );
        }
        if (snapshot.recovering) {
            return reject(
                IntentStatus::RejectedBadMarketState,
                "snapshot recovering: " + snapshot.entity_id,
                version
            );
        }
        if (snapshot.crossed) {
            return reject(
                IntentStatus::RejectedBadMarketState,
                "snapshot crossed: " + snapshot.entity_id,
                version
            );
        }
        if (snapshot.closed) {
            return reject(
                IntentStatus::RejectedBadMarketState,
                "snapshot closed: " + snapshot.entity_id,
                version
            );
        }
        if (snapshot.resolved) {
            return reject(
                IntentStatus::RejectedBadMarketState,
                "snapshot resolved: " + snapshot.entity_id,
                version
            );
        }
        if (quality_rejects(snapshot)) {
            return reject(
                IntentStatus::RejectedBadMarketState,
                "bad snapshot quality: " + snapshot.entity_id,
                version
            );
        }
        if (config.require_usable_for_depth && !snapshot.usable_for_depth) {
            return reject(
                IntentStatus::RejectedBadMarketState,
                "snapshot not usable for depth: " + snapshot.entity_id,
                version
            );
        }
        if (config.require_usable_for_signal && !snapshot.usable_for_signal) {
            return reject(
                IntentStatus::RejectedBadMarketState,
                "snapshot not usable for signal: " + snapshot.entity_id,
                version
            );
        }
        if (staleness.is_stale(snapshot, now_ns, config.max_lob_age_ns)) {
            return reject(
                IntentStatus::RejectedBadMarketState,
                "snapshot stale: " + snapshot.entity_id,
                version
            );
        }
    }

    const auto skew = version.max_book_version - version.min_book_version;
    if (config.consistency_mode == SnapshotConsistencyMode::StrictSameVersion &&
        skew != 0) {
        return reject(
            IntentStatus::RejectedBadMarketState,
            "snapshot versions differ",
            version
        );
    }
    if (config.consistency_mode == SnapshotConsistencyMode::BoundedSkew &&
        skew > config.max_snapshot_version_skew) {
        return reject(
            IntentStatus::RejectedBadMarketState,
            "snapshot version skew too large",
            version
        );
    }

    SnapshotConsistencyResult result;
    result.ok = true;
    result.version = version;
    return result;
}

}  // namespace trading_engine::signal
