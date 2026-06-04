#include "engine/strategy/market_making/tools/MarketMakingTools.h"

#include "engine/execution/adapter/PaperMakerExecutionAdapter.h"
#include "engine/risk/quote/QuoteRiskEvaluator.h"

#include <iostream>
#include <optional>
#include <string>

namespace {

using trading_engine::risk::QuoteRiskDecisionType;
using trading_engine::risk::QuoteRiskEvaluator;
using trading_engine::risk::QuoteRiskInput;
using trading_engine::risk::QuoteRiskPolicy;
using trading_engine::execution::PaperMakerExecutionAdapter;
using trading_engine::execution::PaperMakerFillMode;
using trading_engine::execution::PaperMakerMarketEvent;

bool has_arg(int argc, char** argv, const std::string& arg) {
    for (int i = 1; i < argc; ++i) {
        if (argv[i] == arg) {
            return true;
        }
    }
    return false;
}

struct QuoteRiskSummary {
    std::uint64_t quote_intents_loaded = 0;
    std::uint64_t evaluated = 0;
    std::uint64_t approved_quotes = 0;
    std::uint64_t rejected_quotes = 0;
    std::uint64_t rejected_invalid = 0;
    std::uint64_t rejected_expired = 0;
    std::uint64_t rejected_stale_book = 0;
    std::uint64_t rejected_crossed_book = 0;
    std::uint64_t rejected_inventory_limit = 0;
    std::uint64_t rejected_exposure_limit = 0;
    std::uint64_t rejected_duplicate = 0;
    std::uint64_t rejected_too_frequent = 0;
    std::uint64_t rejected_low_edge_to_fair = 0;
    std::uint64_t rejected_kill_switch = 0;
    std::uint64_t output_hash = 0;
};

struct PaperMakerExecutionSummary {
    std::uint64_t approved_quotes = 0;
    std::uint64_t active_quotes = 0;
    std::uint64_t cancelled_quotes = 0;
    std::uint64_t expired_quotes = 0;
    std::uint64_t duplicate_quotes_ignored = 0;
    std::uint64_t maker_fills = 0;
    std::string fill_mode = "Conservative";
    bool determinism_passed = false;
    std::uint64_t output_hash = 0;
};

void observe(QuoteRiskSummary* summary, QuoteRiskDecisionType decision) {
    ++summary->evaluated;
    switch (decision) {
        case QuoteRiskDecisionType::Approve:
            ++summary->approved_quotes;
            break;
        case QuoteRiskDecisionType::RejectInvalidQuote:
            ++summary->rejected_quotes;
            ++summary->rejected_invalid;
            break;
        case QuoteRiskDecisionType::RejectExpiredQuote:
            ++summary->rejected_quotes;
            ++summary->rejected_expired;
            break;
        case QuoteRiskDecisionType::RejectStaleBook:
            ++summary->rejected_quotes;
            ++summary->rejected_stale_book;
            break;
        case QuoteRiskDecisionType::RejectCrossedBook:
            ++summary->rejected_quotes;
            ++summary->rejected_crossed_book;
            break;
        case QuoteRiskDecisionType::RejectInventoryLimit:
            ++summary->rejected_quotes;
            ++summary->rejected_inventory_limit;
            break;
        case QuoteRiskDecisionType::RejectExposureLimit:
            ++summary->rejected_quotes;
            ++summary->rejected_exposure_limit;
            break;
        case QuoteRiskDecisionType::RejectDuplicateQuote:
            ++summary->rejected_quotes;
            ++summary->rejected_duplicate;
            break;
        case QuoteRiskDecisionType::RejectQuoteTooFrequent:
            ++summary->rejected_quotes;
            ++summary->rejected_too_frequent;
            break;
        case QuoteRiskDecisionType::RejectLowEdgeToFair:
            ++summary->rejected_quotes;
            ++summary->rejected_low_edge_to_fair;
            break;
        case QuoteRiskDecisionType::RejectKillSwitch:
            ++summary->rejected_quotes;
            ++summary->rejected_kill_switch;
            break;
        default:
            ++summary->rejected_quotes;
            break;
    }
}

std::uint64_t mix_hash(std::uint64_t hash, std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        hash ^= (value >> shift) & 0xffU;
        hash *= 1099511628211ULL;
    }
    return hash;
}

QuoteRiskSummary evaluate_quote_risk(
    const trading_engine::strategy::market_making::MarketMakingResult& mm_result
) {
    QuoteRiskSummary summary;
    summary.quote_intents_loaded = mm_result.quote_count;
    if (mm_result.quote_count == 0) {
        return summary;
    }

    QuoteRiskPolicy policy;
    policy.max_quote_notional_tick = 10'000'000;
    policy.max_asset_inventory_lots = 100;
    policy.min_edge_to_fair_tick = -50'000;
    policy.max_book_age_ns = 1'000'000'000ULL;

    const auto fresh_depth =
        trading_engine::strategy::market_making::tools::make_depth_view(
            490'000,
            510'000,
            20.0,
            20.0,
            1,
            100
        );
    auto stale_depth = fresh_depth;
    stale_depth.last_ws_recv_ns = 1;

    QuoteRiskEvaluator evaluator;
    const auto approved = evaluator.evaluate(QuoteRiskInput{
        .quote = &mm_result.quotes[0],
        .depth = &fresh_depth,
        .policy = &policy,
        .current_position_lots = 20,
        .now_ns = 200
    });
    observe(&summary, approved.decision.decision);
    summary.output_hash =
        mix_hash(summary.output_hash, approved.decision.decision_id);

    auto stale_quote = mm_result.quotes[0];
    stale_quote.expires_at_ns = 3'000'000'000ULL;
    const auto stale = evaluator.evaluate(QuoteRiskInput{
        .quote = &stale_quote,
        .depth = &stale_depth,
        .policy = &policy,
        .current_position_lots = 20,
        .now_ns = 2'000'000'500ULL
    });
    observe(&summary, stale.decision.decision);
    summary.output_hash = mix_hash(summary.output_hash, stale.decision.decision_id);
    return summary;
}

std::optional<trading_engine::risk::ApprovedQuote> approve_first_quote(
    const trading_engine::strategy::market_making::MarketMakingResult& mm_result
) {
    if (mm_result.quote_count == 0) {
        return std::nullopt;
    }

    QuoteRiskPolicy policy;
    policy.max_quote_notional_tick = 10'000'000;
    policy.max_asset_inventory_lots = 100;
    policy.min_edge_to_fair_tick = -50'000;
    policy.max_book_age_ns = 1'000'000'000ULL;

    const auto depth = trading_engine::strategy::market_making::tools::make_depth_view(
        490'000,
        510'000,
        20.0,
        20.0,
        1,
        100
    );

    QuoteRiskEvaluator evaluator;
    const auto result = evaluator.evaluate(QuoteRiskInput{
        .quote = &mm_result.quotes[0],
        .depth = &depth,
        .policy = &policy,
        .current_position_lots = 20,
        .now_ns = 200
    });
    if (result.decision.decision != QuoteRiskDecisionType::Approve) {
        return std::nullopt;
    }
    return result.approved_quote;
}

const char* fill_mode_name(PaperMakerFillMode mode) {
    switch (mode) {
        case PaperMakerFillMode::NoFill:
            return "NoFill";
        case PaperMakerFillMode::Conservative:
            return "Conservative";
        case PaperMakerFillMode::BookCross:
            return "BookCross";
        case PaperMakerFillMode::MidCross:
            return "MidCross";
        case PaperMakerFillMode::QueueAware:
            return "QueueAware";
    }
    return "Unknown";
}

void observe_adapter(
    PaperMakerExecutionSummary* summary,
    const PaperMakerExecutionAdapter& adapter
) {
    summary->active_quotes += adapter.quote_book().active_quote_count();
    summary->cancelled_quotes += adapter.quote_book().cancelled_quote_count();
    summary->expired_quotes += adapter.quote_book().expired_quote_count();
    summary->duplicate_quotes_ignored +=
        adapter.quote_book().duplicate_ignored_count();
    summary->output_hash =
        mix_hash(summary->output_hash, adapter.quote_book().active_quote_count());
    summary->output_hash = mix_hash(
        summary->output_hash,
        adapter.quote_book().duplicate_ignored_count()
    );
    summary->output_hash =
        mix_hash(summary->output_hash, adapter.quote_book().cancelled_quote_count());
    summary->output_hash =
        mix_hash(summary->output_hash, adapter.quote_book().expired_quote_count());
}

PaperMakerExecutionSummary run_paper_maker_execution_once(
    const trading_engine::strategy::market_making::MarketMakingResult& mm_result
) {
    PaperMakerExecutionSummary summary;
    const auto approved = approve_first_quote(mm_result);
    if (!approved) {
        return summary;
    }
    summary.approved_quotes = 3;

    {
        PaperMakerExecutionAdapter no_fill(PaperMakerFillMode::NoFill);
        (void)no_fill.submit_approved_quote(*approved, 250);
        PaperMakerMarketEvent event;
        event.ts_ns = 300;
        event.asset_index = approved->bid.asset_index;
        event.has_trade = true;
        event.trade_price_tick = approved->bid.price_tick - 1'000;
        event.trade_qty_lots = approved->bid.quantity_lots;
        const auto reports = no_fill.on_market_event(event);
        summary.maker_fills += reports.size();
        observe_adapter(&summary, no_fill);
    }

    {
        PaperMakerExecutionAdapter conservative(PaperMakerFillMode::Conservative);
        auto quote = *approved;
        quote.idempotency_hash += 101;
        quote.quote_group_id += 101;
        quote.approved_quote_id =
            trading_engine::risk::compute_approved_quote_hash(quote);
        (void)conservative.submit_approved_quote(quote, 250);
        (void)conservative.submit_approved_quote(quote, 260);
        PaperMakerMarketEvent no_trade;
        no_trade.ts_ns = 300;
        no_trade.asset_index = quote.bid.asset_index;
        (void)conservative.on_market_event(no_trade);
        PaperMakerMarketEvent trade_through;
        trade_through.ts_ns = 350;
        trade_through.asset_index = quote.bid.asset_index;
        trade_through.has_trade = true;
        trade_through.trade_price_tick = quote.bid.price_tick - 1'000;
        trade_through.trade_qty_lots = quote.bid.quantity_lots;
        const auto reports = conservative.on_market_event(trade_through);
        for (const auto& report : reports) {
            summary.output_hash = mix_hash(summary.output_hash, report.report_id);
        }
        summary.maker_fills += reports.size();
        summary.fill_mode = fill_mode_name(conservative.fill_mode());
        observe_adapter(&summary, conservative);
    }

    {
        PaperMakerExecutionAdapter book_cross(PaperMakerFillMode::BookCross);
        auto quote = *approved;
        quote.idempotency_hash += 202;
        quote.quote_group_id += 202;
        quote.approved_quote_id =
            trading_engine::risk::compute_approved_quote_hash(quote);
        (void)book_cross.submit_approved_quote(quote, 250);
        const auto depth =
            trading_engine::strategy::market_making::tools::make_depth_view(
                quote.bid.price_tick - 5'000,
                quote.bid.price_tick - 1'000,
                20.0,
                static_cast<double>(quote.bid.quantity_lots),
                3,
                300
            );
        PaperMakerMarketEvent event;
        event.ts_ns = 350;
        event.asset_index = quote.bid.asset_index;
        event.depth = &depth;
        const auto reports = book_cross.on_market_event(event);
        for (const auto& report : reports) {
            summary.output_hash = mix_hash(summary.output_hash, report.report_id);
        }
        summary.maker_fills += reports.size();
        observe_adapter(&summary, book_cross);
    }

    return summary;
}

PaperMakerExecutionSummary evaluate_paper_maker_execution(
    const trading_engine::strategy::market_making::MarketMakingResult& mm_result
) {
    auto summary = run_paper_maker_execution_once(mm_result);
    const auto repeat = run_paper_maker_execution_once(mm_result);
    summary.determinism_passed = summary.output_hash == repeat.output_hash &&
                                 summary.maker_fills == repeat.maker_fills &&
                                 summary.active_quotes == repeat.active_quotes;
    return summary;
}

}  // namespace

int main(int argc, char** argv) {
    const bool check_determinism = has_arg(argc, argv, "--check-determinism");
    const bool require_quote = has_arg(argc, argv, "--require-quote");

    const auto result =
        trading_engine::strategy::market_making::tools::run_small_workflow(
            check_determinism
        );

    const bool determinism_passed = result.ok;
    const auto quote_risk = evaluate_quote_risk(result);
    const auto quote_risk_repeat = evaluate_quote_risk(result);
    const bool quote_risk_determinism =
        quote_risk.output_hash == quote_risk_repeat.output_hash;
    const auto paper_maker = evaluate_paper_maker_execution(result);
    std::cout << "market_making:\n";
    std::cout << "  snapshots_loaded: " << result.snapshots_seen << "\n";
    std::cout << "  quote_intents: " << result.quotes_emitted << "\n";
    std::cout << "  cancel_intents: " << result.cancels_emitted << "\n";
    std::cout << "  approved_quotes: " << result.approved_quotes << "\n";
    std::cout << "  rejected_quotes: " << result.rejected_quotes << "\n";
    std::cout << "  active_quotes: " << result.active_quotes << "\n";
    std::cout << "  maker_fills: " << result.maker_fills << "\n\n";
    std::cout << "quotes:\n";
    std::cout << "  avg_half_spread: " << result.avg_half_spread_tick << "\n";
    std::cout << "  avg_inventory_skew: " << result.avg_inventory_skew_tick << "\n";
    std::cout << "  quote_uptime: " << result.quote_uptime_ns << "\n";
    std::cout << "  cancel_replace_count: " << result.replacements << "\n\n";
    std::cout << "pnl:\n";
    std::cout << "  maker_realized_pnl: " << result.maker_realized_pnl_tick << "\n";
    std::cout << "  maker_unrealized_pnl: " << result.maker_unrealized_pnl_tick << "\n";
    std::cout << "  spread_capture: " << result.spread_capture_tick << "\n";
    std::cout << "  adverse_selection_5s: "
              << result.adverse_selection_5s_tick << "\n\n";
    std::cout << "quote_risk:\n";
    std::cout << "  quote_intents_loaded: "
              << quote_risk.quote_intents_loaded << "\n";
    std::cout << "  evaluated: " << quote_risk.evaluated << "\n";
    std::cout << "  approved_quotes: " << quote_risk.approved_quotes << "\n";
    std::cout << "  rejected_quotes: " << quote_risk.rejected_quotes << "\n";
    std::cout << "  rejected_invalid: " << quote_risk.rejected_invalid << "\n";
    std::cout << "  rejected_expired: " << quote_risk.rejected_expired << "\n";
    std::cout << "  rejected_stale_book: "
              << quote_risk.rejected_stale_book << "\n";
    std::cout << "  rejected_crossed_book: "
              << quote_risk.rejected_crossed_book << "\n";
    std::cout << "  rejected_inventory_limit: "
              << quote_risk.rejected_inventory_limit << "\n";
    std::cout << "  rejected_exposure_limit: "
              << quote_risk.rejected_exposure_limit << "\n";
    std::cout << "  rejected_duplicate: " << quote_risk.rejected_duplicate << "\n";
    std::cout << "  rejected_too_frequent: "
              << quote_risk.rejected_too_frequent << "\n";
    std::cout << "  rejected_low_edge_to_fair: "
              << quote_risk.rejected_low_edge_to_fair << "\n";
    std::cout << "  rejected_kill_switch: "
              << quote_risk.rejected_kill_switch << "\n";
    std::cout << "  determinism_passed: "
              << (quote_risk_determinism ? "true" : "false") << "\n\n";
    std::cout << "paper_maker_execution:\n";
    std::cout << "  approved_quotes: " << paper_maker.approved_quotes << "\n";
    std::cout << "  active_quotes: " << paper_maker.active_quotes << "\n";
    std::cout << "  cancelled_quotes: " << paper_maker.cancelled_quotes << "\n";
    std::cout << "  expired_quotes: " << paper_maker.expired_quotes << "\n";
    std::cout << "  duplicate_quotes_ignored: "
              << paper_maker.duplicate_quotes_ignored << "\n";
    std::cout << "  maker_fills: " << paper_maker.maker_fills << "\n";
    std::cout << "  fill_mode: " << paper_maker.fill_mode << "\n";
    std::cout << "  determinism_passed: "
              << (paper_maker.determinism_passed ? "true" : "false")
              << "\n\n";
    std::cout << "hashes:\n";
    std::cout << "  mm_output_hash: " << result.output_hash << "\n";
    std::cout << "  determinism_passed: "
              << (determinism_passed && quote_risk_determinism &&
                          paper_maker.determinism_passed
                      ? "true"
                      : "false")
              << "\n";

    if (require_quote && result.quotes_emitted == 0) {
        return 1;
    }
    if (check_determinism &&
        (!determinism_passed || !quote_risk_determinism ||
         !paper_maker.determinism_passed)) {
        return 1;
    }
    return 0;
}
