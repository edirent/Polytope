#include "feed/RealtimeFeedEngine.h"

int main() {
    trading_engine::feed::RealtimeFeedConfig config;

    config.asset_ids = {
        "98022490269692409998126496127597032490334070080325855126491859374983463996227"
    };
    config.flush_every_n_packets = 1;

    trading_engine::feed::RealtimeFeedEngine engine(config);
    engine.run();

    return 0;
}
