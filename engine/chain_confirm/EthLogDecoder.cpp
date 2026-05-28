#include "chain_confirm/EthLogDecoder.h"

#include <algorithm>
#include <cctype>
#include <limits>

#include <boost/multiprecision/cpp_int.hpp>

namespace trading_engine::chain_confirm::eth_log_decoder {

namespace {

bool is_hex_char(char c) {
    return std::isxdigit(static_cast<unsigned char>(c)) != 0;
}

boost::multiprecision::cpp_int parse_hex_word(const std::string& word) {
    const std::string normalized = normalize_hex(word);
    boost::multiprecision::cpp_int value = 0;

    for (char c : normalized.substr(2)) {
        value <<= 4;

        if (c >= '0' && c <= '9') {
            value += c - '0';
        } else if (c >= 'a' && c <= 'f') {
            value += 10 + (c - 'a');
        } else if (c >= 'A' && c <= 'F') {
            value += 10 + (c - 'A');
        }
    }

    return value;
}

}  // namespace

std::string normalize_hex(std::string value) {
    if (value.size() >= 2 && value[0] == '0' &&
        (value[1] == 'x' || value[1] == 'X')) {
        value[1] = 'x';
    } else {
        value = "0x" + value;
    }

    std::transform(
        value.begin() + 2,
        value.end(),
        value.begin() + 2,
        [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        }
    );

    return value;
}

bool is_hex_32_word(const std::string& value) {
    const std::string normalized = normalize_hex(value);
    if (normalized.size() != 66) {
        return false;
    }

    return std::all_of(
        normalized.begin() + 2,
        normalized.end(),
        is_hex_char
    );
}

std::string topic_to_address(const std::string& topic) {
    const std::string normalized = normalize_hex(topic);
    if (!is_hex_32_word(normalized)) {
        return {};
    }

    return "0x" + normalized.substr(normalized.size() - 40);
}

std::vector<std::string> split_data_words(const std::string& data) {
    std::string normalized = normalize_hex(data);
    if (normalized == "0x") {
        return {};
    }

    const std::string body = normalized.substr(2);
    if (body.size() % 64 != 0) {
        return {};
    }

    std::vector<std::string> words;
    words.reserve(body.size() / 64);

    for (std::size_t offset = 0; offset < body.size(); offset += 64) {
        words.push_back("0x" + body.substr(offset, 64));
    }

    return words;
}

std::string uint256_to_decimal_string(const std::string& word) {
    if (!is_hex_32_word(word)) {
        return {};
    }

    return parse_hex_word(word).convert_to<std::string>();
}

std::optional<std::uint64_t> uint256_to_u64(const std::string& word) {
    if (!is_hex_32_word(word)) {
        return std::nullopt;
    }

    const auto value = parse_hex_word(word);
    if (value > std::numeric_limits<std::uint64_t>::max()) {
        return std::nullopt;
    }

    return value.convert_to<std::uint64_t>();
}

}  // namespace trading_engine::chain_confirm::eth_log_decoder
