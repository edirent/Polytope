#include "engine/strategy/cross_venue/LiveResearch.h"
#include <algorithm>
#include <fstream>

namespace trading_engine::cross_venue::research {
Object audit_feeds(const std::filesystem::path& directory,const std::filesystem::path& out) {
    require(!out.empty() && (!std::filesystem::exists(out)||std::filesystem::is_empty(out)),"feed audit output must be new/empty");
    std::filesystem::create_directories(out);
    const auto manifest=read_object(directory/"capture_manifest.json");
    auto c=load_config(directory/"capture_config.json");
    require(sha256(json::serialize(c.original))==get_text(manifest,"config_sha256"),"captured config changed");
    c.policy.max_liveness_age_ns=c.heartbeat_ms*1'000'000;c.policy.max_authoritative_age_ns=c.authoritative_ms*1'000'000;
    const auto selected=json::parse(read_bytes(directory/"selected_markets.json")).as_array();
    std::vector<Market> markets;for(const auto& v:selected){
        const auto& o=v.as_object();markets.push_back(normalize_market(get_text(o,"venue"),o.at("raw").as_object()));}
    const auto frozen=read_object(directory/"approved_pairs.json");
    std::vector<ApprovedPair> pairs;for(const auto& p:frozen.at("pairs").as_array())pairs.push_back(parse_pair(p.as_object(),false));
    const auto records=read_capture_records(directory,manifest);
    ResearchState state(c,markets,pairs);Array diagnostics,intervals;
    struct Coverage {std::uint64_t ns=0,observations=0;};std::map<std::string,Coverage> coverage;
    struct Connection {std::uint64_t open=0,down=0,downtime=0,opens=0,disconnects=0,last_frame=0,max_frame_gap=0,planned_requests=0,planned_disconnects=0;bool planned_pending=false;};
    std::map<std::string,Connection> connections;
    const auto start=get_uint(manifest,"start_monotonic_ns",records.empty()?0:get_uint(records.front().as_object(),"recv_monotonic_ns"));
    const auto end=get_uint(manifest,"end_monotonic_ns");std::uint64_t previous=start,poly_any=0,poly_all=0,kalshi_any=0,kalshi_all=0,simultaneous=0,synchronized=0;
    std::map<std::string,std::uint64_t> watched_healthy,watched_aligned;
    auto integrate=[&](std::uint64_t at){
        const auto health=state.health();std::uint64_t any_until=previous,all_until=at,k_any_until=previous,k_all_until=at;unsigned poly_assets=0,kalshi_assets=0;
        std::map<std::string,std::uint64_t> healthy_until;
        for(const auto& v:health){const auto& h=v.as_object();const auto id=get_text(h,"venue")+":"+get_text(h,"market_id")+":"+get_text(h,"instrument_id");
            auto until=previous;
            if(get_bool(h,"usable") && get_bool(h,"tradable") && get_text(h,"transport")=="ws"){
                until=std::min({at,get_uint(h,"last_liveness_ns")+c.heartbeat_ms*1'000'000,get_uint(h,"last_authoritative_snapshot_ns")+c.authoritative_ms*1'000'000});
                if(until>previous)coverage[id].ns+=until-previous;
            }
            if(get_text(h,"venue")=="polymarket_international"){
                ++poly_assets;any_until=std::max(any_until,until);all_until=std::min(all_until,until);
            }else if(get_text(h,"venue")=="kalshi"){
                ++kalshi_assets;k_any_until=std::max(k_any_until,until);k_all_until=std::min(k_all_until,until);
            }
            healthy_until[id]=until;
        }
        if(any_until>previous)poly_any+=any_until-previous;
        if(poly_assets && all_until>previous)poly_all+=all_until-previous;
        if(k_any_until>previous)kalshi_any+=k_any_until-previous;
        if(kalshi_assets && k_all_until>previous)kalshi_all+=k_all_until-previous;
        if(std::min(any_until,k_any_until)>previous)simultaneous+=std::min(any_until,k_any_until)-previous;
        if(const auto* watches=c.original.if_contains("diagnostic_watch_pairs");watches && watches->is_array())for(const auto& v:watches->as_array()){
            const auto& watch=v.as_object();const auto label=get_text(watch,"label");const Market *a=nullptr,*b=nullptr;
            for(const auto& m:markets){if(m.venue=="kalshi" && m.id==get_text(watch,"kalshi_market"))a=&m;
                if(m.venue=="polymarket_international" && m.id==get_text(watch,"polymarket_market"))b=&m;}
            if(!a || !b)continue;
            const auto k=a->venue+":"+a->id+":"+a->yes.instrument_id,p=b->venue+":"+b->id+":"+b->no.instrument_id;
            const auto until=std::min(healthy_until[k],healthy_until[p]);
            if(until>previous){watched_healthy[label]+=until-previous;
                const auto x=state.books().read(a->yes).evidence.recv_monotonic_ns,y=state.books().read(b->no).evidence.recv_monotonic_ns;
                if((x>y?x-y:y-x)<=c.policy.max_skew_ns)watched_aligned[label]+=until-previous;
            }
        }
        std::uint64_t sync_until=previous;
        for(const auto& pair:pairs)try {
            require(state.continuous_pair(pair),"not continuous");const auto b=state.books().read_pair(pair);validate_books(pair,b,c.policy,previous);
            auto until=at;for(const auto& book:b)until=std::min({until,book.evidence.last_liveness_ns+c.heartbeat_ms*1'000'000,
                book.evidence.last_authoritative_snapshot_ns+c.authoritative_ms*1'000'000});
            sync_until=std::max(sync_until,until);
        }catch(...){}
        if(sync_until>previous)synchronized+=sync_until-previous;
    };
    struct WireStats {std::uint64_t snapshots=0,deltas=0,sequence_gaps=0,missing_sid_seq=0,clock_regressions=0,missing_clock=0,subscription_errors=0,deltas_before_snapshot=0,parse_errors=0;
        std::map<std::pair<std::uint64_t,std::uint64_t>,std::uint64_t> sequences;
        std::map<std::string,std::uint64_t> clocks;
        std::vector<double> ages,book_ages,event_ages,processing;};
    std::map<std::string,WireStats> wire;
    struct Recovery {std::string venue,market,instrument;std::uint64_t epoch=0,snapshot_record=0,delta_record=0,deltas=0;};std::map<std::string,Recovery> recoveries;
    std::uint64_t mismatch=0,older=0,inconclusive=0,book_rejects=0;
    for(const auto& v:records){const auto& r=v.as_object();const auto now=get_uint(r,"availability_monotonic_ns",get_uint(r,"recv_monotonic_ns"));
        integrate(now);previous=now;const auto venue=get_text(r,"venue"),type=get_text(r,"classification"),transport=get_text(r,"transport");
        auto& connection=connections[venue];
        if(transport=="event" && type=="connection_open"){
            if(connection.down){connection.downtime+=now-connection.down;
                intervals.push_back(Object{{"venue",venue},{"start_monotonic_ns",connection.down},{"end_monotonic_ns",now},{"duration_sec",static_cast<double>(now-connection.down)/1e9},{"reason","DISCONNECT_TO_REOPEN"}});connection.down=0;}
            connection.open=now;++connection.opens;connection.last_frame=0;
        }
        if(transport=="event" && type=="validation_reconnect"){++connection.planned_requests;connection.planned_pending=true;}
        if(transport=="event" && type=="disconnect") {++connection.disconnects;connection.down=now;connection.open=0;
            if(connection.planned_pending){++connection.planned_disconnects;connection.planned_pending=false;}}
        if(transport=="ws" && type=="market_data"){
            if(connection.last_frame)connection.max_frame_gap=std::max(connection.max_frame_gap,now-connection.last_frame);connection.last_frame=now;
            auto& w=wire[venue];
            if(r.contains("callback_recv_monotonic_ns"))w.processing.push_back(static_cast<double>(now-get_uint(r,"callback_recv_monotonic_ns"))/1e6);
            try{
                const auto parsed=json::parse(get_text(r,"raw_payload"));Array packets;if(parsed.is_array())packets=parsed.as_array();else packets.push_back(parsed);
                for(const auto& v:packets){if(!v.is_object())continue;const auto& packet=v.as_object();const auto kind=get_text(packet,"type",get_text(packet,"event_type"));
                    std::uint64_t source=0;const auto ms=get_uint(packet,"sending_ts_ms");if(ms)source=ms*1'000'000;
                    if(!source){const auto ts=get_text(packet,"timestamp");if(!ts.empty() && ts.find_first_not_of("0123456789")==std::string::npos)source=std::stoull(ts)*1'000'000;}
                    if(source){const auto wall=get_uint(r,"callback_recv_wall_ns",get_uint(r,"recv_wall_ns"));const auto age=(static_cast<double>(wall)-static_cast<double>(source))/1e6;
                        w.ages.push_back(age);if(kind=="orderbook_snapshot" || kind=="orderbook_delta" || kind=="book" || kind=="price_change")w.book_ages.push_back(age);}
                    const auto epoch=get_uint(r,"connection_epoch"),record=get_uint(r,"record_sequence");
                    if(venue=="kalshi"){
                        if(kind=="error"){++w.subscription_errors;continue;}
                        if(kind!="orderbook_snapshot" && kind!="orderbook_delta")continue;
                        const auto sid=get_uint(packet,"sid"),seq=get_uint(packet,"seq");
                        if(!sid || !seq)++w.missing_sid_seq;
                        else {auto& previous_seq=w.sequences[{epoch,sid}];if(previous_seq && seq!=previous_seq+1)++w.sequence_gaps;previous_seq=seq;}
                        const auto& body=packet.at("msg").as_object();const auto market=get_text(body,"market_ticker");const auto clock=get_uint(body,"clock");
                        if(const auto timestamp=get_uint(body,"ts_ms");timestamp){
                            const auto wall=get_uint(r,"callback_recv_wall_ns",get_uint(r,"recv_wall_ns"));w.event_ages.push_back((static_cast<double>(wall)-static_cast<double>(timestamp)*1e6)/1e6);}
                        const auto id=venue+":"+std::to_string(epoch)+":"+market;
                        auto& recovery=recoveries[id];recovery.venue=venue;recovery.market=market;recovery.instrument="YES+NO";recovery.epoch=epoch;
                        if(!clock)++w.missing_clock;
                        if(kind=="orderbook_snapshot"){++w.snapshots;if(!recovery.snapshot_record)recovery.snapshot_record=record;
                            if(clock && w.clocks[id] && clock<w.clocks[id])++w.clock_regressions;w.clocks[id]=clock;}
                        else {++w.deltas;++recovery.deltas;if(!recovery.snapshot_record)++w.deltas_before_snapshot;
                            else if(!recovery.delta_record)recovery.delta_record=record;
                            if(clock && w.clocks[id] && clock<w.clocks[id])++w.clock_regressions;if(clock)w.clocks[id]=clock;}
                    }else if(kind=="book" || kind=="price_change"){
                        kind=="book"?++w.snapshots:++w.deltas;
                        const Object* body=&packet;if(const auto* payload=packet.if_contains("payload");payload && payload->is_object())body=&payload->as_object();
                        const auto market=get_text(*body,"market",get_text(*body,"conditionId"));
                        std::set<std::string> assets;
                        if(kind=="book")assets.insert(get_text(*body,"asset_id",get_text(*body,"assetId")));
                        else {const auto* changes=body->if_contains("price_changes");if(!changes)changes=body->if_contains("priceChanges");
                            if(changes && changes->is_array())for(const auto& change:changes->as_array())assets.insert(get_text(change.as_object(),"asset_id",get_text(change.as_object(),"assetId")));}
                        for(const auto& asset:assets)if(!asset.empty()){
                            const auto id=venue+":"+std::to_string(epoch)+":"+market+":"+asset;
                            auto& recovery=recoveries[id];recovery.venue=venue;recovery.market=market;recovery.instrument=asset;recovery.epoch=epoch;
                            if(kind=="book"){if(!recovery.snapshot_record)recovery.snapshot_record=record;}
                            else {++recovery.deltas;if(!recovery.snapshot_record)++w.deltas_before_snapshot;else if(!recovery.delta_record)recovery.delta_record=record;}
                        }
                    }
                }
            }catch(...){++w.parse_errors;}
        }
        state.process(r);
        for(const auto& d:state.take_diagnostics()){
            const auto& o=d.as_object();const auto reason=get_text(o,"reason");
            mismatch+=reason=="AUTHORITATIVE_SNAPSHOT_MISMATCH";inconclusive+=reason=="RECONCILIATION_INCONCLUSIVE";
            book_rejects+=reason=="BOOK_REJECTED_OR_RECONCILIATION_FAILED";older+=get_bool(o,"older_source_timestamp");diagnostics.push_back(d);
        }
    }
    integrate(end);
    Array connection_rows,coverage_rows,recovery_rows,watch_rows;
    for(auto& [venue,x]:connections){if(x.down)x.downtime+=end-x.down;
        connection_rows.push_back(Object{{"venue",venue},{"connection_opens",x.opens},{"disconnects",x.disconnects},
            {"controlled_reconnect_requests",x.planned_requests},{"controlled_disconnects",x.planned_disconnects},{"unplanned_disconnects",x.disconnects-x.planned_disconnects},
            {"disconnect_to_reopen_downtime_sec",static_cast<double>(x.downtime)/1e9},{"max_in_session_market_frame_gap_sec",static_cast<double>(x.max_frame_gap)/1e9},
            {"exchange_missing_message_count",nullptr},{"gap_evidence",venue=="kalshi"?"See subscription sequence gap events; gap-event count is not a count of all missing exchange actions":"CLOB has no contiguous public sequence; local recording continuity does not prove exchange completeness"}});
    }
    for(const auto& m:markets)for(const auto& key:{m.yes,m.no}){const auto id=m.venue+":"+m.id+":"+key.instrument_id;
        coverage_rows.push_back(Object{{"venue",m.venue},{"market_id",m.id},{"instrument_id",key.instrument_id},
            {"healthy_l2_seconds",static_cast<double>(coverage[id].ns)/1e9},{"classification","OBSERVED_HEALTHY_WS_ONLY_NOT_LOSSLESS_GUARANTEE"}});}
    auto distribution=[](std::vector<double> values){Object out{{"samples",values.size()},{"min",nullptr},{"p50",nullptr},{"p95",nullptr},{"max",nullptr},{"negative_samples",0}};
        if(!values.empty()){std::sort(values.begin(),values.end());out["min"]=values.front();out["p50"]=values[(values.size()-1)/2];out["p95"]=values[(values.size()-1)*95/100];out["max"]=values.back();
            out["negative_samples"]=std::count_if(values.begin(),values.end(),[](double x){return x<0;});}return out;};
    Object wire_rows;
    for(const auto& [venue,w]:wire)wire_rows[venue]=Object{{"snapshot_packets",w.snapshots},{"delta_packets",w.deltas},
        {"subscription_sequence_gap_events",w.sequence_gaps},{"missing_sid_seq_packets",w.missing_sid_seq},{"clock_regressions_within_session_market",w.clock_regressions},
        {"book_packets_missing_exchange_clock",w.missing_clock},{"subscription_errors",w.subscription_errors},{"deltas_before_snapshot",w.deltas_before_snapshot},
        {"wire_parse_errors",w.parse_errors},
        {"source_to_callback_age_ms",distribution(w.ages)},{"book_source_to_callback_age_ms",distribution(w.book_ages)},
        {"exchange_event_to_callback_age_ms",distribution(w.event_ages)},{"callback_to_processing_ms",distribution(w.processing)},
        {"latency_scope","Source-to-callback age includes venue clock offset/batching and network delay; not a measured network RTT. Callback-to-processing is local serialization delay."}};
    for(const auto& [id,r]:recoveries)recovery_rows.push_back(Object{{"venue",r.venue},{"market_id",r.market},{"connection_epoch",r.epoch},
        {"instrument_id",r.instrument},
        {"first_snapshot_record",r.snapshot_record},{"first_delta_after_snapshot_record",r.delta_record},{"delta_count",r.deltas},
        {"snapshot_then_delta_verified",r.snapshot_record>0 && r.delta_record>r.snapshot_record}});
    for(const auto& [label,ns]:watched_healthy)watch_rows.push_back(Object{{"label",label},{"both_market_books_healthy_ws_sec",static_cast<double>(ns)/1e9},
        {"book_update_receive_skew_within_policy_sec",static_cast<double>(watched_aligned[label])/1e9},{"receive_skew_policy_ms",static_cast<double>(c.policy.max_skew_ns)/1e6},
        {"economic_approval",false},{"classification","DIAGNOSTIC_TRANSPORT_COVERAGE_ONLY_NOT_PAYOFF_OR_ARBITRAGE_APPROVAL"}});
    Object summary{{"schema","cross-venue-feed-audit-v0.1.1"},{"source_run",directory.string()},
        {"raw_records_verified",records.size()},{"local_record_sequence_gaps",0},{"connections",connection_rows},
        {"snapshot_reconciliation_mismatches",mismatch},{"inconclusive_comparisons",inconclusive},{"older_rest_snapshot_comparisons",older},
        {"book_rejects",book_rejects},{"polymarket_any_asset_healthy_l2_sec",static_cast<double>(poly_any)/1e9},
        {"polymarket_all_selected_assets_healthy_l2_sec",static_cast<double>(poly_all)/1e9},
        {"kalshi_any_asset_healthy_l2_sec",static_cast<double>(kalshi_any)/1e9},{"kalshi_all_selected_assets_healthy_l2_sec",static_cast<double>(kalshi_all)/1e9},
        {"both_venues_any_asset_healthy_l2_sec",static_cast<double>(simultaneous)/1e9},{"diagnostic_watch_pairs",watch_rows},
        {"wire_protocol",wire_rows},{"snapshot_recovery",recovery_rows},
        {"kalshi_snapshot_and_delta_observed",wire["kalshi"].snapshots>0 && wire["kalshi"].deltas>0},
        {"approved_continuous_synchronized_l2_sec",static_cast<double>(synchronized)/1e9},
        {"kalshi_credentials_required","KALSHI_API_KEY_ID plus KALSHI_PRIVATE_KEY_PATH or KALSHI_PRIVATE_KEY; never place secret values in reports"},
        {"verified_live_realized_pnl",nullptr},{"live_orders_sent",0},{"statistics",state.counters()}};
    write_json(out/"summary.json",summary);write_json(out/"diagnostics.json",diagnostics);write_json(out/"downtime_intervals.json",intervals);
    write_table(out/"snapshot_recovery.csv",{"venue","market_id","instrument_id","connection_epoch","first_snapshot_record","first_delta_after_snapshot_record","delta_count","snapshot_then_delta_verified"},recovery_rows);
    write_table(out/"diagnostic_pair_coverage.csv",{"label","both_market_books_healthy_ws_sec","book_update_receive_skew_within_policy_sec","receive_skew_policy_ms","economic_approval","classification"},watch_rows);
    write_table(out/"asset_coverage.csv",{"venue","market_id","instrument_id","healthy_l2_seconds","classification"},coverage_rows);
    write_table(out/"reconciliation.csv",{"reason","venue","market_id","instrument_id","record_sequence","recv_monotonic_ns","raw_hash","live_raw_hash","live_update_ns","request_started_monotonic_ns","request_overlap","older_source_timestamp","live_source_ts_ns","rest_source_ts_ns","first_bid_difference","first_ask_difference"},diagnostics);
    Array inputs;for(const auto& name:{"capture_manifest.json","capture_config.json","selected_markets.json","approved_pairs.json"})inputs.push_back(Object{{"file",name},{"sha256",sha256(read_bytes(directory/name))}});
    Array sources;const auto source_root=std::filesystem::path(POLYTOPE_SOURCE_DIR);
    for(const auto& folder:{"engine/state/cross_venue","engine/strategy/cross_venue","engine/decode/parser/kalshi","engine/decode/parser/polymarket","replay/cross_venue"})
        for(const auto& entry:std::filesystem::recursive_directory_iterator(source_root/folder))if(entry.is_regular_file() && (entry.path().extension()==".h" || entry.path().extension()==".cpp")){
            const auto relative=std::filesystem::relative(entry.path(),source_root),target=out/"source_snapshot"/relative;
            std::filesystem::create_directories(target.parent_path());std::filesystem::copy_file(entry.path(),target);
            sources.push_back(Object{{"file",relative.string()},{"sha256",sha256(read_bytes(entry.path()))}});
        }
    write_json(out/"audit_manifest.json",Object{{"source_inputs",inputs},{"raw_inventory",manifest.at("raw_inventory")},{"sources",sources},
        {"audit_source_sha256",sha256(read_bytes(std::filesystem::path(POLYTOPE_SOURCE_DIR)/"replay/cross_venue/FeedAudit.cpp"))},
        {"research_state_sha256",sha256(read_bytes(std::filesystem::path(POLYTOPE_SOURCE_DIR)/"engine/strategy/cross_venue/ResearchState.cpp"))}});
    return summary;
}
}
