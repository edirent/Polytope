#!/usr/bin/env python3
"""Build an isolated observation-only copy; preserve the frozen executable."""
import hashlib
import json
import shlex
import subprocess
from pathlib import Path

ROOT=Path('runs/pmxt_shock_fixed_tests_20261006/fill_conditional')
SRC=ROOT/'observer_source'


def replace_once(text,old,new):
    if text.count(old)!=1:raise ValueError('observer source anchor changed: '+old[:80])
    return text.replace(old,new)


def build():
    SRC.mkdir(parents=True,exist_ok=True)
    files=['replay/market_making/LeadLagReplay.h','replay/market_making/LeadLagReplay.cpp',
           'tools/run_btc15m_lead_lag_replay.cpp']
    manifest=json.loads(Path('runs/pmxt_shock_fixed_tests_20261006/frozen_manifest.json').read_text())
    sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
    original_exe=Path('build/run_btc15m_lead_lag_replay')
    if sha(original_exe)!=manifest['executable_sha256']:raise ValueError('frozen executable changed')
    original={name:Path(name).read_text() for name in files}
    h=original[files[0]]
    h=replace_once(h,'struct Summary {', '''struct AuditOrder {
    std::uint64_t id = 0, active_sequence = 0, end_sequence = 0;
    strategy::Channel channel = strategy::Channel::BuyUp;
    std::int64_t created_ms = 0, active_ms = -1, end_ms = -1, quantity = 0;
    bool ended_by_fill = false;
};
struct AuditFill {
    std::uint64_t order_id = 0, sequence = 0;
    std::optional<std::int64_t> visible_spread_tick;
};
struct AuditShock {
    std::int64_t ms = 0;
    std::uint64_t sequence = 0;
    double alpha = 0;
};
struct Summary {''')
    h=replace_once(h,'private:\n    struct Event {','''    [[nodiscard]] const auto& audit_orders() const { return audit_orders_; }
    [[nodiscard]] const auto& audit_fills() const { return audit_fills_; }
    [[nodiscard]] const auto& audit_shocks() const { return audit_shocks_; }
private:
    std::map<std::uint64_t, AuditOrder> audit_orders_;
    std::vector<AuditFill> audit_fills_;
    std::vector<AuditShock> audit_shocks_;
    std::uint64_t audit_sequence_ = 0;
    bool audit_previous_ready_ = false;
    double audit_previous_alpha_ = 0;
    std::int64_t audit_previous_ms_ = -1;
    struct Event {''')
    cpp=original[files[1]]
    cpp=replace_once(cpp,'    o.active = false;\n    // Keep reservations', '''    if (o.active) {
        auto& a = audit_orders_.at(id);
        a.end_ms = now_ms_; a.end_sequence = audit_sequence_; a.ended_by_fill = false;
    }
    o.active = false;
    // Keep reservations''')
    cpp=replace_once(cpp,'    auto d = engine_.decide(input);\n', '''    auto d = engine_.decide(input);
    if (d.ready && audit_previous_ready_ && now_ms_ - audit_previous_ms_ <= 1500 &&
        std::abs(d.alpha) >= .015 && std::abs(audit_previous_alpha_) < .015)
        audit_shocks_.push_back({now_ms_, audit_sequence_, d.alpha});
    audit_previous_ready_ = d.ready; audit_previous_alpha_ = d.alpha; audit_previous_ms_ = now_ms_;
''')
    cpp=replace_once(cpp,'        ++summary_.submitted;\n', '''        audit_orders_.emplace(id, AuditOrder{.id = id, .channel = c, .created_ms = now_ms_, .quantity = leg.quantity_lots});
        ++summary_.submitted;
''')
    cpp=replace_once(cpp,'            order.active = true;\n', '''            order.active = true;
            auto& audit = audit_orders_.at(id);
            audit.active_ms = now_ms_; audit.active_sequence = audit_sequence_;
''')
    cpp=replace_once(cpp,'    const auto id = o.reservation.quote_id;\n', '''    const auto id = o.reservation.quote_id;
    if (o.actual_remaining == 0) {
        auto& a = audit_orders_.at(id);
        a.end_ms = now_ms_; a.end_sequence = audit_sequence_; a.ended_by_fill = true;
    }
    const auto& visible = visible_books_[strategy::LeadLagEngine::token(c)];
    std::optional<std::int64_t> spread;
    if (visible.usable_for_depth && !visible.crossed && !visible.recovering && !visible.closed && !visible.resolved &&
        visible.bid_count && visible.ask_count && visible.bids[0].price_tick < visible.asks[0].price_tick &&
        visible.bids[0].size > 0 && visible.asks[0].size > 0 &&
        now_ms_ - static_cast<std::int64_t>(visible.last_ws_recv_ns / 1'000'000ULL) <= engine_.config().max_data_age_ms)
        spread = visible.asks[0].price_tick - visible.bids[0].price_tick;
    audit_fills_.push_back({id, audit_sequence_, spread});
''')
    cpp=replace_once(cpp,'now_ms_ = e.ms; e.apply();','now_ms_ = e.ms; ++audit_sequence_; e.apply();')
    cli=original[files[2]]
    cli=replace_once(cli,'std::string config_path, events_path, model_path, trace_path;','std::string config_path, events_path, model_path, trace_path, audit_path;')
    cli=replace_once(cli,'            else if (a == "--trace") trace_path = argv[i];','''            else if (a == "--trace") trace_path = argv[i];
            else if (a == "--audit") audit_path = argv[i];''')
    cli=replace_once(cli,'        json::array marks, quantities;','''        if (!audit_path.empty()) {
            std::ofstream audit(audit_path);
            if (!audit) throw std::runtime_error("cannot write audit " + audit_path);
            for (const auto& [id, a] : replay.audit_orders())
                audit << json::serialize(json::object{{"type", "order"}, {"id", id}, {"channel", static_cast<int>(a.channel)},
                    {"created_ms", a.created_ms}, {"active_ms", a.active_ms}, {"end_ms", a.end_ms},
                    {"active_sequence", a.active_sequence}, {"end_sequence", a.end_sequence},
                    {"ended_by_fill", a.ended_by_fill}, {"quantity_lots", a.quantity}}) << '\\n';
            for (const auto& a : replay.audit_shocks())
                audit << json::serialize(json::object{{"type", "shock"}, {"now_ms", a.ms}, {"sequence", a.sequence}, {"alpha", a.alpha}}) << '\\n';
            if (replay.audit_fills().size() != replay.fills().size()) throw std::runtime_error("terminal audit unsupported");
            for (std::size_t i = 0; i < replay.fills().size(); ++i) {
                const auto& f = replay.fills()[i]; const auto& a = replay.audit_fills()[i];
                json::array markouts;
                for (const auto& mo : f.markout) markouts.push_back(mo ? json::value(*mo) : json::value(nullptr));
                audit << json::serialize(json::object{{"type", "fill"}, {"event_ms", f.event_ms}, {"sequence", a.sequence},
                    {"order_id", a.order_id}, {"channel", static_cast<int>(f.channel)}, {"quantity_lots", f.quantity_lots},
                    {"price_tick", f.price_tick}, {"cancel_pending", f.cancel_pending}, {"markout", markouts},
                    {"visible_spread_tick", a.visible_spread_tick ? json::value(*a.visible_spread_tick) : json::value(nullptr)}}) << '\\n';
            }
        }
        json::array marks, quantities;''')
    for name,text in zip(files,[h,cpp,cli]):
        dest=SRC/name;dest.parent.mkdir(parents=True,exist_ok=True);dest.write_text(text)
    args=shlex.split(Path('build/CMakeFiles/run_btc15m_lead_lag_replay.dir/link.txt').read_text())
    libraries=args[args.index('libbtc15m_lead_lag_replay.a')+1:]
    libraries=[str(Path('build')/p) if not p.startswith('/') else p for p in libraries]
    executable=ROOT/'run_fill_observer'
    flags=['/usr/bin/c++','-O3','-DNDEBUG','-std=c++20','-DBOOST_SYSTEM_DYN_LINK','-DBOOST_SYSTEM_NO_LIB','-DORACLE_ENABLE_LLM=0',
           '-I'+str(SRC.resolve()),'-I.','-Iengine','-Ioracle']
    cmd=flags+[str(SRC/files[2]),str(SRC/files[1]),'-o',str(executable)]+libraries
    subprocess.run(cmd,check=True)
    build_manifest={'frozen_executable_sha256':manifest['executable_sha256'],'observer_sha256':sha(executable),
        'base_source_sha256':{name:sha(Path(name)) for name in files},
        'observer_source_sha256':{name:sha(SRC/name) for name in files},'compile_command':cmd,
        'observer_builder_sha256':sha(Path(__file__)),'original_files_modified':False,
        'observation_only':'Extra order lifecycle, shock-onset and fill spread records; execution statements preserved.'}
    (ROOT/'observer_build.json').write_text(json.dumps(build_manifest,indent=2)+'\n')
    if sha(original_exe)!=manifest['executable_sha256']:raise ValueError('original executable changed during observer build')
    print(json.dumps(build_manifest,indent=2))


if __name__=='__main__':build()
