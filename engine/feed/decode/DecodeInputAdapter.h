#pragma once

#include "decode/public/DecodeTypes.h"
#include "feed/raw_ingest/RawPacket.h"

namespace trading_engine::feed {

[[nodiscard]] inline trading_engine::decode::DecodeInputView
to_decode_input_view(const RawPacket& packet) noexcept {
    return trading_engine::decode::DecodeInputView{
        .packet_id = packet.header.packet_id,
        .connection_id = packet.header.connection_id,
        .recv_wall_ns = packet.header.recv_wall_ns,
        .recv_monotonic_ns = packet.header.recv_monotonic_ns,
        .source_id = packet.header.source_id,
        .codec = static_cast<std::uint16_t>(packet.header.codec),
        .flags = packet.header.flags,
        .payload = packet.payload
    };
}

}  // namespace trading_engine::feed
