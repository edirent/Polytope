#include "engine/strategy/cross_venue/LiveResearch.h"
#include "engine/feed/source_runtime/WebSocketClient.h"
#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <thread>

namespace trading_engine::cross_venue::research {
namespace {
volatile std::sig_atomic_t interrupted=0;
void signal_handler(int){interrupted=1;}
Object pair_json(const ApprovedPair& p) {
    Array payoffs;for(const auto& s:p.payoffs)payoffs.push_back(Object{{"name",s.name},{"yes",decimal(s.yes)},{"no",decimal(s.no)}});
    return {{"pair_id",p.pair_id},{"leg_yes",key_json(p.leg_yes)},{"leg_no",key_json(p.leg_no)},
        {"rules_hash_yes",p.rules_hash_yes},{"rules_hash_no",p.rules_hash_no},{"review_id",p.review_id},
        {"approval_revision",p.approval_revision},{"enabled",p.enabled},{"synthetic",p.synthetic},{"payoffs",payoffs},{"artifact_hash",p.artifact_hash}};
}
std::vector<Market> select_markets(const Discovery& d,const Config& c,const std::vector<ApprovedPair>& approved) {
    std::vector<Market> selected;
    for(const auto& venue:{std::string("kalshi"),std::string("polymarket_international")}) {
        std::vector<Market> eligible;for(const auto& m:d.markets)if(m.venue==venue && m.protocol_supported && m.tradable && !m.yes.instrument_id.empty() && !m.no.instrument_id.empty())eligible.push_back(m);
        std::sort(eligible.begin(),eligible.end(),[](const auto& a,const auto& b){return a.volume!=b.volume?a.volume>b.volume:a.liquidity>b.liquidity;});
        std::set<std::string> chosen,categories;
        for(const auto& p:approved)for(const auto& k:{p.leg_yes,p.leg_no})if(venue_name(k.venue)==venue)chosen.insert(k.market_id);
        if(const auto* ids=c.original.if_contains("capture_market_ids");ids && ids->is_object())
            if(const auto* values=ids->as_object().if_contains(venue);values && values->is_array())
                for(const auto& id:values->as_array())chosen.insert(text_value(id));
        for(const auto& m:eligible)if(chosen.size()<c.capture_per_venue && categories.insert(m.category).second)chosen.insert(m.id);
        for(const auto& m:eligible)if(chosen.size()<c.capture_per_venue)chosen.insert(m.id);
        for(const auto& m:eligible)if(chosen.contains(m.id))selected.push_back(m);
    } return selected;
}
void snapshot_sources(const std::filesystem::path& directory) {
    const auto root=std::filesystem::path(POLYTOPE_SOURCE_DIR);Array files;
    for(const auto& folder:{"engine","oracle","tools","replay"}) {
        for(const auto& entry:std::filesystem::recursive_directory_iterator(root/folder)) {
            if(!entry.is_regular_file())continue;const auto ext=entry.path().extension().string();
            if(ext!=".cpp" && ext!=".h" && ext!=".hpp" && entry.path().filename()!="CMakeLists.txt")continue;
            const auto relative=std::filesystem::relative(entry.path(),root);const auto target=directory/"source_snapshot"/relative;
            std::filesystem::create_directories(target.parent_path());std::filesystem::copy_file(entry.path(),target);
            files.push_back(Object{{"file",relative.string()},{"sha256",sha256(read_bytes(entry.path()))}});
        }
    }
    std::filesystem::copy_file(root/"CMakeLists.txt",directory/"source_snapshot/CMakeLists.txt");
    files.push_back(Object{{"file","CMakeLists.txt"},{"sha256",sha256(read_bytes(root/"CMakeLists.txt"))}});
    write_json(directory/"source_manifest.json",Object{{"git_revision",POLYTOPE_GIT_REVISION},{"files",files}});
}
}

CaptureResult capture(const Config& c,unsigned duration,const std::filesystem::path& requested) {
    require(duration>0 && duration<=72*3600,"duration must be 1 second to 72 hours");
    const auto reconnect_after=get_uint(c.original,"diagnostic_reconnect_after_sec");
    require(!reconnect_after || reconnect_after<duration,"diagnostic reconnect must be within bounded capture");
    if(get_bool(c.original,"require_research_ready")){
        const auto id=std::getenv("KALSHI_API_KEY_ID"),path=std::getenv("KALSHI_PRIVATE_KEY_PATH"),key=std::getenv("KALSHI_PRIVATE_KEY");
        require(c.enable_ws && id && *id && ((path && *path)||(key && *key)),"long research capture requires authenticated Kalshi WS credentials");
        require(get_bool(c.original,"require_explicit_revision_review"),"long research capture requires explicit human revision gate");
    }
    const auto directory=requested.empty()?c.runs_root/("capture_"+std::to_string(wall_ns())):requested;
    require(!std::filesystem::exists(directory) || std::filesystem::is_empty(directory),"capture directory must be new/empty; restart into a new immutable run");
    std::filesystem::create_directories(directory);
    write_json(directory/"capture_config.json",c.original);snapshot_sources(directory);
    std::cout<<"Discovering production markets into "<<directory<<std::endl;
    const auto discovery=discover(c,directory);Array approval_rejections;
    auto approved=load_approved_pairs(c,discovery.markets,approval_rejections);
    if(get_bool(c.original,"require_research_ready"))require(!approved.empty(),"long research capture has no valid explicitly approved revisions");
    // The opposite direction follows from the same reviewed binary pair;
    // derive its artifact transparently, without approving a new market match.
    const auto original_count=approved.size();
    for(std::size_t i=0;i<original_count;++i) {
        const auto& p=approved[i];const Market *a=nullptr,*b=nullptr;
        for(const auto& m:discovery.markets){if(m.yes==p.leg_yes || m.no==p.leg_yes)a=&m;if(m.yes==p.leg_no || m.no==p.leg_no)b=&m;}
        if(a && b){auto reverse=p;reverse.pair_id=p.pair_id+":derived_reverse";
            reverse.leg_yes=b->yes==p.leg_no?b->no:b->yes;reverse.leg_no=a->yes==p.leg_yes?a->no:a->yes;
            reverse.rules_hash_yes=p.rules_hash_no;reverse.rules_hash_no=p.rules_hash_yes;reverse.artifact_hash=pair_hash(reverse);validate_pair(reverse);approved.push_back(reverse);}
    }
    Array pairs;for(const auto& p:approved)pairs.push_back(pair_json(p));
    write_json(directory/"approved_pairs.json",Object{{"schema","cross-venue-frozen-approvals-v0.1"},{"pairs",pairs},{"original_human_approved_count",original_count}});
    write_json(directory/"approval_rejections.json",approval_rejections);
    auto selected=select_markets(discovery,c,approved);enrich_selected_fees(c,selected,directory);
    Array selected_json;for(const auto& m:selected)selected_json.push_back(market_json(m));
    write_json(directory/"selected_markets.json",selected_json);
    RawRecorder recorder(directory,c.rotate_bytes,c.max_raw_bytes);ResearchState state(c,selected,approved);
    std::mutex processing_mutex;std::atomic<bool> stop{false};std::atomic<unsigned> fatal{0};
    std::ofstream connections(directory/"connection_events.jsonl"),opportunities(directory/"opportunities.live.jsonl"),lifecycle(directory/"candidate_lifecycle.jsonl"),diagnostics(directory/"feed_diagnostics.jsonl");
    std::ofstream timings(directory/"processing_timings.jsonl");timings.exceptions(std::ios::badbit|std::ios::failbit);
    diagnostics.exceptions(std::ios::badbit|std::ios::failbit);
    connections.exceptions(std::ios::badbit|std::ios::failbit);opportunities.exceptions(std::ios::badbit|std::ios::failbit);lifecycle.exceptions(std::ios::badbit|std::ios::failbit);
    std::set<std::string> observed_liquidity;std::uint64_t duplicate_count=0,eligible_count=0;
    auto emit=[&](const std::string& venue,const std::string& transport,const std::string& classification,const std::string& raw,
        const VenueInstrumentKey* key,int status,std::uint64_t epoch,std::uint64_t arrival,const std::string& source,std::uint64_t request_started=0)->bool {
        const auto callback_wall=wall_ns(),callback_mono=mono_ns();
        const auto* configured_id=std::getenv("KALSHI_API_KEY_ID");
        if((configured_id && *configured_id && raw.find(configured_id)!=std::string::npos) ||
            raw.find("PRIVATE KEY-----")!=std::string::npos || raw.find("KALSHI-ACCESS-SIGNATURE")!=std::string::npos ||
            raw.find("KALSHI-ACCESS-KEY")!=std::string::npos){
            ++fatal;stop=true;std::cerr<<"capture stopped: sensitive authentication material rejected before recording"<<std::endl;return false;
        }
        std::lock_guard lock(processing_mutex);
        if(stop)return false;
        try {
            const auto received_wall=wall_ns(),received_mono=mono_ns();
            Object record{{"schema","cross-venue-raw-v0.1"},{"evidence_level","REAL_PUBLIC_API"},{"venue",venue},{"transport",transport},
                {"classification",classification},{"raw_payload",raw},{"recv_wall_ns",received_wall},{"recv_utc",utc(received_wall)},
                {"recv_monotonic_ns",received_mono},{"availability_monotonic_ns",received_mono},{"exchange_timestamp_ns",0},
                {"callback_recv_wall_ns",callback_wall},{"callback_recv_monotonic_ns",callback_mono},
                {"connection_epoch",epoch},{"connection_id",venue+":"+std::to_string(epoch)},{"arrival_sequence",arrival},
                {"subscription_state",transport=="ws"?"SUBSCRIBED_PUBLIC_MARKET_DATA":"REST_SNAPSHOT_SAMPLING"},{"http_status",status},{"source",source}};
            if(key)record["key"]=key_json(*key);
            if(request_started)record["request_started_monotonic_ns"]=request_started;
            if(transport=="ws")try {
                auto parsed=json::parse(raw);if(parsed.is_object()) {
                    const auto& o=parsed.as_object();record["sid"]=get_uint(o,"sid");record["stream_sequence"]=get_uint(o,"seq");
                    if(const auto* msg=o.if_contains("msg");msg && msg->is_object())record["exchange_clock"]=get_uint(msg->as_object(),"clock");
                    if(const auto* msg=o.if_contains("msg");msg && msg->is_object() && msg->as_object().contains("ts_ms"))
                        record["exchange_event_timestamp_ns"]=get_uint(msg->as_object(),"ts_ms")*1'000'000;
                    const auto ts=get_text(o,"timestamp");if(!ts.empty() && ts.find_first_not_of("0123456789")==std::string::npos)record["exchange_timestamp_ns"]=std::stoull(ts)*1'000'000;
                    if(o.contains("sending_ts_ms"))record["exchange_timestamp_ns"]=get_uint(o,"sending_ts_ms")*1'000'000;
                }
            }catch(...){}
            record=recorder.append(venue,std::move(record)); // durable before strategy/state
            if(transport=="event"){connections<<json::serialize(record)<<'\n';connections.flush();}
            const bool healthy=state.process(record);
            for(const auto& detail:state.take_diagnostics())diagnostics<<json::serialize(detail)<<'\n';
            diagnostics.flush();
            for(auto& candidate:state.scan(received_mono,received_wall)) {
                if(observed_liquidity.size()>=100000)throw std::runtime_error("OPPORTUNITY_STORAGE_LIMIT");
                const bool duplicate=!observed_liquidity.insert(candidate.liquidity_id).second;
                if(duplicate)++duplicate_count;else {opportunities<<json::serialize(candidate.row)<<'\n';++eligible_count;}
                if(!duplicate)for(const auto* stage:{"DETECTED","VALIDATED"})lifecycle<<json::serialize(Object{{"candidate_id",candidate.opportunity.candidate_id},{"lifecycle",stage},{"observed_mono_ns",received_mono}})<<'\n';
                lifecycle<<json::serialize(Object{{"candidate_id",candidate.opportunity.candidate_id},{"liquidity_id",candidate.liquidity_id},
                    {"lifecycle",duplicate?"REJECTED":"SHADOW_ELIGIBLE"},{"reason",duplicate?"DUPLICATE_LIQUIDITY":"READ_ONLY_NO_ORDERS"},
                    {"observed_mono_ns",received_mono}})<<'\n';
            }
            timings<<json::serialize(Object{{"record_sequence",get_uint(record,"record_sequence")},{"raw_hash",get_text(record,"raw_hash")},{"processing_completed_monotonic_ns",mono_ns()},{"processing_completed_wall_ns",wall_ns()}})<<'\n';timings.flush();
            return healthy;
        }catch(const std::exception& e){std::cerr<<"capture stopped: "<<e.what()<<std::endl;++fatal;stop=true;return false;}
    };
    const auto started_wall=wall_ns(),started_mono=mono_ns(),deadline=started_mono+static_cast<std::uint64_t>(duration)*1'000'000'000;
    std::cout<<"Live capture: selected "<<selected.size()<<" markets, "<<original_count<<" approved pairs; duration "<<duration<<"s; no order transport"<<std::endl;
    interrupted=0;auto old_int=std::signal(SIGINT,signal_handler),old_term=std::signal(SIGTERM,signal_handler);
    std::vector<std::thread> workers;
    struct WsControl {std::mutex mutex;std::shared_ptr<feed::WebSocketClient> client;std::atomic<std::uint64_t> last_seen{0},epoch{0};};
    std::array<WsControl,2> controls;
    for(std::size_t vi=0;vi<2;++vi) {
        const std::string venue=vi==0?"kalshi":"polymarket_international";
        std::vector<Market> markets;for(const auto& m:selected)if(m.venue==venue)markets.push_back(m);
        if(markets.empty())continue;
        workers.emplace_back([&,vi,venue,markets] {
            PublicHttpClient client;std::map<std::string,std::uint64_t> next_allowed;std::uint64_t arrival=0;
            while(!stop && mono_ns()<deadline) {
                for(const auto& m:markets)for(const auto& key:{m.yes,m.no}) {
                    if(stop || mono_ns()>=deadline)break;
                    if(venue=="kalshi" && key.instrument_id=="NO")continue; // REST response contains both sides
                    const auto url=venue=="kalshi"?c.kalshi_rest+"/markets/"+m.id+"/orderbook?depth=64":c.clob_rest+"/book?token_id="+key.instrument_id;
                    if(mono_ns()<next_allowed[url])continue;
                    const auto request_started=mono_ns();const auto r=client.get(url,8);
                    const bool healthy=emit(venue,"rest","snapshot",r.body.empty()?r.error:r.body,&key,r.status,0,++arrival,url,request_started);
                    bool streaming=false;
                    {auto& control=controls[vi];std::lock_guard lock(control.mutex);
                        streaming=control.client && control.client->connected();
                        if(!healthy && r.status==200 && streaming){
                            emit(venue,"event","rebuild_requested","REST_RECONCILIATION_FAILURE; see feed_diagnostics.jsonl",nullptr,0,0,0,url);control.client->disconnect();}}
                    // 403/401 is an authorization/access failure, not a signal
                    // to try alternative hosts or geolocation workarounds.
                    const auto interval=streaming?c.snapshot_ms:c.poll_ms;
                    const auto wait=(r.status==403 || r.status==401)?60000ULL:(r.status==429?30000ULL:interval);
                    next_allowed[url]=mono_ns()+wait*1'000'000;
                }
                for(unsigned i=0;i<10 && !stop;++i)std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });
        if(!c.enable_ws)continue;
        const char* key_id=std::getenv("KALSHI_API_KEY_ID");const char* key_path=std::getenv("KALSHI_PRIVATE_KEY_PATH");const char* inline_key=std::getenv("KALSHI_PRIVATE_KEY");
        if(vi==0 && (!key_id || !*key_id || ((!key_path || !*key_path) && (!inline_key || !*inline_key)))) {
            emit(venue,"event","missing_ws_credentials","KALSHI_API_KEY_ID and KALSHI_PRIVATE_KEY_PATH (or KALSHI_PRIVATE_KEY) absent; public REST diagnostic only",nullptr,0,0,0,c.kalshi_ws);
            continue;
        }
        workers.emplace_back([&,vi,venue,markets,key_id,key_path,inline_key] {
            auto& control=controls[vi];std::uint64_t epoch=0,arrival=0;unsigned attempts=0;
            while(!stop && mono_ns()<deadline) {
                auto client=std::make_shared<feed::WebSocketClient>(vi==0?c.kalshi_ws:c.clob_ws);
                {std::lock_guard lock(control.mutex);control.client=client;}
                ++epoch;arrival=0;
                control.epoch=epoch;
                if(vi==0)try {
                    const auto timestamp=std::to_string(wall_ns()/1'000'000);
                    const auto pem=inline_key?std::string(inline_key):read_bytes(key_path);
                    client->set_handshake_headers({{"KALSHI-ACCESS-KEY",key_id},{"KALSHI-ACCESS-TIMESTAMP",timestamp},
                        {"KALSHI-ACCESS-SIGNATURE",kalshi_signature(pem,timestamp+"GET/trade-api/ws/v2")}});
                }catch(...){emit(venue,"event","authentication_error","Kalshi signing key unavailable/invalid; secret contents omitted",nullptr,0,epoch,0,c.kalshi_ws);break;}
                client->set_on_open([&,weak=std::weak_ptr<feed::WebSocketClient>(client)] {
                    auto client=weak.lock();if(!client)return;
                    if(stop || mono_ns()>=deadline){client->disconnect();return;}
                    control.last_seen=mono_ns();emit(venue,"event","connection_open","PUBLIC_DATA_SESSION_OPEN",nullptr,101,epoch,0,client->endpoint());
                    Array ids;for(const auto& m:markets){if(vi==0)ids.emplace_back(m.id);else{ids.emplace_back(m.yes.instrument_id);ids.emplace_back(m.no.instrument_id);}}
                    if(vi==0) {
                        client->send(json::serialize(Object{{"id",1},{"cmd","subscribe"},{"params",Object{{"channels",Array{"orderbook_delta"}},{"market_tickers",ids}}}}));
                        client->send(json::serialize(Object{{"id",2},{"cmd","subscribe"},{"params",Object{{"channels",Array{"market_lifecycle_v2"}}}}}));
                    }else client->send(json::serialize(Object{{"assets_ids",ids},{"type","market"},{"custom_feature_enabled",true}}));
                });
                client->set_on_control([&](const std::string& kind){control.last_seen=mono_ns();emit(venue,"ws","heartbeat",kind,nullptr,101,epoch,++arrival,"websocket_control");});
                client->set_on_message([&,weak=std::weak_ptr<feed::WebSocketClient>(client)](const std::string& raw) {
                    auto client=weak.lock();if(!client)return;
                    control.last_seen=mono_ns();const bool heartbeat=raw=="PONG" || raw=="PING";
                    if(!emit(venue,"ws",heartbeat?"heartbeat":"market_data",raw,nullptr,101,epoch,++arrival,client->endpoint()))client->disconnect();
                });
                client->set_on_error([&](const std::string& error){emit(venue,"event","connection_error",error,nullptr,0,epoch,++arrival,vi==0?c.kalshi_ws:c.clob_ws);});
                const auto attempt_started=mono_ns();
                try{client->connect();if(stop || mono_ns()>=deadline){client->disconnect();break;}client->run();}catch(...){}
                if(mono_ns()-attempt_started>=60'000'000'000ULL)attempts=0;
                client->disconnect();
                emit(venue,"event","disconnect","SESSION_ENDED_REBUILD_REQUIRED",nullptr,0,epoch,++arrival,vi==0?c.kalshi_ws:c.clob_ws);
                {std::lock_guard lock(control.mutex);control.client.reset();}
                const auto backoff=std::min(60000U,1000U*(1U<<std::min(attempts++,6U)));
                for(unsigned waited=0;waited<backoff && !stop && mono_ns()<deadline;waited+=100)std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });
    }
    auto last_ping=mono_ns();std::array<bool,2> validation_reconnected{false,false};
    while(!stop && !interrupted && mono_ns()<deadline) {
        for(std::size_t i=0;i<controls.size();++i){auto& ctrl=controls[i];std::lock_guard lock(ctrl.mutex);
            if(ctrl.client && ctrl.client->connected()) {
                if(reconnect_after && !validation_reconnected[i] && mono_ns()-started_mono>=reconnect_after*1'000'000'000ULL){
                    validation_reconnected[i]=true;emit(i==0?"kalshi":"polymarket_international","event","validation_reconnect",
                        "CONTROLLED_RECONNECT_FOR_SNAPSHOT_RECOVERY_VALIDATION",nullptr,0,ctrl.epoch,0,"bounded_validation");ctrl.client->disconnect();
                }else if(mono_ns()-ctrl.last_seen>c.heartbeat_ms*1'000'000){emit(i==0?"kalshi":"polymarket_international","event","timeout","LIVENESS_TIMEOUT",nullptr,0,ctrl.epoch,0,"watchdog");ctrl.client->disconnect();}
                else if(i==1 && mono_ns()-last_ping>10'000'000'000ULL)try{ctrl.client->send("PING");}catch(...){}
            }
        }
        if(mono_ns()-last_ping>10'000'000'000ULL)last_ping=mono_ns();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    stop=true;
    for(auto& control:controls){std::lock_guard lock(control.mutex);if(control.client)control.client->disconnect();}
    for(auto& worker:workers)worker.join();
    std::signal(SIGINT,old_int);std::signal(SIGTERM,old_term);
    opportunities.flush();lifecycle.flush();connections.flush();
    {std::ifstream candidates(directory/"opportunities.live.jsonl");std::string line;while(std::getline(candidates,line)){
        const auto candidate=json::parse(line).as_object();lifecycle<<json::serialize(Object{{"candidate_id",get_text(candidate,"candidate_id")},
            {"lifecycle","EXPIRED"},{"reason","CAPTURE_ENDED_WITHOUT_ANY_ORDER_SUBMISSION"}})<<'\n';}lifecycle.flush();}
    const auto ended_wall=wall_ns(),ended_mono=mono_ns();const auto stats=state.counters();
    write_json(directory/"capture_health.json",state.health());
    write_table(directory/"market_health.csv",{"venue","market_id","instrument_id","usable","snapshot_version","last_market_update_ns","last_authoritative_snapshot_ns","last_liveness_ns","sequence","sid","exchange_clock","health_reason","protocol","tradable"},state.health());
    Object discovered_counts;for(const auto& m:discovery.markets)discovered_counts[m.venue]=get_uint(discovered_counts,m.venue)+1;
    Object complete;for(const auto& [k,v]:discovery.complete)complete[k]=v;
    Object manifest{{"schema","cross-venue-capture-v0.1"},{"evidence_level","REAL_PUBLIC_API_CAPTURE"},
        {"execution_mode","READ_ONLY_NO_ORDERS"},{"data_completeness","PER_VENUE_TRANSPORT_AND_ERRORS_REPORTED"},
        {"start_utc",utc(started_wall)},{"end_utc",utc(ended_wall)},{"start_wall_ns",started_wall},{"end_wall_ns",ended_wall},
        {"start_monotonic_ns",started_mono},{"end_monotonic_ns",ended_mono},{"requested_duration_sec",duration},
        {"actual_duration_sec",static_cast<double>(ended_mono-started_mono)/1e9},{"interrupted",interrupted!=0},{"fatal_errors",fatal.load()},
        {"market_counts",discovered_counts},{"discovery_complete",complete},{"candidate_pairs",discovery.candidates.size()},
        {"approved_pairs",original_count},{"approved_directions",approved.size()},{"selected_markets",selected.size()},
        {"config_sha256",sha256(json::serialize(c.original))},{"software_revision",POLYTOPE_GIT_REVISION},
        {"source_manifest_sha256",sha256(read_bytes(directory/"source_manifest.json"))},
        {"processing_timing_file","processing_timings.jsonl"},{"processing_timing_sha256",sha256(read_bytes(directory/"processing_timings.jsonl"))},
        {"raw_inventory",recorder.inventory()},{"raw_bytes",recorder.bytes()},{"statistics",stats},
        {"unique_positive_candidates",eligible_count},{"duplicate_observations",duplicate_count},
        {"live_orders_sent",0},{"verified_live_realized_pnl",nullptr}};
    write_json(directory/"capture_manifest.json",manifest);
    std::cout<<"Capture saved: "<<directory<<"; raw bytes="<<recorder.bytes()<<"; approved pairs="<<original_count<<"; live orders=0"<<std::endl;
    return {directory,manifest};
}
}  // namespace trading_engine::cross_venue::research
