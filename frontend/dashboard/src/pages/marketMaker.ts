import { element } from "../components/dom";
import {
  formatInteger,
  formatMicros,
  formatPercent,
  formatTick
} from "../components/format";
import { metricGrid } from "../components/metricGrid";
import { shortAssetId } from "../state/filledOrders";
import type { MarketMakerRecentFill } from "../api/dashboardTypes";
import type { PageDefinition } from "./pageTypes";

function pnlTone(value: number | undefined): "good" | "bad" | "neutral" {
  if ((value ?? 0) > 0) {
    return "good";
  }
  if ((value ?? 0) < 0) {
    return "bad";
  }
  return "neutral";
}

function formatRuntime(seconds: number | undefined): string {
  const value = Math.max(0, Math.floor(seconds ?? 0));
  const minutes = Math.floor(value / 60);
  const rest = value % 60;
  return `${minutes}m ${rest}s`;
}

function percent(
  numerator: number | undefined,
  denominator: number | undefined
): string {
  if (!denominator) {
    return "0.00%";
  }
  return formatPercent((numerator ?? 0) / denominator);
}

function topReason(
  reasons: Record<string, number> | undefined,
  excludedReason?: string
): string {
  if (!reasons) {
    return "none";
  }
  let bestReason = "none";
  let bestCount = 0;
  for (const [reason, count] of Object.entries(reasons)) {
    if (reason === excludedReason) {
      continue;
    }
    if (count > bestCount) {
      bestReason = reason;
      bestCount = count;
    }
  }
  return bestCount > 0 ? `${bestReason} ${formatInteger(bestCount)}` : "none";
}

function reasonTotal(reasons: Record<string, number> | undefined): number {
  return Object.values(reasons ?? {}).reduce((total, count) => total + count, 0);
}

function markoutText(ready: boolean | undefined, tick: number | undefined) {
  return ready ? formatTick(tick) : "-";
}

function fillsTable(fills: MarketMakerRecentFill[] | undefined): HTMLElement {
  const wrapper = element("div", "table-wrap");
  if (!fills || fills.length === 0) {
    wrapper.appendChild(element("p", "empty", "No maker fills yet."));
    return wrapper;
  }

  const table = document.createElement("table");
  table.className = "data-table";
  const thead = document.createElement("thead");
  const headRow = document.createElement("tr");
  for (const label of [
    "Report",
    "Asset",
    "Side",
    "Qty",
    "Price",
    "M@Fill",
    "1s",
    "5s",
    "Remaining",
    "Reason"
  ]) {
    const th = document.createElement("th");
    th.textContent = label;
    headRow.appendChild(th);
  }
  thead.appendChild(headRow);
  table.appendChild(thead);

  const tbody = document.createElement("tbody");
  for (const fill of fills.slice().reverse()) {
    const tr = document.createElement("tr");
    const cells = [
      String(fill.report_id),
      shortAssetId(fill.asset_id),
      fill.side,
      formatInteger(fill.qty_lots),
      formatTick(fill.fill_price_tick),
      formatTick(fill.mark_at_fill_tick),
      markoutText(fill.markout_1s_ready, fill.markout_1s_tick),
      markoutText(fill.markout_5s_ready, fill.markout_5s_tick),
      formatInteger(fill.remaining_qty_lots),
      fill.reason
    ];

    for (let i = 0; i < cells.length; ++i) {
      const td = document.createElement("td");
      td.textContent = cells[i];
      if (i === 0 || i === 1) {
        td.className = "mono";
      }
      tr.appendChild(td);
    }
    tbody.appendChild(tr);
  }
  table.appendChild(tbody);
  wrapper.appendChild(table);
  return wrapper;
}

export const marketMakerPage: PageDefinition = {
  id: "market-maker",
  title: "Market Maker",
  render(state) {
    const mm = state.snapshot?.market_maker;
    const page = element("div", "page");
    page.appendChild(element("h1", undefined, "Market Maker"));

    page.appendChild(
      metricGrid([
        {
          label: "Mode",
          value: mm?.mode ?? "read_only_live",
          detail: `${mm?.fill_mode ?? "book-cross"} / rest ${formatInteger(mm?.queue_min_rest_ms)}ms`
        },
        {
          label: "Runtime",
          value: formatRuntime(mm?.runtime_seconds),
          detail: `${formatInteger(mm?.dashboard_samples)} samples`
        },
        {
          label: "Starting Cash",
          value: formatTick(mm?.starting_cash_tick)
        },
        {
          label: "Equity Mid",
          value: formatTick(mm?.equity_mid_tick),
          tone: pnlTone(
            (mm?.equity_mid_tick ?? 0) - (mm?.starting_cash_tick ?? 0)
          )
        },
        {
          label: "Cash",
          value: formatTick(mm?.cash_tick)
        },
        {
          label: "Realized",
          value: formatTick(mm?.realized_pnl_tick),
          tone: pnlTone(mm?.realized_pnl_tick)
        },
        {
          label: "Unrealized",
          value: formatTick(mm?.unrealized_pnl_mid_tick),
          detail: mm?.mark_quality ?? "NoPosition",
          tone: pnlTone(mm?.unrealized_pnl_mid_tick)
        },
        {
          label: "Seed PnL",
          value: formatTick(mm?.seed_total_pnl_mid_tick),
          detail: `${formatInteger(mm?.seed_position_lots)} lots`,
          tone: pnlTone(mm?.seed_total_pnl_mid_tick)
        },
        {
          label: "Strategy PnL",
          value: formatTick(mm?.strategy_total_pnl_mid_tick),
          detail: `spread ${formatTick(mm?.strategy_spread_capture_tick)}`,
          tone: pnlTone(mm?.strategy_total_pnl_mid_tick)
        },
        {
          label: "Liquidation Equity",
          value: formatTick(mm?.equity_liquidation_tick),
          tone: pnlTone(
            (mm?.equity_liquidation_tick ?? 0) -
              (mm?.starting_cash_tick ?? 0)
          )
        },
        {
          label: "Position",
          value: formatInteger(mm?.open_position_lots),
          detail: `target ${formatInteger(mm?.target_position_lots)} / max ${formatInteger(mm?.max_inventory_lots)}`
        },
        {
          label: "Condition Net",
          value: formatInteger(mm?.condition_net_exposure_lots),
          detail: `sets ${formatInteger(mm?.condition_complete_sets_lots)} / comp ${formatInteger(mm?.complement_position_lots)}`
        },
        {
          label: "Avg Cost",
          value: formatTick(mm?.avg_cost_tick),
          detail: `min ${formatInteger(mm?.min_inventory_lots)}`
        },
        {
          label: "Maker Fills",
          value: formatInteger(mm?.maker_fills_applied),
          detail: `${formatInteger(mm?.maker_fills_rejected)} rejected`
        },
        {
          label: "Active Quotes",
          value: formatInteger(mm?.active_quotes),
          detail: `${formatInteger(mm?.submitted_quotes)} submitted`
        },
        {
          label: "Risk Approval",
          value: percent(mm?.risk_approved, mm?.risk_evaluated),
          detail: `${formatInteger(mm?.risk_rejected)} rejected`
        }
      ])
    );

    page.appendChild(
      metricGrid([
        {
          label: "Best Bid",
          value: formatTick(mm?.best_bid_tick)
        },
        {
          label: "Best Ask",
          value: formatTick(mm?.best_ask_tick)
        },
        {
          label: "Spread",
          value: formatTick(mm?.spread_tick)
        },
        {
          label: "WS Packets",
          value: formatInteger(mm?.ws_packets),
          detail: `${formatInteger(mm?.filtered_events)} filtered`
        },
        {
          label: "Depth Updates",
          value: formatInteger(mm?.depth_updates),
          detail: `${formatInteger(mm?.book_snapshots)} snapshots`
        },
        {
          label: "Quote Intents",
          value: formatInteger(mm?.quote_intents),
          detail: `${formatInteger(mm?.cancel_intents)} cancels`
        },
        {
          label: "Fair Gate",
          value: formatTick(mm?.max_quote_fair_deviation_tick),
          detail: `${formatInteger(mm?.max_quote_fair_deviation_bps)} bps`
        },
        {
          label: "Complement Fair",
          value: `${formatInteger(mm?.complement_fair_weight_bps)} bps`,
          detail: shortAssetId(mm?.complement_asset_id ?? "")
        },
        {
          label: "External Fair",
          value: formatTick(mm?.external_fair_value_tick),
          detail: `${formatInteger(mm?.external_fair_weight_bps)} bps / ${
            mm?.require_external_fair_for_opening_quotes ? "required" : "optional"
          }`
        },
        {
          label: "Fair Quality",
          value: mm?.latest_fair_value_quality ?? "unknown",
          detail: `${formatInteger(mm?.latest_fair_confidence_bps)} bps / spread ${formatTick(mm?.latest_fair_book_spread_tick)}`,
          tone:
            mm?.latest_fair_value_quality &&
            mm.latest_fair_value_quality !== "valid"
              ? "bad"
              : "neutral"
        },
        {
          label: "Latest Fair",
          value: formatTick(mm?.latest_fair_value_tick),
          detail: mm?.external_fair_value_tick
            ? "external-first"
            : "book fallback"
        },
        {
          label: "BTC Oracle",
          value: mm?.btc_oracle_enabled ? formatInteger(mm?.btc_oracle_spot) : "off",
          detail: `${mm?.btc_oracle_stale ? "stale" : "fresh"} / ${formatInteger(mm?.btc_oracle_age_ms)}ms`,
          tone:
            mm?.btc_oracle_enabled && (mm?.btc_oracle_stale || !mm?.btc_oracle_has_spot)
              ? "bad"
              : "neutral"
        },
        {
          label: "BTC Vol",
          value: `${formatInteger(
            mm?.btc_use_realized_vol
              ? mm?.btc_realized_vol_annual_bps
              : mm?.btc_vol_annual_bps
          )} bps`,
          detail: mm?.btc_use_realized_vol
            ? `${formatInteger(mm?.btc_realized_vol_sample_count)} samples`
            : "configured"
        },
        {
          label: "BTC 1s Move",
          value: `${(mm?.btc_move_1s_bps ?? 0).toFixed(1)} bps`,
          detail: `limit ${formatInteger(mm?.btc_toxic_move_1s_bps)} bps`,
          tone: mm?.btc_toxic_bid || mm?.btc_toxic_ask ? "bad" : "neutral"
        },
        {
          label: "Toxic Halt",
          value: `${mm?.btc_toxic_bid ? "bid" : "-"} / ${mm?.btc_toxic_ask ? "ask" : "-"}`,
          detail: `${formatInteger(mm?.btc_oracle_updates)} oracle updates`
        },
        {
          label: "AS Spread",
          value: mm?.as_model_enabled ? formatTick(mm?.as_half_spread_tick) : "off",
          detail: `${mm?.as_model_ok ? "ok" : "idle"} / x${(mm?.as_spread_multiplier ?? 1).toFixed(1)}`
        },
        {
          label: "AS Skew",
          value: formatTick(mm?.as_inventory_skew_tick),
          detail: `risk ${formatTick(mm?.as_reservation_risk_tick)}`
        },
        {
          label: "Latency",
          value: `${mm?.assumed_latency_ms ?? 2}ms`,
          detail: `buffer ${formatTick(mm?.latency_buffer_tick)}`
        },
        {
          label: "Requote Gate",
          value: `${formatInteger(mm?.min_requote_interval_ms)}ms`,
          detail: formatTick(mm?.min_quote_price_change_tick)
        },
        {
          label: "No Quote",
          value: formatInteger(reasonTotal(mm?.no_quote_reasons)),
          detail: topReason(mm?.no_quote_reasons)
        },
        {
          label: "Risk Reason",
          value: formatInteger(mm?.risk_rejected),
          detail: topReason(mm?.risk_decisions, "approve")
        },
        {
          label: "Pipeline",
          value: formatMicros(mm?.latest_pipeline_latency_ns)
        },
        {
          label: "Errors",
          value: formatInteger(
            (mm?.decode_errors ?? 0) +
              (mm?.state_errors ?? 0) +
              (mm?.transport_errors ?? 0) +
              (mm?.dashboard_write_errors ?? 0)
          ),
          detail: "decode/state/transport/dashboard",
          tone:
            (mm?.decode_errors ?? 0) +
              (mm?.state_errors ?? 0) +
              (mm?.transport_errors ?? 0) +
              (mm?.dashboard_write_errors ?? 0) >
            0
              ? "bad"
              : "good"
        }
      ])
    );

    page.appendChild(element("h2", undefined, "Recent Maker Fills"));
    page.appendChild(fillsTable(mm?.recent_fills));
    return page;
  }
};
