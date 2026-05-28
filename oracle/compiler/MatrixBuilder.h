#pragma once

#include "oracle/compiler/ConstraintCompiler.h"

#include <string>
#include <vector>

namespace trading_engine::oracle {

class MatrixBuilder {
public:
    [[nodiscard]] bool write(
        const CompiledConstraintSet& compiled,
        const std::string& directory,
        std::vector<std::string>* errors = nullptr
    ) const;
};

}  // namespace trading_engine::oracle
