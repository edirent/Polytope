#pragma once

#include "oracle/public/CandidateBundle.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace trading_engine::signal {

using CandidateBundle = trading_engine::oracle::CandidateBundle;

struct OracleLoadResult {
    bool ok = false;
    std::string error;

    std::uint64_t artifact_version = 0;
    std::uint64_t bundle_count = 0;
};

class OracleArtifactReader {
public:
    [[nodiscard]] OracleLoadResult load(
        const std::filesystem::path& artifact_dir
    );

    [[nodiscard]] std::span<const CandidateBundle> active_bundles() const;

    [[nodiscard]] std::uint64_t artifact_version() const;
    [[nodiscard]] std::uint64_t artifact_hash() const;
    [[nodiscard]] std::uint64_t constraint_hash() const;
    [[nodiscard]] std::uint64_t bundle_hash() const;

private:
    std::vector<CandidateBundle> bundles_;
    std::uint64_t artifact_version_ = 0;
    std::uint64_t artifact_hash_ = 0;
    std::uint64_t constraint_hash_ = 0;
    std::uint64_t bundle_hash_ = 0;
};

}  // namespace trading_engine::signal
