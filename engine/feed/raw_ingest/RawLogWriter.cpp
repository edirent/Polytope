#include "feed/raw_ingest/RawLogWriter.h"

#include <utility>

namespace trading_engine::feed {

void RawLogWriter::append(RawPacket packet) {
    packets_.push_back(std::move(packet));
}

void RawLogWriter::clear() noexcept {
    packets_.clear();
}

const std::vector<RawPacket>& RawLogWriter::packets() const noexcept {
    return packets_;
}

}  // namespace trading_engine::feed
