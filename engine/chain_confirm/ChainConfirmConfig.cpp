#include "chain_confirm/ChainConfirmConfig.h"

#include <string_view>

namespace trading_engine::chain_confirm {

std::string redact_rpc_url(const std::string& url) {
    constexpr std::string_view marker{"/v2/"};
    const auto pos = url.find(marker);
    if (pos == std::string::npos) {
        return url.empty() ? std::string{} : std::string{"<redacted>"};
    }

    return url.substr(0, pos + marker.size()) + "<redacted>";
}

}  // namespace trading_engine::chain_confirm
