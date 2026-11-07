# Maker cancel and replace protocol

The desired quote belongs to the plugin. ExecutionGateway owns actual orders. Each child has a stable internal/client ID, owner/epoch/model, account/venue/asset, original/filled/remaining quantity, price, post-only/GTD policy, reservation ID, status and last simulated lifecycle evidence.

## Cancellation

A request is durably authorized before the adapter accepts it. CANCEL_PENDING retains cash/inventory. Partial fills while pending debit/credit the canonical ledger once and shrink only unfilled resources. Cancel rejection preserves the remaining commitment. Cancel timeout remains pending/unknown and freezes increases. Only an explicit simulated adapter terminal acknowledgement releases unfilled resources. No timeout or desired quote update is treated as cancellation.

Fills carry receipt identity. Duplicate receipt delivery with a different receive time is idempotent; conflicting economics for the same receipt fail closed. A valid late fill after terminal cancellation is posted and freezes reconciliation rather than disappearing. An overfill or unfunded sell remains a durable unresolved incident and cannot be fabricated into balanced cash.

## Replacement policies

| Policy | Behavior |
|---|---|
| CANCEL_THEN_NEW | Retain latest desired request, cancel existing orders, wait for terminal evidence, then recheck current book, expiry, inventory and shared cash. Reject stale desired evidence rather than reuse it. |
| OVERLAP_WITH_EXTRA_RESERVATION | Old commitments remain; independently fund the new order and reject own-order crossing. |
| BLOCK_REPLACEMENT | Reject replacement while an old order remains outstanding. |

No exchange atomic replacement is assumed. Unknown/recovery-blocked old orders cannot be bypassed with a new desired quote. An old fill while replacement waits changes canonical inventory before the replacement admission.

## Quote constraints

Validate tradability, fresh admitted state/version, minimum size, integral quantity step, tick grid, own opposite orders and post-only crossing. The default tick policy REJECT refuses off-grid prices. Explicit PASSIVE_OUTWARD execution policy V1 floors bids and ceils asks, records original and effective prices, then reruns quote/global risk. This preserves the unchanged strategy's desired quote; it never silently converts it into a taker order. Changed instrument tick size freezes the configuration epoch for review.

GTD expiration is a scheduled simulated venue terminal event, distinct from a local ACK timeout. Submission and cancellation each have positive configured latency. Physical callback time cannot make pre-arrival evidence fill an order after queue delay. Only current/past received evidence is used; future marks are post-hoc research outputs.

## Completed validation evidence

All final faults passed: cancel before ACK, fill while pending, remaining-quantity release after simulated terminal ACK, rejection/timeout, duplicate/late receipts, uncertain and overlapping replacements, funded buy/sell/restart, and whole-portfolio freeze on cancel rejection before first ACK. RC1's valuation-projection sell failure remains retained; RC3 reuses existing sell application and preserves unresolved exposure. No request/timeout is an external cancellation receipt.
