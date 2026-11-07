#pragma once

#include "engine/strategy/cross_venue/FixtureIO.h"
#include "engine/paper/ledger/TwoAccountPaperLedger.h"
#include <atomic>
#include <functional>
#include <mutex>
#include <memory>
#include <set>

namespace trading_engine::cross_venue::research {
namespace json = boost::json;
using Object = json::object;
using Array = json::array;

std::uint64_t wall_ns();
std::uint64_t mono_ns();
std::string utc(std::uint64_t ns);
std::string text_value(const json::value& v);
std::string get_text(const Object& o, std::string_view name, std::string fallback = {});
bool get_bool(const Object& o, std::string_view name, bool fallback = false);
std::uint64_t get_uint(const Object& o, std::string_view name, std::uint64_t fallback = 0);
Object read_object(const std::filesystem::path& path);
void write_json(const std::filesystem::path& path, const json::value& value);
void write_table(const std::filesystem::path& path, const std::vector<std::string>& columns, const Array& rows);
Object key_json(const VenueInstrumentKey& key);
VenueInstrumentKey parse_key(const Object& o);

struct Config {
    std::filesystem::path path;
    Object original;
    std::filesystem::path runs_root = "runs/cross_venue";
    std::filesystem::path approvals = "config/cross_venue_approved_pairs.json";
    std::string kalshi_rest = "https://external-api.kalshi.com/trade-api/v2";
    std::string gamma_rest = "https://gamma-api.polymarket.com";
    std::string clob_rest = "https://clob.polymarket.com";
    std::string kalshi_ws = "wss://external-api-ws.kalshi.com/trade-api/ws/v2";
    std::string clob_ws = "wss://ws-subscriptions-clob.polymarket.com/ws/market";
    std::size_t discovery_limit = 1200, page_size = 200, capture_per_venue = 4, max_pairs = 200;
    std::uint64_t poll_ms = 5000, snapshot_ms = 15000, heartbeat_ms = 30000, authoritative_ms = 45000;
    std::uint64_t max_raw_bytes = 256 * 1024 * 1024, rotate_bytes = 8 * 1024 * 1024;
    bool enable_ws = true;
    Money capital = 200000 * unit;
    std::array<Money, 2> allocations{100000 * unit, 100000 * unit};
    PairPolicy policy;
};
Config load_config(const std::filesystem::path& path);

struct HttpResponse {
    int status = 0;
    std::string body, error, content_type;
    std::uint64_t started_ns = 0, received_ns = 0, received_mono_ns = 0;
};
class PublicHttpClient {
public:
    HttpResponse get(const std::string& url, unsigned timeout_seconds = 10) const;
};

// Synchronous append-before-process recorder: no unbounded asynchronous queue.
// On restart, create a new immutable segment and connection epoch.
class RawRecorder {
public:
    RawRecorder(std::filesystem::path directory, std::uint64_t segment_limit, std::uint64_t total_limit);
    Object append(const std::string& venue, Object record);
    std::uint64_t bytes() const;
    Array inventory() const;
private:
    struct Segment { unsigned index = 0; std::uint64_t bytes = 0, records = 0; std::filesystem::path path; };
    std::filesystem::path directory_;
    std::uint64_t segment_limit_, total_limit_, total_ = 0, sequence_ = 0;
    mutable std::mutex mutex_;
    std::map<std::string, Segment> active_;
};

struct Market {
    std::string venue, id, event_id, title, rules, rules_hash, category, close_time;
    VenueInstrumentKey yes, no;
    std::string protocol, status, rejection;
    Money tick = 0, min_qty_micro = 0, qty_step_micro = 0;
    double liquidity = 0, volume = 0;  // screening only, never money accounting
    bool tradable = false, protocol_supported = false;
    Object fee, raw;
};
Market normalize_market(std::string_view venue, const Object& raw);
Object market_json(const Market& m);
Array match_markets(const std::vector<Market>& markets, std::size_t limit);

struct Discovery {
    std::vector<Market> markets;
    Array candidates, requests;
    std::map<std::string, bool> complete;
};
Discovery discover(const Config& config, const std::filesystem::path& run_directory);
std::vector<ApprovedPair> load_approved_pairs(const Config& config, const std::vector<Market>& markets, Array& rejections);

struct FeeSchedule {
    std::string version, source, market_id, role = "taker", type;
    std::uint64_t observed_wall_ns = 0, expires_wall_ns = 0;
    Money rate = 0;
    bool valid = false;
};
FeeSchedule fee_schedule(const Market& market);
void enrich_selected_fees(const Config& config,std::vector<Market>& markets,const std::filesystem::path& directory);
Money estimate_fee_micro(const FeeSchedule&, const VersionedBook&, std::int64_t quantity_micro, std::uint64_t wall_time);
Money estimate_fee(const FeeSchedule& schedule, const VersionedBook& book, std::int64_t quantity, std::uint64_t wall_time);

// Controls reconnect at subscription scope before messages reach market parsers.
class KalshiStreamSequence {
public:
    bool accept(const Object& packet);
    void reset() { sequences_.clear(); }
private:
    std::map<std::uint64_t, std::uint64_t> sequences_;
};

struct AnalyzedOpportunity {
    CrossVenueOpportunity opportunity;
    ApprovedPair pair;
    FeeQuote fee;
    std::string liquidity_id;
    std::uint64_t observed_mono_ns = 0, observed_wall_ns = 0;
    bool continuous_l2 = false;
    Object row;
};
class ResearchState {
public:
    ResearchState(Config config,std::vector<Market> markets,std::vector<ApprovedPair> approved);
    ~ResearchState();
    bool process(const Object& record);
    std::vector<AnalyzedOpportunity> scan(std::uint64_t monotonic_time,std::uint64_t wall_time);
    Array health() const;
    Object counters() const;
    Array take_diagnostics();
    SynchronizedBookStore& books();
    const std::vector<ApprovedPair>& pairs() const;
    bool websocket_book(const VenueInstrumentKey& key) const;
    bool continuous_pair(const ApprovedPair& pair) const;
    const Market& market(const VenueInstrumentKey& key) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct CaptureResult {
    std::filesystem::path directory;
    Object manifest;
};
CaptureResult capture(const Config& config, unsigned duration_sec, const std::filesystem::path& directory = {});
Object replay(const std::filesystem::path& directory, Money capital,
              const std::vector<unsigned>& latencies_ms, const std::filesystem::path& output = {});
Array read_capture_records(const std::filesystem::path& directory,const Object& manifest);
Object economic_discovery(const std::filesystem::path& directory,const std::filesystem::path& output);
Object audit_feeds(const std::filesystem::path& directory,const std::filesystem::path& output);
std::string kalshi_signature(std::string_view pem, std::string_view message);
Object inspect_kalshi_signing_key(const std::filesystem::path& path);

}  // namespace trading_engine::cross_venue::research
