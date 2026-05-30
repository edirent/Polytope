#include "oracle/bundles/CandidateBundleGenerator.h"
#include "oracle/ingestion/MarketApiClient.h"
#include "oracle/ingestion/MarketDescriptionLoader.h"
#include "oracle/ingestion/MarketUniverseBuilder.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace {

using trading_engine::oracle::MarketDescriptionLoader;
using trading_engine::oracle::MarketUniverseBuilder;
using trading_engine::oracle::CandidateBundleGenerator;
using trading_engine::oracle::MarketApiClient;
using trading_engine::oracle::MarketApiFetchOptions;
using trading_engine::oracle::RawMarketRecord;
using trading_engine::oracle::to_jsonl_line;

struct Config {
    std::string fixture_path;
    std::string out_path;
    std::string candidate_bundles_out_path;
    bool polymarket_live = false;
    MarketApiFetchOptions api_options;
};

std::uint32_t parse_u32(const std::string& value, const char* option) {
    try {
        std::size_t consumed = 0;
        const auto parsed = std::stoul(value, &consumed);
        if (consumed != value.size()) {
            throw std::runtime_error("");
        }
        return static_cast<std::uint32_t>(parsed);
    } catch (...) {
        throw std::runtime_error(std::string(option) + " requires uint32");
    }
}

bool parse_bool(const std::string& value, const char* option) {
    if (value == "true" || value == "1") {
        return true;
    }
    if (value == "false" || value == "0") {
        return false;
    }
    throw std::runtime_error(std::string(option) + " requires true/false");
}

Config parse_args(int argc, char** argv) {
    Config config;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&](const char* option) -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error(std::string(option) + " requires value");
            }
            return argv[++i];
        };

        if (arg == "--fixture") {
            config.fixture_path = value("--fixture");
        } else if (arg == "--polymarket-live") {
            config.polymarket_live = true;
        } else if (arg == "--out") {
            config.out_path = value("--out");
        } else if (arg == "--candidate-bundles-out") {
            config.candidate_bundles_out_path =
                value("--candidate-bundles-out");
        } else if (arg == "--limit") {
            config.api_options.limit = parse_u32(value("--limit"), "--limit");
        } else if (arg == "--offset") {
            config.api_options.offset = parse_u32(value("--offset"), "--offset");
        } else if (arg == "--active") {
            config.api_options.active = parse_bool(value("--active"), "--active");
        } else if (arg == "--closed") {
            config.api_options.closed = parse_bool(value("--closed"), "--closed");
        } else if (arg == "--archived") {
            config.api_options.archived =
                parse_bool(value("--archived"), "--archived");
        } else if (arg == "--allow-no-orderbook") {
            config.api_options.require_order_book = false;
        } else if (arg == "--help" || arg == "-h") {
            throw std::runtime_error(
                "usage: fetch_market_universe "
                "(--fixture PATH | --polymarket-live) --out PATH "
                "[--limit N] [--offset N] [--active true|false] "
                "[--closed true|false] [--archived true|false] "
                "[--candidate-bundles-out PATH]"
            );
        } else {
            throw std::runtime_error("unknown argument: " + arg);
        }
    }

    if (config.fixture_path.empty() == !config.polymarket_live) {
        throw std::runtime_error(
            "exactly one of --fixture or --polymarket-live is required"
        );
    }
    if (config.out_path.empty()) {
        throw std::runtime_error("--out is required");
    }

    return config;
}

std::unordered_set<std::string> known_market_ids(
    const std::vector<RawMarketRecord>& records
) {
    std::unordered_set<std::string> out;
    for (const auto& record : records) {
        out.insert(record.market_id);
    }
    return out;
}

std::unordered_set<std::string> known_asset_ids(
    const std::vector<RawMarketRecord>& records
) {
    std::unordered_set<std::string> out;
    for (const auto& record : records) {
        for (const auto& asset_id : record.asset_ids) {
            out.insert(asset_id);
        }
    }
    return out;
}

int run(const Config& config) {
    std::vector<RawMarketRecord> records;
    std::size_t source_warning_count = 0;
    std::uint32_t pages_fetched = 0;

    if (config.polymarket_live) {
        MarketApiClient client;
        constexpr std::uint32_t kGammaPageLimit = 100;
        const auto target_records = config.api_options.limit;
        auto next_offset = config.api_options.offset;

        while (records.size() < target_records) {
            auto page_options = config.api_options;
            page_options.offset = next_offset;
            page_options.limit = std::min<std::uint32_t>(
                kGammaPageLimit,
                static_cast<std::uint32_t>(
                    target_records - records.size()
                )
            );

            const auto fetched = client.fetch_markets(page_options);
            ++pages_fetched;
            for (const auto& warning : fetched.warnings) {
                std::cerr << "warning: " << warning << '\n';
            }
            for (const auto& error : fetched.errors) {
                std::cerr << "error: " << error << '\n';
            }
            if (!fetched.ok()) {
                return 1;
            }

            source_warning_count += fetched.warnings.size();
            records.insert(
                records.end(),
                fetched.records.begin(),
                fetched.records.end()
            );

            if (fetched.response_count < page_options.limit ||
                fetched.response_count == 0) {
                break;
            }
            next_offset += page_options.limit;
        }
    } else {
        MarketDescriptionLoader loader;
        const auto loaded = loader.load_jsonl(config.fixture_path);

        for (const auto& warning : loaded.warnings) {
            std::cerr << "warning: " << warning << '\n';
        }
        for (const auto& error : loaded.errors) {
            std::cerr << "error: " << error << '\n';
        }
        if (!loaded.ok()) {
            return 1;
        }
        source_warning_count = loaded.warnings.size();
        records = loaded.records;
    }

    MarketUniverseBuilder builder;
    const auto built = builder.build(records);
    for (const auto& warning : built.warnings) {
        std::cerr << "warning: " << warning << '\n';
    }
    for (const auto& error : built.errors) {
        std::cerr << "error: " << error << '\n';
    }
    if (!built.ok()) {
        return 1;
    }

    std::ofstream output(config.out_path);
    if (!output) {
        throw std::runtime_error("failed to open output: " + config.out_path);
    }

    for (const auto& record : built.universe.markets) {
        output << to_jsonl_line(record) << '\n';
    }
    if (!output) {
        throw std::runtime_error("failed to write output: " + config.out_path);
    }

    std::size_t candidate_bundle_count = 0;
    std::size_t bundle_warning_count = 0;
    if (!config.candidate_bundles_out_path.empty()) {
        CandidateBundleGenerator generator;
        const auto generated = generator.generate_buy_all_outcomes(
            built.universe.markets,
            known_market_ids(built.universe.markets),
            known_asset_ids(built.universe.markets)
        );
        for (const auto& warning : generated.warnings) {
            std::cerr << "warning: " << warning << '\n';
        }
        for (const auto& error : generated.errors) {
            std::cerr << "error: " << error << '\n';
        }
        if (!generated.ok()) {
            return 1;
        }

        std::vector<std::string> errors;
        if (!generator.export_fixture_artifact(
                generated.bundles,
                config.candidate_bundles_out_path,
                &errors
            )) {
            for (const auto& error : errors) {
                std::cerr << "error: " << error << '\n';
            }
            return 1;
        }
        candidate_bundle_count = generated.bundles.size();
        bundle_warning_count = generated.warnings.size();
    }

    std::cout << "market_universe:\n";
    std::cout << "  source: "
              << (config.polymarket_live ? "polymarket_gamma" : "fixture")
              << '\n';
    std::cout << "  market_count: "
              << built.universe.manifest.market_count << '\n';
    std::cout << "  asset_count: "
              << built.universe.manifest.asset_count << '\n';
    std::cout << "  warnings: "
              << source_warning_count + built.warnings.size() +
                     bundle_warning_count
              << '\n';
    std::cout << "  errors: 0\n";
    std::cout << "  candidate_bundles: " << candidate_bundle_count << '\n';
    if (config.polymarket_live) {
        std::cout << "  pages_fetched: " << pages_fetched << '\n';
    }
    std::cout << "  out: " << config.out_path << '\n';
    if (!config.candidate_bundles_out_path.empty()) {
        std::cout << "  candidate_bundles_out: "
                  << config.candidate_bundles_out_path << '\n';
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(parse_args(argc, argv));
    } catch (const std::exception& error) {
        std::cerr << "fetch_market_universe failed: "
                  << error.what() << '\n';
        return 1;
    }
}
