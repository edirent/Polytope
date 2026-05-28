#pragma once

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace trading_engine::oracle {

class ArtifactWriter {
public:
    [[nodiscard]] static bool write_text(
        const std::filesystem::path& path,
        std::string_view content,
        std::vector<std::string>* errors
    ) {
        std::ofstream output(path, std::ios::binary);
        if (!output) {
            if (errors) {
                errors->push_back("failed to open artifact file: " + path.string());
            }
            return false;
        }

        output.write(
            content.data(),
            static_cast<std::streamsize>(content.size())
        );
        if (!output) {
            if (errors) {
                errors->push_back("failed to write artifact file: " + path.string());
            }
            return false;
        }

        return true;
    }

    [[nodiscard]] static bool write_bytes(
        const std::filesystem::path& path,
        std::span<const std::byte> bytes,
        std::vector<std::string>* errors
    ) {
        std::ofstream output(path, std::ios::binary);
        if (!output) {
            if (errors) {
                errors->push_back("failed to open artifact file: " + path.string());
            }
            return false;
        }

        if (!bytes.empty()) {
            output.write(
                reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size())
            );
        }
        if (!output) {
            if (errors) {
                errors->push_back("failed to write artifact file: " + path.string());
            }
            return false;
        }

        return true;
    }
};

}  // namespace trading_engine::oracle
