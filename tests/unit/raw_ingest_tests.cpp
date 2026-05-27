#include "feed/raw_ingest/RawLogReader.h"
#include "feed/raw_ingest/RawLogWriter.h"
#include "feed/raw_ingest/RawPacket.h"

#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using trading_engine::feed::RawLogReadError;
using trading_engine::feed::RawLogReadResult;
using trading_engine::feed::RawLogReader;
using trading_engine::feed::RawLogWriter;
using trading_engine::feed::RawPacket;
using trading_engine::feed::RawPacketHeader;
using trading_engine::feed::RAW_PACKET_MAGIC;
using trading_engine::feed::RAW_PACKET_VERSION;
using trading_engine::feed::SourceId;
using trading_engine::feed::crc32;
using trading_engine::feed::make_raw_packet;

class TempRawFile {
public:
    explicit TempRawFile(std::string_view test_name)
        : path_(
              std::filesystem::temp_directory_path() /
              (
                  "polytope_raw_ingest_" +
                  std::string(test_name) +
                  "_" +
                  std::to_string(
                      std::chrono::steady_clock::now()
                          .time_since_epoch()
                          .count()
                  ) +
                  ".raw"
              )
          ) {
        cleanup();
    }

    TempRawFile(const TempRawFile&) = delete;
    TempRawFile& operator=(const TempRawFile&) = delete;

    ~TempRawFile() {
        cleanup();
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

    [[nodiscard]] std::string string() const {
        return path_.string();
    }

private:
    void cleanup() const noexcept {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

    std::filesystem::path path_;
};

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void expect_true(bool value, const std::string& message) {
    if (!value) {
        fail(message);
    }
}

template <typename T, typename U>
void expect_equal(const T& actual, const U& expected, const std::string& field) {
    if (!(actual == expected)) {
        fail("mismatch: " + field);
    }
}

void expect_packet_round_trip_equal(
    const RawPacket& actual,
    const RawPacket& expected
) {
    expect_equal(actual.header.magic, RAW_PACKET_MAGIC, "magic");
    expect_equal(actual.header.version, RAW_PACKET_VERSION, "version");
    expect_equal(
        actual.header.header_size,
        static_cast<std::uint16_t>(sizeof(RawPacketHeader)),
        "header_size"
    );
    expect_equal(
        actual.header.packet_id,
        expected.header.packet_id,
        "packet_id"
    );
    expect_equal(
        actual.header.source_id,
        expected.header.source_id,
        "source_id"
    );
    expect_equal(
        actual.header.connection_id,
        expected.header.connection_id,
        "connection_id"
    );
    expect_equal(actual.payload, expected.payload, "payload");
    expect_equal(
        actual.header.payload_len,
        static_cast<std::uint32_t>(actual.payload.size()),
        "payload_len"
    );
    expect_equal(
        actual.header.payload_len,
        expected.header.payload_len,
        "expected payload_len"
    );
    expect_equal(
        actual.header.payload_crc32,
        crc32(actual.payload),
        "recomputed payload_crc32"
    );
    expect_equal(
        actual.header.payload_crc32,
        expected.header.payload_crc32,
        "expected payload_crc32"
    );
}

void expect_read_error(
    const RawLogReadResult& result,
    RawLogReadError expected
) {
    expect_true(!result.packet.has_value(), "error result unexpectedly has packet");
    expect_equal(result.error, expected, "read error");
}

void write_packets(
    const std::filesystem::path& path,
    const std::vector<RawPacket>& packets
) {
    RawLogWriter writer(path.string());
    expect_true(writer.is_open(), "writer is not open");

    for (const auto& packet : packets) {
        writer.write_packet(packet);
    }

    writer.flush();
    expect_equal(
        writer.packets_written(),
        static_cast<std::uint64_t>(packets.size()),
        "packets_written"
    );
}

void write_raw_record(
    const std::filesystem::path& path,
    const RawPacketHeader& header,
    std::string_view payload
) {
    std::ofstream out(path, std::ios::binary | std::ios::out | std::ios::trunc);
    expect_true(out.is_open(), "raw test file is not open for writing");

    out.write(
        reinterpret_cast<const char*>(&header),
        static_cast<std::streamsize>(sizeof(RawPacketHeader))
    );

    if (!payload.empty()) {
        out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    }

    out.flush();
    expect_true(static_cast<bool>(out), "failed to write raw test record");
}

void write_partial_header(
    const std::filesystem::path& path,
    const RawPacketHeader& header
) {
    std::ofstream out(path, std::ios::binary | std::ios::out | std::ios::trunc);
    expect_true(out.is_open(), "raw test file is not open for writing");

    out.write(
        reinterpret_cast<const char*>(&header),
        static_cast<std::streamsize>(sizeof(RawPacketHeader) - 1)
    );
    out.flush();
    expect_true(static_cast<bool>(out), "failed to write partial header");
}

RawPacket sample_packet(
    SourceId source_id,
    std::uint64_t connection_id,
    std::uint64_t packet_id,
    std::string payload
) {
    return make_raw_packet(
        source_id,
        connection_id,
        packet_id,
        std::move(payload)
    );
}

void WriteThenReadOnePacket() {
    TempRawFile file("WriteThenReadOnePacket");
    const auto expected = sample_packet(
        SourceId::PolymarketMarket,
        17,
        42,
        R"({"type":"book","market":"m1"})"
    );

    write_packets(file.path(), {expected});

    RawLogReader reader(file.string());
    const auto result = reader.next();
    expect_true(result.ok(), "expected one readable packet");
    expect_packet_round_trip_equal(*result.packet, expected);
    expect_equal(reader.packets_read(), 1U, "packets_read");

    expect_read_error(reader.next(), RawLogReadError::EndOfFile);
}

void WriteThenReadMultiplePackets() {
    TempRawFile file("WriteThenReadMultiplePackets");
    const std::vector<RawPacket> expected{
        sample_packet(SourceId::PolymarketMarket, 7, 1, R"({"type":"book"})"),
        sample_packet(SourceId::PolymarketUser, 7, 2, R"({"type":"order"})"),
        sample_packet(SourceId::PolymarketSports, 8, 3, "")
    };

    write_packets(file.path(), expected);

    RawLogReader reader(file.string());
    for (const auto& packet : expected) {
        const auto result = reader.next();
        expect_true(result.ok(), "expected readable packet");
        expect_packet_round_trip_equal(*result.packet, packet);
    }

    expect_equal(
        reader.packets_read(),
        static_cast<std::uint64_t>(expected.size()),
        "packets_read"
    );
    expect_read_error(reader.next(), RawLogReadError::EndOfFile);
}

void DetectBadMagic() {
    TempRawFile file("DetectBadMagic");
    auto packet = sample_packet(SourceId::PolymarketMarket, 1, 1, "payload");
    packet.header.magic = 0xDEADBEEFu;

    write_raw_record(file.path(), packet.header, packet.payload);

    RawLogReader reader(file.string());
    expect_read_error(reader.next(), RawLogReadError::BadMagic);
}

void DetectCrcMismatch() {
    TempRawFile file("DetectCrcMismatch");
    auto packet = sample_packet(SourceId::PolymarketMarket, 1, 1, "payload");
    packet.header.payload_crc32 ^= 0xFFFFFFFFu;

    write_raw_record(file.path(), packet.header, packet.payload);

    RawLogReader reader(file.string());
    expect_read_error(reader.next(), RawLogReadError::CrcMismatch);
}

void DetectTruncatedHeader() {
    TempRawFile file("DetectTruncatedHeader");
    const auto packet = sample_packet(SourceId::PolymarketMarket, 1, 1, "payload");

    write_partial_header(file.path(), packet.header);

    RawLogReader reader(file.string());
    expect_read_error(reader.next(), RawLogReadError::TruncatedHeader);
}

void DetectTruncatedPayload() {
    TempRawFile file("DetectTruncatedPayload");
    const auto packet = sample_packet(SourceId::PolymarketMarket, 1, 1, "payload");

    write_raw_record(file.path(), packet.header, "pay");

    RawLogReader reader(file.string());
    expect_read_error(reader.next(), RawLogReadError::TruncatedPayload);
}

void ResetReader() {
    TempRawFile file("ResetReader");
    const std::vector<RawPacket> expected{
        sample_packet(SourceId::PolymarketMarket, 21, 100, R"({"type":"book"})"),
        sample_packet(SourceId::PolymarketUser, 22, 101, R"({"type":"trade"})")
    };

    write_packets(file.path(), expected);

    RawLogReader reader(file.string());
    for (const auto& packet : expected) {
        const auto result = reader.next();
        expect_true(result.ok(), "expected readable packet before reset");
        expect_packet_round_trip_equal(*result.packet, packet);
    }
    expect_read_error(reader.next(), RawLogReadError::EndOfFile);

    reader.reset();
    expect_equal(reader.packets_read(), 0U, "packets_read after reset");
    expect_equal(reader.bytes_read(), 0U, "bytes_read after reset");

    for (const auto& packet : expected) {
        const auto result = reader.next();
        expect_true(result.ok(), "expected readable packet after reset");
        expect_packet_round_trip_equal(*result.packet, packet);
    }
    expect_read_error(reader.next(), RawLogReadError::EndOfFile);
}

void WriteThenReadRealPolymarketPackets() {
    TempRawFile file("WriteThenReadRealPolymarketPackets");

    const std::vector<std::string> payloads{
        R"({"market":"0x1fad72fae204143ff1c3035e99e7c0f65ea8d5cd9bd1070987bd1a3316f772be","price_changes":[{"asset_id":"98022490269692409998126496127597032490334070080325855126491859374983463996227","price":"0.12","size":"2791.41","side":"BUY","hash":"832581e0c2df5f89944c9e65fdc25b083e0926c8","best_bid":"0.54","best_ask":"0.55"},{"asset_id":"53831553061883006530739877284105938919721408776239639687877978808906551086026","price":"0.88","size":"2791.41","side":"SELL","hash":"0694212aa7a741d3537e90e87407f7ec0dc1ce2f","best_bid":"0.45","best_ask":"0.46"}],"timestamp":"1779862966670","event_type":"price_change"})",
        R"({"market":"0x1fad72fae204143ff1c3035e99e7c0f65ea8d5cd9bd1070987bd1a3316f772be","price_changes":[{"asset_id":"98022490269692409998126496127597032490334070080325855126491859374983463996227","price":"0.07","size":"2928.57","side":"BUY","hash":"832581e0c2df5f89944c9e65fdc25b083e0926c8","best_bid":"0.54","best_ask":"0.55"},{"asset_id":"53831553061883006530739877284105938919721408776239639687877978808906551086026","price":"0.93","size":"2928.57","side":"SELL","hash":"0694212aa7a741d3537e90e87407f7ec0dc1ce2f","best_bid":"0.45","best_ask":"0.46"}],"timestamp":"1779862966670","event_type":"price_change"})"
    };

    std::vector<RawPacket> expected;
    expected.reserve(payloads.size());

    for (std::size_t i = 0; i < payloads.size(); ++i) {
        expected.push_back(sample_packet(
            SourceId::PolymarketMarket,
            1,
            static_cast<std::uint64_t>(i + 1),
            payloads[i]
        ));
    }

    write_packets(file.path(), expected);

    RawLogReader reader(file.string());
    std::uint64_t previous_packet_id = 0;

    for (const auto& packet : expected) {
        const auto result = reader.next();
        expect_true(result.ok(), "expected readable Polymarket packet");
        expect_packet_round_trip_equal(*result.packet, packet);
        expect_true(
            result.packet->header.packet_id > previous_packet_id,
            "packet_id is not strictly increasing"
        );
        previous_packet_id = result.packet->header.packet_id;
    }

    expect_read_error(reader.next(), RawLogReadError::EndOfFile);
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"WriteThenReadOnePacket", &WriteThenReadOnePacket},
        {"WriteThenReadMultiplePackets", &WriteThenReadMultiplePackets},
        {"WriteThenReadRealPolymarketPackets", &WriteThenReadRealPolymarketPackets},
        {"DetectBadMagic", &DetectBadMagic},
        {"DetectCrcMismatch", &DetectCrcMismatch},
        {"DetectTruncatedHeader", &DetectTruncatedHeader},
        {"DetectTruncatedPayload", &DetectTruncatedPayload},
        {"ResetReader", &ResetReader}
    };
    return test_map;
}

int run_test(const std::string& name) {
    const auto it = tests().find(name);
    if (it == tests().end()) {
        std::cerr << "unknown test: " << name << '\n';
        return 2;
    }

    try {
        it->second();
    } catch (const std::exception& error) {
        std::cerr << name << " failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << name << " passed\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2) {
        return run_test(argv[1]);
    }

    int failures = 0;
    for (const auto& [name, _] : tests()) {
        failures += run_test(name) == 0 ? 0 : 1;
    }

    return failures == 0 ? 0 : 1;
}
