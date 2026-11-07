#include "engine/execution/adapter/CrossVenueShadowAdapter.h"
#include "engine/execution/state/PairExecutionStateMachine.h"
#include "engine/paper/ledger/TwoAccountPaperLedger.h"
#include "engine/risk/ledger/MultiVenueReservationBook.h"
#include "engine/strategy/cross_venue/FixtureIO.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>

namespace cv = trading_engine::cross_venue;
namespace {
std::ofstream output(const std::filesystem::path& path) {
    std::ofstream stream(path, std::ios::binary); cv::require(stream.good(), "cannot create output");
    stream.exceptions(std::ios::badbit | std::ios::failbit); return stream;
}
std::string joined(std::vector<std::string> values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    std::string out; for (const auto& v : values) { if (!out.empty()) out += ';'; out += v; } return out;
}
void event(std::ostream& stream, std::string_view scenario, std::string_view state) {
    stream << boost::json::serialize(boost::json::object{{"scenario", scenario}, {"mode", "SIMULATED"}, {"state", state}}) << '\n';
}
}  // namespace
int main(int argc, char** argv) {
    try {
        std::filesystem::path input, fills_path, out, policy_path;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--help") {
                std::cout << "run_cross_venue_shadow --input SYNTHETIC-books.json --out DIR [--policy shadow-policy.json] [--simulated-fills SYNTHETIC-fills.json]\nRead-only V0; no live mode or order transport.\n"; return 0;
            }
            cv::require(i + 1 < argc, "option requires value");
            if (arg == "--input") input = argv[++i];
            else if (arg == "--out") out = argv[++i];
            else if (arg == "--simulated-fills") fills_path = argv[++i];
            else if (arg == "--policy") policy_path = argv[++i];
            else throw std::invalid_argument("unsupported option (no live order mode)");
        }
        cv::require(!input.empty() && !out.empty(), "--input and --out are required");
        auto books = cv::load_books(input);
        if (!policy_path.empty()) books.policy = cv::load_policy(policy_path);
        std::optional<cv::FillFixture> fills;
        if (!fills_path.empty()) {
            fills = cv::load_fills(fills_path);
            cv::require(fills->pair.artifact_hash == books.pair.artifact_hash, "books/receipts pair mismatch");
            cv::require(fills->starting_cash == books.starting_cash, "books/receipts account funding mismatch");
        }
        cv::require(!std::filesystem::exists(out) || (std::filesystem::is_directory(out) && std::filesystem::is_empty(out)), "--out must be a new or empty directory; preserve prior audit artifacts");
        std::filesystem::create_directories(out);
        auto opportunities = output(out / "opportunities.csv");
        auto rejects = output(out / "rejections.jsonl");
        auto lifecycle = output(out / "execution_events.simulated.jsonl");
        auto risk_snapshots = output(out / "risk_snapshots.simulated.jsonl");
        auto pnl = output(out / "pnl_audit.simulated.csv");
        opportunities << "scenario,mode,candidate_id,qty,cost_yes_usd,cost_no_usd,fee_budget_usd,slippage_reserve_usd,quoted_edge_usd,risk_eligible,artifact_hash,approval_revision,fee_version,yes_snapshot_version,no_snapshot_version,yes_raw_hash,no_raw_hash\n";
        pnl << "scenario,mode,qty_yes,qty_no,actual_spend_usd,fees_paid_usd,payout_if_yes_usd,payout_if_no_usd,filled_locked_floor_usd,illustrative_settled_pnl_usd,verified_live_realized_pnl_usd,suspense_usd,late_receipts,artifact_hash,approval_revision,cutoff_ns,fill_ids,fee_versions,raw_references,input_sha256\n";
        struct Demonstration { cv::CrossVenueOpportunity opportunity; cv::SynchronizedBookStore store; cv::FeeQuote fee; std::uint64_t now_ns; };
        std::optional<Demonstration> demonstration;
        std::size_t candidates = 0;
        for (const auto& s : books.scenarios) {
            cv::SynchronizedBookStore store;
            for (const auto& u : s.updates) store.apply(u);
            for (const auto& fee : books.fees) {
                std::optional<cv::CrossVenueOpportunity> candidate;
                try { candidate = cv::PairScanner::quote(books.pair, store.read_pair(books.pair), fee, books.policy, s.now_ns); }
                catch (const std::exception& e) {
                    rejects << boost::json::serialize(boost::json::object{{"scenario", s.name}, {"qty", fee.qty}, {"reason", e.what()}}) << '\n'; continue;
                }
                const auto& o = *candidate;
                bool eligible = false;
                cv::MultiVenueReservationBook risk(books.pair, books.starting_cash);
                try {
                    auto reservation = risk.reserve(o, books.pair, store, fee, books.policy, s.now_ns); eligible = true;
                    // A single explicit reference plan is used for the four receipt demos.
                    if (s.name == "matched_100" && fee.qty == 100) demonstration = Demonstration{o, store, fee, s.now_ns};
                } catch (const std::exception& e) {
                    rejects << boost::json::serialize(boost::json::object{{"scenario", s.name}, {"qty", fee.qty}, {"stage", "risk"}, {"reason", e.what()}}) << '\n';
                }
                opportunities << cv::csv(s.name) << ",SIMULATED," << o.candidate_id << ',' << fee.qty << ','
                    << cv::decimal(o.costs[0]) << ',' << cv::decimal(o.costs[1]) << ',' << cv::decimal(cv::add(o.fees[0], o.fees[1])) << ','
                    << cv::decimal(cv::add(o.slippage[0], o.slippage[1])) << ',' << cv::decimal(o.intent.total_edge_tick) << ','
                    << (eligible ? "true" : "false") << ',' << o.artifact_hash << ',' << o.approval_revision << ',' << cv::csv(o.fee_version) << ','
                    << o.evidence[0].snapshot_version << ',' << o.evidence[1].snapshot_version << ',' << o.evidence[0].raw_hash << ',' << o.evidence[1].raw_hash << '\n';
                ++candidates;
            }
        }
        if (fills) {
            cv::require(demonstration.has_value(), "receipt demo requires risk-approved matched_100 reference plan");
            const auto input_hash = cv::sha256(cv::read_bytes(fills_path));
            for (const auto& s : fills->scenarios) {
                cv::require(s.planned_quantity == demonstration->opportunity.intent.bundle_qty, "receipt scenario quantity/plan mismatch");
                cv::TwoAccountPaperLedger ledger(fills->pair, fills->starting_cash, s.cutoff_ns);
                cv::MultiVenueReservationBook risk(books.pair, books.starting_cash);
                const auto reserved = risk.reserve(demonstration->opportunity, books.pair, demonstration->store,
                    demonstration->fee, books.policy, demonstration->now_ns);
                cv::PairExecutionStateMachine execution(fills->pair, s.planned_quantity);
                execution.reserve(reserved); execution.plan();
                std::array<std::string, 2> orders;
                for (const auto& receipt : s.receipts) {
                    const auto i = receipt.canonical_yes ? 0U : 1U;
                    if (orders[i].empty()) { orders[i] = receipt.order_id; execution.submitted(i, receipt.order_id); execution.acknowledged(i); }
                    // Accounting and lifecycle use the same validated incremental receipt.
                    execution.confirmed_fill(receipt); ledger.apply(receipt);
                }
                const auto spent = execution.confirmed_spend();
                if (spent[0] > reserved.cash[0] || spent[1] > reserved.cash[1]) execution.freeze("ACTUAL_SPEND_EXCEEDS_RESERVED_BUDGET");
                if (execution.state() == cv::PairState::FullyHedged) execution.settlement_pending();
                else if (!s.receipts.empty() && execution.state() != cv::PairState::Freeze) {
                    for (std::size_t i = 0; i < 2; ++i) if (!orders[i].empty() && execution.filled(i) < s.planned_quantity) { execution.cancel_requested(i); execution.cancel_confirmed(i); }
                    if (execution.all_orders_terminal()) execution.residual_stop();
                    else execution.freeze("unconfirmed/unsubmitted hedge order");
                }
                for (const auto& state : execution.trace()) event(lifecycle, s.name, state);
                risk_snapshots << boost::json::serialize(boost::json::object{{"scenario", s.name}, {"mode", "SIMULATED"},
                    {"state", cv::state_name(execution.state())}, {"reservation_id", reserved.id},
                    {"reserved_cash_yes", cv::decimal(risk.reserved_cash(0))}, {"reserved_cash_no", cv::decimal(risk.reserved_cash(1))},
                    {"release_allowed", false}, {"reconciliation_status", "PENDING_SIMULATED_SETTLEMENT"}}) << '\n';
                const auto a = ledger.audit(s.illustrative_yes);
                std::vector<std::string> ids, versions, raw_refs;
                for (const auto& r : a.receipts) { ids.push_back(r.identity()); versions.push_back(r.fee_version); raw_refs.push_back(cv::venue_name(r.key.venue) + ":" + std::to_string(r.raw_log_offset) + ":" + r.raw_hash); }
                pnl << cv::csv(s.name) << ",SIMULATED," << a.quantities[0] << ',' << a.quantities[1] << ','
                    << cv::decimal(cv::add(a.spend[0], a.spend[1])) << ',' << cv::decimal(a.fees) << ',' << cv::decimal(a.payout_if_yes) << ',' << cv::decimal(a.payout_if_no) << ','
                    << cv::decimal(a.floor_pnl) << ',' << (a.illustrative_settled_pnl ? cv::decimal(*a.illustrative_settled_pnl) : "") << ",," << cv::decimal(a.suspense) << ','
                    << a.late_receipts << ',' << fills->pair.artifact_hash << ',' << fills->pair.approval_revision << ',' << s.cutoff_ns << ','
                    << cv::csv(joined(ids)) << ',' << cv::csv(joined(versions)) << ',' << cv::csv(joined(raw_refs)) << ',' << input_hash << '\n';
            }
        }
        opportunities.flush(); rejects.flush(); lifecycle.flush(); risk_snapshots.flush(); pnl.flush();
        auto manifest = output(out / "manifest.json");
        manifest << boost::json::serialize(boost::json::object{
            {"schema", "cross-venue-shadow-v0"}, {"mode", "SYNTHETIC_REPLAY"}, {"live_enabled", false},
            {"live_orders_sent", 0}, {"live_inventory", 0}, {"verified_live_realized_pnl_usd", nullptr},
            {"reconciliation_status", "NOT_LIVE_VERIFIED"}, {"currency", books.pair.leg_yes.currency},
            {"artifact_hash", books.pair.artifact_hash}, {"books_input_sha256", cv::sha256(cv::read_bytes(input))},
            {"policy_input_sha256", policy_path.empty() ? boost::json::value(nullptr) : boost::json::value(cv::sha256(cv::read_bytes(policy_path)))},
            {"fills_input_sha256", fills ? boost::json::value(cv::sha256(cv::read_bytes(fills_path))) : boost::json::value(nullptr)},
            {"candidate_count", candidates}, {"simulation_scenarios", fills ? fills->scenarios.size() : 0}}) << '\n';
        manifest.flush();
        std::cout << "SYNTHETIC_REPLAY: " << candidates << " indicative candidates; live orders=0; verified live PnL=null\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "cross_venue_shadow: " << e.what() << '\n'; return 1; }
}
