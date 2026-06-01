export type StreamStatus = "connecting" | "live" | "reconnecting" | "closed";

export interface HealthResponse {
  ok: boolean;
  mode: "readonly" | string;
  paper_trading_running?: boolean;
  paper_trading_pid?: number;
  dashboard_file?: string;
}

export interface PaperAccountSnapshot {
  starting_cash_tick: number;
  cash_balance_tick: number;
  reserved_cash_tick: number;
  realized_pnl_tick: number;
  unrealized_pnl_tick: number;
}

export interface PerformanceSnapshot {
  intents_observed: number;
  approvals_observed: number;
  plans_observed: number;
  execution_reports_observed: number;
  filled_plans: number;
  failed_plans: number;
  gross_pnl_tick: number;
  net_pnl_tick: number;
  max_drawdown_tick: number;
  max_drawdown_ratio: number;
  returns_count: number;
  latest_return: number;
  latest_return_status: string;
  volatility: number;
  volatility_status: string;
  sharpe: number;
  sharpe_status: string;
  fill_rate: number;
  fill_rate_status: string;
  risk_approval_rate: number;
  risk_approval_rate_status: string;
  intent_conversion_rate: number;
  intent_conversion_rate_status: string;
  turnover: number;
  turnover_status: string;
  version: number;
  updated_ts_ns: number;
}

export interface RegimeSnapshot {
  data: string;
  liquidity: string;
  chain: string;
  signal: string;
  risk: string;
  execution: string;
  version: number;
  ts_ns: number;
}

export interface LatencySnapshot {
  feed_to_state_ns: number;
  state_to_signal_ns: number;
  signal_to_risk_ns: number;
  risk_to_execution_ns: number;
  end_to_end_ns: number;
}

export interface SignalDashboardSnapshot {
  intents_published: number;
  paper_opportunities: number;
  rejected: number;
  output_hash: number;
}

export interface RiskDashboardSnapshot {
  decisions: number;
  approved: number;
  rejected: number;
  output_hash: number;
}

export interface ExecutionDashboardSnapshot {
  plans_created: number;
  plans_filled: number;
  plans_failed: number;
  output_hash: number;
}

export interface FilledOrder {
  execution_report_id: number;
  plan_id: number;
  child_order_id: number;
  bundle_id: number;
  source_intent_id: number;
  approved_intent_id: number;
  reservation_id: number;
  market_id: string;
  asset_id: string;
  market_index: number;
  asset_index: number;
  side: string;
  filled_lots: number;
  remaining_lots: number;
  avg_fill_price_tick: number;
  limit_price_tick: number;
  estimated_vwap_tick: number;
  worst_price_tick: number;
  notional_tick: number;
  mark_price_tick: number;
  unrealized_pnl_tick: number;
  mark_quality: string;
  event_ts_ns: number;
}

export interface DashboardSnapshot {
  seq_no: number;
  ts_ns: number;
  account: PaperAccountSnapshot;
  performance: PerformanceSnapshot;
  regime: RegimeSnapshot;
  latency: LatencySnapshot;
  signal: SignalDashboardSnapshot;
  risk: RiskDashboardSnapshot;
  execution: ExecutionDashboardSnapshot;
  filled_orders: FilledOrder[];
}

export interface LatestSnapshotEmptyResponse {
  snapshot: null;
}

export type LatestSnapshotResponse =
  | DashboardSnapshot
  | LatestSnapshotEmptyResponse;

export interface EquityResponse {
  equity_mid: number;
  equity_liquidation: number;
}

export interface MarketsResponse {
  markets: Array<Record<string, unknown>>;
}

export interface IntentsResponse {
  intents: Array<Record<string, unknown>>;
}

export interface RiskDecisionsResponse {
  risk_decisions: Array<Record<string, unknown>>;
}

export interface ExecutionReportsResponse {
  execution_reports: FilledOrder[];
}
