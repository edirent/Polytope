#include "engine/runtime/LivePacketSource.h"
#include "engine/runtime/LatencyMetrics.h"
#include "engine/strategy/cross_venue/LiveResearch.h"
#include "engine/feed/source_runtime/WebSocketClient.h"
#include <condition_variable>
#include "engine/memory/RingBuffer.h"
#include <deque>
#include <fstream>
#include <thread>
#include <atomic>
#include <cstdlib>
namespace trading_engine::runtime {
namespace j=boost::json;namespace r=cross_venue::research;
struct LivePacketSource::Impl {
    struct Control {std::mutex mutex;std::shared_ptr<feed::WebSocketClient> client;std::atomic<std::uint64_t> last{0},epoch{0},messages{0};std::atomic<bool> connected{false},verified{false};};
    std::array<Control,2> controls;
    std::mutex mutex;std::condition_variable cv;memory::RingBuffer<j::object> queue;std::size_t bytes=0,capacity;
    std::atomic<bool> stopping{false},done{false},overflow{false};std::atomic<std::uint64_t> dropped{0};
    std::vector<std::thread> workers;std::thread maintenance;
    std::uint64_t deadline,start,reconnect_after,seed;bool require_auth;
    j::array markets;
    Impl(const j::array& m,std::size_t cap,std::uint64_t duration,bool require,std::uint64_t reconnect,std::uint64_t epoch)
        :queue(cap),capacity(cap),start(monotonic_ns()),reconnect_after(reconnect),seed(epoch),require_auth(require),markets(m) {
        deadline=start+duration*1'000'000'000ULL;
        if(cap<4 || cap>65536 || duration==0 || duration>72*3600) throw std::runtime_error("invalid bounded live source configuration");
        const auto* id=std::getenv("KALSHI_API_KEY_ID");
        if(require && (!id || !*id)) throw std::runtime_error("KALSHI_KEY_ID_NOT_INHERITED_USE_SECURE_LOCAL_LAUNCH");
        for(unsigned venue=0;venue<2;++venue) workers.emplace_back([this,venue]{run(venue);});
        workers.emplace_back([this]{rest_diagnostics();});
        maintenance=std::thread([this]{watch();});
    }
    bool push(j::object row) {
        const auto* id=std::getenv("KALSHI_API_KEY_ID");auto raw=r::get_text(row,"raw_payload");
        if((id && *id && raw.find(id)!=std::string::npos) || raw.find("PRIVATE KEY-----")!=std::string::npos || raw.find("KALSHI-ACCESS-SIGNATURE")!=std::string::npos) {
            overflow=true;stopping=true;cv.notify_all();return false;
        }
        std::lock_guard lock(mutex);if(stopping)return false;
        const auto size=j::serialize(row).size();
        if(size>1024*1024 || queue.size()>=capacity || bytes+size>64*1024*1024) {
            ++dropped;overflow=true;stopping=true;cv.notify_all();return false;
        }
        row["_queue_bytes"]=size;bytes+=size;queue.push_back(std::move(row));cv.notify_one();return true;
    }
    void event(unsigned v,const std::string& classification,std::uint64_t epoch) {
        push({{"kind","venue_packet"},{"venue",v==0?"kalshi":"polymarket_international"},{"transport","event"},
              {"classification",classification},{"connection_epoch",epoch},{"raw_payload",classification},{"callback_recv_monotonic_ns",monotonic_ns()},{"recv_wall_ns",r::wall_ns()}});
    }
    void run(unsigned v) {
        auto& control=controls[v];j::array ids;
        for(auto& value:markets) {auto& row=value.as_object();if(r::get_text(row,"venue")!=(v==0?"kalshi":"polymarket_international"))continue;
            auto m=r::normalize_market(r::get_text(row,"venue"),row.at("raw").as_object());
            if(v==0)ids.emplace_back(m.id);else {ids.emplace_back(m.yes.instrument_id);ids.emplace_back(m.no.instrument_id);}}
        if(ids.empty())return;
        const auto* id=std::getenv("KALSHI_API_KEY_ID");
        if(v==0 && (!id || !*id)) {event(v,"missing_ws_credentials",seed);return;}
        unsigned attempts=0;
        while(!stopping && monotonic_ns()<deadline) {
            const auto epoch=seed+(++control.epoch);control.verified=false;
            const std::string endpoint=v==0?"wss://external-api-ws.kalshi.com/trade-api/ws/v2":"wss://ws-subscriptions-clob.polymarket.com/ws/market";
            auto client=std::make_shared<feed::WebSocketClient>(endpoint);
            {std::lock_guard lock(control.mutex);control.client=client;}
            if(v==0) try {
                const auto* path=std::getenv("KALSHI_PRIVATE_KEY_PATH");
                const auto key_path=path&&*path?path:"/home/edirent/.secrets/kalshi.pem";
                std::ifstream file(key_path,std::ios::binary);if(!file)throw std::runtime_error("key unavailable");
                const std::string pem{std::istreambuf_iterator<char>(file),{}};const auto timestamp=std::to_string(r::wall_ns()/1'000'000);
                client->set_handshake_headers({{"KALSHI-ACCESS-KEY",id},{"KALSHI-ACCESS-TIMESTAMP",timestamp},
                    {"KALSHI-ACCESS-SIGNATURE",r::kalshi_signature(pem,timestamp+"GET/trade-api/ws/v2")}});
            } catch(...) {event(v,"authentication_error",epoch);break;}
            std::uint64_t arrival=0;
            client->set_on_open([&,weak=std::weak_ptr(client)] {
                auto socket=weak.lock();if(!socket || stopping)return;
                control.connected=true;control.last=monotonic_ns();event(v,"connection_open",epoch);
                if(v==0) {socket->send(j::serialize(j::object{{"id",1},{"cmd","subscribe"},{"params",j::object{{"channels",j::array{"orderbook_delta"}},{"market_tickers",ids}}}}));
                    socket->send(j::serialize(j::object{{"id",2},{"cmd","subscribe"},{"params",j::object{{"channels",j::array{"market_lifecycle_v2"}}}}}));}
                else socket->send(j::serialize(j::object{{"assets_ids",ids},{"type","market"},{"custom_feature_enabled",true}}));
            });
            auto receive=[&](const std::string& raw,bool heartbeat) {
                control.last=monotonic_ns();if(!heartbeat){++control.messages;
                    if(raw.find("orderbook_snapshot")!=std::string::npos || raw.find("orderbook_delta")!=std::string::npos || raw.find("asset_id")!=std::string::npos)
                        control.verified=true;
                }
                // Same RawPacket time/provenance contract as recorded ingestion; Kalshi's venue is explicit in envelope.
                auto packet=feed::make_raw_packet(v==0?feed::SourceId::Unknown:feed::SourceId::PolymarketMarket,epoch,++arrival,raw);
                if(!push({{"kind","venue_packet"},{"venue",v==0?"kalshi":"polymarket_international"},{"transport","ws"},
                    {"classification",heartbeat?"heartbeat":"market_data"},{"raw_payload",packet.payload},{"connection_epoch",epoch},
                    {"callback_recv_monotonic_ns",packet.header.recv_monotonic_ns},{"recv_wall_ns",packet.header.recv_wall_ns},
                    {"arrival_sequence",arrival},{"packet_crc32",packet.header.payload_crc32}}))client->disconnect();
            };
            client->set_on_message([&](const std::string& raw){receive(raw,raw=="PING" || raw=="PONG");});
            client->set_on_control([&](const std::string& kind){receive(kind,true);});
            client->set_on_error([&](const std::string&){event(v,"connection_error",epoch);});
            try {client->connect();if(!stopping)client->run();}catch(...){event(v,"connection_error",epoch);}
            control.connected=false;client->disconnect();event(v,"disconnect",epoch);
            {std::lock_guard lock(control.mutex);control.client.reset();}
            auto wait=std::min(10000U,500U*(1U<<std::min(attempts++,4U)));
            for(unsigned ms=0;ms<wait && !stopping;ms+=50)std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    void rest_diagnostics() {
        r::PublicHttpClient http;
        while(!stopping && monotonic_ns()<deadline) {
            for(const auto& value:markets) {
                const auto& row=value.as_object();const auto venue=r::get_text(row,"venue");
                auto market=r::normalize_market(venue,row.at("raw").as_object());
                for(const auto& key:{market.yes,market.no}) {
                    if(stopping)break;if(venue=="kalshi" && key.instrument_id=="NO")continue;
                    const auto url=venue=="kalshi"?"https://external-api.kalshi.com/trade-api/v2/markets/"+market.id+"/orderbook?depth=64":
                        "https://clob.polymarket.com/book?token_id="+key.instrument_id;
                    const auto began=monotonic_ns();auto response=http.get(url,5);
                    push({{"kind","venue_packet"},{"venue",venue},{"transport","rest"},{"classification","snapshot"},
                          {"raw_payload",response.body},{"http_status",response.status},{"key",r::key_json(key)},
                          {"request_started_monotonic_ns",began},{"callback_recv_monotonic_ns",monotonic_ns()},
                          {"recv_wall_ns",r::wall_ns()},{"connection_epoch",0}});
                }
            }
            for(unsigned n=0;n<150 && !stopping;++n)std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    void watch() {
        std::array<bool,2> forced{false,false};auto ping=monotonic_ns();
        while(!stopping && monotonic_ns()<deadline) {
            auto now=monotonic_ns();
            for(unsigned v=0;v<2;++v) {auto& c=controls[v];std::lock_guard lock(c.mutex);
                if(!c.client || !c.connected)continue;
                if((reconnect_after && !forced[v] && now-start>=reconnect_after*1'000'000'000ULL) || now-c.last>30'000'000'000ULL) {
                    forced[v]=true;event(v,now-c.last>30'000'000'000ULL?"timeout":"validation_reconnect",seed+c.epoch);c.client->disconnect();
                } else if(v==1 && now-ping>=10'000'000'000ULL) try{c.client->send("PING");}catch(...){}
            }
            if(now-ping>=10'000'000'000ULL)ping=now;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        stopping=true;
        for(auto& c:controls){std::lock_guard lock(c.mutex);if(c.client)c.client->disconnect();}
        done=true;cv.notify_all();
    }
    void stop() {
        stopping=true;for(auto& c:controls){std::lock_guard lock(c.mutex);if(c.client)c.client->disconnect();}
        cv.notify_all();if(maintenance.joinable())maintenance.join();for(auto& t:workers)if(t.joinable())t.join();done=true;
    }
};
LivePacketSource::LivePacketSource(const j::array& m,std::size_t cap,std::uint64_t seconds,bool required,std::uint64_t reconnect,std::uint64_t seed)
    :impl_(std::make_unique<Impl>(m,cap,seconds,required,reconnect,seed)){}
LivePacketSource::~LivePacketSource(){stop();}
void LivePacketSource::stop(){impl_->stop();}
bool LivePacketSource::finished() const {std::lock_guard lock(impl_->mutex);return impl_->done && impl_->queue.empty();}
bool LivePacketSource::pop(j::object& row,unsigned timeout) {
    std::unique_lock lock(impl_->mutex);impl_->cv.wait_for(lock,std::chrono::milliseconds(timeout),[&]{return !impl_->queue.empty() || impl_->done;});
    if(impl_->queue.empty())return false;row=std::move(impl_->queue.front());impl_->queue.pop_front();
    impl_->bytes-=r::get_uint(row,"_queue_bytes");row.erase("_queue_bytes");return true;
}
j::object LivePacketSource::health() const {
    j::object out;for(unsigned v=0;v<2;++v) {auto& c=impl_->controls[v];out[v==0?"kalshi":"polymarket_international"]=j::object{
        {"connected",c.connected.load()},{"subscription_data_seen",c.verified.load()},{"messages",c.messages.load()},{"epoch",impl_->seed+c.epoch.load()}};}
    std::lock_guard lock(impl_->mutex);out["queue_depth"]=impl_->queue.size();out["queue_bytes"]=impl_->bytes;out["overflow"]=impl_->overflow.load();out["dropped"]=impl_->dropped.load();return out;
}
}

namespace trading_engine::runtime {
void LivePacketSource::request_rebuild(const std::string& venue) {
    unsigned v=venue=="kalshi"?0:1;auto& c=impl_->controls[v];std::lock_guard lock(c.mutex);
    if(c.client) { impl_->event(v,"validation_reconnect",impl_->seed+c.epoch);c.client->disconnect(); }
}
}
