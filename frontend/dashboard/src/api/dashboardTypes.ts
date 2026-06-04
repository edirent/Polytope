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
  terminal_payout_tick: number;
  terminal_cost_tick: number;
  terminal_pnl_tick: number;
  terminal_complete_plans: number;
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

export interface TerminalPnL {
  plan_id: number;
  bundle_id: number;
  source_intent_id: number;
  approved_intent_id: number;
  reservation_id: number;
  expected_child_orders: number;
  filled_child_orders: number;
  complete: boolean;
  chosen_bundle_qty: number;
  guaranteed_payout_tick: number;
  expected_terminal_pnl_tick: number;
  actual_buy_cost_tick: number;
  terminal_pnl_tick: number;
  updated_ts_ns: number;
}

export interface MarketMakerRecentFill {
  report_id: number;
  quote_id: number;
  quote_group_id: number;
  asset_id: string;
  side: string;
  qty_lots: number;
  fill_price_tick: number;
  remaining_qty_lots: number;
  reason: string;
  ts_ns: number;
  mark_at_fill_tick: number;
  markout_1s_ready: boolean;
  markout_1s_tick: number;
  markout_5s_ready: boolean;
  markout_5s_tick: number;
  markout_30s_ready: boolean;
  markout_30s_tick: number;
}

export interface MarketMakerSnapshot {
  mode: string;
  fill_mode: string;
  queue_min_rest_ms: number;
  asset_id: string;
  complement_asset_id: string;
  market_id: string;
  runtime_seconds: number;
  target_position_lots: number;
  min_inventory_lots: number;
  max_inventory_lots: number;
  max_quote_fair_deviation_tick: number;
  max_quote_fair_deviation_bps: number;
  complement_fair_weight_bps: number;
  external_fair_weight_bps: number;
  external_fair_value_tick: number;
  latest_fair_value_quality: string;
  latest_fair_confidence_bps: number;
  latest_fair_book_spread_tick: number;
  latest_fair_value_tick: number;
  external_fair_invert: boolean;
  require_external_fair_for_opening_quotes: boolean;
  assumed_latency_ms: number;
  latency_buffer_tick: number;
  latency_buffer_tick_per_ms: number;
  min_requote_interval_ms: number;
  min_quote_price_change_tick: number;
  btc_spot: number;
  btc_threshold: number;
  btc_vol_annual_bps: number;
  btc_drift_annual_bps: number;
  btc_use_realized_vol: boolean;
  btc_realized_vol_annual_bps: number;
  btc_realized_vol_sample_count: number;
  btc_realized_vol_window_seconds: number;
  btc_oracle_enabled: boolean;
  btc_oracle_endpoint: string;
  btc_oracle_spot: number;
  btc_oracle_has_spot: boolean;
  btc_oracle_stale: boolean;
  btc_oracle_age_ms: number;
  btc_move_1s_bps: number;
  btc_toxic_move_1s_bps: number;
  btc_toxic_bid: boolean;
  btc_toxic_ask: boolean;
  btc_oracle_updates: number;
  btc_oracle_parse_errors: number;
  btc_oracle_transport_errors: number;
  as_model_enabled: boolean;
  as_model_ok: boolean;
  as_risk_aversion: number;
  as_order_arrival_k: number;
  as_spread_multiplier: number;
  as_half_spread_tick: number;
  as_inventory_skew_tick: number;
  as_reservation_risk_tick: number;
  starting_cash_tick: number;
  cash_tick: number;
  realized_pnl_tick: number;
  unrealized_pnl_mid_tick: number;
  unrealized_pnl_liquidation_tick: number;
  equity_mid_tick: number;
  equity_liquidation_tick: number;
  fees_paid_tick: number;
  seed_position_lots: number;
  seed_cost_basis_tick: number;
  seed_realized_pnl_tick: number;
  seed_unrealized_pnl_mid_tick: number;
  seed_total_pnl_mid_tick: number;
  strategy_position_lots: number;
  strategy_cost_basis_tick: number;
  strategy_realized_pnl_tick: number;
  strategy_unrealized_pnl_mid_tick: number;
  strategy_total_pnl_mid_tick: number;
  strategy_spread_capture_tick: number;
  unattributed_pnl_mid_tick: number;
  maker_fill_count: number;
  open_position_lots: number;
  complement_position_lots: number;
  condition_complete_sets_lots: number;
  condition_net_exposure_lots: number;
  avg_cost_tick: number;
  cost_basis_tick: number;
  mark_quality: string;
  best_bid_tick: number;
  best_ask_tick: number;
  spread_tick: number;
  book_version: number;
  active_quotes: number;
  submitted_quotes: number;
  replaced_quotes: number;
  cancelled_quotes: number;
  cancel_errors: number;
  duplicate_ignored: number;
  maker_reports: number;
  maker_fills_applied: number;
  maker_fills_rejected: number;
  gross_fill_notional_tick: number;
  ws_packets: number;
  normalized_events: number;
  filtered_events: number;
  book_snapshots: number;
  book_deltas: number;
  depth_updates: number;
  quote_intents: number;
  cancel_intents: number;
  no_quote_reasons: Record<string, number>;
  risk_evaluated: number;
  risk_approved: number;
  risk_rejected: number;
  risk_decisions: Record<string, number>;
  decode_errors: number;
  state_errors: number;
  transport_errors: number;
  dashboard_write_errors: number;
  latest_pipeline_latency_ns: number;
  dashboard_samples: number;
  paper_ledger_applied_fills: number;
  paper_ledger_position_count: number;
  recent_fills: MarketMakerRecentFill[];
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
  terminal_pnl: TerminalPnL[];
  market_maker?: MarketMakerSnapshot;
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
