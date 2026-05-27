#include "feed/raw_ingest/RawPacket.h"

#include <chrono>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <utility>

namespace trading_engine::feed {

/**
 * @brief Return current wall-clock time in nanoseconds.
 *
 * This timestamp is derived from std::chrono::system_clock.
 *
 * Use this timestamp for:
 *
 * - human-readable logs,
 * - aligning packets with real-world time,
 * - debugging when a packet was received in calendar time.
 *
 * Do not use this timestamp for timeout or latency logic.
 *
 * Reason:
 *
 * std::chrono::system_clock can move forward or backward if the operating
 * system adjusts time through NTP, manual clock changes, VM clock correction,
 * or other system-level synchronization.
 *
 * For timeout/latency/stale detection, use now_monotonic_ns().
 *
 * @return Current wall-clock timestamp in nanoseconds since Unix epoch.
 */
std::uint64_t now_wall_ns() {
    const auto now = std::chrono::system_clock::now();

    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()
    );

    return static_cast<std::uint64_t>(ns.count());
}

/**
 * @brief Return current monotonic time in nanoseconds.
 *
 * This timestamp is derived from std::chrono::steady_clock.
 *
 * Use this timestamp for:
 *
 * - heartbeat timeout,
 * - stale feed detection,
 * - latency measurement,
 * - packet interval measurement,
 * - reconnect backoff timing.
 *
 * Unlike system_clock, steady_clock is intended to be monotonic. That means it
 * should not go backward during process execution.
 *
 * Important:
 *
 * The absolute value of steady_clock time is not meaningful. Only differences
 * between two monotonic timestamps are meaningful.
 *
 * Example:
 *
 *     auto dt = now_monotonic_ns() - packet.header.recv_monotonic_ns;
 *
 * @return Current monotonic timestamp in nanoseconds.
 */
std::uint64_t now_monotonic_ns() {
    const auto now = std::chrono::steady_clock::now();

    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()
    );

    return static_cast<std::uint64_t>(ns.count());
}

/**
 * @brief Compute CRC32 checksum for a raw byte buffer.
 *
 * This implementation uses the standard reflected CRC32 polynomial:
 *
 *     0xEDB88320
 *
 * This is the same polynomial commonly used by Ethernet, gzip, PNG, and many
 * other systems.
 *
 * Purpose inside the feed engine:
 *
 * Raw logs are written as:
 *
 *     [RawPacketHeader][payload bytes]
 *
 * The RawPacketHeader stores payload_crc32.
 *
 * During replay, RawLogReader can recompute CRC32 over the payload bytes and
 * compare it against header.payload_crc32.
 *
 * If the values differ, then the raw log entry is corrupted, truncated,
 * misaligned, or otherwise unsafe.
 *
 * This is a correctness check, not a cryptographic hash.
 *
 * Performance note:
 *
 * This is a simple bit-by-bit CRC implementation. It is correct and fine for
 * MVP development, but not optimized. Later, if raw ingest throughput becomes
 * important, replace this with:
 *
 * - table-based CRC32,
 * - hardware CRC32 instruction,
 * - or a library implementation.
 *
 * @param data Pointer to raw bytes.
 * @param size Number of bytes to checksum.
 *
 * @return CRC32 checksum.
 */
std::uint32_t crc32(const char* data, std::size_t size) {
    // Standard reflected CRC32 polynomial.
    constexpr std::uint32_t polynomial = 0xEDB88320u;

    // CRC32 starts with all bits set.
    std::uint32_t crc = 0xFFFFFFFFu;

    for (std::size_t i = 0; i < size; ++i) {
        // Convert char to unsigned byte explicitly.
        //
        // This avoids platform-dependent signed-char behavior.
        // Some platforms treat char as signed, some as unsigned.
        //
        // We want the payload byte value, not a sign-extended integer.
        const auto byte = static_cast<std::uint32_t>(
            static_cast<unsigned char>(data[i])
        );

        // Mix the next byte into the low bits of the CRC state.
        crc ^= byte;

        // Process each bit in the byte.
        for (int bit = 0; bit < 8; ++bit) {
            const bool lsb_set = (crc & 1u) != 0;

            crc >>= 1u;

            if (lsb_set) {
                crc ^= polynomial;
            }
        }
    }

    // Final XOR step.
    return crc ^ 0xFFFFFFFFu;
}

/**
 * @brief Compute CRC32 checksum for a std::string payload.
 *
 * This is a convenience overload around:
 *
 *     crc32(payload.data(), payload.size())
 *
 * It should be used when computing checksums for RawPacket::payload.
 *
 * @param payload Raw payload string.
 *
 * @return CRC32 checksum.
 */
std::uint32_t crc32(const std::string& payload) {
    return crc32(payload.data(), payload.size());
}

/**
 * @brief Construct a complete RawPacket from raw payload bytes.
 *
 * This is the central helper used by raw ingest.
 *
 * WebSocketClient should not manually fill RawPacketHeader fields all over
 * the codebase. Instead, it should call this function so every raw packet is
 * constructed consistently.
 *
 * This function fills:
 *
 * - magic,
 * - version,
 * - header_size,
 * - wall-clock receive timestamp,
 * - monotonic receive timestamp,
 * - connection_id,
 * - packet_id,
 * - source_id,
 * - codec,
 * - flags,
 * - payload_len,
 * - payload_crc32,
 * - payload bytes.
 *
 * Important distinction:
 *
 * packet_id is local to this feed engine. It is not an exchange sequence
 * number. If Polymarket or another venue provides its own sequence number,
 * that should be extracted later by the decoder/normalizer and stored in a
 * decoded event structure, not here.
 *
 * @param source_id Logical source that produced this packet.
 * @param connection_id Local connection id. Increment this after reconnect.
 * @param packet_id Local raw packet id. Usually monotonically increasing.
 * @param payload Original raw WebSocket/source payload.
 * @param codec Compression codec used by the payload.
 * @param flags Packet-level flags.
 *
 * @throws std::length_error if payload is too large to fit in uint32_t.
 *
 * @return Fully initialized RawPacket.
 */
RawPacket make_raw_packet(
    SourceId source_id,
    std::uint64_t connection_id,
    std::uint64_t packet_id,
    std::string payload,
    Codec codec,
    std::uint32_t flags
) {
    // RawPacketHeader stores payload_len as uint32_t.
    //
    // If payload.size() exceeds uint32_t max, writing it into payload_len would
    // truncate the size and corrupt the raw log format.
    //
    // That would cause RawLogReader to read the wrong number of bytes and lose
    // packet boundary alignment.
    if (payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error(
            "RawPacket payload is too large for uint32_t payload_len"
        );
    }

    RawPacket packet;

    // Move the payload into the packet to avoid an unnecessary copy.
    //
    // This matters because WebSocket payloads can be large, and raw ingest is
    // on the hot path.
    packet.payload = std::move(payload);

    // Fixed raw packet format fields.
    //
    // These allow RawLogReader to identify and validate records.
    packet.header.magic = RAW_PACKET_MAGIC;
    packet.header.version = RAW_PACKET_VERSION;
    packet.header.header_size = sizeof(RawPacketHeader);

    // Record both wall time and monotonic time.
    //
    // wall time:
    //     useful for logs and human debugging.
    //
    // monotonic time:
    //     useful for timeouts, latency, and stale detection.
    packet.header.recv_wall_ns = now_wall_ns();
    packet.header.recv_monotonic_ns = now_monotonic_ns();

    // Record connection identity and local packet ordering.
    //
    // connection_id tells us which WebSocket connection produced this packet.
    // packet_id tells us the local raw-log ordering.
    packet.header.connection_id = connection_id;
    packet.header.packet_id = packet_id;

    // Source/routing metadata.
    //
    // source_id decides which decoder/normalizer should handle this payload.
    // codec decides whether decompression is required before decoding.
    // flags carries packet-level metadata such as heartbeat/replayed/unsafe.
    packet.header.source_id = source_id;
    packet.header.codec = codec;
    packet.header.flags = flags;

    // Payload metadata.
    //
    // payload_len lets RawLogReader know exactly how many bytes to read.
    // payload_crc32 lets RawLogReader verify payload integrity.
    packet.header.payload_len =
        static_cast<std::uint32_t>(packet.payload.size());

    packet.header.payload_crc32 = crc32(packet.payload);

    return packet;
}

/**
 * @brief Check whether a packet flag is set.
 *
 * PacketFlags is used as a bitmask. A packet can have multiple flags at once.
 *
 * Example:
 *
 *     if (has_flag(packet.header.flags, PacketHeartbeat)) {
 *         // Handle heartbeat packet.
 *     }
 *
 * @param flags Raw integer flags field from RawPacketHeader.
 * @param flag Specific flag to check.
 *
 * @return true if the flag is set; false otherwise.
 */
bool has_flag(std::uint32_t flags, PacketFlags flag) {
    return (flags & static_cast<std::uint32_t>(flag)) != 0;
}

/**
 * @brief Set a packet flag in-place.
 *
 * Example:
 *
 *     set_flag(packet.header.flags, PacketUnsafe);
 *
 * This preserves existing flags and adds the requested one.
 *
 * @param flags Mutable flags field.
 * @param flag Flag to add.
 */
void set_flag(std::uint32_t& flags, PacketFlags flag) {
    flags |= static_cast<std::uint32_t>(flag);
}

/**
 * @brief Clear a packet flag in-place.
 *
 * Example:
 *
 *     clear_flag(packet.header.flags, PacketDecodeErr);
 *
 * This removes the requested flag while preserving all other flags.
 *
 * @param flags Mutable flags field.
 * @param flag Flag to remove.
 */
void clear_flag(std::uint32_t& flags, PacketFlags flag) {
    flags &= ~static_cast<std::uint32_t>(flag);
}

}  // namespace trading_engine::feed