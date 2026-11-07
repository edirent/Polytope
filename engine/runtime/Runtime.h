#pragma once
#include <filesystem>
namespace trading_engine::runtime {
// Bounded, single-writer native runtime. Live mode is intentionally absent.
int run(const std::filesystem::path& config, bool recover = false);
}
