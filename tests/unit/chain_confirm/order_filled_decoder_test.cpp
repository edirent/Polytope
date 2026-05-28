#include "chain_confirm/EthLog.h"
#include "chain_confirm/OrderFilledDecoder.h"

#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::chain_confirm::ConfirmedDirection;
using trading_engine::chain_confirm::FillMappingStatus;
using trading_engine::chain_confirm::OrderFilledDecoder;
using trading_engine::chain_confirm::EthLog;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void expect_true(bool value, const std::string& field) {
    if (!value) {
        fail("expected true: " + field);
    }
}

void expect_false(bool value, const std::string& field) {
    if (value) {
        fail("expected false: " + field);
    }
}

template <typename Actual, typename Expected>
void expect_equal(
    const Actual& actual,
    const Expected& expected,
    const std::string& field
) {
    if (!(actual == expected)) {
        fail("mismatch: " + field);
    }
}

std::string word(std::uint64_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << std::setfill('0') << std::setw(64) << value;
    return out.str();
}

std::string topic_address(const std::string& address40) {
    return "0x" + std::string(24, '0') + address40;
}

std::string join_data(std::initializer_list<std::uint64_t> values) {
    std::string data{"0x"};
    for (std::uint64_t value : values) {
        data += word(value).substr(2);
    }
    return data;
}

EthLog buy_log(bool removed = false) {
    EthLog log;
    log.topics = {
        OrderFilledDecoder::kOrderFilledTopic0,
        word(0xabc),
        topic_address("1111111111111111111111111111111111111111"),
        topic_address("2222222222222222222222222222222222222222")
    };
    log.data = join_data({
        0,      // makerAssetId: buy-side semantics
        12345,  // takerAssetId
        500,    // maker amount
        1000,   // taker amount
        7
    });
    log.block_number = 123;
    log.tx_hash = word(0xfeed);
    log.log_index = 4;
    log.removed = removed;
    return log;
}

EthLog sell_log() {
    EthLog log = buy_log();
    log.data = join_data({
        12345,  // makerAssetId
        0,      // takerAssetId: sell-side semantics
        1000,   // maker amount
        500,    // taker amount
        7
    });
    return log;
}

void OrderFilledDecoder_DecodesFields() {
    const OrderFilledDecoder decoder;
    const auto decoded = decoder.decode(buy_log());

    expect_true(decoded.ok, "decode ok");
    expect_equal(decoded.event.maker_asset_id, std::string{"0"}, "maker asset");
    expect_equal(decoded.event.taker_asset_id, std::string{"12345"}, "taker asset");
    expect_equal(decoded.event.maker_amount_filled, 500ULL, "maker amount");
    expect_equal(decoded.event.taker_amount_filled, 1000ULL, "taker amount");
    expect_equal(decoded.event.fee, 7ULL, "fee");
    expect_equal(decoded.event.block_number, 123ULL, "block number");
    expect_equal(decoded.event.log_index, 4U, "log index");
    expect_false(decoded.event.removed, "removed");
}

void OrderFilledDecoder_RejectsWrongTopic() {
    const OrderFilledDecoder decoder;
    EthLog log = buy_log();
    log.topics[0] = word(0x1234);

    const auto decoded = decoder.decode(log);
    expect_false(decoded.ok, "decode ok");
}

void OrderFilledDecoder_HandlesRemovedLog() {
    const OrderFilledDecoder decoder;
    const auto decoded = decoder.decode_confirmed_fill(
        buy_log(true),
        "market-a",
        1000
    );

    expect_true(decoded.ok, "confirmed fill ok");
    expect_true(decoded.fill.removed, "removed");
}

void OrderFilledDecoder_DirectionFromAssetFields() {
    const OrderFilledDecoder decoder;
    const auto buy = decoder.decode_confirmed_fill(
        buy_log(),
        "market-a",
        1000
    );
    const auto sell = decoder.decode_confirmed_fill(
        sell_log(),
        "market-a",
        1000
    );

    expect_true(buy.ok, "buy ok");
    expect_equal(
        buy.fill.direction,
        ConfirmedDirection::BuyAggressor,
        "buy direction"
    );
    expect_equal(buy.fill.asset_id, std::string{"12345"}, "buy asset");
    expect_equal(buy.fill.price_tick, 500000LL, "buy price");
    expect_equal(buy.fill.size_lots, 1000LL, "buy size");
    expect_equal(
        buy.fill.mapping_status,
        FillMappingStatus::Mapped,
        "buy mapped"
    );

    expect_true(sell.ok, "sell ok");
    expect_equal(
        sell.fill.direction,
        ConfirmedDirection::SellAggressor,
        "sell direction"
    );
    expect_equal(sell.fill.asset_id, std::string{"12345"}, "sell asset");
    expect_equal(sell.fill.price_tick, 500000LL, "sell price");
    expect_equal(sell.fill.size_lots, 1000LL, "sell size");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"OrderFilledDecoder_DecodesFields", &OrderFilledDecoder_DecodesFields},
        {"OrderFilledDecoder_RejectsWrongTopic", &OrderFilledDecoder_RejectsWrongTopic},
        {"OrderFilledDecoder_HandlesRemovedLog", &OrderFilledDecoder_HandlesRemovedLog},
        {"OrderFilledDecoder_DirectionFromAssetFields", &OrderFilledDecoder_DirectionFromAssetFields}
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
