#include "engine/signal/reader/FixtureMarketSnapshotReader.h"

#include <boost/json.hpp>

#include <fstream>
#include <sstream>
#include <unordered_set>

namespace trading_engine::signal {

namespace {

namespace json = boost::json;

std::string string_field(const json::object& object, const char* name) {
    const auto it = object.find(name);
    if (it == object.end() || !it->value().is_string()) {
        return {};
    }
    return json::value_to<std::string>(it->value());
}

bool bool_field(
    const json::object& object,
    const char* name,
    bool default_value = false
) {
    const auto it = object.find(name);
    if (it == object.end() || !it->value().is_bool()) {
        return default_value;
    }
    return it->value().as_bool();
}

std::uint64_t u64_field(const json::object& object, const char* name) {
    const auto it = object.find(name);
    if (it == object.end()) {
        return 0;
    }
    if (it->value().is_uint64()) {
        return it->value().as_uint64();
    }
    if (it->value().is_int64() && it->value().as_int64() >= 0) {
        return static_cast<std::uint64_t>(it->value().as_int64());
    }
    return 0;
}

std::int64_t i64_field(const json::object& object, const char* name) {
    const auto it = object.find(name);
    if (it == object.end()) {
        return 0;
    }
    if (it->value().is_int64()) {
        return it->value().as_int64();
    }
    if (it->value().is_uint64()) {
        return static_cast<std::int64_t>(it->value().as_uint64());
    }
    return 0;
}

double double_field(const json::object& object, const char* name) {
    const auto it = object.find(name);
    if (it == object.end()) {
        return 0.0;
    }
    if (it->value().is_double()) {
        return it->value().as_double();
    }
    if (it->value().is_int64()) {
        return static_cast<double>(it->value().as_int64());
    }
    if (it->value().is_uint64()) {
        return static_cast<double>(it->value().as_uint64());
    }
    return 0.0;
}

std::uint32_t u32_field(const json::object& object, const char* name) {
    return static_cast<std::uint32_t>(u64_field(object, name));
}

trading_engine::state::BookQuality quality_from_string(
    const std::string& text
) {
    using trading_engine::state::BookQuality;
    if (text == "Good") {
        return BookQuality::Good;
    }
    if (text == "Stale") {
        return BookQuality::Stale;
    }
    if (text == "Recovering") {
        return BookQuality::Recovering;
    }
    if (text == "Crossed") {
        return BookQuality::Crossed;
    }
    if (text == "ChainMismatch") {
        return BookQuality::ChainMismatch;
    }
    if (text == "ChainLagging") {
        return BookQuality::ChainLagging;
    }
    if (text == "Closed") {
        return BookQuality::Closed;
    }
    if (text == "Resolved") {
        return BookQuality::Resolved;
    }
    return BookQuality::Unknown;
}

void parse_levels(
    const json::object& object,
    const char* name,
    std::array<trading_engine::state::PriceLevel, trading_engine::state::kMaxSnapshotDepth>* out
) {
    const auto it = object.find(name);
    if (it == object.end() || !it->value().is_array()) {
        return;
    }

    std::uint32_t index = 0;
    for (const auto& value : it->value().as_array()) {
        if (index >= trading_engine::state::kMaxSnapshotDepth) {
            break;
        }
        if (!value.is_object()) {
            continue;
        }

        const auto& level = value.as_object();
        (*out)[index++] = trading_engine::state::PriceLevel{
            .price_tick = i64_field(level, "price_tick"),
            .price = double_field(level, "price"),
            .size = double_field(level, "size")
        };
    }
}

MarketStateSnapshot parse_snapshot(const json::object& object) {
    MarketStateSnapshot snapshot;
    snapshot.entity_id = string_field(object, "entity_id");
    snapshot.market_id = string_field(object, "market_id");
    snapshot.version = u64_field(object, "version");
    snapshot.last_book_update_ns = u64_field(object, "last_book_update_ns");
    if (snapshot.last_book_update_ns == 0) {
        snapshot.last_book_update_ns = u64_field(object, "last_ws_recv_ns");
    }
    snapshot.live = bool_field(object, "live");
    snapshot.recovering = bool_field(object, "recovering");
    snapshot.closed = bool_field(object, "closed");
    snapshot.resolved = bool_field(object, "resolved");
    snapshot.crossed = bool_field(object, "crossed");
    snapshot.has_bid = bool_field(object, "has_bid");
    snapshot.has_ask = bool_field(object, "has_ask");
    snapshot.best_bid_tick = i64_field(object, "best_bid_tick");
    snapshot.best_ask_tick = i64_field(object, "best_ask_tick");
    snapshot.bid_count = u32_field(object, "bid_count");
    snapshot.ask_count = u32_field(object, "ask_count");
    parse_levels(object, "bids", &snapshot.bids);
    parse_levels(object, "asks", &snapshot.asks);
    snapshot.state_hash = u64_field(object, "state_hash");
    snapshot.snapshot_version_hash =
        u64_field(object, "snapshot_version_hash");
    if (snapshot.snapshot_version_hash == 0) {
        snapshot.snapshot_version_hash = snapshot.state_hash;
    }
    snapshot.quality = quality_from_string(string_field(object, "quality"));
    snapshot.usable_for_depth = bool_field(object, "usable_for_depth");
    snapshot.usable_for_signal = bool_field(object, "usable_for_signal");
    return snapshot;
}

}  // namespace

bool FixtureMarketSnapshotReader::load(
    const std::filesystem::path& path,
    std::string* error
) {
    snapshots_.clear();

    std::ifstream input(path);
    if (!input) {
        if (error) {
            *error = "failed to open snapshot fixture: " + path.string();
        }
        return false;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();

    boost::json::error_code parse_error;
    const auto parsed = json::parse(buffer.str(), parse_error);
    if (parse_error || !parsed.is_object()) {
        if (error) {
            *error = "malformed snapshot fixture";
        }
        return false;
    }

    const auto& root = parsed.as_object();
    const auto snapshots_it = root.find("snapshots");
    if (snapshots_it == root.end() || !snapshots_it->value().is_array()) {
        if (error) {
            *error = "snapshot fixture missing snapshots";
        }
        return false;
    }

    for (const auto& value : snapshots_it->value().as_array()) {
        if (!value.is_object()) {
            if (error) {
                *error = "snapshot fixture entry is not object";
            }
            return false;
        }
        auto snapshot = parse_snapshot(value.as_object());
        if (snapshot.entity_id.empty()) {
            if (error) {
                *error = "snapshot fixture entry missing entity_id";
            }
            return false;
        }
        snapshots_.emplace(snapshot.entity_id, std::move(snapshot));
    }

    return true;
}

SnapshotReadResult FixtureMarketSnapshotReader::read_for_bundle(
    const CandidateBundle& bundle,
    const SignalConfig& config,
    std::uint64_t now_ns
) const {
    std::vector<MarketStateSnapshot> snapshots;
    snapshots.reserve(bundle.leg_count);
    std::unordered_set<std::string> seen_assets;
    for (std::uint16_t i = 0; i < bundle.leg_count; ++i) {
        const auto& leg = bundle.legs[i];
        if (!seen_assets.insert(leg.asset_id).second) {
            continue;
        }
        const auto it = snapshots_.find(leg.asset_id);
        if (it != snapshots_.end()) {
            snapshots.push_back(it->second);
        }
    }

    return validate_bundle_snapshots(bundle, config, snapshots, now_ns);
}

SnapshotBatchReadResult FixtureMarketSnapshotReader::read_for_plan(
    const BundleRuntimePlan& plan,
    const SignalConfig& config,
    std::uint64_t now_ns
) const {
    std::array<MarketStateSnapshot, kMaxIntentLegs> snapshots{};
    std::uint16_t snapshot_count = 0;
    for (std::uint16_t i = 0;
         i < plan.unique_asset_count && snapshot_count < kMaxIntentLegs;
         ++i) {
        const auto* asset_id = plan.unique_asset_ids[i];
        if (!asset_id) {
            continue;
        }
        const auto it = snapshots_.find(*asset_id);
        if (it != snapshots_.end()) {
            snapshots[snapshot_count++] = it->second;
        }
    }

    return validate_plan_snapshots(
        plan,
        config,
        snapshots,
        snapshot_count,
        now_ns
    );
}

DepthReadResult FixtureMarketSnapshotReader::read_depth_for_plan(
    const BundleRuntimePlan& plan,
    const SignalConfig& config,
    std::uint64_t now_ns
) const {
    std::array<trading_engine::state::MarketDepthView, kMaxIntentLegs>
        depth_views{};
    std::uint16_t depth_count = 0;
    for (std::uint16_t i = 0;
         i < plan.unique_asset_count && depth_count < kMaxIntentLegs;
         ++i) {
        const auto* asset_id = plan.unique_asset_ids[i];
        if (!asset_id) {
            continue;
        }
        const auto it = snapshots_.find(*asset_id);
        if (it != snapshots_.end()) {
            depth_views[depth_count++] =
                trading_engine::state::market_depth_view_from_snapshot(
                    it->second,
                    plan.unique_asset_indices[i]
                );
        }
    }

    return validate_plan_depth_views(
        plan,
        config,
        depth_views,
        depth_count,
        now_ns
    );
}

}  // namespace trading_engine::signal
