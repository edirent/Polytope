#include "engine/execution/adapter/CrossVenueShadowAdapter.h"
#include "engine/execution/state/PairExecutionStateMachine.h"
#include "engine/paper/ledger/TwoAccountPaperLedger.h"
#include "engine/risk/ledger/MultiVenueReservationBook.h"
#include "engine/strategy/cross_venue/FixtureIO.h"
#include "engine/decode/parser/kalshi/KalshiBookNormalizer.h"
#include "engine/decode/parser/polymarket/CrossVenueClobNormalizer.h"
#include <functional>
#include <iostream>
#include <limits>

namespace cv = trading_engine::cross_venue;
namespace {
int checks = 0;
void expect(bool value, const char* name) { if (!value) throw std::runtime_error(name); ++checks; }
template <typename F> void rejects(F f, const char* name) {
    bool rejected = false; try { f(); } catch (const std::exception&) { rejected = true; }
    expect(rejected, name);
}
}
int main() {
    try {
        const auto root = std::filesystem::path(POLYTOPE_SOURCE_DIR) / "research/cross_venue/fixtures";
        auto books = cv::load_books(root / "synthetic_books.json");
        const auto fills = cv::load_fills(root / "synthetic_fills.json");
        const auto& p = books.pair; const auto& policy = books.policy; const auto fee = books.fees.at(2);
        cv::SynchronizedBookStore store;
        for (const auto& u : books.scenarios[0].updates) expect(store.apply(u), "initial snapshots");
        auto quote = [&] { return cv::PairScanner::quote(p, store.read_pair(p), fee, policy, 1'000'000'000); };
        const auto o = quote();
        expect(cv::money("0.000001") == 1 && cv::decimal(cv::money("-18.60")) == "-18.600000", "exact micro precision");
        expect(cv::money(cv::decimal(std::numeric_limits<cv::Money>::min())) == std::numeric_limits<cv::Money>::min(), "int64 minimum roundtrip");
        for (const auto& bad : {"0.0000001", "1e-2", "nan", ".1", "1.", "+1", "9223372036854.775808"}) rejects([&] { cv::money(bad); }, "bad decimal");
        rejects([] { cv::add(std::numeric_limits<cv::Money>::max(), 1); }, "money addition overflow");
        expect(o.intent.total_edge_tick == cv::money("1.51") && o.intent.leg_count == 2, "matched edge uses full cost and fees");
        expect(o.candidate_id == quote().candidate_id, "stable candidate ID");
        for (const auto& s : books.scenarios) {
            cv::SynchronizedBookStore replay; for (const auto& u : s.updates) replay.apply(u);
            if (s.name == "stale" || s.name == "receive_skew" || s.name == "sequence_gap" || s.name == "disconnect") {
                rejects([&] { cv::PairScanner::quote(p, replay.read_pair(p), fee, policy, s.now_ns); }, "bad quality cannot emit intent");
            } else {
                auto q = cv::PairScanner::quote(p, replay.read_pair(p), fee, policy, s.now_ns);
                if (s.name == "depth_walk") expect(q.intent.total_edge_tick == cv::money("0.31"), "walk depth rather than extrapolate touch");
                if (s.name == "resnapshot") expect(q.candidate_id != o.candidate_id, "recovery changes evidence ID");
            }
        }
        for (int kind = 0; kind < 7; ++kind) {
            auto update = books.scenarios[0].updates[0];
            if (kind == 0) update.complete = false;
            if (kind == 1) update.asks.clear();
            if (kind == 2) update.bids[0].first = cv::money("0.60");
            if (kind == 3) update.asks[0].second = -1;
            if (kind == 4) update.kind = cv::BookUpdate::Kind::Delta;
            if (kind == 5) update.evidence.recv_monotonic_ns = 0;
            if (kind == 6) update.asks.push_back(update.asks[0]);
            cv::SynchronizedBookStore invalid; expect(!invalid.apply(update), "invalid/incomplete book rejected");
        }
        auto disabled = p; disabled.enabled = false; disabled.artifact_hash = cv::pair_hash(disabled);
        rejects([&] { cv::validate_pair(disabled); }, "disabled pair");
        auto unreviewed = p; unreviewed.review_id.clear(); unreviewed.artifact_hash = cv::pair_hash(unreviewed);
        rejects([&] { cv::validate_pair(unreviewed); }, "missing rule review");
        auto voided = p; voided.payoffs.push_back({"independent_void", 0, 0}); voided.artifact_hash = cv::pair_hash(voided);
        rejects([&] { cv::validate_pair(voided); }, "independent void is not a hedge");
        auto fx = p; fx.leg_no.currency = "USDC"; fx.artifact_hash = cv::pair_hash(fx);
        rejects([&] { cv::validate_pair(fx); }, "currencies never silently netted");
        auto mutated = p; mutated.rules_hash_yes[0] = mutated.rules_hash_yes[0] == 'a' ? 'b' : 'a';
        rejects([&] { cv::validate_pair(mutated); }, "frozen artifact detects rule mutation");
        auto negative = fee; negative.fees[0] = -1;
        rejects([&] { cv::PairScanner::quote(p, store.read_pair(p), negative, policy, 1'000'000'000); }, "negative fee");
        auto expired = fee; expired.valid_until_ns = 1'000'000'000;
        rejects([&] { cv::PairScanner::quote(p, store.read_pair(p), expired, policy, 1'000'000'000); }, "expired fee version");
        rejects([&] { cv::PairScanner::quote(p, store.read_pair(p), books.fees.at(3), policy, 1'000'000'000); }, "insufficient real L2 quantity");
        cv::MultiVenueReservationBook risk(p, books.starting_cash);
        const auto reservation = risk.reserve(o, p, store, fee, policy, 1'000'000'000);
        expect(risk.reserved_cash(0) == cv::money("48.75") && risk.reserved_cash(1) == cv::money("49.74"), "separate local cash reservations");
        rejects([&] { risk.reserve(o, p, store, fee, policy, 1'000'000'000); }, "no duplicate reservation");
        cv::MultiVenueReservationBook poor(p, {cv::money("1000"), cv::money("1")});
        rejects([&] { poor.reserve(o, p, store, fee, policy, 1'000'000'000); }, "cannot borrow buying power across venues");
        expect(poor.reserved_cash(0) == 0 && poor.reserved_cash(1) == 0, "failed second leg has no partial reservation");
        auto lowprofit = cv::PairScanner::quote(p, store.read_pair(p), books.fees[0], policy, 1'000'000'000);
        cv::MultiVenueReservationBook small(p, books.starting_cash);
        rejects([&] { small.reserve(lowprofit, p, store, books.fees[0], policy, 1'000'000'000); }, "minimum absolute profit enforced");
        auto cap = policy; cap.max_total_payout = cv::money("199");
        auto capquote = cv::PairScanner::quote(p, store.read_pair(p), fee, cap, 1'000'000'000);
        rejects([&] { small.reserve(capquote, p, store, fee, cap, 1'000'000'000); }, "both legs count toward payout cap");
        cap = policy; cap.max_single_leg_loss = cv::money("48");
        capquote = cv::PairScanner::quote(p, store.read_pair(p), fee, cap, 1'000'000'000);
        rejects([&] { small.reserve(capquote, p, store, fee, cap, 1'000'000'000); }, "single-leg residual loss cap");
        auto changed = books.scenarios[0].updates[0]; changed.kind = cv::BookUpdate::Kind::Delta;
        changed.evidence.sequence = 2; changed.evidence.recv_monotonic_ns = 980'000'000; changed.asks[0].first = cv::money("0.46");
        store.apply(changed);
        cv::MultiVenueReservationBook changed_risk(p, books.starting_cash);
        rejects([&] { changed_risk.reserve(o, p, store, fee, policy, 1'000'000'000); }, "snapshot revision locked at approval");
        const std::array<cv::Money, 4> expected{cv::money("1.51"), cv::money("-18.60"), cv::money("-2.49"), 0};
        for (std::size_t i = 0; i < fills.scenarios.size(); ++i) {
            const auto& s = fills.scenarios[i]; cv::TwoAccountPaperLedger ledger(p, fills.starting_cash, s.cutoff_ns);
            for (const auto& f : s.receipts) expect(ledger.apply(f), "receipt applies");
            const auto a = ledger.audit(s.illustrative_yes);
            expect(a.floor_pnl == expected[i], "golden accounting fixture");
            expect(!a.verified_live_realized_pnl, "no fabricated live PnL");
            if (!s.receipts.empty()) {
                expect(!ledger.apply(s.receipts[0]) && ledger.audit().floor_pnl == a.floor_pnl, "fill idempotency");
                auto conflict = s.receipts[0]; ++conflict.qty;
                rejects([&] { ledger.apply(conflict); }, "conflicting fill identity quarantined");
            }
        }
        auto receipt = fills.scenarios[0].receipts[0];
        cv::TwoAccountPaperLedger ledger(p, fills.starting_cash, 3'000'000'000);
        auto bad = receipt; bad.fee_paid = -1; rejects([&] { ledger.apply(bad); }, "negative actual fee rejected");
        bad = receipt; bad.received_time_ns = bad.exchange_time_ns - 1; rejects([&] { ledger.apply(bad); }, "reversed receipt time rejected");
        bad = receipt; bad.simulated = false; rejects([&] { ledger.apply(bad); }, "live/simulated receipt separation");
        bad = receipt; bad.received_time_ns = 2'500'000'000; ledger.apply(bad);
        auto accounting = ledger.audit(); expect(accounting.late_receipts == 1, "late receipt flagged");
        auto closing = accounting.closing_cash; closing[0] += cv::unit; closing[1] -= cv::unit;
        expect(ledger.audit({}, closing).suspense == 2 * cv::unit, "opposite account mismatches do not net");
        cv::PairExecutionStateMachine machine(p, 100);
        rejects([&] { machine.plan(); }, "cannot plan without risk reservation");
        machine.reserve(reservation); machine.plan();
        machine.submitted(0, receipt.order_id); machine.acknowledged(0);
        expect(machine.filled(0) == 0, "ACK is not FILL");
        machine.cancel_requested(0);
        expect(!machine.all_orders_terminal() && risk.reserved_cash(0) > 0, "cancel request does not release risk");
        machine.freeze("ACCOUNT_DISCONNECT");
        expect(machine.confirmed_fill(receipt) && machine.filled(0) == 100 && machine.state() == cv::PairState::Freeze, "late fill after freeze still creates inventory");
        rejects([&] { machine.submitted(1, "forbidden"); }, "freeze blocks submissions");
        rejects([&] { risk.release_reconciled(machine, {0, 0}); }, "frozen risk cannot be released");
        cv::PairExecutionStateMachine complete(p, 100); complete.reserve(reservation); complete.plan();
        for (const auto& f : fills.scenarios[0].receipts) { auto i = f.canonical_yes ? 0 : 1; complete.submitted(i, f.order_id); complete.acknowledged(i); complete.confirmed_fill(f); }
        expect(complete.state() == cv::PairState::FullyHedged, "both fills produce full hedge");
        complete.settlement_pending(); rejects([&] { complete.reconciled(false, true); }, "balance match required");
        complete.reconciled(true, true);
        rejects([&] { risk.release_reconciled(complete, {0, 0}); }, "risk release cannot understate confirmed spending");
        risk.release_reconciled(complete, {cv::money("48.75"), cv::money("49.74")});
        expect(risk.reserved_cash(0) == 0 && risk.available_cash(1) == cv::money("950.26"), "release consumes actual cash");
        expect(!cv::CrossVenueShadowAdapter::live_enabled, "live forcibly disabled");
        rejects([] { cv::CrossVenueShadowAdapter::submit_order(); }, "shadow cannot transmit orders");
        // Synthetic messages in the documented venue wire formats. These are
        // parser/pipeline tests, not evidence of a live API connection.
        cv::KalshiBookNormalizer kalshi(p.leg_yes, true);
        cv::CrossVenueClobNormalizer clob(p.leg_no);
        cv::BookProvenance stamp; stamp.recv_monotonic_ns = 900'000'000; stamp.source_ts_ns = 800'000'000;
        const auto kr = R"({"orderbook_fp":{"yes_dollars":[["0.4500","100.00"]],"no_dollars":[["0.5300","100.75"]]}})";
        const auto ks = R"({"type":"orderbook_snapshot","sid":2,"seq":2,"msg":{"market_ticker":"SYNTHETIC-K","yes_dollars_fp":[["0.4500","100.00"]],"no_dollars_fp":[["0.5300","100.75"]]}})";
        const auto kd = R"({"type":"orderbook_delta","sid":2,"seq":3,"msg":{"market_ticker":"SYNTHETIC-K","side":"no","price_dollars":"0.5300","delta_fp":"-0.75"}})";
        const auto kg = R"({"type":"orderbook_delta","sid":2,"seq":5,"msg":{"market_ticker":"SYNTHETIC-K","side":"no","price_dollars":"0.5300","delta_fp":"-1.00"}})";
        const auto ps = R"({"event_type":"book","market":"SYNTHETIC-P","asset_id":"NO","bids":[{"price":"0.46","size":"100"}],"asks":[{"price":"0.48","size":"100.99"}]})";
        const auto pd = R"({"event_type":"price_change","market":"SYNTHETIC-P","price_changes":[{"asset_id":"NO","price":"0.48","size":"100.25","side":"SELL"}]})";
        cv::SynchronizedBookStore venue_store;
        expect(venue_store.apply(kalshi.rest_snapshot(kr, stamp)), "Kalshi REST snapshot becomes canonical YES asks");
        expect(kalshi.websocket(kd, stamp).kind == cv::BookUpdate::Kind::Disconnect, "REST cannot bootstrap WS sequence");
        expect(venue_store.apply(kalshi.websocket(ks, stamp)), "Kalshi WS snapshot resets session");
        expect(venue_store.apply(kalshi.websocket(kd, stamp)), "Kalshi additive fractional delta");
        expect(venue_store.read(p.leg_yes).depth.asks[0].size == 100.0, "fractional size never rounded up");
        expect(venue_store.apply(clob.normalize(ps, stamp, 1)), "international CLOB snapshot");
        expect(venue_store.apply(clob.normalize(pd, stamp, 2)), "CLOB delta size is absolute, not additive");
        expect(venue_store.read(p.leg_no).depth.asks[0].size == 100.0, "CLOB absolute fractional size preserved conservatively");
        const auto venue_quote = cv::PairScanner::quote(p, venue_store.read_pair(p), fee, policy, 1'000'000'000);
        expect(venue_quote.intent.total_edge_tick == cv::money("1.51"), "venue wire → decode → state → signal");
        expect(!venue_store.apply(kalshi.websocket(kg, stamp)), "Kalshi subscription seq gap invalidates state");
        rejects([&] { cv::PairScanner::quote(p, venue_store.read_pair(p), fee, policy, 1'000'000'000); }, "no intent after wire seq gap");
        expect(venue_store.apply(kalshi.websocket(ks, stamp)), "Kalshi authoritative resnapshot recovers");
        expect(!venue_store.apply(clob.disconnect(stamp)), "CLOB disconnect invalidates state");
        expect(!venue_store.apply(clob.normalize(pd, stamp, 3)), "CLOB deltas cannot recover disconnected state");
        expect(venue_store.apply(clob.normalize(ps, stamp, 4)), "CLOB resnapshot restores state");
        expect(!venue_store.apply(clob.normalize(pd, stamp, 6)), "local capture sequence gap invalidates state");
        auto us_key = p.leg_no; us_key.venue = cv::VenueId::PolymarketUS;
        rejects([&] { cv::CrossVenueClobNormalizer wrong(us_key); }, "US and international adapters separated");
        std::cout << "CrossVenueV0: " << checks << " contract/state/risk/execution/accounting checks passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
