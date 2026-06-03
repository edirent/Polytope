#include "engine/execution/adapter/PaperMakerExecutionAdapter.h"
#include "engine/paper/ledger/PaperEventAdapter.h"
#include "engine/paper/ledger/PaperLedger.h"
#include "engine/paper/pnl/MakerPnLEngine.h"
#include "engine/risk/quote/QuoteRiskEvaluator.h"
#include "engine/strategy/market_making/core/MarketMakingEngine.h"
#include "engine/strategy/market_making/tools/MarketMakingTools.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace execution = trading_engine::execution;
namespace paper = trading_engine::paper;
namespace risk = trading_engine::risk;
namespace state = trading_engine::state;
namespace mm = trading_engine::strategy::market_making;

using Clock = std::chrono::steady_clock;

struct Args {
    std::uint64_t iterations = 50'000;
    std::uint64_t warmup = 5'000;
    std::int64_t starting_cash_tick = 1'000'000'000'000LL;
    execution::PaperMakerFillMode fill_mode =
        execution::PaperMakerFillMode::Conservative;
};

struct LatencyStats {
    std::uint64_t count = 0;
    std::uint64_t min = 0;
    std::uint64_t p50 = 0;
    std::uint64_t p95 = 0;
    std::uint64_t p99 = 0;
    std::uint64_t max = 0;
    double mean = 0.0;
};

struct LatencySamples {
    std::vector<std::uint64_t> market_making_ns;
    std::vector<std::uint64_t> quote_risk_ns;
    std::vector<std::uint64_t> maker_submit_ns;
    std::vector<std::uint64_t> maker_fill_sim_ns;
    std::vector<std::uint64_t> paper_accounting_ns;
    std::vector<std::uint64_t> total_ns;
};

struct ProbeSummary {
    std::uint64_t attempted = 0;
    std::uint64_t measured = 0;
    std::uint64_t quotes_emitted = 0;
    std::uint64_t approved_quotes = 0;
    std::uint64_t maker_reports = 0;
    std::uint64_t maker_fills_applied = 0;
    std::uint64_t rejected_or_skipped = 0;
    std::uint64_t duplicate_ignored = 0;
    std::int64_t final_cash_tick = 0;
    std::int64_t realized_pnl_tick = 0;
    std::int64_t open_position_lots = 0;
    paper::MakerPnLSnapshot final_pnl;
};

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

std::uint64_t elapsed_ns(Clock::time_point start, Clock::time_point end) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()
    );
}

LatencyStats summarize(std::vector<std::uint64_t> values) {
    LatencyStats stats;
    if (values.empty()) {
        return stats;
    }

    std::sort(values.begin(), values.end());
    stats.count = values.size();
    stats.min = values.front();
    stats.max = values.back();

    auto percentile = [&values](double p) -> std::uint64_t {
        if (values.empty()) {
            return 0;
        }
        const auto index = static_cast<std::size_t>(
            (static_cast<double>(values.size() - 1) * p) + 0.5
        );
        return values[std::min(index, values.size() - 1)];
    };

    stats.p50 = percentile(0.50);
    stats.p95 = percentile(0.95);
    stats.p99 = percentile(0.99);

    long double total = 0.0;
    for (const auto value : values) {
        total += static_cast<long double>(value);
    }
    stats.mean = static_cast<double>(total / values.size());
    return stats;
}

void print_stats(const char* name, const LatencyStats& stats) {
    std::cout << "  " << name << ":\n"
              << "    count: " << stats.count << "\n"
              << "    min: " << stats.min << "\n"
              << "    p50: " << stats.p50 << "\n"
              << "    p95: " << stats.p95 << "\n"
              << "    p99: " << stats.p99 << "\n"
              << "    max: " << stats.max << "\n"
              << "    mean: " << stats.mean << "\n";
}

execution::PaperMakerFillMode parse_fill_mode(const std::string& value) {
    if (value == "nofill" || value == "NoFill") {
        return execution::PaperMakerFillMode::NoFill;
    }
    if (value == "book-cross" || value == "BookCross") {
        return execution::PaperMakerFillMode::BookCross;
    }
    if (value == "conservative" || value == "Conservative") {
        return execution::PaperMakerFillMode::Conservative;
    }
    fail("unknown fill mode: " + value);
}

Args parse_args(int argc, char** argv) {
    Args args;
    for (int index = 1; index < argc; ++index) {
        const std::string arg{argv[index]};
        auto require_value = [&](const char* name) -> std::string {
            if (index + 1 >= argc) {
                fail(std::string{"missing value for "} + name);
            }
            return argv[++index];
        };

        if (arg == "--iterations") {
            args.iterations = std::stoull(require_value("--iterations"));
        } else if (arg == "--warmup") {
            args.warmup = std::stoull(require_value("--warmup"));
        } else if (arg == "--starting-cash") {
            args.starting_cash_tick = std::stoll(require_value("--starting-cash"));
        } else if (arg == "--fill-mode") {
            args.fill_mode = parse_fill_mode(require_value("--fill-mode"));
        } else if (arg == "--help" || arg == "-h") {
            std::cout
                << "usage: probe_market_making_e2e_latency "
                << "[--iterations N] [--warmup N] "
                << "[--fill-mode conservative|nofill|book-cross] "
                << "[--starting-cash TICKS]\n";
            std::exit(0);
        } else {
            fail("unknown argument: " + arg);
        }
    }
    if (args.iterations == 0) {
        fail("--iterations must be positive");
    }
    return args;
}

mm::MarketMakingConfig market_making_config() {
    mm::MarketMakingConfig config;
    config.strategy_id = 99;
    config.oracle_artifact_hash = 11;
    config.policy_hash = 22;
    config.min_half_spread_tick = mm::kDefaultDefensiveHalfSpreadTick;
    config.max_inventory_skew_tick =
        mm::kDefaultDefensiveInventorySkewTick;
    config.base_quote_size_lots = mm::kDefaultDefensiveQuoteSizeLots;
    config.max_inventory_lots = 100;
    config.quote_ttl_ns = 5'000'000'000ULL;
    config.requote_threshold_tick = 1'000;
    return config;
}

risk::QuoteRiskPolicy quote_risk_policy() {
    risk::QuoteRiskPolicy policy;
    policy.max_quote_notional_tick = 10'000'000;
    policy.max_asset_inventory_lots = 100;
    policy.min_edge_to_fair_tick = -50'000;
    policy.max_book_age_ns = 1'000'000'000ULL;
    return policy;
}

state::MarketDepthView depth_for_iteration(std::uint64_t iteration) {
    const auto offset = (iteration % 2 == 0) ? 0 : 20'000;
    const auto version = iteration + 1;
    const auto now_ns = 1'000'000'000ULL + iteration * 1'000'000ULL;
    return mm::tools::make_depth_view(
        490'000 + offset,
        510'000 + offset,
        100.0,
        100.0,
        version,
        now_ns
    );
}

std::int64_t open_position_lots(const paper::PaperLedger& ledger) {
    std::int64_t lots = 0;
    for (const auto& [_, position] : ledger.position_ledger().positions()) {
        lots += position.qty_lots;
    }
    return lots;
}

void maybe_record(
    LatencySamples* samples,
    bool measured,
    std::uint64_t market_making_ns,
    std::uint64_t quote_risk_ns,
    std::uint64_t maker_submit_ns,
    std::uint64_t maker_fill_sim_ns,
    std::uint64_t paper_accounting_ns,
    std::uint64_t total_ns
) {
    if (!measured) {
        return;
    }
    samples->market_making_ns.push_back(market_making_ns);
    samples->quote_risk_ns.push_back(quote_risk_ns);
    samples->maker_submit_ns.push_back(maker_submit_ns);
    samples->maker_fill_sim_ns.push_back(maker_fill_sim_ns);
    samples->paper_accounting_ns.push_back(paper_accounting_ns);
    samples->total_ns.push_back(total_ns);
}

ProbeSummary run_probe(const Args& args, LatencySamples* samples) {
    mm::MarketMakingEngine market_making{market_making_config()};
    risk::QuoteRiskEvaluator risk_evaluator;
    const auto policy = quote_risk_policy();
    execution::PaperMakerExecutionAdapter maker_execution{args.fill_mode};
    paper::PaperEventAdapter paper_adapter;
    paper::PaperLedger ledger{args.starting_cash_tick};
    paper::MakerPnLEngine pnl_engine;

    ProbeSummary summary;
    const auto total_iterations = args.warmup + args.iterations;

    for (std::uint64_t i = 0; i < total_iterations; ++i) {
        const bool measured = i >= args.warmup;
        if (measured) {
            ++summary.attempted;
        }

        auto depth = depth_for_iteration(i);
        const auto now_ns = depth.last_ws_recv_ns;
        const auto total_start = Clock::now();

        const auto mm_start = Clock::now();
        const auto mm_result = market_making.on_market_update(mm::MarketMakingInput{
            .market_id = "m1",
            .asset_id = "asset_yes",
            .market_index = 1,
            .asset_index = depth.asset_index,
            .depth = &depth,
            .current_position_lots = open_position_lots(ledger),
            .now_ns = now_ns
        });
        const auto mm_end = Clock::now();

        if (mm_result.quote_count == 0) {
            if (measured) {
                ++summary.rejected_or_skipped;
            }
            continue;
        }
        if (measured) {
            summary.quotes_emitted += mm_result.quote_count;
        }

        const auto risk_start = Clock::now();
        const auto risk_result = risk_evaluator.evaluate(risk::QuoteRiskInput{
            .quote = &mm_result.quotes[0],
            .depth = &depth,
            .policy = &policy,
            .current_position_lots = open_position_lots(ledger),
            .now_ns = now_ns
        });
        const auto risk_end = Clock::now();

        if (risk_result.decision.decision !=
                risk::QuoteRiskDecisionType::Approve ||
            !risk_result.approved_quote) {
            if (measured) {
                ++summary.rejected_or_skipped;
            }
            continue;
        }
        if (measured) {
            ++summary.approved_quotes;
        }

        const auto submit_start = Clock::now();
        const auto& approved_quote = *risk_result.approved_quote;
        const auto submit = maker_execution.submit_approved_quote(
            approved_quote,
            now_ns
        );
        const auto submit_end = Clock::now();

        if (!submit.ok) {
            if (submit.duplicate_ignored && measured) {
                ++summary.duplicate_ignored;
            } else if (measured) {
                ++summary.rejected_or_skipped;
            }
            continue;
        }

        execution::PaperMakerMarketEvent event;
        event.ts_ns = now_ns + 1;
        event.asset_index = depth.asset_index;
        event.depth = &depth;
        event.has_trade = args.fill_mode != execution::PaperMakerFillMode::NoFill;
        event.trade_qty_lots = approved_quote.bid.quantity_lots;
        if (i % 2 == 0) {
            event.trade_aggressor_side = execution::OrderSide::Sell;
            event.trade_price_tick =
                approved_quote.bid.price_tick - 1;
        } else {
            event.trade_aggressor_side = execution::OrderSide::Buy;
            event.trade_price_tick =
                approved_quote.ask.price_tick + 1;
        }

        const auto fill_start = Clock::now();
        const auto reports = maker_execution.on_market_event(event);
        const auto fill_end = Clock::now();
        if (measured) {
            summary.maker_reports += reports.size();
        }

        const auto paper_start = Clock::now();
        for (const auto& report : reports) {
            const auto observed = paper_adapter.observe(report);
            if (!observed.has_paper_fill) {
                continue;
            }
            const auto applied = ledger.apply_fill(observed.paper_fill);
            if (applied.applied && measured) {
                ++summary.maker_fills_applied;
            } else if (
                applied.status == paper::PaperLedgerApplyStatus::DuplicateIgnored &&
                measured
            ) {
                ++summary.duplicate_ignored;
            }
        }
        const auto depth_span =
            std::span<const state::MarketDepthView>(&depth, 1);
        summary.final_pnl = pnl_engine.compute(ledger, depth_span, event.ts_ns);
        const auto paper_end = Clock::now();

        const auto total_end = Clock::now();
        if (measured) {
            ++summary.measured;
        }

        maybe_record(
            samples,
            measured,
            elapsed_ns(mm_start, mm_end),
            elapsed_ns(risk_start, risk_end),
            elapsed_ns(submit_start, submit_end),
            elapsed_ns(fill_start, fill_end),
            elapsed_ns(paper_start, paper_end),
            elapsed_ns(total_start, total_end)
        );
    }

    summary.final_cash_tick = ledger.cash_ledger().cash_tick;
    summary.realized_pnl_tick = ledger.cash_ledger().realized_pnl_tick;
    summary.open_position_lots = open_position_lots(ledger);
    return summary;
}

const char* fill_mode_name(execution::PaperMakerFillMode mode) noexcept {
    switch (mode) {
        case execution::PaperMakerFillMode::NoFill:
            return "NoFill";
        case execution::PaperMakerFillMode::Conservative:
            return "Conservative";
        case execution::PaperMakerFillMode::BookCross:
            return "BookCross";
    }
    return "Unknown";
}

void print_report(
    const Args& args,
    const ProbeSummary& summary,
    const LatencySamples& samples
) {
    std::cout << "market_making_e2e_latency:\n"
              << "  iterations: " << args.iterations << "\n"
              << "  warmup: " << args.warmup << "\n"
              << "  fill_mode: " << fill_mode_name(args.fill_mode) << "\n"
              << "  measured: " << summary.measured << "\n"
              << "  rejected_or_skipped: " << summary.rejected_or_skipped << "\n"
              << "  quotes_emitted: " << summary.quotes_emitted << "\n"
              << "  approved_quotes: " << summary.approved_quotes << "\n"
              << "  maker_reports: " << summary.maker_reports << "\n"
              << "  maker_fills_applied: " << summary.maker_fills_applied << "\n"
              << "  duplicate_ignored: " << summary.duplicate_ignored << "\n\n";

    std::cout << "ledger:\n"
              << "  final_cash: " << summary.final_cash_tick << "\n"
              << "  realized_pnl: " << summary.realized_pnl_tick << "\n"
              << "  open_position_lots: " << summary.open_position_lots << "\n"
              << "  equity_mid: " << summary.final_pnl.equity_mid_tick << "\n\n";

    std::cout << "latency_ns:\n";
    print_stats("market_making", summarize(samples.market_making_ns));
    print_stats("quote_risk", summarize(samples.quote_risk_ns));
    print_stats("maker_submit", summarize(samples.maker_submit_ns));
    print_stats("maker_fill_simulation", summarize(samples.maker_fill_sim_ns));
    print_stats("paper_accounting_and_pnl", summarize(samples.paper_accounting_ns));
    print_stats("total_e2e", summarize(samples.total_ns));
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const auto args = parse_args(argc, argv);
        LatencySamples samples;
        samples.market_making_ns.reserve(args.iterations);
        samples.quote_risk_ns.reserve(args.iterations);
        samples.maker_submit_ns.reserve(args.iterations);
        samples.maker_fill_sim_ns.reserve(args.iterations);
        samples.paper_accounting_ns.reserve(args.iterations);
        samples.total_ns.reserve(args.iterations);

        const auto summary = run_probe(args, &samples);
        print_report(args, summary, samples);
        return summary.measured == args.iterations ? 0 : 2;
    } catch (const std::exception& error) {
        std::cerr << "probe_market_making_e2e_latency failed: "
                  << error.what() << "\n";
        return 1;
    }
}
