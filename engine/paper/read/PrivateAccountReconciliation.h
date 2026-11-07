#pragma once
#include "engine/paper/public/PaperAccount.h"
#include "engine/paper/portfolio/Position.h"
#include <optional>
#include <vector>
namespace trading_engine::paper {
// Design boundary only. No authenticated venue implementation is supplied.
// Public market messages and simulated fills cannot satisfy this authority.
struct PrivateAccountSnapshot {
    std::string venue,account_id,source_receipt_id;
    std::uint64_t exchange_asof_ns=0,received_wall_ns=0;
    PaperAccount balances;
    std::vector<Position> positions;
    std::vector<std::string> open_order_ids;
    bool authenticated_private_source=false;
};
class IReadOnlyPrivateAccountSource {
public:
    virtual ~IReadOnlyPrivateAccountSource()=default;
    virtual std::optional<PrivateAccountSnapshot> fetch_snapshot()=0;
    // Deliberately no place/cancel/settle/risk-release methods.
};
enum class PrivateReconciliationStatus { Unconfigured, Unknown, Mismatch, AuthenticatedSnapshotMatched };
}
