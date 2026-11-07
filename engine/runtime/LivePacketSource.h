#pragma once
#include "engine/feed/raw_ingest/RawPacket.h"
#include <boost/json.hpp>
#include <memory>
namespace trading_engine::runtime {
class LivePacketSource {
public:
    LivePacketSource(const boost::json::array& markets,std::size_t queue_capacity,std::uint64_t duration_seconds,
                     bool require_kalshi,std::uint64_t reconnect_after_seconds=0,std::uint64_t epoch_seed=0);
    ~LivePacketSource();
    // Source RawPacket metadata is preserved in the envelope; sequencer availability is assigned by the runtime.
    bool pop(boost::json::object& record,unsigned timeout_ms=50);
    bool finished() const;
    boost::json::object health() const;
    void stop();
    void request_rebuild(const std::string& venue);
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
