#include "engine/decision_fastpath/core/DifferentialVerifier.h"
#include "engine/decision_fastpath/core/EventLocalDecisionPipeline.h"
#include "engine/signal/reader/OracleArtifactReader.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

namespace fast = trading_engine::decision_fastpath;
namespace risk = trading_engine::risk;
namespace signal = trading_engine::signal;
namespace state = trading_engine::state;

using Clock = std::chrono::steady_clock;

struct Config {
    std::filesystem::path raw_path;
    std::filesystem::path artifact_path{
        "runs/worldcup_30615_full_20260530_062519/"
        "oracle_artifact_30615_top8"
    };
    std::string mode = "shadow";
    std::string kernel = "scalar";
    std::uint32_t repeat = 200;
};

struct Percentiles {
    double p50 = 0.0;
    double p95 = 0.0;
    double p99 = 0.0;
    double max = 0.0;
};

struct Probe {
    std::uint64_t eligible = 0;
    std::uint64_t taken = 0;
    std::uint64_t fallback = 0;
    std::uint64_t mismatch = 0;
    std::uint64_t disabled = 0;

    std::vector<std::uint64_t> generic_filter_to_plan_ns;
    std::vector<std::uint64_t> fast_filter_to_plan_ns;
    std::vector<std::uint64_t> fast_kernel_ns;
    std::vector<std::uint64_t> verifier_ns;
    std::vector<std::uint64_t> fallback_generic_ns;
    std::vector<double> speedups;

    std::uint64_t last_fast_combined_hash = 0;
    std::uint64_t last_generic_combined_hash = 0;
};

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

[[nodiscard]] std::uint64_t elapsed_ns(
    const Clock::time_point& start,
    const Clock::time_point& end
) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start)
            .count()
    );
}

[[nodiscard]] Percentiles percentiles(std::vector<std::uint64_t> values) {
    Percentiles out;
    if (values.empty()) {
        return out;
    }
    std::sort(values.begin(), values.end());
    const auto at = [&](double q) {
        const auto index = std::min<std::size_t>(
            values.size() - 1U,
            static_cast<std::size_t>(q * static_cast<double>(values.size() - 1U))
        );
        return static_cast<double>(values[index]) / 1'000.0;
    };
    out.p50 = at(0.50);
    out.p95 = at(0.95);
    out.p99 = at(0.99);
    out.max = static_cast<double>(values.back()) / 1'000.0;
    return out;
}

[[nodiscard]] Percentiles speedup_percentiles(std::vector<double> values) {
    Percentiles out;
    if (values.empty()) {
        return out;
    }
    std::sort(values.begin(), values.end());
    const auto at = [&](double q) {
        const auto index = std::min<std::size_t>(
            values.size() - 1U,
            static_cast<std::size_t>(q * static_cast<double>(values.size() - 1U))
        );
        return values[index];
    };
    out.p50 = at(0.50);
    out.p95 = at(0.95);
    out.p99 = at(0.99);
    out.max = values.back();
    return out;
}

[[nodiscard]] state::MarketDepthView make_depth_view(std::uint32_t asset_index) {
    state::MarketDepthView view;
    view.asset_index = asset_index;
    view.book_version = 10 + asset_index;
    view.snapshot_version_hash = 1'000 + asset_index;
    view.last_ws_recv_ns = 1'000;
    view.usable_for_depth = true;
    view.ask_count = 2;
    view.asks[0] = state::PriceLevel{
        .price_tick = 400'000,
        .price = 0.40,
        .size = 10.0
    };
    view.asks[1] = state::PriceLevel{
        .price_tick = 410'000,
        .price = 0.41,
        .size = 10.0
    };
    state::build_depth_prefix(
        view.bids,
        view.bid_count,
        view.asks,
        view.ask_count,
        &view.prefix
    );
    return view;
}

[[nodiscard]] std::vector<state::MarketDepthView> depths_for_specs(
    std::span<const fast::FixedShapeKernelSpec> specs
) {
    std::unordered_map<std::uint32_t, state::MarketDepthView> by_asset;
    for (const auto& spec : specs) {
        for (std::uint8_t i = 0; i < spec.leg_count; ++i) {
            by_asset.emplace(spec.asset_indices[i], make_depth_view(spec.asset_indices[i]));
        }
    }
    std::vector<state::MarketDepthView> depths;
    depths.reserve(by_asset.size());
    for (const auto& [_, depth] : by_asset) {
        depths.push_back(depth);
    }
    std::sort(
        depths.begin(),
        depths.end(),
        [](const state::MarketDepthView& lhs,
           const state::MarketDepthView& rhs) {
            return lhs.asset_index < rhs.asset_index;
        }
    );
    return depths;
}

[[nodiscard]] risk::RiskPolicySnapshot policy() {
    risk::RiskPolicySnapshot out;
    out.max_book_age_ns = 1'000'000'000;
    out.min_depth_margin_ratio = 1.0;
    out.min_depth_margin_bps = 10'000;
    out.max_approvals_per_second = 1'000'000;
    return risk::with_computed_policy_hash(out);
}

[[nodiscard]] fast::EventLocalPipelineMode parse_mode(
    const std::string& mode
) {
    if (mode == "disabled") {
        return fast::EventLocalPipelineMode::Disabled;
    }
    if (mode == "shadow") {
        return fast::EventLocalPipelineMode::ShadowCompare;
    }
    if (mode == "verified-paper") {
        return fast::EventLocalPipelineMode::VerifiedPaper;
    }
    if (mode == "paper-authoritative") {
        return fast::EventLocalPipelineMode::PaperAuthoritative;
    }
    fail("unknown mode: " + mode);
}

[[nodiscard]] Config parse_args(int argc, char** argv) {
    Config config;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                fail("missing value for " + arg);
            }
            return argv[++i];
        };
        if (arg == "--raw") {
            config.raw_path = next();
        } else if (arg == "--artifact") {
            config.artifact_path = next();
        } else if (arg == "--mode") {
            config.mode = next();
        } else if (arg == "--kernel") {
            config.kernel = next();
        } else if (arg == "--repeat") {
            config.repeat = static_cast<std::uint32_t>(std::stoul(next()));
        } else if (arg == "--help") {
            std::cout
                << "usage: probe_fastpath_latency --artifact <dir> "
                   "[--raw <raw>] [--repeat N] "
                   "[--mode disabled|shadow|verified-paper|paper-authoritative] "
                   "[--kernel scalar|avx2|avx512]\n";
            std::exit(0);
        } else {
            fail("unknown arg: " + arg);
        }
    }
    return config;
}

[[nodiscard]] fast::DecisionPathSnapshot best_generic_for_dirty_asset(
    std::span<const fast::FixedShapeKernelSpec> specs,
    std::span<const state::MarketDepthView> depths,
    const risk::RiskPolicySnapshot& policy,
    const risk::RiskLedgerSnapshot& ledger,
    std::uint64_t now_ns
) {
    fast::DecisionPathSnapshot best;
    bool have_best = false;
    for (const auto& spec : specs) {
        const auto candidate = fast::reference_generic_decision(
            spec,
            depths,
            policy,
            ledger,
            now_ns
        );
        if (candidate.fallback_required) {
            return candidate;
        }
        if (!have_best ||
            candidate.total_edge_tick > best.total_edge_tick ||
            (candidate.total_edge_tick == best.total_edge_tick &&
             candidate.bundle_id < best.bundle_id)) {
            best = candidate;
            have_best = true;
        }
    }
    return best;
}

void print_percentiles(const std::string& label, const Percentiles& p) {
    std::cout << "  " << label << ":\n"
              << "    p50: " << p.p50 << "\n"
              << "    p95: " << p.p95 << "\n"
              << "    p99: " << p.p99 << "\n"
              << "    max: " << p.max << "\n";
}

void run_probe(const Config& config) {
    if (config.kernel != "scalar") {
        std::cout << "fastpath:\n"
                  << "  eligible: 0\n"
                  << "  taken: 0\n"
                  << "  fallback: 0\n"
                  << "  mismatch: 0\n"
                  << "  disabled: 1\n\n"
                  << "latency:\n"
                  << "  kernel_disabled: true\n\n"
                  << "speedup:\n"
                  << "  p50: 0\n"
                  << "  p95: 0\n"
                  << "  p99: 0\n";
        return;
    }

    signal::OracleArtifactReader reader;
    const auto load = reader.load(config.artifact_path);
    if (!load.ok) {
        fail("failed to load artifact: " + load.error);
    }

    fast::FixedShapeKernelRegistry registry;
    registry.build_from_oracle_artifact(reader);
    if (registry.specs().empty()) {
        fail("artifact has no fastpath specs");
    }

    const auto risk_policy = policy();
    const risk::RiskLedgerSnapshot ledger;
    const auto depths = depths_for_specs(registry.specs());

    std::vector<std::uint32_t> dirty_assets;
    for (const auto& spec : registry.specs()) {
        for (std::uint8_t i = 0; i < spec.leg_count; ++i) {
            dirty_assets.push_back(spec.asset_indices[i]);
        }
    }
    std::sort(dirty_assets.begin(), dirty_assets.end());
    dirty_assets.erase(
        std::unique(dirty_assets.begin(), dirty_assets.end()),
        dirty_assets.end()
    );

    fast::EventLocalDecisionPipelineConfig pipeline_config;
    pipeline_config.expected_policy_hash = risk_policy.policy_hash;
    pipeline_config.expected_artifact_hash = registry.specs().front().artifact_hash;
    pipeline_config.expected_constraint_hash = registry.specs().front().constraint_hash;
    pipeline_config.mode = parse_mode(config.mode);
    pipeline_config.fast_path.mode = parse_mode(config.mode);
    pipeline_config.fast_path.enable_fixed_buy_kernel = true;
    pipeline_config.fast_path.sample_verify_rate =
        pipeline_config.fast_path.mode == fast::FastPathMode::PaperAuthoritative
            ? 0.0
            : 1.0;
    fast::EventLocalDecisionPipeline pipeline{&registry, pipeline_config};
    fast::FixedBuyBundleKernelScalar kernel;
    fast::FastPathScratch scratch;

    Probe probe;
    probe.generic_filter_to_plan_ns.reserve(config.repeat * dirty_assets.size());
    probe.fast_filter_to_plan_ns.reserve(config.repeat * dirty_assets.size());
    probe.fast_kernel_ns.reserve(config.repeat * dirty_assets.size());
    probe.verifier_ns.reserve(config.repeat * dirty_assets.size());
    probe.speedups.reserve(config.repeat * dirty_assets.size());

    for (std::uint32_t repeat = 0; repeat < config.repeat; ++repeat) {
        for (const auto dirty_asset : dirty_assets) {
            const auto affected = registry.specs_for_asset(dirty_asset);
            if (affected.empty()) {
                ++probe.disabled;
                continue;
            }
            ++probe.eligible;

            const auto generic_start = Clock::now();
            const auto generic = best_generic_for_dirty_asset(
                affected,
                depths,
                risk_policy,
                ledger,
                2'000 + repeat
            );
            const auto generic_end = Clock::now();
            const auto generic_ns = elapsed_ns(generic_start, generic_end);
            probe.generic_filter_to_plan_ns.push_back(generic_ns);

            for (const auto& spec : affected) {
                const auto kernel_start = Clock::now();
                (void)kernel.run(
                    spec,
                    depths.data(),
                    static_cast<std::uint16_t>(depths.size()),
                    risk_policy,
                    ledger,
                    2'000 + repeat,
                    &scratch
                );
                const auto kernel_end = Clock::now();
                probe.fast_kernel_ns.push_back(
                    elapsed_ns(kernel_start, kernel_end)
                );
            }

            const auto fast_start = Clock::now();
            const auto fast_result = pipeline.process({
                .now_ns = 2'000 + repeat,
                .dirty_asset_index = dirty_asset,
                .depth_views = depths.data(),
                .depth_view_count = static_cast<std::uint16_t>(depths.size()),
                .policy = &risk_policy,
                .ledger = &ledger,
                .current_true_mask = 0,
                .current_false_mask = 0
            });
            const auto fast_end = Clock::now();
            const auto fast_ns = elapsed_ns(fast_start, fast_end);
            probe.fast_filter_to_plan_ns.push_back(fast_ns);

            if (fast_result.fallback_required) {
                ++probe.fallback;
                probe.fallback_generic_ns.push_back(generic_ns);
            } else {
                ++probe.taken;
            }

            const auto verifier_start = Clock::now();
            const auto diff = fast::compare_decision_snapshots(
                fast::snapshot_from_fast_result(fast_result),
                generic
            );
            probe.last_fast_combined_hash = diff.fast_combined_hash;
            probe.last_generic_combined_hash = diff.generic_combined_hash;
            const auto verifier_end = Clock::now();
            probe.verifier_ns.push_back(
                elapsed_ns(verifier_start, verifier_end)
            );
            if (!diff.match) {
                ++probe.mismatch;
            }
            if (fast_ns > 0) {
                probe.speedups.push_back(
                    static_cast<double>(generic_ns) /
                    static_cast<double>(fast_ns)
                );
            }
        }
    }

    std::cout << "fastpath:\n"
              << "  eligible: " << probe.eligible << "\n"
              << "  taken: " << probe.taken << "\n"
              << "  fallback: " << probe.fallback << "\n"
              << "  mismatch: " << probe.mismatch << "\n"
              << "  disabled: " << probe.disabled << "\n\n";

    std::cout << "hashes:\n"
              << "  fast_combined_hash: "
              << probe.last_fast_combined_hash << "\n"
              << "  generic_combined_hash: "
              << probe.last_generic_combined_hash << "\n"
              << "  combined_hash_match: "
              << (probe.last_fast_combined_hash ==
                          probe.last_generic_combined_hash
                      ? "true"
                      : "false")
              << "\n\n";

    std::cout << "latency:\n";
    print_percentiles(
        "generic_filter_to_plan",
        percentiles(probe.generic_filter_to_plan_ns)
    );
    print_percentiles(
        "fast_filter_to_plan",
        percentiles(probe.fast_filter_to_plan_ns)
    );
    print_percentiles("fast_kernel", percentiles(probe.fast_kernel_ns));
    print_percentiles("verifier", percentiles(probe.verifier_ns));
    print_percentiles(
        "fallback_generic",
        percentiles(probe.fallback_generic_ns)
    );

    const auto speedup = speedup_percentiles(probe.speedups);
    std::cout << "\nspeedup:\n"
              << "  p50: " << speedup.p50 << "\n"
              << "  p95: " << speedup.p95 << "\n"
              << "  p99: " << speedup.p99 << "\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        run_probe(parse_args(argc, argv));
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
