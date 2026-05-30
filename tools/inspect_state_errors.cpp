#include "decode/core/DecodePipeline.h"
#include "feed/decode/DecodeInputAdapter.h"
#include "feed/raw_ingest/RawLogReader.h"
#include "state/core/MarketStateEventAdapter.h"
#include "state/core/MarketStateEventFilter.h"
#include "state/core/MarketStateStore.h"
#include "state/core/StateUniverse.h"
#include "engine/signal/reader/OracleArtifactReader.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

namespace decode = trading_engine::decode;
namespace feed = trading_engine::feed;
namespace signal = trading_engine::signal;
namespace state = trading_engine::state;

struct Config {
    std::string raw_path;
    std::string artifact_path;
    std::size_t limit = 200;
    bool apply_artifact_universe_filter = false;
};

struct ErrorRecord {
    std::uint64_t packet_id = 0;
    std::string asset_id;
    std::string event_type;
    std::string error_code;
    std::string reason;
};

struct Summary {
    std::uint64_t packets_read = 0;
    std::uint64_t normalized_events = 0;
    std::uint64_t state_errors_total = 0;
    std::uint64_t decode_errors = 0;

    std::map<std::string, std::uint64_t> by_error_code;
    std::map<std::string, std::uint64_t> by_asset_id;
    std::map<std::string, std::uint64_t> by_event_type;
    std::vector<ErrorRecord> first_errors;
};

struct FilterSummary {
    std::uint64_t events_seen = 0;
    std::uint64_t events_passed = 0;
    std::uint64_t events_filtered = 0;
    std::uint64_t filtered_paired_asset = 0;
    std::uint64_t filtered_non_universe_asset = 0;
    std::uint64_t filtered_non_universe_market = 0;
    std::uint64_t filtered_missing_asset = 0;
};

[[noreturn]] void fail(const std::string& message) {
    std::cerr << message << '\n';
    std::exit(1);
}

void print_usage() {
    std::cerr
        << "usage: inspect_state_errors --raw <feed.raw> "
        << "--artifact <oracle_artifact_dir> [--limit N] "
        << "[--apply-artifact-universe-filter]\n";
}

Config parse_args(int argc, char** argv) {
    Config config;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--raw" && i + 1 < argc) {
            config.raw_path = argv[++i];
        } else if (arg == "--artifact" && i + 1 < argc) {
            config.artifact_path = argv[++i];
        } else if (arg == "--limit" && i + 1 < argc) {
            config.limit = static_cast<std::size_t>(
                std::stoull(argv[++i])
            );
        } else if (arg == "--apply-artifact-universe-filter") {
            config.apply_artifact_universe_filter = true;
        } else if (arg == "--help" || arg == "-h") {
            print_usage();
            std::exit(0);
        } else {
            print_usage();
            fail("unknown or incomplete argument: " + arg);
        }
    }

    if (config.raw_path.empty()) {
        print_usage();
        fail("missing --raw");
    }
    if (config.artifact_path.empty()) {
        print_usage();
        fail("missing --artifact");
    }
    return config;
}

void initialize_summary(Summary* summary) {
    for (const std::string& key : {
             "DeltaBeforeSnapshot",
             "UnknownAsset",
             "NonTargetAsset",
             "PairedAssetFilteredLate",
             "MissingBook",
             "CrossedBook",
             "ClosedOrResolved",
             "InvalidDelta",
             "InternalError"}) {
        summary->by_error_code.emplace(key, 0);
    }

    for (const std::string& key : {
             "Snapshot",
             "Delta",
             "Lifecycle",
             "Heartbeat"}) {
        summary->by_event_type.emplace(key, 0);
    }
}

state::StateUniverse load_target_universe(const std::string& artifact_path) {
    signal::OracleArtifactReader reader;
    const auto result = reader.load(std::filesystem::path(artifact_path));
    if (!result.ok) {
        fail("failed to load oracle artifact: " + result.error);
    }

    state::StateUniverse universe;
    for (const auto& bundle : reader.active_bundles()) {
        for (std::uint16_t i = 0; i < bundle.leg_count; ++i) {
            const auto& leg = bundle.legs[i];
            if (!leg.asset_id.empty()) {
                universe.active_asset_ids.insert(leg.asset_id);
            }
            if (!leg.market_id.empty()) {
                universe.active_market_ids.insert(leg.market_id);
            }
        }
    }
    return universe;
}

std::string asset_for_event(
    const state::MarketStateEvent& event,
    const state::StateApplyResult& result
) {
    if (!event.asset_id.empty()) {
        return event.asset_id;
    }
    if (!event.ws_event.asset_id.empty()) {
        return event.ws_event.asset_id;
    }
    if (!event.ws_event.entity_id.empty()) {
        return event.ws_event.entity_id;
    }
    if (!result.entity_id.empty()) {
        return result.entity_id;
    }
    return "<missing>";
}

std::string classify_error(
    const state::MarketStateEvent& event,
    const state::StateApplyResult& result,
    const state::StateUniverse& universe
) {
    const std::string asset_id = asset_for_event(event, result);
    const bool has_asset = !asset_id.empty() && asset_id != "<missing>";
    const bool is_target_asset =
        has_asset &&
        universe.active_asset_ids.find(asset_id) !=
            universe.active_asset_ids.end();
    const bool is_target_market =
        !event.market_id.empty() &&
        universe.active_market_ids.find(event.market_id) !=
            universe.active_market_ids.end();

    if (has_asset && !is_target_asset) {
        return is_target_market ? "PairedAssetFilteredLate" : "NonTargetAsset";
    }

    switch (result.code) {
        case state::StateApplyCode::DeltaBeforeSnapshot:
            return "DeltaBeforeSnapshot";
        case state::StateApplyCode::MissingEntityId:
            return "UnknownAsset";
        case state::StateApplyCode::UnknownSide:
        case state::StateApplyCode::InvalidValue:
            return "InvalidDelta";
        case state::StateApplyCode::ClosedEntityIgnored:
            return "ClosedOrResolved";
        case state::StateApplyCode::Applied:
        case state::StateApplyCode::IgnoredHeartbeat:
        case state::StateApplyCode::IgnoredUnknown:
        case state::StateApplyCode::IgnoredTrade:
        case state::StateApplyCode::Noop:
            return "InternalError";
        default:
            return "InternalError";
    }
}

std::string output_event_type(state::MarketStateEventType type) {
    switch (type) {
        case state::MarketStateEventType::WsBookSnapshot:
            return "Snapshot";
        case state::MarketStateEventType::WsBookDelta:
            return "Delta";
        case state::MarketStateEventType::WsLifecycle:
            return "Lifecycle";
        case state::MarketStateEventType::WsHeartbeat:
            return "Heartbeat";
        default:
            return state::to_string(type);
    }
}

void record_error(
    Summary* summary,
    const state::MarketStateEvent& event,
    const state::StateApplyResult& result,
    const state::StateUniverse& universe,
    std::size_t limit
) {
    const std::string error_code = classify_error(event, result, universe);
    const std::string asset_id = asset_for_event(event, result);
    const std::string event_type = output_event_type(event.type);

    ++summary->state_errors_total;
    ++summary->by_error_code[error_code];
    ++summary->by_asset_id[asset_id];
    ++summary->by_event_type[event_type];

    if (summary->first_errors.size() < limit) {
        summary->first_errors.push_back(ErrorRecord{
            .packet_id = event.ws_event.packet_id != 0
                ? event.ws_event.packet_id
                : event.source_sequence,
            .asset_id = asset_id,
            .event_type = event_type,
            .error_code = error_code,
            .reason = result.message
        });
    }
}

void record_filter(
    FilterSummary* summary,
    const state::MarketStateEventFilterResult& result
) {
    ++summary->events_seen;
    if (result.passed()) {
        ++summary->events_passed;
        return;
    }

    ++summary->events_filtered;
    switch (result.reason) {
        case state::MarketStateEventFilterReason::PairedAssetNotInUniverse:
            ++summary->filtered_paired_asset;
            break;
        case state::MarketStateEventFilterReason::AssetNotInUniverse:
            ++summary->filtered_non_universe_asset;
            break;
        case state::MarketStateEventFilterReason::MarketNotInUniverse:
        case state::MarketStateEventFilterReason::MissingMarketId:
            ++summary->filtered_non_universe_market;
            break;
        case state::MarketStateEventFilterReason::MissingAssetId:
            ++summary->filtered_missing_asset;
            break;
        case state::MarketStateEventFilterReason::None:
            break;
    }
}

void print_count_map(
    const std::map<std::string, std::uint64_t>& counts
) {
    for (const auto& [key, value] : counts) {
        std::cout << "  " << key << ": " << value << '\n';
    }
}

void print_top_assets(
    const std::map<std::string, std::uint64_t>& counts
) {
    std::vector<std::pair<std::string, std::uint64_t>> sorted(
        counts.begin(),
        counts.end()
    );
    std::sort(
        sorted.begin(),
        sorted.end(),
        [](const auto& lhs, const auto& rhs) {
            if (lhs.second != rhs.second) {
                return lhs.second > rhs.second;
            }
            return lhs.first < rhs.first;
        }
    );

    for (const auto& [asset_id, count] : sorted) {
        std::cout << "  " << asset_id << ": " << count << '\n';
    }
}

void print_error_section(const Summary& summary) {
    std::cout << "state_errors_total: "
              << summary.state_errors_total << '\n';
    std::cout << "by_error_code:\n";
    print_count_map(summary.by_error_code);

    std::cout << "\nby_asset_id:\n";
    print_top_assets(summary.by_asset_id);

    std::cout << "\nby_event_type:\n";
    print_count_map(summary.by_event_type);

    std::cout << "\nfirst_errors:\n";
    for (const auto& error : summary.first_errors) {
        std::cout << "  - packet_id: " << error.packet_id << '\n';
        std::cout << "    asset_id: " << error.asset_id << '\n';
        std::cout << "    event_type: " << error.event_type << '\n';
        std::cout << "    error_code: " << error.error_code << '\n';
        std::cout << "    reason: " << error.reason << '\n';
    }
}

void print_filter_section(const FilterSummary& summary) {
    std::cout << "state_filter:\n";
    std::cout << "  events_seen: " << summary.events_seen << '\n';
    std::cout << "  events_passed: " << summary.events_passed << '\n';
    std::cout << "  events_filtered: " << summary.events_filtered << '\n';
    std::cout << "  filtered_paired_asset: "
              << summary.filtered_paired_asset << '\n';
    std::cout << "  filtered_non_universe_asset: "
              << summary.filtered_non_universe_asset << '\n';
    std::cout << "  filtered_non_universe_market: "
              << summary.filtered_non_universe_market << '\n';
    std::cout << "  filtered_missing_asset: "
              << summary.filtered_missing_asset << '\n';
}

void print_summary(const Summary& summary) {
    std::cout << "inspect_state_errors:\n";
    std::cout << "  packets_read: " << summary.packets_read << '\n';
    std::cout << "  normalized_events: " << summary.normalized_events << '\n';
    std::cout << "  decode_errors: " << summary.decode_errors << "\n\n";
    print_error_section(summary);
}

void print_filtered_summary(
    const Summary& without_filter,
    const Summary& with_filter,
    const FilterSummary& filter_summary
) {
    std::cout << "inspect_state_errors:\n";
    std::cout << "  packets_read: " << without_filter.packets_read << '\n';
    std::cout << "  normalized_events: "
              << without_filter.normalized_events << '\n';
    std::cout << "  decode_errors: " << without_filter.decode_errors << '\n';
    std::cout << "  apply_artifact_universe_filter: true\n\n";

    std::cout << "without_filter:\n";
    print_error_section(without_filter);
    std::cout << "\nwith_filter:\n";
    print_error_section(with_filter);
    std::cout << '\n';
    print_filter_section(filter_summary);
}

}  // namespace

int main(int argc, char** argv) {
    const Config config = parse_args(argc, argv);
    const state::StateUniverse universe =
        load_target_universe(config.artifact_path);

    Summary without_filter;
    Summary with_filter;
    FilterSummary filter_summary;
    initialize_summary(&without_filter);
    initialize_summary(&with_filter);

    feed::RawLogReader reader(config.raw_path);
    decode::DecodePipeline pipeline;
    state::MarketStateStore unfiltered_store;
    state::MarketStateStore filtered_store;
    state::MarketStateEventFilter event_filter(universe);

    while (true) {
        auto raw = reader.next();
        if (raw.eof()) {
            break;
        }
        if (!raw.ok()) {
            fail("raw read failed: " + raw.message);
        }

        ++without_filter.packets_read;
        ++with_filter.packets_read;

        decode::NormalizedEventBatch batch;
        const auto decoded = pipeline.decode(
            feed::to_decode_input_view(*raw.packet),
            &batch
        );
        if (!decoded.ok() &&
            decoded.payload_kind != decode::JsonDecodeKind::NonJsonControl) {
            ++without_filter.decode_errors;
            ++with_filter.decode_errors;
        }

        without_filter.normalized_events += batch.size();
        with_filter.normalized_events += batch.size();
        for (const auto& event : state::from_normalized_batch(batch)) {
            const auto unfiltered_result = unfiltered_store.apply(event);
            if (!unfiltered_result.ok()) {
                record_error(
                    &without_filter,
                    event,
                    unfiltered_result,
                    universe,
                    config.limit
                );
            }

            if (!config.apply_artifact_universe_filter) {
                continue;
            }

            const auto filter_result = event_filter.filter(event);
            record_filter(&filter_summary, filter_result);
            if (!filter_result.passed()) {
                continue;
            }

            const auto filtered_result = filtered_store.apply(event);
            if (!filtered_result.ok()) {
                record_error(
                    &with_filter,
                    event,
                    filtered_result,
                    universe,
                    config.limit
                );
            }
        }
    }

    if (config.apply_artifact_universe_filter) {
        print_filtered_summary(without_filter, with_filter, filter_summary);
    } else {
        print_summary(without_filter);
    }
    return without_filter.decode_errors == 0 ? 0 : 1;
}
