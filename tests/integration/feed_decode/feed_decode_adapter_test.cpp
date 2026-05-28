#include "feed_decode_test_utils.h"

#include <exception>
#include <iostream>

namespace {

void FeedDecodeAdapter_PreservesRawPacketMetadata() {
    auto packet = feed_decode_test::make_test_packet(R"({"event_type":"book"})");
    const auto input = trading_engine::feed::to_decode_input_view(packet);

    feed_decode_test::expect_equal(
        input.packet_id,
        packet.header.packet_id,
        "packet_id"
    );
    feed_decode_test::expect_equal(
        input.connection_id,
        packet.header.connection_id,
        "connection_id"
    );
    feed_decode_test::expect_equal(
        input.source_id,
        packet.header.source_id,
        "source_id"
    );
    feed_decode_test::expect_equal(
        input.recv_wall_ns,
        packet.header.recv_wall_ns,
        "recv_wall_ns"
    );
    feed_decode_test::expect_equal(
        input.recv_monotonic_ns,
        packet.header.recv_monotonic_ns,
        "recv_monotonic_ns"
    );
    feed_decode_test::expect_equal(
        input.codec,
        static_cast<std::uint16_t>(packet.header.codec),
        "codec"
    );
    feed_decode_test::expect_equal(input.flags, packet.header.flags, "flags");
    feed_decode_test::expect_equal(
        input.payload.size(),
        packet.payload.size(),
        "payload length"
    );
    feed_decode_test::expect_true(
        input.payload.data() == packet.payload.data(),
        "payload view points to original payload"
    );
}

}  // namespace

int main() {
    try {
        FeedDecodeAdapter_PreservesRawPacketMetadata();
    } catch (const std::exception& error) {
        std::cerr
            << "FeedDecodeAdapter_PreservesRawPacketMetadata failed: "
            << error.what() << '\n';
        return 1;
    }

    std::cout << "FeedDecodeAdapter_PreservesRawPacketMetadata passed\n";
    return 0;
}
