import type { FilledOrder } from "../api/dashboardTypes";
import type { DashboardUiState } from "./dashboardStore";

export interface FilledOrderAggregate {
  key: string;
  market_id: string;
  asset_id: string;
  asset_index: number;
  fills: number;
  qty_lots: number;
  notional_tick: number;
  avg_fill_price_tick: number;
  mark_price_tick: number;
  unrealized_pnl_tick: number;
  mark_quality: string;
}

export function activeFilledOrders(state: DashboardUiState): FilledOrder[] {
  const snapshotOrders = state.snapshot?.filled_orders ?? [];
  return snapshotOrders.length > 0 ? snapshotOrders : state.executionReports;
}

export function shortAssetId(assetId: string): string {
  if (assetId.length <= 16) {
    return assetId;
  }
  return `${assetId.slice(0, 8)}...${assetId.slice(-6)}`;
}

export function displayMarket(order: Pick<FilledOrder, "market_id" | "asset_id">): string {
  if (order.market_id.length > 0) {
    return order.market_id;
  }
  return shortAssetId(order.asset_id);
}

export function aggregateFilledOrders(
  orders: FilledOrder[]
): FilledOrderAggregate[] {
  const byAsset = new Map<string, FilledOrderAggregate>();

  for (const order of orders) {
    const key = `${order.market_id}:${order.asset_id}`;
    const current =
      byAsset.get(key) ??
      ({
        key,
        market_id: order.market_id,
        asset_id: order.asset_id,
        asset_index: order.asset_index,
        fills: 0,
        qty_lots: 0,
        notional_tick: 0,
        avg_fill_price_tick: 0,
        mark_price_tick: 0,
        unrealized_pnl_tick: 0,
        mark_quality: order.mark_quality
      } satisfies FilledOrderAggregate);

    current.fills += 1;
    current.qty_lots += order.filled_lots;
    current.notional_tick += order.notional_tick;
    current.unrealized_pnl_tick += order.unrealized_pnl_tick;
    current.mark_price_tick = order.mark_price_tick;
    current.mark_quality = order.mark_quality;
    current.avg_fill_price_tick =
      current.qty_lots > 0
        ? Math.round(current.notional_tick / current.qty_lots)
        : 0;

    byAsset.set(key, current);
  }

  return Array.from(byAsset.values()).sort((left, right) => {
    const pnlDelta = right.unrealized_pnl_tick - left.unrealized_pnl_tick;
    if (pnlDelta !== 0) {
      return pnlDelta;
    }
    return left.key.localeCompare(right.key);
  });
}
