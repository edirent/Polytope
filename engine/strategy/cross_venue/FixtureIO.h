#pragma once
#include "engine/strategy/cross_venue/PairScanner.h"
#include <boost/json.hpp>
#include <filesystem>
#include <optional>

namespace trading_engine::cross_venue {
struct BookScenario {
    std::string name;
    std::uint64_t now_ns = 0;
    std::vector<BookUpdate> updates;
};
struct BookFixture {
    ApprovedPair pair;
    PairPolicy policy;
    std::array<Money, 2> starting_cash{};
    std::vector<FeeQuote> fees;
    std::vector<BookScenario> scenarios;
};
struct FillScenario {
    std::string name;
    std::uint64_t cutoff_ns = 0;
    std::int64_t planned_quantity = 100;
    std::optional<bool> illustrative_yes;
    std::vector<FillReceipt> receipts;
};
struct FillFixture {
    ApprovedPair pair;
    std::array<Money, 2> starting_cash{};
    std::vector<FillScenario> scenarios;
};
std::string read_bytes(const std::filesystem::path& path);
BookFixture load_books(const std::filesystem::path& path);
FillFixture load_fills(const std::filesystem::path& path);
PairPolicy load_policy(const std::filesystem::path& path);
ApprovedPair parse_pair(const boost::json::object& object, bool require_synthetic = true);
std::string csv(std::string_view value);
}  // namespace trading_engine::cross_venue
