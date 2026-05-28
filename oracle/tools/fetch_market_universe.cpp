#include "oracle/ingestion/MarketDescriptionLoader.h"
#include "oracle/ingestion/MarketUniverseBuilder.h"

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using trading_engine::oracle::MarketDescriptionLoader;
using trading_engine::oracle::MarketUniverseBuilder;
using trading_engine::oracle::to_jsonl_line;

struct Config {
    std::string fixture_path;
    std::string out_path;
};

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
        } else if (arg == "--out") {
            config.out_path = value("--out");
        } else if (arg == "--help" || arg == "-h") {
            throw std::runtime_error(
                "usage: fetch_market_universe --fixture PATH --out PATH"
            );
        } else {
            throw std::runtime_error("unknown argument: " + arg);
        }
    }

    if (config.fixture_path.empty()) {
        throw std::runtime_error("--fixture is required");
    }
    if (config.out_path.empty()) {
        throw std::runtime_error("--out is required");
    }

    return config;
}

int run(const Config& config) {
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

    MarketUniverseBuilder builder;
    const auto built = builder.build(loaded.records);
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

    std::cout << "market_universe:\n";
    std::cout << "  market_count: "
              << built.universe.manifest.market_count << '\n';
    std::cout << "  asset_count: "
              << built.universe.manifest.asset_count << '\n';
    std::cout << "  warnings: "
              << loaded.warnings.size() + built.warnings.size() << '\n';
    std::cout << "  errors: 0\n";
    std::cout << "  out: " << config.out_path << '\n';
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
