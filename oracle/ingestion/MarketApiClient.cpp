#include "oracle/ingestion/MarketApiClient.h"

namespace trading_engine::oracle {

MarketApiFetchResult MarketApiClient::fetch_active_markets() const {
    MarketApiFetchResult result;
    result.errors.push_back("MarketApiClient live fetch is not implemented");
    return result;
}

}  // namespace trading_engine::oracle
