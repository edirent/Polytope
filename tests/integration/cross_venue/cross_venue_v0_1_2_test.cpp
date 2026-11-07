#include "engine/strategy/cross_venue/LiveResearch.h"
#include "engine/state/cross_venue/JointBookAsOf.h"
#include "engine/common/math/FractionalVwap.h"
#include <iostream>
#include "engine/decode/parser/kalshi/KalshiBookNormalizer.h"
namespace cv=trading_engine::cross_venue;namespace r=cv::research;
int main(){try{unsigned n=0;auto check=[&](bool b,const char* s){if(!b)throw std::runtime_error(s);++n;};
    cv::SynchronizedBookStore store;cv::VenueInstrumentKey k{cv::VenueId::Kalshi,"TEST","YES","USD"},p{cv::VenueId::PolymarketInternational,"TEST","NO","USD"};
    auto put=[&](const auto& key,std::uint64_t t,std::uint64_t epoch=1){cv::BookUpdate u;u.key=key;u.evidence.recv_monotonic_ns=t;u.evidence.last_liveness_ns=t;u.evidence.authoritative_snapshot=true;u.evidence.sequence=1;u.evidence.connection_epoch=epoch;u.evidence.raw_hash=cv::sha256(std::to_string(t));u.fractional_quantities=true;u.bids={{400000,10000000}};u.asks={{450000,1250000},{460000,2000000}};check(store.apply(u),"snapshot");};
    constexpr std::uint64_t sec=1000000000;put(k,sec);put(p,3*sec);store.note_liveness(k,3*sec);
    cv::JointBookPolicy policy;auto books=[&](){return std::array{store.read(k),store.read(p)};};auto join=[&](std::uint64_t t){return cv::joint_book_asof(books(),t,policy,{true,true},{true,true});};
    check(join(3*sec).diagnostic_eligible,"asof accepts nonsimultaneous books");policy.legacy_skew=true;check(!join(3*sec).diagnostic_eligible,"legacy rejects update skew");policy.legacy_skew=false;
    check(!join(2*sec).available,"no lookahead latest view cannot answer past");
    check(!cv::joint_book_asof(books(),3*sec,policy,{false,true},{true,true}).transport_healthy,"REST not WS");
    check(!cv::joint_book_asof(books(),3*sec,policy,{true,true},{false,true}).diagnostic_eligible,"status closed");
    auto changed_after_authority=books();changed_after_authority[0].evidence.last_book_change_ns=2*sec;
    auto tight=policy;tight.change_ns=sec;
    check(cv::joint_book_asof(changed_after_authority,10*sec,tight,{true,true},{true,true}).reason=="STALE_BOOK","quiet state changed after last authority needs reconfirmation");
    check(!join(3*sec).strict_strategy_eligible,"diagnostic never approves");policy.research_only=true;
    check(!cv::joint_book_asof(books(),3*sec,policy,{true,true},{true,true},true).strict_strategy_eligible,"permissive not strict");policy.research_only=false;
    const auto oldchange=store.read(k).evidence.last_book_change_ns;put(k,20*sec);put(p,20*sec);
    check(store.read(k).evidence.last_book_change_ns==oldchange,"unchanged snapshot retains last actual change");check(join(30*sec).diagnostic_eligible,"long quiet validated state");
    store.note_liveness(k,70*sec);store.note_liveness(p,70*sec);check(join(70*sec).reason=="STALE_AUTHORITY","healthy socket stale book rejected");
    put(k,71*sec);put(p,71*sec);auto ref=store.read(k);ref.ask_qty_micro[0]++;check(!store.reconcile(k,ref,72*sec),"reconcile mismatch");check(join(72*sec).reason=="BOOK_INTEGRITY","unresolved mismatch rejected");
    put(k,73*sec,2);check(join(73*sec).diagnostic_eligible,"new epoch snapshot recovery");
    auto b=store.read(k);auto vw=trading_engine::common::math::buy_vwap_micro(b,1500000);check(vw.ok && vw.cost==677500,"fractional two level walk exact");check(!trading_engine::common::math::buy_vwap_micro(b,4000000).ok,"depth not invented");
    r::FeeSchedule fee;fee.valid=true;fee.observed_wall_ns=1;fee.expires_wall_ns=100;fee.rate=70000;fee.market_id="TEST";fee.type="quadratic_cent_ceiling";
    auto f=r::estimate_fee_micro(fee,b,1500000,10);check(f==32500,"fee plus notional per fragment conservative cent rounding");
    b.evidence.source_ts_ns=999999999999999999ULL;b.evidence.recv_wall_ns=1;auto bb=books();bb[0]=b;check(cv::joint_book_asof(bb,73*sec,policy,{true,true},{true,true}).diagnostic_eligible,"source clock anomaly cannot change local availability");
    store.invalidate(k,"DISCONNECT",74*sec);check(!join(74*sec).diagnostic_eligible,"disconnect invalidates epoch");
    cv::BookProvenance e;e.recv_monotonic_ns=75*sec;e.last_liveness_ns=75*sec;e.authoritative_snapshot=true;e.connection_epoch=3;
    cv::KalshiBookNormalizer parser(k,true,true);const auto raw=R"({"type":"orderbook_snapshot","sid":3,"seq":1,"msg":{"market_ticker":"TEST","clock":5,"no_dollars_fp":[["0.9980","662161.94"]]}})";
    auto u=parser.websocket(raw,e,true);check(u.kind==cv::BookUpdate::Kind::Snapshot && u.bids.empty() && u.asks.size()==1,"omitted empty Kalshi side is valid wire state");
    cv::SynchronizedBookStore empty_store(true,true);check(empty_store.apply(u),"known one-sided book reconstructs without corrupting session");
    cv::BookUpdate empty=u;empty.key=p;empty.bids.clear();empty.asks.clear();check(empty_store.apply(empty),"empty market is representable");
    auto empty_join=cv::joint_book_asof({empty_store.read(k),empty_store.read(p)},75*sec,policy,{true,true},{true,true});
    check(empty_join.reconstruction_valid && !empty_join.diagnostic_eligible && empty_join.reason=="NO_EXECUTABLE_ASK","empty liquidity is not eligible or corruption");
    check(!trading_engine::common::math::buy_vwap_micro(empty_store.read(p),cv::unit).ok,"no invented empty liquidity");
    std::cout<<n<<" V0.1.2 As-Of/fractional/clock/approval checks passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
