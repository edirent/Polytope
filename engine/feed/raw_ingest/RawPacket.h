#pragma once

#include <cstdint>
#include <string>

namespace trading_engine::feed {

/**
 * @brief Magic number used to identify Helix raw packet files/records.
 *
 * The value 0x484C5852 corresponds to the ASCII bytes:
 *
 *     H L X R
 *
 * This is used by RawLogReader to quickly verify that the bytes being read
 * are actually a valid RawPacketHeader and not:
 *
 * - a corrupted file,
 * - a file of the wrong format,
 * - a bad read offset,
 * - or random payload bytes interpreted as a header.
 *
 * Every RawPacketHeader should start with this value.
 */
constexpr std::uint32_t RAW_PACKET_MAGIC = 0x484C5852;  // "HLXR"

/**
 * @brief Version of the raw packet header format.
 *
 * This allows the raw log format to evolve over time.
 *
 * For example, version 2 might add:
 *
 * - exchange timestamp,
 * - channel id,
 * - compression metadata,
 * - checksum algorithm id,
 * - or source-specific metadata.
 *
 * RawLogReader can use this field to decide how to parse older logs.
 */
constexpr std::uint16_t RAW_PACKET_VERSION = 1;

/**
 * @brief Identifies the logical data source that produced a packet.
 *
 * This is intentionally an enum instead of a string.
 *
 * A string-based source such as "market", "Market", "POLYMARKET_MARKET",
 * or "polymarket_market" is too loose and error-prone. An enum gives the
 * system a stable internal representation.
 *
 * The source id is used later by:
 *
 * - RawLogReader,
 * - JsonDecoder,
 * - EventNormalizer,
 * - ReplayRunner,
 * - HealthPublisher,
 * - and source-specific routing logic.
 */
enum class SourceId : std::uint16_t {
    /**
     * @brief Default invalid source.
     *
     * If a RawPacket still has Unknown after construction, something upstream
     * failed to label the packet correctly.
     */
    Unknown = 0,

    /**
     * @brief Polymarket public market-data WebSocket.
     *
     * Expected event types include:
     *
     * - book,
     * - price_change,
     * - tick_size_change,
     * - last_trade_price,
     * - best_bid_ask,
     * - new_market,
     * - market_resolved.
     */
    PolymarketMarket = 1,

    /**
     * @brief Polymarket authenticated user WebSocket.
     *
     * Expected event types include:
     *
     * - order,
     * - trade.
     */
    PolymarketUser = 2,

    /**
     * @brief Polymarket sports-data WebSocket.
     *
     * Expected event types include:
     *
     * - sport_result.
     */
    PolymarketSports = 3,

    /**
     * @brief Polymarket real-time data stream.
     *
     * This is treated as an optional/reference stream in the MVP.
     */
    PolymarketRTDS = 4
};

/**
 * @brief Compression format of the packet payload.
 *
 * For the first MVP, most WebSocket payloads will likely be uncompressed JSON,
 * so Codec::None is expected.
 *
 * The field exists so that future raw logs can store compressed payloads
 * without changing the higher-level packet format.
 */
enum class Codec : std::uint16_t {
    /**
     * @brief Payload is stored as raw bytes with no compression.
     */
    None = 0,

    /**
     * @brief Payload is compressed with gzip.
     */
    Gzip = 1,

    /**
     * @brief Payload is compressed with Zstandard.
     */
    Zstd = 2
};

/**
 * @brief Bitmask flags describing packet-level metadata or abnormal state.
 *
 * These values are powers of two so they can be combined:
 *
 *     header.flags = PacketHeartbeat | PacketReplayed;
 *
 * Use bitwise checks:
 *
 *     if (header.flags & PacketHeartbeat) { ... }
 *
 * These flags describe the raw packet, not the normalized market event.
 */
enum PacketFlags : std::uint32_t {
    /**
     * @brief Normal packet with no special flags.
     */
    PacketNone = 0,

    /**
     * @brief Packet is a heartbeat/control message, such as PING/PONG.
     *
     * Heartbeat packets are useful for connection health, but they usually
     * should not update market state.
     */
    PacketHeartbeat = 1 << 0,

    /**
     * @brief Packet was produced during replay rather than live capture.
     *
     * This is usually added by replay logic, not during original live capture.
     */
    PacketReplayed = 1 << 1,

    /**
     * @brief Packet payload failed decoding.
     *
     * In strict raw-capture mode, this may not be known at write time.
     * It can be set later by the decode/replay pipeline.
     */
    PacketDecodeErr = 1 << 2,

    /**
     * @brief Packet is unsafe or should not be trusted blindly.
     *
     * Examples:
     *
     * - CRC mismatch,
     * - truncated payload,
     * - unknown source,
     * - missing required fields,
     * - schema violation.
     */
    PacketUnsafe = 1 << 3
};

/**
 * @brief Fixed-size metadata header stored before each raw payload.
 *
 * Raw log file layout:
 *
 *     [RawPacketHeader][payload bytes]
 *     [RawPacketHeader][payload bytes]
 *     [RawPacketHeader][payload bytes]
 *
 * The header tells RawLogReader:
 *
 * - how to validate the record,
 * - how many payload bytes to read,
 * - which source produced the packet,
 * - when the packet was received,
 * - and how to verify payload integrity.
 *
 * This struct is deliberately low-level. It should not contain decoded
 * business fields such as price, size, side, asset_id, or event type.
 * Those belong in decoded/normalized event structures.
 */
struct RawPacketHeader {
    /**
     * @brief Format marker.
     *
     * Must equal RAW_PACKET_MAGIC.
     *
     * RawLogReader should reject the packet if this value does not match.
     */
    std::uint32_t magic{RAW_PACKET_MAGIC};

    /**
     * @brief Raw packet header format version.
     *
     * Must equal RAW_PACKET_VERSION for the current writer.
     *
     * A future reader may support multiple versions.
     */
    std::uint16_t version{RAW_PACKET_VERSION};

    /**
     * @brief Size of this header in bytes.
     *
     * This makes the raw log format easier to evolve. If future versions add
     * fields, readers can use header_size to skip unknown header extensions.
     */
    std::uint16_t header_size{sizeof(RawPacketHeader)};

    /**
     * @brief Wall-clock receive timestamp in nanoseconds.
     *
     * This should usually be derived from std::chrono::system_clock.
     *
     * Purpose:
     *
     * - human-readable logs,
     * - cross-system debugging,
     * - aligning packets to real-world time.
     *
     * Do not use wall time for timeout logic because system time can jump due
     * to NTP adjustment or manual clock changes.
     */
    std::uint64_t recv_wall_ns{0};

    /**
     * @brief Monotonic receive timestamp in nanoseconds.
     *
     * This should usually be derived from std::chrono::steady_clock.
     *
     * Purpose:
     *
     * - heartbeat timeout,
     * - stale detection,
     * - latency measurement,
     * - packet interval measurement.
     *
     * Unlike wall time, monotonic time should not go backward.
     */
    std::uint64_t recv_monotonic_ns{0};

    /**
     * @brief Local connection identifier.
     *
     * This should increment whenever the WebSocket reconnects.
     *
     * Example:
     *
     * - connection_id = 1 for the first connection,
     * - connection_id = 2 after the first reconnect,
     * - connection_id = 3 after the next reconnect.
     *
     * This helps detect and debug state transitions around reconnects.
     */
    std::uint64_t connection_id{0};

    /**
     * @brief Local packet identifier.
     *
     * This is the sequence number assigned by this feed engine to raw packets
     * as they are captured.
     *
     * It is not the exchange sequence number.
     *
     * Use this field to:
     *
     * - preserve raw log ordering,
     * - generate replay traces,
     * - debug packet-level state transitions.
     */
    std::uint64_t packet_id{0};

    /**
     * @brief Logical source of the packet.
     *
     * Determines which decoder/normalizer should process the payload.
     */
    SourceId source_id{SourceId::Unknown};

    /**
     * @brief Compression codec used for the payload.
     *
     * For current WebSocket JSON payloads, this is usually Codec::None.
     */
    Codec codec{Codec::None};

    /**
     * @brief Packet-level bitmask flags.
     *
     * See PacketFlags for supported values.
     */
    std::uint32_t flags{PacketNone};

    /**
     * @brief Length of payload in bytes.
     *
     * RawLogReader uses this value to know exactly how many bytes to read after
     * the header.
     *
     * This must match payload.size() when writing.
     */
    std::uint32_t payload_len{0};

    /**
     * @brief CRC32 checksum of the payload bytes.
     *
     * RawLogWriter should compute this before writing the packet.
     * RawLogReader should recompute it after reading the payload.
     *
     * A mismatch means the raw record is corrupted or incomplete.
     */
    std::uint32_t payload_crc32{0};
};

/**
 * @brief In-memory representation of one raw packet.
 *
 * This combines:
 *
 * - RawPacketHeader: metadata needed for validation, replay, and routing;
 * - payload: the original raw message bytes stored as a string.
 *
 * The payload is intentionally raw. It should still be the exact WebSocket
 * message or raw source message, not a parsed JSON object.
 */
struct RawPacket {
    /**
     * @brief Packet metadata.
     */
    RawPacketHeader header;

    /**
     * @brief Original raw payload bytes.
     *
     * For Polymarket Market WebSocket, this is typically a JSON string such as:
     *
     *     {"type":"book", ...}
     *
     * But it may also be a heartbeat/control message depending on the source.
     */
    std::string payload;
};

/**
 * @brief Check whether a flag is set.
 */
bool has_flag(std::uint32_t flags, PacketFlags flag);

/**
 * @brief Add a packet flag.
 */
void set_flag(std::uint32_t& flags, PacketFlags flag);

/**
 * @brief Remove a packet flag.
 */
void clear_flag(std::uint32_t& flags, PacketFlags flag);

}  // namespace trading_engine::feed