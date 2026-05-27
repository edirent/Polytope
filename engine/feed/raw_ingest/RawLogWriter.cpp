#include "feed/raw_ingest/RawLogWriter.h"

#include <ios>
#include <limits>
#include <stdexcept>
#include <streambuf>
#include <type_traits>

namespace trading_engine::feed {

namespace {

/**
 * @brief Validate that RawPacketHeader can be safely written as raw bytes.
 *
 * In this MVP, RawLogWriter writes RawPacketHeader directly into the binary
 * raw log file using reinterpret_cast<const char*>(&header).
 *
 * That requires RawPacketHeader to be trivially copyable.
 *
 * If someone later adds std::string, std::vector, virtual functions, or other
 * non-trivial fields to RawPacketHeader, this static_assert will fail and
 * prevent silently writing invalid binary data.
 */
static_assert(
    std::is_trivially_copyable_v<RawPacketHeader>,
    "RawPacketHeader must be trivially copyable for binary raw-log writes"
);

/**
 * @brief Validate packet metadata before writing to disk.
 *
 * RawLogWriter should not blindly write malformed packets.
 *
 * If a bad packet reaches the writer and gets persisted, then replay will
 * later fail or, worse, produce misleading results.
 *
 * This function checks the fields that define the raw log format:
 *
 * - magic: confirms this is a Helix raw packet;
 * - version: confirms the writer/reader format version;
 * - header_size: confirms the binary header size;
 * - payload_len: confirms payload boundary information is correct;
 * - payload_crc32: confirms payload integrity metadata is correct.
 *
 * This validation is defensive. Even if make_raw_packet() already filled these
 * fields correctly, validating here protects against:
 *
 * - manually constructed RawPacket objects;
 * - corrupted in-memory packet data;
 * - tests that accidentally build invalid packets;
 * - future code paths that skip make_raw_packet().
 *
 * @param packet RawPacket to validate.
 *
 * @throws std::runtime_error if packet metadata is invalid.
 */
void validate_packet_before_write(const RawPacket& packet) {
    if (packet.header.magic != RAW_PACKET_MAGIC) {
        throw std::runtime_error(
            "RawLogWriter refused to write packet: invalid magic"
        );
    }

    if (packet.header.version != RAW_PACKET_VERSION) {
        throw std::runtime_error(
            "RawLogWriter refused to write packet: unsupported raw packet version"
        );
    }

    if (packet.header.header_size != sizeof(RawPacketHeader)) {
        throw std::runtime_error(
            "RawLogWriter refused to write packet: invalid header size"
        );
    }

    if (packet.payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error(
            "RawLogWriter refused to write packet: payload too large"
        );
    }

    const auto actual_payload_len =
        static_cast<std::uint32_t>(packet.payload.size());

    if (packet.header.payload_len != actual_payload_len) {
        throw std::runtime_error(
            "RawLogWriter refused to write packet: payload_len does not match payload size"
        );
    }

    const auto actual_crc = crc32(packet.payload);

    if (packet.header.payload_crc32 != actual_crc) {
        throw std::runtime_error(
            "RawLogWriter refused to write packet: payload CRC32 mismatch"
        );
    }
}

/**
 * @brief Convert a size_t byte count into std::streamsize safely.
 *
 * std::ostream::write expects std::streamsize, which is signed.
 *
 * On normal 64-bit platforms this will not be a practical issue for our MVP,
 * because payload_len is uint32_t. Still, this helper keeps the conversion
 * explicit and prevents silent overflow if the platform has a smaller
 * streamsize type.
 *
 * @param size Number of bytes to write.
 *
 * @return size converted to std::streamsize.
 *
 * @throws std::runtime_error if size cannot fit in std::streamsize.
 */
std::streamsize to_stream_size(std::size_t size) {
    const auto max_stream_size =
        static_cast<std::uint64_t>(
            std::numeric_limits<std::streamsize>::max()
        );

    if (static_cast<std::uint64_t>(size) > max_stream_size) {
        throw std::runtime_error(
            "RawLogWriter cannot write buffer: size exceeds std::streamsize max"
        );
    }

    return static_cast<std::streamsize>(size);
}

}  // namespace

/**
 * @brief Open an append-only binary raw log file.
 *
 * The writer opens the file in:
 *
 * - binary mode:
 *      Required because the file contains RawPacketHeader bytes and raw payload
 *      bytes. Text mode may transform newlines on some platforms.
 *
 * - append mode:
 *      New packets are appended to the end of the file. Existing raw data is
 *      not overwritten.
 *
 * This means packets_written_ and bytes_written_ count only packets written
 * by this RawLogWriter instance, not packets already present in the file.
 *
 * If you want a clean file in tests, delete the file before constructing the
 * writer or use a temporary path.
 *
 * @param path Filesystem path to the raw log file.
 *
 * @throws std::runtime_error if the file cannot be opened.
 */
RawLogWriter::RawLogWriter(const std::string& path)
    : out_(path, std::ios::binary | std::ios::out | std::ios::app) {
    if (!out_.is_open()) {
        throw std::runtime_error("RawLogWriter failed to open file: " + path);
    }
}

/**
 * @brief Flush buffered bytes on destruction.
 *
 * Destructors should not throw.
 *
 * We attempt to flush the stream, but deliberately do not throw if flushing
 * fails here. Runtime write errors should be handled in write_packet() or an
 * explicit flush() call.
 */
RawLogWriter::~RawLogWriter() {
    try {
        if (out_.is_open()) {
            out_.flush();
        }
    } catch (...) {
        // Destructors must not allow exceptions to escape.
    }
}

/**
 * @brief Return whether the underlying file stream is open.
 *
 * This only checks whether the file handle is open. It does not guarantee that
 * the next write will succeed.
 *
 * @return true if the output file is open; false otherwise.
 */
bool RawLogWriter::is_open() const noexcept {
    return out_.is_open();
}

/**
 * @brief Append one RawPacket to the binary raw log file.
 *
 * File layout for each packet:
 *
 *     [RawPacketHeader][payload bytes]
 *
 * This function does not decode the payload and does not understand market
 * events. It only persists the raw packet exactly as received.
 *
 * Steps:
 *
 * 1. Validate writer is open.
 * 2. Validate packet header and payload metadata.
 * 3. Write RawPacketHeader bytes.
 * 4. Write payload bytes.
 * 5. Check stream state.
 * 6. Update write counters.
 *
 * Important:
 *
 * This function does not call fsync(). It writes through std::ofstream.
 * For MVP this is acceptable. If you later need stronger crash guarantees,
 * add an explicit fsync-based writer or a flush policy.
 *
 * @param packet RawPacket to append.
 *
 * @throws std::runtime_error if validation or writing fails.
 */
void RawLogWriter::write_packet(const RawPacket& packet) {
    if (!out_.is_open()) {
        throw std::runtime_error(
            "RawLogWriter cannot write packet: file is not open"
        );
    }

    validate_packet_before_write(packet);

    const auto* header_bytes =
        reinterpret_cast<const char*>(&packet.header);

    out_.write(
        header_bytes,
        to_stream_size(sizeof(RawPacketHeader))
    );

    if (!out_) {
        throw std::runtime_error(
            "RawLogWriter failed while writing RawPacketHeader"
        );
    }

    if (!packet.payload.empty()) {
        out_.write(
            packet.payload.data(),
            to_stream_size(packet.payload.size())
        );

        if (!out_) {
            throw std::runtime_error(
                "RawLogWriter failed while writing payload bytes"
            );
        }
    }

    ++packets_written_;

    bytes_written_ +=
        static_cast<std::uint64_t>(sizeof(RawPacketHeader)) +
        static_cast<std::uint64_t>(packet.payload.size());
}

/**
 * @brief Flush the output stream.
 *
 * This pushes buffered bytes from the C++ stream buffer to the operating
 * system.
 *
 * It does not necessarily force data all the way to physical storage.
 * For that stronger guarantee, you would need platform-specific fsync logic.
 *
 * @throws std::runtime_error if flushing fails.
 */
void RawLogWriter::flush() {
    if (!out_.is_open()) {
        throw std::runtime_error(
            "RawLogWriter cannot flush: file is not open"
        );
    }

    out_.flush();

    if (!out_) {
        throw std::runtime_error(
            "RawLogWriter failed to flush output stream"
        );
    }
}

/**
 * @brief Return number of packets written by this writer instance.
 *
 * This does not include packets that may already have existed in the file
 * before this writer opened it in append mode.
 *
 * @return Packet count written by this instance.
 */
std::uint64_t RawLogWriter::packets_written() const noexcept {
    return packets_written_;
}

/**
 * @brief Return number of bytes written by this writer instance.
 *
 * Each packet contributes:
 *
 *     sizeof(RawPacketHeader) + payload.size()
 *
 * This does not include bytes that existed in the file before this writer was
 * opened.
 *
 * @return Byte count written by this instance.
 */
std::uint64_t RawLogWriter::bytes_written() const noexcept {
    return bytes_written_;
}

}  // namespace trading_engine::feed