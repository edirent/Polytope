#pragma once

#include "oracle/artifact/ArtifactExporter.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace trading_engine::oracle {

struct ArtifactLoadResult {
    OracleArtifactContents contents;
    bool checksums_ok = false;
    std::map<std::string, std::string> checksums;
    std::vector<std::string> errors;

    [[nodiscard]] bool ok() const noexcept {
        return checksums_ok && errors.empty();
    }
};

class ArtifactLoader {
public:
    [[nodiscard]] ArtifactLoadResult load(
        const std::filesystem::path& artifact_dir
    ) const;
};

}  // namespace trading_engine::oracle
