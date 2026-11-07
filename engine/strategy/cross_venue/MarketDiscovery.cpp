#include "engine/strategy/cross_venue/LiveResearch.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>
#include <thread>
#include <limits>

namespace trading_engine::cross_venue::research {
namespace {
std::string lower(std::string s) { for (auto& c : s) c = std::tolower(static_cast<unsigned char>(c)); return s; }
std::string category(const std::string& title) {
    const auto t = lower(title);
    for (const auto& [name,words] : std::vector<std::pair<std::string,std::vector<std::string>>>{
        {"crypto",{"bitcoin","ethereum","crypto","btc ","solana"}},
        {"economics",{"inflation","cpi","gdp","federal reserve","fed rate","unemployment","interest rate","recession"}},
        {"elections",{"election","president","nomination","senate","governor","democratic","republican","congress"}},
        {"sports",{" points"," goals"," assists","nba","nhl","nfl","world cup"," vs."," win","champion","playoff"}}})
        for (const auto& word : words) if (t.find(word) != std::string::npos) return name;
    return "other";
}
Money metadata_amount(const Object& o, std::string_view name, Money fallback = 0) {
    const auto* value = o.if_contains(name); if (!value || value->is_null()) return fallback;
    try {
        if (!value->is_double()) return money(text_value(*value));
        const auto d = value->as_double(); require(std::isfinite(d) && std::abs(d) < 1e10,"bad metadata precision");
        const auto rounded = std::llround(d*unit); require(std::abs(d*unit-rounded) < 0.0001,"sub-micro metadata unsupported"); return rounded;
    } catch (...) { return fallback; }
}
double screen_number(const Object& o, std::string_view key) { try { const auto n=std::stod(get_text(o,key,"0"));return std::isfinite(n)&&n>=0?n:0; } catch (...) { return 0; } }
Array array_field(const Object& o, std::string_view key) {
    const auto* v = o.if_contains(key); if (!v || v->is_null()) return {};
    return v->is_array() ? v->as_array() : json::parse(text_value(*v)).as_array();
}
std::string encode(std::string_view s) {
    std::ostringstream out; for (unsigned char c : s) {
        if (std::isalnum(c) || c=='-' || c=='_' || c=='.') out << c;
        else out << '%' << std::uppercase << std::hex << std::setfill('0') << std::setw(2) << static_cast<unsigned>(c);
    } return out.str();
}
std::set<std::string> tokens(std::string title) {
    title = lower(title); for (auto& c : title) if (!std::isalnum(static_cast<unsigned char>(c))) c=' ';
    static const std::set<std::string> stop{"will","the","a","an","to","of","in","be","on","for","by","or","at","and","is","yes","no"};
    std::set<std::string> result; std::istringstream in(title); std::string word;
    while (in >> word) if (!stop.contains(word) && word.size()>1) result.insert(word);
    return result;
}
}

Market normalize_market(std::string_view venue, const Object& raw) {
    Market m; m.venue=venue; m.raw=raw;
    if (venue == "kalshi") {
        m.id=get_text(raw,"ticker"); m.event_id=get_text(raw,"event_ticker"); m.title=get_text(raw,"title");
        m.rules=get_text(raw,"rules_primary")+"\n"+get_text(raw,"rules_secondary");
        m.close_time=get_text(raw,"close_time"); m.protocol="kalshi_fixed_point"; m.status=get_text(raw,"status");
        m.yes={VenueId::Kalshi,m.id,"YES","USD"}; m.no={VenueId::Kalshi,m.id,"NO","USD"};
        m.tradable=m.status=="active" || m.status=="open";
        m.protocol_supported=get_text(raw,"market_type")=="binary" && !m.id.starts_with("KXMVE");
        m.volume=screen_number(raw,"volume_24h_fp"); m.liquidity=screen_number(raw,"open_interest_fp");
        for (const auto& r : array_field(raw,"price_ranges")) {
            auto step=metadata_amount(r.as_object(),"step"); if (step>0) m.tick=m.tick==0 ? step : std::min(m.tick,step);
        }
        m.min_qty_micro=metadata_amount(raw,"minimum_order_size_fp");
        m.qty_step_micro=metadata_amount(raw,"quantity_step_fp");
        if (const auto* f=raw.if_contains("_fee_record"); f && f->is_object()) m.fee=f->as_object();
    } else if (venue == "polymarket_international") {
        m.id=get_text(raw,"conditionId"); m.title=get_text(raw,"question"); m.rules=get_text(raw,"description");
        m.close_time=get_text(raw,"endDate"); m.protocol=get_text(raw,"version","UNKNOWN");
        m.status=get_bool(raw,"closed") ? "closed" : (get_bool(raw,"active") ? "active" : "inactive");
        m.tradable=get_bool(raw,"active") && !get_bool(raw,"closed") && get_bool(raw,"enableOrderBook") && get_bool(raw,"acceptingOrders");
        auto outcomes=array_field(raw,"outcomes"), ids=array_field(raw,"clobTokenIds");
        std::string yes,no;
        if (outcomes.size()==2 && ids.size()==2) for (std::size_t i=0;i<2;++i) {
            auto side=lower(text_value(outcomes[i]));const bool directional=get_bool(raw,"_directional_binary_aliases"); if (side=="yes" || (directional && side=="up")) yes=text_value(ids[i]); if (side=="no" || (directional && side=="down")) no=text_value(ids[i]);
        }
        const std::regex integer("[0-9]{1,78}");
        m.protocol_supported=m.protocol=="v1" && std::regex_match(yes,integer) && std::regex_match(no,integer) && m.id.starts_with("0x") && m.id.size()==66;
        const auto collateral=get_text(raw,"collateralToken",get_text(raw,"collateralAddress","UNKNOWN_POLY_COLLATERAL"));
        m.yes={VenueId::PolymarketInternational,m.id,yes,collateral}; m.no={VenueId::PolymarketInternational,m.id,no,collateral};
        m.tick=metadata_amount(raw,"orderPriceMinTickSize"); m.min_qty_micro=metadata_amount(raw,"orderMinSize");
        // CTF-compatible books are captured with exact micro-share sizes. The
        // actual execution step must be confirmed in an approved constraint.
        m.qty_step_micro=metadata_amount(raw,"quantityStep");
        m.liquidity=screen_number(raw,"liquidityNum"); m.volume=screen_number(raw,"volume24hr");
        if (const auto events=array_field(raw,"events"); !events.empty()) m.event_id=get_text(events[0].as_object(),"id");
        if (const auto* f=raw.if_contains("feeSchedule"); f && f->is_object()) {
            m.fee=f->as_object(); m.fee["enabled"]=get_bool(raw,"feesEnabled"); m.fee["enabled_known"]=raw.contains("feesEnabled");
            m.fee["observed_wall_ns"]=get_uint(raw,"_observed_wall_ns"); m.fee["source"]="https://gamma-api.polymarket.com/markets/"+get_text(raw,"id");
        } else if (raw.contains("feesEnabled") && !get_bool(raw,"feesEnabled")) {
            m.fee={{"enabled",false},{"enabled_known",true},{"rate","0"},{"exponent",1},
                {"observed_wall_ns",get_uint(raw,"_observed_wall_ns")},{"source","https://gamma-api.polymarket.com/markets/"+get_text(raw,"id")}};
        }
        if (get_bool(raw,"negRisk")) m.rejection="NEGRISK_REQUIRES_SEPARATE_REVIEW";
    } else throw std::invalid_argument("unsupported discovery venue");
    require(!m.id.empty() && !m.title.empty(),"malformed market identity");
    if(const auto* reviewed=raw.if_contains("_reviewed_constraints");reviewed && reviewed->is_object()){
        const auto& proof=reviewed->as_object();
        if(get_text(proof,"review_status")=="APPROVED" && valid_sha256(get_text(proof,"source_sha256")) &&
            !get_text(proof,"source").empty() && get_text(proof,"market_id")==m.id && get_text(proof,"venue")==m.venue){
            m.min_qty_micro=std::max(m.min_qty_micro,metadata_amount(proof,"minimum_quantity"));
            if(m.qty_step_micro==0)m.qty_step_micro=metadata_amount(proof,"quantity_step");
            if(m.tick==0)m.tick=metadata_amount(proof,"tick");
        }
    }
    m.rules_hash=sha256(m.rules); m.category=category(m.title);
    if (!m.protocol_supported) m.rejection="UNSUPPORTED_PROTOCOL_OR_NONBINARY";
    else if (!m.tradable) m.rejection="MARKET_NOT_TRADABLE";
    else if (m.rules.size()<20) m.rejection="MISSING_FULL_RULES";
    return m;
}
Object market_json(const Market& m) {
    return {{"venue",m.venue},{"market_id",m.id},{"event_id",m.event_id},{"title",m.title},{"rules",m.rules},{"rules_hash",m.rules_hash},
        {"category",m.category},{"close_time",m.close_time},{"protocol",m.protocol},{"status",m.status},
        {"yes",key_json(m.yes)},{"no",key_json(m.no)},{"tradable",m.tradable},{"protocol_supported",m.protocol_supported},
        {"screening_rejection",m.rejection},{"tick",decimal(m.tick)},{"minimum_quantity",decimal(m.min_qty_micro)},
        {"quantity_step",decimal(m.qty_step_micro)},{"liquidity",m.liquidity},{"volume_24h",m.volume},{"fee_metadata",m.fee},{"raw",m.raw}};
}
Array match_markets(const std::vector<Market>& markets,std::size_t limit) {
    std::vector<Object> matches;
    for (const auto& a : markets) if (a.venue=="kalshi" && a.protocol_supported) {
        auto ta=tokens(a.title);
        for (const auto& b : markets) if (b.venue=="polymarket_international" && b.protocol_supported) {
            auto tb=tokens(b.title); std::vector<std::string> intersection,united;
            std::set_intersection(ta.begin(),ta.end(),tb.begin(),tb.end(),std::back_inserter(intersection));
            std::set_union(ta.begin(),ta.end(),tb.begin(),tb.end(),std::back_inserter(united));
            const double score=united.empty() ? 0 : static_cast<double>(intersection.size())/united.size();
            if (intersection.size()<2 || score<0.2) continue;
            Array common; for (const auto& word:intersection) common.emplace_back(word);
            Array flags{"RULES_NOT_REVIEWED","VOID_POSTPONEMENT_TIE_SOURCE_NOT_EQUIVALENCE_PROOF","ECONOMIC_CURRENCY_REVIEW_REQUIRED"};
            if (a.category!=b.category) flags.push_back("CATEGORY_MISMATCH");
            if (a.close_time.substr(0,10)!=b.close_time.substr(0,10)) flags.push_back("CLOSE_DATE_MISMATCH");
            if (!a.rejection.empty()) flags.emplace_back(a.rejection); if (!b.rejection.empty()) flags.emplace_back(b.rejection);
            matches.push_back({{"candidate_pair_id",sha256(field(a.id)+field(b.id))},{"kalshi_market",a.id},{"polymarket_market",b.id},
                {"kalshi_title",a.title},{"polymarket_title",b.title},{"category",a.category},{"match_score",score},
                {"common_tokens",common},{"kalshi_close",a.close_time},{"polymarket_close",b.close_time},
                {"kalshi_rules_hash",a.rules_hash},{"polymarket_rules_hash",b.rules_hash},{"risk_flags",flags},{"approval_status","PENDING_HUMAN_REVIEW"}});
        }
    }
    std::sort(matches.begin(),matches.end(),[](const auto& a,const auto& b) {
        if (a.at("match_score").as_double()!=b.at("match_score").as_double()) return a.at("match_score").as_double()>b.at("match_score").as_double();
        return get_text(a,"candidate_pair_id")<get_text(b,"candidate_pair_id");
    });
    Array out; for (std::size_t i=0;i<std::min(limit,matches.size());++i) out.push_back(matches[i]); return out;
}

Discovery discover(const Config& c,const std::filesystem::path& dir) {
    std::filesystem::create_directories(dir); Discovery d; PublicHttpClient http;
    std::ofstream requests(dir/"metadata_requests.jsonl",std::ios::app); requests.exceptions(std::ios::badbit|std::ios::failbit);
    unsigned request_id=0;
    auto request=[&](const std::string& url)->std::optional<json::value> {
        for (int attempt=0;attempt<3;++attempt) {
            const auto r=http.get(url); const auto filename="metadata_response."+std::to_string(request_id++)+".json";
            std::ofstream raw(dir/filename,std::ios::binary); raw.exceptions(std::ios::badbit|std::ios::failbit); raw<<r.body; raw.close();
            Object event{{"url",url},{"status",r.status},{"error",r.error},{"received_wall_ns",r.received_ns},{"received_utc",utc(r.received_ns)},
                {"raw_file",filename},{"sha256",sha256(r.body)},{"attempt",attempt}};
            d.requests.push_back(event); requests<<json::serialize(event)<<'\n';requests.flush();
            if (r.status==200) { try { return json::parse(r.body); } catch (...) { return {}; } }
            if (r.status!=429 && r.status!=408 && r.status<500 && r.status!=0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(250*(1<<attempt)));
        } return {};
    };
    std::set<std::string> ids;
    auto append=[&](std::string_view venue,const Array& rows) {
        for (const auto& row:rows) { try {
            auto raw=row.as_object();raw["_observed_wall_ns"]=wall_ns();if(venue=="polymarket_international" && get_bool(c.original,"allow_directional_binary_outcomes"))raw["_directional_binary_aliases"]=true; auto m=normalize_market(venue,raw);
            if (ids.insert(field(venue)+field(m.id)).second) d.markets.push_back(std::move(m));
        } catch (const std::exception& e) { d.requests.push_back(Object{{"venue",venue},{"rejection","MALFORMED_METADATA"},{"error",e.what()}}); } }
    };
    // Reviewed research targets must not disappear merely because they fell
    // outside a bounded general discovery page. This fetch does not approve.
    if(const auto* seeds=c.original.if_contains("research_seed_markets");seeds && seeds->is_array()){
        require(seeds->as_array().size()<=100,"too many bounded research seeds");
        for(const auto& seed:seeds->as_array()){
            const auto& o=seed.as_object();const auto venue=get_text(o,"venue"),url=get_text(o,"metadata_url");
            require((venue=="kalshi" && url.starts_with(c.kalshi_rest+"/markets/")) ||
                (venue=="polymarket_international" && url.starts_with(c.gamma_rest+"/markets/")),"seed must reference official market metadata");
            if(const auto value=request(url);value && value->is_object()){
                const auto& raw=value->as_object();append(venue,Array{venue=="kalshi"?raw.at("market"):*value});
            }
        }
    }
    // Optional bounded structured strata; all original discovery behavior remains default.
    if(const auto* queries=c.original.if_contains("research_seed_queries");queries && queries->is_array()){
        require(queries->as_array().size()<=24,"too many structured discovery strata");
        for(const auto& v:queries->as_array()){
            const auto& q=v.as_object();const auto venue=get_text(q,"venue"),filter=get_text(q,"query");
            require((venue=="kalshi" || venue=="polymarket_international") && filter.find_first_of("#\r\n")==std::string::npos,"invalid discovery stratum");
            const auto pages=get_uint(q,"max_pages",2);require(pages>0 && pages<=10,"stratum page bound");
            std::string next;std::set<std::string> seen;std::size_t offset=0;bool complete=false;
            for(std::uint64_t page=0;page<pages;++page){const auto base=venue=="kalshi"?c.kalshi_rest:c.gamma_rest;
                const auto url=base+"/markets?limit=100&"+filter+(venue=="kalshi"?(next.empty()?"":"&cursor="+encode(next)):"&offset="+std::to_string(offset));
                auto value=request(url);if(!value)break;
                const auto& rows=venue=="kalshi"?value->as_object().at("markets").as_array():value->as_array();append(venue,rows);offset+=rows.size();
                next=venue=="kalshi"?get_text(value->as_object(),"cursor"):"";
                if((venue=="kalshi" && next.empty()) || (venue!="kalshi" && rows.size()<100)){complete=true;break;}
                if(venue=="kalshi" && !seen.insert(next).second)break;
            }
            d.requests.push_back(Object{{"stratum",filter},{"venue",venue},{"markets_consumed",offset},{"pagination_exhausted",complete},{"page_limit",pages},{"omitted_remaining",complete?json::value(0):json::value(nullptr)}});
        }
    }
    std::string cursor; std::set<std::string> cursors;
    const auto general_limit=std::max<std::size_t>(c.page_size,c.discovery_limit/2);
    std::size_t count=0; d.complete["kalshi"]=false;
    do {
        auto value=request(c.kalshi_rest+"/markets?status=open&mve_filter=exclude&limit="+std::to_string(c.page_size)+(cursor.empty()?"":"&cursor="+encode(cursor)));
        if (!value || !value->is_object()) break; const auto& o=value->as_object();
        const auto* rows=o.if_contains("markets"); if (!rows || !rows->is_array()) break;
        append("kalshi",rows->as_array());count+=rows->as_array().size();cursor=get_text(o,"cursor");
        if (cursor.empty()) {d.complete["kalshi"]=true;break;}
        if (!cursors.insert(cursor).second) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    } while (count<general_limit);
    // Diversify by categories discovered from the official series catalogue,
    // rather than letting newest sports/combination markets monopolize intake.
    if (auto series=request(c.kalshi_rest+"/series?include_volume=true"); series && series->is_object()) {
        if (auto* list=series->as_object().if_contains("series");list && list->is_array()) {
            std::map<std::string,std::vector<Object>> categories;
            for (const auto& v:list->as_array()) if (v.is_object()) categories[get_text(v.as_object(),"category","other")].push_back(v.as_object());
            const auto remaining=c.discovery_limit>count?c.discovery_limit-count:0;
            const auto category_page=std::min<std::size_t>(c.page_size,std::max<std::size_t>(10,remaining/std::max<std::size_t>(1,categories.size())/2));
            for (auto& [cat,values]:categories) {
                std::sort(values.begin(),values.end(),[](const auto& a,const auto& b){return screen_number(a,"volume_fp")>screen_number(b,"volume_fp");});
                unsigned chosen=0;
                for (const auto& s:values) {
                    if (chosen>=2 || count>=c.discovery_limit) break;
                    const auto ticker=get_text(s,"ticker"); if (ticker.empty() || ticker.starts_with("KXMVE")) continue;
                    auto value=request(c.kalshi_rest+"/markets?status=open&limit="+std::to_string(category_page)+"&series_ticker="+encode(ticker));
                    ++chosen;
                    if (value && value->is_object()) if (auto* rows=value->as_object().if_contains("markets");rows&&rows->is_array()) {
                        append("kalshi",rows->as_array()); count+=rows->as_array().size();
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(150));
                }
            }
        }
    }
    d.complete["polymarket_international"]=false;
    // Gamma can cap a response at 100 even when a larger limit was requested.
    // Use a supported bounded page and advance by the actual consumed count.
    const auto gamma_page=std::min<std::size_t>(100,c.page_size);
    for (std::size_t offset=0;offset<c.discovery_limit;) {
        auto value=request(c.gamma_rest+"/markets?active=true&closed=false&limit="+std::to_string(gamma_page)+"&offset="+std::to_string(offset)+"&order=liquidityNum&ascending=false");
        if (!value || !value->is_array()) break;
        append("polymarket_international",value->as_array());
        if (value->as_array().size()<gamma_page) { d.complete["polymarket_international"]=true; break; }
        offset+=value->as_array().size();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    // Explicit reviewed, source-pinned constraints/fees supplement fields not
    // exposed by a venue API. Missing records stay unknown and fail closed.
    for(auto& m:d.markets){auto raw=m.raw;
        if(const auto* list=c.original.if_contains("reviewed_instrument_constraints");list && list->is_array())for(const auto& v:list->as_array())
            if(get_text(v.as_object(),"venue")==m.venue && get_text(v.as_object(),"market_id")==m.id)raw["_reviewed_constraints"]=v;
        if(const auto* list=c.original.if_contains("reviewed_fee_schedules");list && list->is_array())for(const auto& v:list->as_array())
            if(get_text(v.as_object(),"venue")==m.venue && get_text(v.as_object(),"market_id")==m.id && get_text(v.as_object(),"review_status")=="APPROVED")raw["_fee_record"]=v;
        m=normalize_market(m.venue,raw);
    }
    if(!get_bool(c.original,"structured_discovery_only"))d.candidates=match_markets(d.markets,c.max_pairs); Array markets;
    for (const auto& m:d.markets) markets.push_back(market_json(m));
    write_json(dir/"market_metadata.json",Object{{"schema","cross-venue-metadata-v0.1"},{"evidence_level","REAL_PUBLIC_METADATA"},{"markets",markets}});
    write_json(dir/"candidate_pairs.json",d.candidates);
    write_table(dir/"market_discovery.csv",{"venue","market_id","event_id","title","category","close_time","protocol","status","protocol_supported","screening_rejection","volume_24h","liquidity","rules_hash"},markets);
    write_table(dir/"candidate_pairs.csv",{"candidate_pair_id","kalshi_market","polymarket_market","kalshi_title","polymarket_title","category","match_score","common_tokens","risk_flags","approval_status","kalshi_rules_hash","polymarket_rules_hash"},d.candidates);
    return d;
}

std::vector<ApprovedPair> load_approved_pairs(const Config& c,const std::vector<Market>& markets,Array& rejected) {
    std::vector<ApprovedPair> out;
    if (!std::filesystem::exists(c.approvals)) {rejected.push_back(Object{{"reason","APPROVAL_FILE_ABSENT"}});return out;}
    auto file=read_object(c.approvals); require(get_text(file,"schema")=="cross-venue-approvals-v0.1","invalid approvals schema");
    for (const auto& v:file.at("pairs").as_array()) try {
        const auto& o=v.as_object(); auto pair=parse_pair(o,false);require(!pair.synthetic,"synthetic approval cannot enter real capture");
        const auto& economic=o.at("economic_review").as_object();
        require(get_text(economic,"status")=="APPROVED" && !get_text(economic,"reviewer").empty(),"missing economic/collateral review");
        if(get_bool(c.original,"require_explicit_revision_review")){
            const auto manifest_path=get_text(c.original,"human_review_manifest");
            require(!manifest_path.empty(),"missing explicit human review manifest");
            const auto review_manifest=read_object(manifest_path);
            require(get_text(review_manifest,"schema")=="cross-venue-human-review-manifest-v0.1.1","invalid human review manifest");
            bool explicit_approval=false;
            for(const auto& entry:review_manifest.at("reviews").as_array()){
                const auto& review=entry.as_object();
                explicit_approval |= get_text(review,"candidate_pair_id")==pair.pair_id &&
                    get_text(review,"status")=="APPROVED" && get_text(review,"decision")=="APPROVED" &&
                    !get_text(review,"reviewer").empty() && !get_text(review,"reviewed_at").empty() &&
                    get_uint(review,"approved_revision")==pair.approval_revision &&
                    get_text(review,"approved_artifact_hash")==pair.artifact_hash && valid_sha256(get_text(review,"dossier_sha256")) &&
                    get_bool(review,"coverage_proven") && get_bool(review,"settlement_assets_reviewed") && get_bool(review,"quantity_constraints_reviewed");
            }
            require(explicit_approval,"no explicit human approval bound to this exact revision/artifact");
        }
        for (std::size_t i=0;i<2;++i) {
            const auto& k=i==0?pair.leg_yes:pair.leg_no;
            const auto it=std::find_if(markets.begin(),markets.end(),[&](const auto& m){return (m.yes==k || m.no==k);});
            require(it!=markets.end() && it->protocol_supported && it->tradable && it->rejection.empty(),"approved market unsupported/unavailable");
            require(it->rules_hash==(i==0?pair.rules_hash_yes:pair.rules_hash_no),"rule metadata hash changed");
            require(it->min_qty_micro>0 && it->qty_step_micro>0 && it->tick>0,"unverified size/tick constraints");
        }
        out.push_back(std::move(pair));
    } catch (const std::exception& e) { rejected.push_back(Object{{"pair_id",v.is_object()?get_text(v.as_object(),"pair_id"):""},{"reason",e.what()}}); }
    return out;
}

FeeSchedule fee_schedule(const Market& m) {
    FeeSchedule s;s.market_id=m.id;s.source=get_text(m.fee,"source");s.version=sha256(json::serialize(m.fee));
    s.observed_wall_ns=get_uint(m.fee,"observed_wall_ns");s.expires_wall_ns=s.observed_wall_ns+3600ULL*1'000'000'000;
    if (m.venue=="polymarket_international" && get_bool(m.fee,"enabled_known") && get_uint(m.fee,"exponent",1)==1) {
        s.type="quadratic";s.rate=get_bool(m.fee,"enabled")?metadata_amount(m.fee,"rate",-1):0;
        s.valid=!s.source.empty() && s.observed_wall_ns>0 && s.rate>=0 && s.rate<=unit;
    } else if (m.venue=="kalshi" && get_text(m.fee,"formula")=="quadratic" && get_text(m.fee,"role")=="taker") {
        s.type="quadratic_cent_ceiling";s.rate=metadata_amount(m.fee,"coefficient",-1);
        s.valid=valid_sha256(get_text(m.fee,"schedule_sha256")) && !s.source.empty() && s.rate>=0 && s.rate<=unit && s.observed_wall_ns>0;
    }
    return s;
}
void enrich_selected_fees(const Config& c,std::vector<Market>& markets,const std::filesystem::path& directory){
    const auto model_path=std::filesystem::path(get_text(c.original,"fee_models","config/cross_venue_fee_models.json"));
    if(!std::filesystem::exists(model_path))return;
    const auto models=read_object(model_path);write_json(directory/"fee_models.frozen.json",models);
    const auto& model=models.at("kalshi").as_object();require(get_text(models,"schema")=="cross-venue-public-fee-models-v0.1","invalid fee model schema");
    PublicHttpClient http;std::map<std::string,std::optional<Object>> cache;Array audit;
    auto fetch=[&](const std::string& url)->std::optional<Object>{
        if(cache.contains(url))return cache.at(url);const auto response=http.get(url);const auto file="fee_response."+std::to_string(cache.size())+".json";
        std::ofstream body(directory/file,std::ios::binary);body.exceptions(std::ios::badbit|std::ios::failbit);body<<response.body;body.close();
        audit.push_back(Object{{"source",url},{"http_status",response.status},{"error",response.error},{"received_wall_ns",response.received_ns},{"raw_file",file},{"sha256",sha256(response.body)}});
        std::optional<Object> object;if(response.status==200)try{object=json::parse(response.body).as_object();}catch(...){}
        cache[url]=object;return object;
    };
    for(auto& market:markets)if(market.venue=="kalshi" && !market.id.starts_with("KXMVE")){
        const auto event_url=c.kalshi_rest+"/events/"+market.event_id;const auto event=fetch(event_url);if(!event)continue;
        const auto* event_value=event->if_contains("event");if(!event_value || !event_value->is_object())continue;
        const auto& info=event_value->as_object();const auto series_id=get_text(info,"series_ticker");if(series_id.empty())continue;
        const auto series_url=c.kalshi_rest+"/series/"+series_id;const auto series=fetch(series_url);if(!series)continue;
        const auto* series_value=series->if_contains("series");if(!series_value || !series_value->is_object())continue;
        const auto& schedule=series_value->as_object();
        const auto type=get_text(info,"fee_type_override",get_text(schedule,"fee_type"));
        const auto multiplier=info.contains("fee_multiplier_override") && !info.at("fee_multiplier_override").is_null()
            ?metadata_amount(info,"fee_multiplier_override",-1):metadata_amount(schedule,"fee_multiplier",-1);
        if((type!="quadratic" && type!="quadratic_with_maker_fees") || multiplier<0)continue;
        const auto base=money(get_text(model,"general_taker_coefficient"));
        const auto coefficient=static_cast<Money>((static_cast<__int128>(base)*multiplier+unit-1)/unit);
        Object record{{"formula","quadratic"},{"coefficient",decimal(coefficient)},{"role","taker"},{"market_id",market.id},
            {"source",series_url+" ; "+event_url+" ; "+get_text(model,"source")},{"observed_wall_ns",wall_ns()},
            {"model_effective_date",get_text(model,"effective_date")},{"rounding_assumption",get_text(model,"rounding_assumption")},
            {"schedule_sha256",sha256(json::serialize(models)+json::serialize(*series)+json::serialize(*event))},
            {"series_fee_type",type},{"series_fee_multiplier",decimal(multiplier)}};
        market.raw["_fee_record"]=record;market=normalize_market(market.venue,market.raw);
    }
    write_json(directory/"fee_source_audit.json",audit);
}
Money estimate_fee_micro(const FeeSchedule& s,const VersionedBook& b,std::int64_t q,std::uint64_t at) {
    require(s.valid && at>=s.observed_wall_ns && at<s.expires_wall_ns && s.role=="taker" && s.market_id==b.key.market_id,"INVALID_FEE_SCHEDULE");
    require(q>0 && q<=multiply(max_quantity,unit),"bad fee quantity");std::int64_t remaining=q;Money fee=0,fragment_notional_ceilings=0;__int128 exact_notional_numerator=0;
    for (std::uint16_t i=0;i<b.depth.ask_count && remaining>0;++i) {
        const auto available=b.ask_qty_micro[i]; const auto take=std::min(remaining,available);const auto p=b.depth.asks[i].price_tick;
        const __int128 numerator=static_cast<__int128>(take)*s.rate*p*(unit-p);
        const auto denominator=static_cast<__int128>(unit)*unit*unit;
        const auto part=static_cast<Money>((numerator+denominator-1)/denominator);
        // Per-level/order-fragment upward rounding is conservative when the
        // matching engine's actual grouping of fees is unavailable.
        const Money quantum=s.type=="quadratic_cent_ceiling"?10000:10;
        if(s.type=="quadratic_cent_ceiling"){
            const auto notional=static_cast<Money>((static_cast<__int128>(take)*p+unit-1)/unit);
            exact_notional_numerator+=static_cast<__int128>(take)*p;fragment_notional_ceilings=add(fragment_notional_ceilings,notional);
            fee=add(fee,((add(notional,part)+quantum-1)/quantum)*quantum-notional);
        }else fee=add(fee,((part+quantum-1)/quantum)*quantum);
        remaining-=take;
    }
    require(remaining==0,"fee estimate lacks depth");
    if(s.type=="quadratic_cent_ceiling"){const auto total_notional_ceiling=static_cast<Money>((exact_notional_numerator+unit-1)/unit);fee=add(fee,fragment_notional_ceilings-total_notional_ceiling);}
    return fee;
}
Money estimate_fee(const FeeSchedule& s,const VersionedBook& b,std::int64_t q,std::uint64_t at) { return estimate_fee_micro(s,b,multiply(q,unit),at); }
bool KalshiStreamSequence::accept(const Object& p) {
    const auto type=get_text(p,"type");if(type!="orderbook_snapshot" && type!="orderbook_delta")return true;
    const auto sid=get_uint(p,"sid"),seq=get_uint(p,"seq");if(!sid||!seq)return false;
    auto it=sequences_.find(sid);
    if(it==sequences_.end()) {if(type!="orderbook_snapshot")return false;sequences_[sid]=seq;return true;}
    if(it->second==std::numeric_limits<std::uint64_t>::max() || seq!=it->second+1)return false;
    it->second=seq;return true;
}
}  // namespace trading_engine::cross_venue::research
