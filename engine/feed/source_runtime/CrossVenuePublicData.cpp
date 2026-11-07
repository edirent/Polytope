#include "engine/strategy/cross_venue/LiveResearch.h"
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thread>

namespace trading_engine::cross_venue::research {
std::uint64_t wall_ns() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
std::uint64_t mono_ns() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
std::string utc(std::uint64_t ns) {
    std::time_t seconds = ns / 1'000'000'000; std::tm t{}; gmtime_r(&seconds, &t);
    std::ostringstream out; out << std::put_time(&t, "%Y-%m-%dT%H:%M:%S") << '.' << std::setfill('0') << std::setw(9) << ns % 1'000'000'000 << 'Z'; return out.str();
}
std::string text_value(const json::value& v) {
    if (v.is_string()) return std::string(v.as_string());
    if (v.is_null()) return {};
    return json::serialize(v);
}
std::string get_text(const Object& o, std::string_view name, std::string fallback) {
    const auto* v = o.if_contains(name); return !v || v->is_null() ? fallback : text_value(*v);
}
bool get_bool(const Object& o, std::string_view name, bool fallback) {
    const auto* v = o.if_contains(name); return v && v->is_bool() ? v->as_bool() : fallback;
}
std::uint64_t get_uint(const Object& o, std::string_view name, std::uint64_t fallback) {
    const auto* v = o.if_contains(name); if (!v || v->is_null()) return fallback;
    if (v->is_uint64()) return v->as_uint64();
    if (v->is_int64()) { require(v->as_int64() >= 0, "negative integer"); return v->as_int64(); }
    throw std::invalid_argument("expected integer: " + std::string(name));
}
Object read_object(const std::filesystem::path& p) { return json::parse(read_bytes(p)).as_object(); }
void write_json(const std::filesystem::path& path, const json::value& value) {
    auto temporary = path; temporary += ".tmp";
    std::ofstream stream(temporary, std::ios::binary); stream.exceptions(std::ios::badbit | std::ios::failbit);
    stream << json::serialize(value) << '\n'; stream.close(); std::filesystem::rename(temporary, path);
}
void write_table(const std::filesystem::path& path, const std::vector<std::string>& names, const Array& rows) {
    std::ofstream f(path); f.exceptions(std::ios::badbit | std::ios::failbit);
    for (std::size_t i = 0; i < names.size(); ++i) f << (i ? "," : "") << csv(names[i]); f << '\n';
    for (const auto& row : rows) {
        const auto& o = row.as_object();
        for (std::size_t i = 0; i < names.size(); ++i) {
            const auto* v = o.if_contains(names[i]); f << (i ? "," : "") << csv(v ? text_value(*v) : "");
        }
        f << '\n';
    }
}
Object key_json(const VenueInstrumentKey& k) { return {{"venue", venue_name(k.venue)}, {"market_id", k.market_id}, {"instrument_id", k.instrument_id}, {"currency", k.currency}}; }
VenueInstrumentKey parse_key(const Object& o) { return {venue_id(get_text(o,"venue")), get_text(o,"market_id"), get_text(o,"instrument_id"), get_text(o,"currency")}; }
Config load_config(const std::filesystem::path& p) {
    Config c; c.path = std::filesystem::absolute(p); c.original = read_object(p); const auto& o = c.original;
    require(get_text(o,"schema") == "cross-venue-research-v0.1" && get_text(o,"mode") == "read_only_shadow", "invalid V0.1 schema/mode");
    require(!o.contains("live_enabled") && !o.contains("credentials"), "V0.1 has no live flag or persisted credentials");
    c.runs_root = get_text(o,"runs_root",c.runs_root.string()); c.approvals = get_text(o,"approved_pairs",c.approvals.string());
    c.discovery_limit = get_uint(o,"discovery_limit",c.discovery_limit); c.page_size = get_uint(o,"page_size",c.page_size);
    c.capture_per_venue = get_uint(o,"capture_markets_per_venue",c.capture_per_venue); c.max_pairs = get_uint(o,"candidate_pair_limit",c.max_pairs);
    c.poll_ms = get_uint(o,"rest_poll_ms",c.poll_ms); c.snapshot_ms = get_uint(o,"reconcile_interval_ms",c.snapshot_ms);
    c.heartbeat_ms = get_uint(o,"heartbeat_timeout_ms",c.heartbeat_ms); c.authoritative_ms = get_uint(o,"authoritative_max_age_ms",c.authoritative_ms);
    c.rotate_bytes = get_uint(o,"rotate_bytes",c.rotate_bytes); c.max_raw_bytes = get_uint(o,"max_raw_bytes",c.max_raw_bytes);
    c.enable_ws = get_bool(o,"websocket",true);
    c.capital = money(get_text(o,"capital","200000"));
    const auto& a = o.at("venue_allocations").as_array(); require(a.size() == 2, "two venue wallets required");
    c.allocations = {money(text_value(a[0])), money(text_value(a[1]))};
    require(c.allocations[0] >= 0 && c.allocations[1] >= 0 && add(c.allocations[0],c.allocations[1]) <= c.capital, "portfolio capital exceeded");
    c.policy.max_skew_ns = get_uint(o,"max_receive_skew_ms",100) * 1'000'000;
    c.policy.asof_enabled = get_bool(o,"asof_enabled");
    c.policy.asof.liveness_ns=c.heartbeat_ms*1000000;
    c.policy.asof.authority_ns=c.authoritative_ms*1000000;
    c.policy.asof.change_ns=get_uint(o,"book_change_max_age_ms",15000)*1000000;
    c.policy.asof.research_only=get_bool(o,"asof_research_only");
    c.policy.max_age_ns = get_uint(o,"decision_max_age_ms",250) * 1'000'000;
    require(get_uint(o,"decision_max_age_ms",250)<=60000 && get_uint(o,"max_receive_skew_ms",100)<=60000 &&
        c.poll_ms<=3600000 && c.snapshot_ms<=3600000 && c.heartbeat_ms<=3600000 && c.authoritative_ms<=3600000,"time policy exceeds supported bound");
    require(c.discovery_limit > 0 && c.discovery_limit <= 20000 && c.page_size > 0 && c.page_size <= 1000 &&
            c.capture_per_venue > 0 && c.capture_per_venue <= 50 && c.poll_ms >= 1000 && c.snapshot_ms >= 1000 &&
            c.heartbeat_ms >= 1000 && c.rotate_bytes >= 4096 && c.max_raw_bytes >= c.rotate_bytes, "invalid bounded capture policy");
    return c;
}

HttpResponse PublicHttpClient::get(const std::string& url, unsigned timeout) const {
    namespace net = boost::asio; namespace beast = boost::beast; namespace http = beast::http; namespace ssl = net::ssl;
    using tcp = net::ip::tcp;
    HttpResponse result; result.started_ns = wall_ns();
    try {
        require(url.starts_with("https://"), "public data client requires HTTPS");
        const auto split = url.find('/',8); const auto host = url.substr(8,split == std::string::npos ? split : split-8);
        const auto path = split == std::string::npos ? "/" : url.substr(split);
        require(host == "external-api.kalshi.com" || host == "gamma-api.polymarket.com" || host == "clob.polymarket.com", "unapproved market-data host");
        require(path.find("/orders") == std::string::npos && path.find("/portfolio") == std::string::npos, "private/trading endpoints prohibited");
        net::io_context ioc; ssl::context ctx(ssl::context::tls_client); ctx.set_default_verify_paths(); ctx.set_verify_mode(ssl::verify_peer);
        tcp::resolver resolver(ioc); beast::ssl_stream<beast::tcp_stream> stream(ioc,ctx);
        require(SSL_set_tlsext_host_name(stream.native_handle(),host.c_str()) == 1,"TLS SNI failed");
        stream.set_verify_callback(ssl::host_name_verification(host));
        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(timeout));
        beast::get_lowest_layer(stream).connect(resolver.resolve(host,"443")); stream.handshake(ssl::stream_base::client);
        http::request<http::empty_body> req(http::verb::get,path,11); req.set(http::field::host,host);
        req.set(http::field::user_agent,"Polytope-CrossVenue-Research/0.1"); req.set(http::field::accept,"application/json");
        http::write(stream,req); beast::flat_buffer buffer; http::response_parser<http::string_body> parser; parser.body_limit(32*1024*1024);
        http::read(stream,buffer,parser); auto response = parser.release();
        result.status = response.result_int(); result.body = std::move(response.body()); result.content_type = std::string(response[http::field::content_type]);
        // Closing TCP after the complete body avoids a blocking TLS close_notify.
        boost::system::error_code ec; beast::get_lowest_layer(stream).socket().close(ec);
    } catch (const std::exception& e) { result.error = e.what(); }
    result.received_ns = wall_ns(); result.received_mono_ns = mono_ns(); return result;
}

RawRecorder::RawRecorder(std::filesystem::path d, std::uint64_t segment, std::uint64_t total)
    : directory_(std::move(d)), segment_limit_(segment), total_limit_(total) {
    std::filesystem::create_directories(directory_);
    // Never append into an unknown previous session. The caller resumes into a
    // fresh run directory; old segments remain immutable audit input.
    for (const auto& e : std::filesystem::directory_iterator(directory_))
        require(e.path().filename().string().find(".raw.") == std::string::npos,"raw directory already contains a session");
}
Object RawRecorder::append(const std::string& venue, Object o) {
    std::lock_guard lock(mutex_);
    require(venue == "kalshi" || venue == "polymarket_international", "invalid recorder venue");
    auto& s = active_[venue];
    if (s.path.empty() || s.bytes >= segment_limit_) {
        if (!s.path.empty()) ++s.index;
        const std::string stem = venue == "kalshi" ? "kalshi" : "polymarket";
        s.path = directory_ / (stem + ".raw." + std::to_string(s.index) + ".jsonl"); s.bytes = 0;
    }
    o["record_sequence"] = ++sequence_; o["raw_log_offset"] = s.bytes; o["raw_file"] = s.path.filename().string();
    o["raw_hash"] = sha256(get_text(o,"raw_payload")); o["parser_version"] = "cross-venue-v0.1";
    const auto bytes = json::serialize(o) + "\n"; require(bytes.size() <= 8*1024*1024 && total_ + bytes.size() <= total_limit_,"RAW_STORAGE_LIMIT");
    std::ofstream f(s.path,std::ios::app|std::ios::binary); f.exceptions(std::ios::badbit|std::ios::failbit); f.write(bytes.data(),bytes.size()); f.flush();
    s.bytes += bytes.size(); total_ += bytes.size(); ++s.records; return o;
}
std::uint64_t RawRecorder::bytes() const { std::lock_guard lock(mutex_); return total_; }
Array RawRecorder::inventory() const {
    std::lock_guard lock(mutex_); Array files;
    for (const auto& e : std::filesystem::directory_iterator(directory_)) {
        if (e.path().filename().string().find(".raw.") == std::string::npos) continue;
        files.push_back(Object{{"file",e.path().filename().string()},{"bytes",std::filesystem::file_size(e.path())},{"sha256",sha256(read_bytes(e.path()))}});
    }
    return files;
}

std::string kalshi_signature(std::string_view pem, std::string_view message) {
    BIO* bio = BIO_new_mem_buf(pem.data(),static_cast<int>(pem.size())); require(bio != nullptr,"key buffer failed");
    EVP_PKEY* key = PEM_read_bio_PrivateKey(bio,nullptr,[](char*,int,int,void*){return 0;},nullptr); BIO_free(bio); require(key != nullptr,"invalid Kalshi signing key");
    EVP_MD_CTX* ctx = EVP_MD_CTX_new(); EVP_PKEY_CTX* key_ctx = nullptr;
    const bool ed = EVP_PKEY_base_id(key) == EVP_PKEY_ED25519; std::size_t length = 0;
    bool ok = EVP_DigestSignInit(ctx,&key_ctx,ed ? nullptr : EVP_sha256(),nullptr,key) == 1;
    if (!ed) ok = ok && EVP_PKEY_base_id(key) == EVP_PKEY_RSA && EVP_PKEY_CTX_set_rsa_padding(key_ctx,RSA_PKCS1_PSS_PADDING) > 0 && EVP_PKEY_CTX_set_rsa_pss_saltlen(key_ctx,RSA_PSS_SALTLEN_DIGEST) > 0;
    ok = ok && EVP_DigestSign(ctx,nullptr,&length,reinterpret_cast<const unsigned char*>(message.data()),message.size()) == 1;
    std::vector<unsigned char> signature(length);
    ok = ok && EVP_DigestSign(ctx,signature.data(),&length,reinterpret_cast<const unsigned char*>(message.data()),message.size()) == 1;
    EVP_MD_CTX_free(ctx); EVP_PKEY_free(key); require(ok,"Kalshi data handshake signing failed");
    std::string encoded(4*((length+2)/3),'\0'); EVP_EncodeBlock(reinterpret_cast<unsigned char*>(encoded.data()),signature.data(),length); return encoded;
}
Object inspect_kalshi_signing_key(const std::filesystem::path& path) {
    const auto pem=read_bytes(path);BIO* bio=BIO_new_mem_buf(pem.data(),static_cast<int>(pem.size()));require(bio!=nullptr,"key validation buffer failed");
    EVP_PKEY* key=PEM_read_bio_PrivateKey(bio,nullptr,[](char*,int,int,void*){return 0;},nullptr);BIO_free(bio);
    require(key!=nullptr,"signing key unreadable/unsupported; contents omitted");
    const auto type=EVP_PKEY_base_id(key);const bool ed=type==EVP_PKEY_ED25519;
    if(type!=EVP_PKEY_RSA && !ed){EVP_PKEY_free(key);throw std::invalid_argument("unsupported signing key algorithm; contents omitted");}
    const std::string message="Polytope read-only local signing compatibility check";
    const auto encoded=kalshi_signature(pem,message);std::vector<unsigned char> signature(encoded.size());
    int count=EVP_DecodeBlock(signature.data(),reinterpret_cast<const unsigned char*>(encoded.data()),encoded.size());
    for(auto i=encoded.rbegin();i!=encoded.rend() && *i=='=';++i)--count;
    EVP_MD_CTX* ctx=EVP_MD_CTX_new();EVP_PKEY_CTX* verify=nullptr;
    bool valid=ctx && EVP_DigestVerifyInit(ctx,&verify,ed?nullptr:EVP_sha256(),nullptr,key)==1;
    if(!ed)valid=valid && EVP_PKEY_CTX_set_rsa_padding(verify,RSA_PKCS1_PSS_PADDING)>0 && EVP_PKEY_CTX_set_rsa_pss_saltlen(verify,RSA_PSS_SALTLEN_DIGEST)>0;
    valid=valid && count>0 && EVP_DigestVerify(ctx,signature.data(),count,reinterpret_cast<const unsigned char*>(message.data()),message.size())==1;
    const auto bits=EVP_PKEY_bits(key);EVP_MD_CTX_free(ctx);EVP_PKEY_free(key);require(valid,"local signature validation failed; contents omitted");
    return {{"readable",true},{"algorithm",ed?"Ed25519":"RSA-PSS-SHA256"},{"key_bits",bits},{"compatible_with_existing_signer",true},
        {"local_signature_verified",true},{"credentials_recorded",false},{"live_orders_sent",0}};
}
}  // namespace trading_engine::cross_venue::research
