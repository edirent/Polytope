#include "engine/strategy/cross_venue/LiveResearch.h"
#include "engine/decode/parser/kalshi/KalshiBookNormalizer.h"
#include "engine/decode/parser/polymarket/CrossVenueClobNormalizer.h"
#include "engine/common/math/FractionalVwap.h"
#include "engine/execution/adapter/CrossVenueShadowAdapter.h"
#include "engine/risk/ledger/MultiVenueReservationBook.h"
#include <fstream>
#include <iostream>
#include <openssl/pem.h>
#include <openssl/rsa.h>

namespace cv=trading_engine::cross_venue;
namespace r=cv::research;
namespace {
unsigned checks=0;
void check(bool b,const char* message){if(!b)throw std::runtime_error(message);++checks;}
template<class F>void rejects(F f,const char* message){bool rejected=false;try{f();}catch(...){rejected=true;}check(rejected,message);}
r::Object configuration(){return {{"schema","cross-venue-research-v0.1"},{"mode","read_only_shadow"},{"capital","200000"},
    {"venue_allocations",r::Array{"100000","100000"}},{"slippage_per_share","0.001"},{"execution_risk_per_share","0.001"}};}
r::Object kalshi_metadata(){return {{"ticker","LAB-K"},{"event_ticker","LAB-E"},{"title","Will the synthetic lab event win?"},
    {"rules_primary","SYNTHETIC regression: complementary binary event; not human reviewed for trading"},{"rules_secondary",""},{"status","open"},{"market_type","binary"},
    {"price_ranges",r::Array{r::Object{{"start","0"},{"end","1"},{"step","0.01"}}}},{"minimum_order_size_fp","1"},{"quantity_step_fp","0.01"},
    {"_fee_record",r::Object{{"formula","quadratic"},{"coefficient","0"},{"role","taker"},{"source","SYNTHETIC_FEE_SOURCE"},
        {"schedule_sha256",cv::sha256("LAB-FEES")},{"observed_wall_ns",1000000000000ULL}}}};}
r::Object poly_metadata(){return {{"id","LAB-P"},{"conditionId","0x"+std::string(64,'a')},{"question","Will the synthetic lab event win?"},
    {"description","SYNTHETIC regression: complementary binary event; not human reviewed for trading"},{"version","v1"},
    {"outcomes",r::Array{"Yes","No"}},{"clobTokenIds",r::Array{"111","222"}},{"active",true},{"closed",false},{"enableOrderBook",true},
    {"acceptingOrders",true},{"collateralToken","USD"},{"orderMinSize","1"},{"quantityStep","0.01"},{"orderPriceMinTickSize","0.01"},
    {"feesEnabled",true},{"feeSchedule",r::Object{{"rate","0"},{"exponent",1}}},{"_observed_wall_ns",1000000000000ULL}};}
cv::ApprovedPair pair_for(const r::Market& k,const r::Market& p){cv::ApprovedPair a;a.pair_id="LAB_PAIR";a.leg_yes=k.yes;a.leg_no=p.no;
    a.rules_hash_yes=k.rules_hash;a.rules_hash_no=p.rules_hash;a.review_id="SYNTHETIC_REGRESSION_ONLY";a.approval_revision=1;a.enabled=true;a.synthetic=true;
    a.payoffs={{"YES",cv::unit,0},{"NO",0,cv::unit}};a.artifact_hash=cv::pair_hash(a);return a;}
r::Object pair_json(const cv::ApprovedPair& p){return {{"pair_id",p.pair_id},{"leg_yes",r::key_json(p.leg_yes)},{"leg_no",r::key_json(p.leg_no)},
    {"rules_hash_yes",p.rules_hash_yes},{"rules_hash_no",p.rules_hash_no},{"review_id",p.review_id},{"approval_revision",p.approval_revision},
    {"enabled",true},{"synthetic",true},{"payoffs",r::Array{r::Object{{"name","YES"},{"yes","1"},{"no","0"}},r::Object{{"name","NO"},{"yes","0"},{"no","1"}}}},{"artifact_hash",p.artifact_hash}};}
std::string kalshi_snapshot(std::string ticker="LAB-K",unsigned seq=1){return r::json::serialize(r::Object{{"type","orderbook_snapshot"},{"sid",2},{"seq",seq},
    {"msg",r::Object{{"market_ticker",ticker},{"clock",45},{"yes_dollars_fp",r::Array{r::Array{"0.45","100"}}},{"no_dollars_fp",r::Array{r::Array{"0.53","100"}}}}}});}
std::string poly_snapshot(){
    r::Object book{{"event_type","book"},{"market","0x"+std::string(64,'a')},{"asset_id","222"}};
    book["bids"]=r::Array{r::Object{{"price","0.46"},{"size","100"}}};
    book["asks"]=r::Array{r::Object{{"price","0.48"},{"size","100"}}};return r::json::serialize(book);
}
std::filesystem::path make_lab_capture(const std::filesystem::path& root,bool rest_only=false,bool disconnect=false,bool second_opportunity=false,bool reconnect=false){
    std::filesystem::create_directories(root);r::write_json(root/"capture_config.json",configuration());
    const auto k=r::normalize_market("kalshi",kalshi_metadata()),p=r::normalize_market("polymarket_international",poly_metadata());
    const auto pair=pair_for(k,p);r::write_json(root/"selected_markets.json",r::Array{r::market_json(k),r::market_json(p)});
    r::write_json(root/"approved_pairs.json",r::Object{{"pairs",r::Array{pair_json(pair)}}});
    r::RawRecorder recorder(root,16384,1000000);
    std::uint64_t epoch=1;
    auto append=[&](std::string venue,std::string transport,std::string classification,std::string raw,std::uint64_t t,const cv::VenueInstrumentKey* key=nullptr){
        r::Object record{{"schema","cross-venue-raw-v0.1"},{"evidence_level","SYNTHETIC_REGRESSION"},{"venue",venue},{"transport",transport},
            {"classification",classification},{"raw_payload",raw},{"recv_monotonic_ns",t},{"availability_monotonic_ns",t},
            {"recv_wall_ns",1000000000000ULL+t},{"connection_epoch",epoch},{"arrival_sequence",t},{"http_status",transport=="rest"?200:101}};
        if(key)record["key"]=r::key_json(*key);recorder.append(venue,std::move(record));
    };
    if(rest_only){
        append("kalshi","rest","snapshot",R"({"orderbook_fp":{"yes_dollars":[["0.45","100"]],"no_dollars":[["0.53","100"]]}})",1000000000,&k.yes);
        append("polymarket_international","rest","snapshot",poly_snapshot(),1010000000,&p.no);
    }else{
        append("kalshi","ws","market_data",kalshi_snapshot(),1000000000);
        append("polymarket_international","ws","market_data",poly_snapshot(),1010000000);
    }
    if(disconnect)append("kalshi","event","disconnect","TEST_CONNECTION_FAILURE",1050000000);
    append("kalshi","ws","heartbeat","PONG",1100000000);
    append("polymarket_international","ws","heartbeat","PONG",1110000000);
    if(!rest_only && !disconnect){
        const auto changes=r::json::serialize(r::Object{{"event_type","price_change"},{"market",p.id},{"price_changes",r::Array{
            r::Object{{"asset_id","222"},{"price","0.48"},{"size","0"},{"side","SELL"}},
            r::Object{{"asset_id","222"},{"price",second_opportunity?"0.47":"0.60"},{"size","100"},{"side","SELL"}}}}});
        append("polymarket_international","ws","market_data",changes,1400000000);
        if(second_opportunity)append("kalshi","ws","market_data",R"({"type":"orderbook_delta","sid":2,"seq":2,"msg":{"market_ticker":"LAB-K","clock":46,"side":"no","price_dollars":"0.54","delta_fp":"100"}})",1410000000);
    }
    append("kalshi","ws","heartbeat","PONG",2500000000);
    append("polymarket_international","ws","heartbeat","PONG",2510000000);
    if(reconnect){
        append("kalshi","event","validation_reconnect","SYNTHETIC_CONTROLLED_RECOVERY",2600000000);
        append("kalshi","event","disconnect","SYNTHETIC_CONTROLLED_DISCONNECT",2610000000);epoch=2;
        append("kalshi","event","connection_open","SYNTHETIC_SECOND_SESSION",2700000000);
        append("kalshi","ws","market_data",kalshi_snapshot("LAB-K",1),2710000000);
        append("kalshi","ws","market_data",R"({"type":"orderbook_delta","sid":2,"seq":2,"msg":{"market_ticker":"LAB-K","clock":46,"side":"no","price_dollars":"0.53","delta_fp":"1"}})",2720000000);
    }
    r::write_json(root/"capture_manifest.json",r::Object{{"schema","cross-venue-capture-v0.1"},{"evidence_level","SYNTHETIC_REGRESSION"},
        {"config_sha256",cv::sha256(r::json::serialize(configuration()))},{"raw_inventory",recorder.inventory()},{"end_monotonic_ns",3000000000ULL},
        {"market_counts",r::Object{{"kalshi",1},{"polymarket_international",1}}},{"candidate_pairs",1},{"approved_pairs",1},
        {"actual_duration_sec",2},{"start_utc","SYNTHETIC_START"},{"end_utc","SYNTHETIC_END"}});
    return root;
}
void signature_test(int type){
    EVP_PKEY_CTX* generator=EVP_PKEY_CTX_new_id(type,nullptr);check(generator!=nullptr,"test key generator");
    check(EVP_PKEY_keygen_init(generator)==1,"test key init");if(type==EVP_PKEY_RSA)check(EVP_PKEY_CTX_set_rsa_keygen_bits(generator,2048)>0,"RSA size");
    EVP_PKEY* key=nullptr;check(EVP_PKEY_keygen(generator,&key)==1,"ephemeral test key");EVP_PKEY_CTX_free(generator);
    BIO* pem=BIO_new(BIO_s_mem());check(PEM_write_bio_PrivateKey(pem,key,nullptr,nullptr,0,nullptr,nullptr)==1,"ephemeral PEM");
    char* bytes=nullptr;const auto length=BIO_get_mem_data(pem,&bytes);const std::string text(bytes,length),message="1793859000000GET/trade-api/ws/v2";
    const auto signed_text=r::kalshi_signature(text,message);BIO_free(pem);
    std::vector<unsigned char> signature(signed_text.size());int size=EVP_DecodeBlock(signature.data(),reinterpret_cast<const unsigned char*>(signed_text.data()),signed_text.size());
    for(auto i=signed_text.rbegin();i!=signed_text.rend() && *i=='=';++i)--size;
    EVP_MD_CTX* ctx=EVP_MD_CTX_new();EVP_PKEY_CTX* verify=nullptr;
    check(EVP_DigestVerifyInit(ctx,&verify,type==EVP_PKEY_ED25519?nullptr:EVP_sha256(),nullptr,key)==1,"handshake verify init");
    if(type==EVP_PKEY_RSA){check(EVP_PKEY_CTX_set_rsa_padding(verify,RSA_PKCS1_PSS_PADDING)>0,"RSA PSS padding");check(EVP_PKEY_CTX_set_rsa_pss_saltlen(verify,RSA_PSS_SALTLEN_DIGEST)>0,"RSA PSS salt");}
    check(EVP_DigestVerify(ctx,signature.data(),size,reinterpret_cast<const unsigned char*>(message.data()),message.size())==1,"signed handshake verifies; no credential persisted");
    EVP_MD_CTX_free(ctx);EVP_PKEY_free(key);
}
}
int main(int argc,char** argv){
    const bool keep=argc==3 && std::string(argv[1])=="--keep-output";
    const auto root=keep?std::filesystem::path(argv[2]):std::filesystem::temp_directory_path()/("polytope-v0.1-tests-"+std::to_string(r::mono_ns()));
    try{
        check(!std::filesystem::exists(root),"isolated test output directory");
        std::filesystem::create_directories(root);r::write_json(root/"config.json",configuration());auto config=r::load_config(root/"config.json");
        const auto k=r::normalize_market("kalshi",kalshi_metadata()),p=r::normalize_market("polymarket_international",poly_metadata());
        const auto pair=pair_for(k,p);check(p.protocol_supported && k.protocol_supported,"verified CTF-compatible subset");
        auto unsupported=poly_metadata();unsupported["version"]="v2";check(!r::normalize_market("polymarket_international",unsupported).protocol_supported,"protocol mismatch rejected");
        const auto matches=r::match_markets({k,p},10);check(matches.size()==1 && r::get_text(matches[0].as_object(),"approval_status")=="PENDING_HUMAN_REVIEW","title matching never approves rules");
        r::KalshiStreamSequence sequence;
        check(sequence.accept(r::json::parse(kalshi_snapshot("LAB-K",10)).as_object()),"subscription starts with snapshot");
        check(sequence.accept(r::json::parse(kalshi_snapshot("LAB-OTHER",11)).as_object()),"second market shares stream sequence");
        auto delta=r::json::parse(R"({"type":"orderbook_delta","sid":2,"seq":12,"msg":{"market_ticker":"LAB-K","clock":45,"side":"no","price_dollars":"0.53","delta_fp":"-0.50"}})").as_object();
        check(sequence.accept(delta),"stream seq includes messages for other markets");
        auto gap=delta;gap["seq"]=14;check(!sequence.accept(gap),"stream gap rejected");
        cv::KalshiBookNormalizer decoder(k.yes,true,true);cv::BookProvenance stamp;stamp.recv_monotonic_ns=1;stamp.last_liveness_ns=1;stamp.authoritative_snapshot=true;
        cv::SynchronizedBookStore books;check(books.apply(decoder.websocket(kalshi_snapshot("LAB-K",10),stamp,true)),"Kalshi snapshot");
        stamp.recv_monotonic_ns=2;stamp.last_liveness_ns=2;stamp.authoritative_snapshot=false;
        check(books.apply(decoder.websocket(r::json::serialize(delta),stamp,true)),"routed market parser need not see global contiguous seq");
        check(books.read(k.yes).ask_qty_micro[0]==99500000 && books.read(k.yes).evidence.exchange_clock==45,"exact fractional size and exchange clock retained");
        auto regressed=delta;regressed["seq"]=13;regressed.at("msg").as_object()["clock"]=44;
        stamp.recv_monotonic_ns=3;
        check(!books.apply(decoder.websocket(r::json::serialize(regressed),stamp,true)),"same-session per-market exchange clock regression invalidates reconstruction");
        stamp.recv_monotonic_ns=4;stamp.authoritative_snapshot=true;
        check(books.apply(decoder.websocket(kalshi_snapshot("LAB-K",20),stamp,true)),"fresh subscription snapshot recovers after a clock regression");
        cv::BookUpdate fractional;fractional.key=p.no;fractional.fractional_quantities=true;
        fractional.evidence.recv_monotonic_ns=10;fractional.evidence.sequence=1;fractional.evidence.raw_hash=cv::sha256("fractional");
        fractional.bids={{cv::money("0.40"),1000000}};fractional.asks={{cv::money("0.47"),400000},{cv::money("0.48"),600000}};
        check(books.apply(fractional),"fractional snapshot");
        check(trading_engine::common::math::buy_vwap_exact(books.read(p.no),1).total_cost_tick==cv::money("0.476"),"walk fractional depth without float money");
        auto dense=fractional;dense.evidence.recv_monotonic_ns=11;dense.bounded_prefix=true;dense.asks.clear();
        for(int i=0;i<144;++i)dense.asks.emplace_back(cv::money("0.45")+i*1000,1000000);
        check(books.apply(dense) && books.read(p.no).depth.ask_count==64 && books.read(p.no).bounded_prefix,"full recorded snapshot exposes an explicit bounded prefix");
        check(!trading_engine::common::math::buy_vwap_exact(books.read(p.no),65).ok,"bounded view never invents hidden depth");
        cv::SynchronizedBookStore retained(true);check(retained.apply(dense),"V0.1.1 retains actual observed deep levels in existing store");
        cv::BookUpdate remove;remove.key=p.no;remove.kind=cv::BookUpdate::Kind::Delta;remove.fractional_quantities=true;remove.bounded_prefix=true;
        remove.evidence=dense.evidence;remove.evidence.recv_monotonic_ns=12;remove.evidence.sequence=2;remove.asks={{cv::money("0.45"),0}};
        check(retained.apply(remove) && retained.read(p.no).depth.ask_count==64 &&
            retained.read(p.no).depth.asks[63].price_tick==cv::money("0.514"),"deletion promotes actually observed 65th level rather than losing tail");
        check(!trading_engine::common::math::buy_vwap_exact(retained.read(p.no),65).ok,"execution view remains bounded after retention fix");
        {
            auto safe=config;safe.original["reconciliation_policy"]="quiet_request_v0.1.1";
            r::ResearchState state(safe,{k,p},{});
            auto event=[&](std::string transport,std::string raw,std::uint64_t time,std::uint64_t started){
                return r::Object{{"venue","polymarket_international"},{"transport",transport},{"classification","market_data"},
                    {"raw_payload",raw},{"raw_hash",cv::sha256(raw)},{"recv_monotonic_ns",time},{"recv_wall_ns",time},
                    {"http_status",200},{"connection_epoch",1},{"request_started_monotonic_ns",started},{"key",r::key_json(p.no)}};};
            check(state.process(event("ws",poly_snapshot(),100,0)),"new reconciliation scenario seeds real-shaped WS snapshot");
            auto different=r::json::parse(poly_snapshot()).as_object();different.at("asks").as_array()[0].as_object()["size"]="99";
            check(state.process(event("rest",r::json::serialize(different),110,90)) && state.books().read(p.no).depth.usable_for_depth,
                "REST request overlapping a WS update is inconclusive, not a disconnect");
            check(state.books().read(p.no).evidence.last_authoritative_snapshot_ns==100,"inconclusive comparison never extends authoritative validity");
            auto diagnostic=state.take_diagnostics();check(diagnostic.size()==1 && r::get_text(diagnostic[0].as_object(),"reason")=="RECONCILIATION_INCONCLUSIVE","concurrent comparison has explicit evidence");
            check(!state.process(event("rest",r::json::serialize(different),120,115)) && !state.books().read(p.no).depth.usable_for_depth,
                "quiet-window genuine mismatch still fails closed");
            auto error=event("ws",R"({"type":"error","msg":{"code":9,"msg":"synthetic subscription refusal"}})",130,0);error["venue"]="kalshi";
            check(!state.process(error),"Kalshi subscription errors cannot masquerade as healthy no-data sessions");
        }
        cv::PairPolicy policy;policy.max_authoritative_age_ns=3000000000;policy.max_liveness_age_ns=1000000000;policy.max_skew_ns=100000000;
        cv::SynchronizedBookStore quiet;auto q=fractional;q.key=k.yes;q.evidence.recv_monotonic_ns=1000000000;q.evidence.last_liveness_ns=1000000000;
        q.evidence.authoritative_snapshot=true;quiet.apply(q);q.key=p.no;quiet.apply(q);
        quiet.note_liveness(k.yes,2000000000);quiet.note_liveness(p.no,2000000000);
        cv::validate_books(pair,quiet.read_pair(pair),policy,2000000000);check(true,"inactive market can remain healthy with recent authority and liveness");
        rejects([&]{cv::validate_books(pair,quiet.read_pair(pair),policy,4000000000);},"connection health expires independently");
        cv::CrossVenueClobNormalizer clob(p.no,true);cv::SynchronizedBookStore poly;
        stamp.recv_monotonic_ns=100;check(poly.apply(clob.normalize(poly_snapshot(),stamp,1)),"CLOB snapshot");
        const auto absolute=r::json::serialize(r::Object{{"event_type","price_change"},{"market",p.id},{"price_changes",r::Array{
            r::Object{{"asset_id","222"},{"price","0.48"},{"size","17.25"},{"side","SELL"}}}}});
        stamp.recv_monotonic_ns=101;check(poly.apply(clob.normalize(absolute,stamp,2)) && poly.read(p.no).ask_qty_micro[0]==17250000,"CLOB size is absolute");
        auto fx=pair;fx.leg_no.currency="USDC";fx.artifact_hash=cv::pair_hash(fx);rejects([&]{cv::validate_pair(fx);},"collateral never silently netted");
        check(!cv::CrossVenueShadowAdapter::live_enabled,"no live-order invariant");rejects([]{cv::CrossVenueShadowAdapter::submit_order();},"no submission transport");
        signature_test(EVP_PKEY_RSA);signature_test(EVP_PKEY_ED25519);
        {
            auto approved=pair;approved.synthetic=false;approved.artifact_hash=cv::pair_hash(approved);
            auto object=pair_json(approved);object["synthetic"]=false;object["economic_review"]=r::Object{{"status","APPROVED"},{"reviewer","SYNTHETIC_TEST_REVIEWER"}};
            r::write_json(root/"approvals.json",r::Object{{"schema","cross-venue-approvals-v0.1"},{"pairs",r::Array{object}}});
            auto gated=config;gated.approvals=root/"approvals.json";gated.original["require_explicit_revision_review"]=true;
            gated.original["human_review_manifest"]=(root/"human-review.json").string();
            r::Object decision{{"candidate_pair_id",approved.pair_id},{"status","PENDING_HUMAN_REVIEW"},{"decision",nullptr},
                {"approved_revision",1},{"approved_artifact_hash",approved.artifact_hash},{"dossier_sha256",cv::sha256("SYNTHETIC_DOSSIER")},
                {"reviewer","SYNTHETIC_TEST_REVIEWER"},{"reviewed_at","SYNTHETIC_TIME"},{"coverage_proven",true},
                {"settlement_assets_reviewed",true},{"quantity_constraints_reviewed",true}};
            auto save=[&]{r::write_json(root/"human-review.json",r::Object{{"schema","cross-venue-human-review-manifest-v0.1.1"},{"reviews",r::Array{decision}}});};
            save();r::Array rejected;
            check(r::load_approved_pairs(gated,{k,p},rejected).empty(),"pending human review is never ingested as an approval");
            decision["status"]="APPROVED";decision["decision"]="APPROVED";save();rejected.clear();
            check(r::load_approved_pairs(gated,{k,p},rejected).size()==1,"exact explicitly approved regression revision is ingested");
            decision["approved_revision"]=2;save();rejected.clear();
            check(r::load_approved_pairs(gated,{k,p},rejected).empty(),"approval of another revision cannot authorize this artifact");
        }
        rejects([]{r::kalshi_signature("invalid-secret-must-not-be-logged","message");},"invalid credentials fail without printing material");
        r::PublicHttpClient public_only;
        check(public_only.get("https://external-api.kalshi.com/trade-api/v2/portfolio/orders").status==0,"private/order HTTP path prohibited before network");
        {r::RawRecorder bounded(root/"bounded",4096,14000);
            const r::Object body{{"raw_payload",std::string(5000,'x')}};bounded.append("kalshi",body);bounded.append("kalshi",body);
            check(bounded.inventory().size()==2,"safe append-only segment rotation");
            rejects([&]{bounded.append("kalshi",body);},"bounded storage stops before overrun");check(bounded.bytes()<=14000,"storage ceiling");}
        const auto real=r::read_object(std::filesystem::path(POLYTOPE_SOURCE_DIR)/"tests/fixtures/cross_venue/real_protocol_samples.json");
        std::vector<r::Market> real_markets;for(const auto& m:real.at("selected_markets").as_array())real_markets.push_back(r::normalize_market(r::get_text(m.as_object(),"venue"),m.as_object().at("raw").as_object()));
        r::ResearchState real_state(config,real_markets,{});
        for(const auto& record:real.at("records").as_array())check(real_state.process(record.as_object()),"actual recorded Kalshi REST and Polymarket WS protocol samples");
        check(real_state.scan(1,1).empty(),"real samples without approval never generate opportunities");
        {
            const auto sample=r::read_object(std::filesystem::path(POLYTOPE_SOURCE_DIR)/"tests/fixtures/cross_venue/real_kalshi_authenticated_samples.json");
            std::vector<r::Market> instruments;for(const auto& v:sample.at("selected_markets").as_array())instruments.push_back(
                r::normalize_market(r::get_text(v.as_object(),"venue"),v.as_object().at("raw").as_object()));
            auto safe=config;safe.original["reconciliation_policy"]="quiet_request_v0.1.1";r::ResearchState captured(safe,instruments,{});
            for(const auto& record:sample.at("records").as_array())check(captured.process(record.as_object()),"actual unrelated Kalshi lifecycle event cannot break subscribed books");
            for(const auto& m:instruments){const auto book=captured.books().read(m.yes);
                check(book.depth.usable_for_depth,"actual authenticated snapshots stay usable after unrelated lifecycle event");
                check(book.evidence.last_liveness_ns==r::get_uint(sample.at("records").as_array().back().as_object(),"recv_monotonic_ns") &&
                    book.evidence.recv_monotonic_ns<book.evidence.last_liveness_ns,"actual global socket traffic refreshes connection liveness without inventing a market price update");}
            check(r::get_uint(captured.counters().at("counts").as_object(),"UNWATCHED_LIFECYCLE_OR_TICK_EVENT")==1,"unwatched lifecycle is counted independently from reconstruction failures");
        }
        {
            const auto sample=r::read_object(std::filesystem::path(POLYTOPE_SOURCE_DIR)/"tests/fixtures/cross_venue/real_locked_book_sample.json");
            std::vector<r::Market> instruments;for(const auto& v:sample.at("selected_markets").as_array())instruments.push_back(
                r::normalize_market(r::get_text(v.as_object(),"venue"),v.as_object().at("raw").as_object()));
            auto safe=config;safe.original["reconciliation_policy"]="quiet_request_v0.1.1";r::ResearchState captured(safe,instruments,{});
            bool actual_rejection=false;for(const auto& record:sample.at("records").as_array()){
                const bool accepted=captured.process(record.as_object());
                if(r::get_uint(record.as_object(),"record_sequence")==35)actual_rejection=!accepted;
            }
            check(actual_rejection,"new real locked-book frame is rejected even after deep-state fix");
            bool locked=false;for(const auto& diagnostic:captured.take_diagnostics())locked|=r::get_text(diagnostic.as_object(),"apply_rejection")=="CROSSED_OR_LOCKED_BOOK";
            check(locked,"real rejection is diagnosed as a locked reconstructed book");
        }
        const auto lab=make_lab_capture(root/"lab");const auto summary=r::replay(lab,200000*cv::unit,{0,100,250,500,1000},root/"analysis-a");
        const auto feed=r::audit_feeds(lab,root/"feed-audit");
        check(r::get_uint(feed,"raw_records_verified")>0 && feed.at("approved_continuous_synchronized_l2_sec").as_double()>0,"feed audit measures approved healthy WS coverage without requiring positive edge");
        check(feed.at("both_venues_any_asset_healthy_l2_sec").as_double()>0,"simultaneous WS transport coverage is measured independently of profitability");
        check(r::get_uint(feed.at("wire_protocol").as_object().at("kalshi").as_object(),"snapshot_packets")==1,"native audit counts actual wire snapshots");
        const auto recovery_lab=make_lab_capture(root/"recovery-feed",false,false,false,true);
        const auto recovery_audit=r::audit_feeds(recovery_lab,root/"recovery-feed-audit");bool recovered=false;
        for(const auto& row:recovery_audit.at("snapshot_recovery").as_array())if(r::get_uint(row.as_object(),"connection_epoch")==2)recovered=r::get_bool(row.as_object(),"snapshot_then_delta_verified");
        check(recovered,"second connection proves fresh snapshot then real-shaped delta with restarted sid/seq");
        check(r::get_uint(recovery_audit.at("wire_protocol").as_object().at("kalshi").as_object(),"subscription_sequence_gap_events")==0,"sequence tracking never compares distinct connection epochs");
        const auto rest_lab=make_lab_capture(root/"rest-feed",true);
        check(r::audit_feeds(rest_lab,root/"rest-feed-audit").at("approved_continuous_synchronized_l2_sec").as_double()==0,"REST-only data never becomes synchronized continuous L2 coverage");
        (void)r::replay(lab,200000*cv::unit,{0,100,250,500,1000},root/"analysis-b");
        for(const auto& entry:std::filesystem::directory_iterator(root/"analysis-a"))if(entry.is_regular_file())
            check(cv::read_bytes(entry.path())==cv::read_bytes(root/"analysis-b"/entry.path().filename()),"deterministic ordered outputs");
        const auto& scenarios=summary.at("latency_scenarios").as_array();
        bool partial=false,adverse=false,failed=false,zero=false,slow=false;
        for(const auto& v:scenarios){const auto& row=v.as_object();const auto model=r::get_text(row,"execution_model");const auto latency=r::get_uint(row,"latency_ms");
            if(latency==0 && model=="VISIBLE_DEPTH_HYPOTHETICAL"){check(r::get_uint(row,"simulated_trades")==1,"zero latency hypothetical fill");zero=true;}
            if(latency==0 && model=="SECOND_LEG_60_PERCENT"){check(cv::money(r::get_text(row,"simulated_net_pnl"))<0,"partial hedge includes full residual loss");partial=true;}
            if(latency==0 && model=="SECOND_LEG_FAILS"){check(cv::money(r::get_text(row,"simulated_net_pnl"))<0,"failed hedge negative PnL");failed=true;}
            if(latency==0 && model=="SECOND_LEG_ADVERSE_4C"){check(cv::money(r::get_text(row,"modeled_execution_loss"))>0,"adverse execution loss separated");adverse=true;}
            if(latency>=500){check(r::get_uint(row,"simulated_trades")==0,"latency reprices later book without lookahead");slow=true;}
            check(row.at("verified_live_realized_pnl").is_null(),"never fabricate realized live profit");
            check(std::stod(r::get_text(row,"max_capital_utilization"))<=1.0,"capital ceiling");
        }
        check(partial&&adverse&&failed&&zero&&slow,"all mandatory scenario families exercised");
        const auto rest=make_lab_capture(root/"rest",true);const auto rest_result=r::replay(rest,200000*cv::unit,{0,1000},root/"rest-analysis");
        check(r::get_uint(rest_result,"simulated_trades_across_scenarios")==0,"REST sampling cannot become L2 execution backtest");
        const auto disconnected=make_lab_capture(root/"disconnected",false,true);const auto disconnected_result=r::replay(disconnected,200000*cv::unit,{100},root/"disconnected-analysis");
        check(r::get_uint(disconnected_result,"simulated_trades_across_scenarios")==0,"disconnect before execution invalidates candidate");
        const auto low=r::replay(lab,80*cv::unit,{0},root/"capital-analysis");
        for(const auto& row:low.at("latency_scenarios").as_array())check(cv::money(r::get_text(row.as_object(),"max_capital_committed"))<=80*cv::unit,"independent shared wallets prevent double spending");
        const auto multiple=make_lab_capture(root/"multiple",false,false,true);
        const auto portfolio=r::replay(multiple,200000*cv::unit,{0},root/"multiple-analysis");
        for(const auto& v:portfolio.at("latency_scenarios").as_array()){
            const auto& row=v.as_object();check(r::get_uint(row,"simulated_trades")==2,"distinct observed price-level liquidity supports two sequential simulations");
            if(r::get_text(row,"execution_model")=="SECOND_LEG_FAILS")check(cv::money(r::get_text(row,"max_residual_exposure"))==200*cv::unit,"portfolio residual exposure accumulates across open positions");
        }
        // Mutation is detected independently of strategy results.
        const auto clean_manifest=r::read_object(lab/"capture_manifest.json");auto bad_manifest=clean_manifest;auto& raw=bad_manifest.at("raw_inventory").as_array()[0].as_object();raw["sha256"]=std::string(64,'0');
        r::write_json(lab/"capture_manifest.json",bad_manifest);
        rejects([&]{r::replay(lab,200000*cv::unit,{0},root/"tampered-analysis");},"raw capture tampering rejected");
        r::write_json(lab/"capture_manifest.json",clean_manifest);
        std::cout<<"CrossVenueV0.1: "<<checks<<" checks passed; synthetic execution regressions and real-data classification remain separate\n";
        if(!keep)std::filesystem::remove_all(root);return 0;
    }catch(const std::exception& e){std::cerr<<"CrossVenueV0.1 failure: "<<e.what()<<"; retained evidence "<<root<<'\n';return 1;}
}
