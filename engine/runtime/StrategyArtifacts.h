#pragma once
#include "engine/runtime/MakerPlugin.h"
#include <boost/json.hpp>
#include <set>

namespace trading_engine::runtime {
inline strategy::market_making::MarketMakingConfig maker_model_config(const boost::json::object& model,const std::string& instance,
    const std::string& model_hash,std::uint64_t age) {
    namespace mm=strategy::market_making;mm::MarketMakingConfig c;c.strategy_id=strategy_identity_hash(instance);c.oracle_artifact_hash=strategy_identity_hash(model_hash);c.max_book_age_ns=age;
    const std::set<std::string> supported={"base_quote_size_lots","min_half_spread_tick","max_inventory_lots","min_inventory_lots","target_position_lots","max_inventory_skew_tick",
        "quote_ttl_ns","requote_threshold_tick","min_requote_interval_ns","external_fair_value_tick","external_fair_weight_bps","enable_bid_quotes","enable_ask_quotes"};
    for(const auto& field:model)if(!supported.contains(std::string(field.key())))throw std::runtime_error("unsupported maker model parameter");
    auto n=[&](const char* key,std::int64_t fallback){auto p=model.if_contains(key);return p?p->to_number<std::int64_t>():fallback;};
    auto b=[&](const char* key,bool fallback){auto p=model.if_contains(key);return p?p->as_bool():fallback;};
#define MODEL_NUMBER(key) c.key=n(#key,c.key)
    MODEL_NUMBER(base_quote_size_lots);MODEL_NUMBER(min_half_spread_tick);MODEL_NUMBER(max_inventory_lots);MODEL_NUMBER(min_inventory_lots);
    MODEL_NUMBER(target_position_lots);MODEL_NUMBER(max_inventory_skew_tick);MODEL_NUMBER(quote_ttl_ns);MODEL_NUMBER(requote_threshold_tick);
    MODEL_NUMBER(min_requote_interval_ns);MODEL_NUMBER(external_fair_value_tick);MODEL_NUMBER(external_fair_weight_bps);
#undef MODEL_NUMBER
    c.enable_bid_quotes=b("enable_bid_quotes",true);c.enable_ask_quotes=b("enable_ask_quotes",true);return c;
}
inline boost::json::object quote_payload_json(const strategy::market_making::QuoteIntent& q) {
    namespace j=boost::json;auto leg=[](const strategy::market_making::QuoteLeg& l){return j::object{{"market_id",l.market_id},{"asset_id",l.asset_id},
        {"market_index",l.market_index},{"asset_index",l.asset_index},{"side",static_cast<int>(l.side)},{"price_tick",l.price_tick},{"quantity_lots",l.quantity_lots},
        {"fair_value_tick",l.fair_value_tick},{"edge_to_fair_tick",l.edge_to_fair_tick},{"risk_reducing",l.risk_reducing},
        {"allow_fair_deviation_exemption",l.allow_fair_deviation_exemption},{"allow_spread_exemption",l.allow_spread_exemption},
        {"book_version",l.book_version},{"snapshot_version_hash",l.snapshot_version_hash}};};
    j::object out{{"type",static_cast<int>(q.type)},{"bid",leg(q.bid)},{"ask",leg(q.ask)}};
#define QUOTE_FIELD(key) out[#key]=q.key
    QUOTE_FIELD(quote_intent_id);QUOTE_FIELD(strategy_id);QUOTE_FIELD(quote_group_id);QUOTE_FIELD(market_id);QUOTE_FIELD(asset_id);QUOTE_FIELD(market_index);QUOTE_FIELD(asset_index);
    QUOTE_FIELD(has_bid);QUOTE_FIELD(has_ask);QUOTE_FIELD(fair_value_tick);QUOTE_FIELD(asset_fair_tick);QUOTE_FIELD(canonical_yes_fair_tick);QUOTE_FIELD(canonical_yes_position_lots);
    QUOTE_FIELD(target_canonical_yes_lots);QUOTE_FIELD(edge_tick);QUOTE_FIELD(half_spread_tick);QUOTE_FIELD(inventory_skew_tick);QUOTE_FIELD(target_position_lots);QUOTE_FIELD(current_position_lots);
    QUOTE_FIELD(product_external_fair_tick);QUOTE_FIELD(product_z_score_bps);QUOTE_FIELD(product_tau_ms);QUOTE_FIELD(product_dynamic_inventory_cap_lots);
    QUOTE_FIELD(product_opening_allowed);QUOTE_FIELD(product_settlement_source_ok);QUOTE_FIELD(product_reward_size_lift_allowed);QUOTE_FIELD(product_reward_spread_compression_allowed);
    QUOTE_FIELD(reward_config_present);QUOTE_FIELD(reward_eligible);QUOTE_FIELD(reward_condition_id);QUOTE_FIELD(reward_max_spread_tick);QUOTE_FIELD(reward_min_size_lots);QUOTE_FIELD(reward_reason);
    QUOTE_FIELD(created_ts_ns);QUOTE_FIELD(expires_at_ns);QUOTE_FIELD(snapshot_version_hash);QUOTE_FIELD(oracle_artifact_hash);QUOTE_FIELD(policy_hash);QUOTE_FIELD(idempotency_hash);QUOTE_FIELD(reason);
#undef QUOTE_FIELD
    out["risk_mode"]=static_cast<int>(q.risk_mode);out["product_kind"]=static_cast<int>(q.product_kind);out["product_asset_direction"]=static_cast<int>(q.product_asset_direction);out["product_winning_direction"]=static_cast<int>(q.product_winning_direction);return out;
}
inline boost::json::object maker_frame_json(const MakerFrameAsset& a,std::uint64_t now) {
    namespace j=boost::json;j::array bids,asks;for(std::uint16_t n=0;n<a.depth.bid_count;++n)bids.push_back(j::array{a.depth.bids[n].price_tick,a.depth.bids[n].size});
    for(std::uint16_t n=0;n<a.depth.ask_count;++n)asks.push_back(j::array{a.depth.asks[n].price_tick,a.depth.asks[n].size});
    return {{"asset_id",a.asset_id},{"market_id",a.market_id},{"asset_index",a.asset_index},{"market_index",a.market_index},{"position_lots",a.position_lots},{"now_ns",now},
        {"book_version",a.depth.book_version},{"snapshot_version_hash",a.depth.snapshot_version_hash},{"last_ws_recv_ns",a.depth.last_ws_recv_ns},
        {"usable_for_depth",a.depth.usable_for_depth},{"recovering",a.depth.recovering},{"crossed",a.depth.crossed},{"closed",a.depth.closed},{"resolved",a.depth.resolved},{"bids",bids},{"asks",asks}};
}
inline MakerFrameAsset maker_frame_from_json(const boost::json::object& v) {
    MakerFrameAsset a;auto n=[&](const char* k){return v.at(k).to_number<std::int64_t>();};a.asset_id=std::string(v.at("asset_id").as_string());a.market_id=std::string(v.at("market_id").as_string());
    a.asset_index=n("asset_index");a.market_index=n("market_index");a.position_lots=n("position_lots");a.depth.asset_index=a.asset_index;
    a.depth.book_version=n("book_version");a.depth.snapshot_version_hash=n("snapshot_version_hash");a.depth.last_ws_recv_ns=n("last_ws_recv_ns");
    a.depth.usable_for_depth=v.at("usable_for_depth").as_bool();a.depth.recovering=v.at("recovering").as_bool();a.depth.crossed=v.at("crossed").as_bool();a.depth.closed=v.at("closed").as_bool();a.depth.resolved=v.at("resolved").as_bool();
    for(const auto& value:v.at("bids").as_array()){const auto& x=value.as_array();auto& level=a.depth.bids.at(a.depth.bid_count++);level.price_tick=x[0].to_number<std::int64_t>();level.size=x[1].to_number<double>();}
    for(const auto& value:v.at("asks").as_array()){const auto& x=value.as_array();auto& level=a.depth.asks.at(a.depth.ask_count++);level.price_tick=x[0].to_number<std::int64_t>();level.size=x[1].to_number<double>();}
    state::build_depth_prefix(a.depth.bids,a.depth.bid_count,a.depth.asks,a.depth.ask_count,&a.depth.prefix);return a;
}
}
