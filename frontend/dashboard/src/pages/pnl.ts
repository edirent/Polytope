import { element } from "../components/dom";
import {
  formatInteger,
  formatPercent,
  formatRatio,
  formatTick
} from "../components/format";
import { metricGrid } from "../components/metricGrid";
import {
  activeFilledOrders,
  activeTerminalPnL,
  aggregateFilledOrders,
  shortAssetId
} from "../state/filledOrders";
import type { PageDefinition } from "./pageTypes";

function pnlClass(value: number): string {
  if (value > 0) {
    return "pnl-positive";
  }
  if (value < 0) {
    return "pnl-negative";
  }
  return "";
}

function terminalPnlTable(state: Parameters<PageDefinition["render"]>[0]): HTMLElement {
  const plans = activeTerminalPnL(state);
  const wrapper = element("div", "table-wrap");
  if (plans.length === 0) {
    wrapper.appendChild(
      element("p", "empty", "No completed terminal PnL plans yet.")
    );
    return wrapper;
  }

  const table = document.createElement("table");
  table.className = "data-table";
  const thead = document.createElement("thead");
  const headRow = document.createElement("tr");
  for (const label of [
    "Plan",
    "Bundle",
    "Complete",
    "Qty",
    "Buy Cost",
    "Guaranteed Payout",
    "Terminal PnL"
  ]) {
    const th = document.createElement("th");
    th.textContent = label;
    headRow.appendChild(th);
  }
  thead.appendChild(headRow);
  table.appendChild(thead);

  const tbody = document.createElement("tbody");
  for (const plan of plans.slice().reverse()) {
    const tr = document.createElement("tr");
    const cells = [
      String(plan.plan_id),
      String(plan.bundle_id),
      `${plan.filled_child_orders}/${plan.expected_child_orders}`,
      formatInteger(plan.chosen_bundle_qty),
      formatTick(plan.actual_buy_cost_tick),
      formatTick(plan.guaranteed_payout_tick),
      plan.complete ? formatTick(plan.terminal_pnl_tick) : "Incomplete"
    ];

    for (let i = 0; i < cells.length; ++i) {
      const td = document.createElement("td");
      td.textContent = cells[i];
      if (i === 0) {
        td.className = "mono";
      }
      if (i === 6 && plan.complete) {
        td.classList.add(pnlClass(plan.terminal_pnl_tick));
      }
      tr.appendChild(td);
    }
    tbody.appendChild(tr);
  }
  table.appendChild(tbody);
  wrapper.appendChild(table);
  return wrapper;
}

function openPnlTable(state: Parameters<PageDefinition["render"]>[0]): HTMLElement {
  const positions = aggregateFilledOrders(activeFilledOrders(state));
  const wrapper = element("div", "table-wrap");
  if (positions.length === 0) {
    wrapper.appendChild(
      element("p", "empty", "No filled paper orders to mark yet.")
    );
    return wrapper;
  }

  const table = document.createElement("table");
  table.className = "data-table";
  const thead = document.createElement("thead");
  const headRow = document.createElement("tr");
  for (const label of [
    "Market",
    "Asset",
    "Fills",
    "Qty",
    "Cost",
    "Avg Fill",
    "Mark",
    "Open PnL"
  ]) {
    const th = document.createElement("th");
    th.textContent = label;
    headRow.appendChild(th);
  }
  thead.appendChild(headRow);
  table.appendChild(thead);

  const tbody = document.createElement("tbody");
  for (const position of positions) {
    const tr = document.createElement("tr");
    const cells = [
      position.market_id,
      shortAssetId(position.asset_id),
      formatInteger(position.fills),
      formatInteger(position.qty_lots),
      formatTick(position.notional_tick),
      formatTick(position.avg_fill_price_tick),
      position.mark_price_tick === 0
        ? position.mark_quality
        : `${formatTick(position.mark_price_tick)} ${position.mark_quality}`,
      formatTick(position.unrealized_pnl_tick)
    ];

    for (let i = 0; i < cells.length; ++i) {
      const td = document.createElement("td");
      td.textContent = cells[i];
      if (i === 1) {
        td.className = "mono";
      }
      if (i === 7) {
        td.classList.add(pnlClass(position.unrealized_pnl_tick));
      }
      tr.appendChild(td);
    }
    tbody.appendChild(tr);
  }
  table.appendChild(tbody);
  wrapper.appendChild(table);
  return wrapper;
}

export const pnlPage: PageDefinition = {
  id: "pnl",
  title: "PnL",
  render(state) {
    const page = element("div", "page");
    page.appendChild(element("h1", undefined, "PnL"));
    page.appendChild(
      metricGrid([
        {
          label: "Starting Cash",
          value: formatTick(state.snapshot?.account.starting_cash_tick),
          detail: "paper account"
        },
        {
          label: "Equity Mid",
          value: formatTick(state.equity?.equity_mid)
        },
        {
          label: "Equity Liquidation",
          value: formatTick(state.equity?.equity_liquidation)
        },
        {
          label: "Realized",
          value: formatTick(state.snapshot?.account.realized_pnl_tick)
        },
        {
          label: "Unrealized",
          value: formatTick(state.snapshot?.account.unrealized_pnl_tick)
        },
        {
          label: "Gross PnL",
          value: formatTick(state.performance?.gross_pnl_tick)
        },
        {
          label: "Net PnL",
          value: formatTick(state.performance?.net_pnl_tick)
        },
        {
          label: "Terminal PnL",
          value: formatTick(state.performance?.terminal_pnl_tick),
          detail: "guaranteed settlement",
          tone: (state.performance?.terminal_pnl_tick ?? 0) >= 0 ? "good" : "bad"
        },
        {
          label: "Terminal Cost",
          value: formatTick(state.performance?.terminal_cost_tick),
          detail: "actual filled cost"
        },
        {
          label: "Terminal Payout",
          value: formatTick(state.performance?.terminal_payout_tick)
        },
        {
          label: "Terminal Plans",
          value: formatInteger(state.performance?.terminal_complete_plans),
          detail: "complete bundles"
        },
        {
          label: "Max Drawdown",
          value: formatTick(state.performance?.max_drawdown_tick),
          tone: "warn"
        },
        {
          label: "Drawdown %",
          value: formatPercent(state.performance?.max_drawdown_ratio),
          tone: "warn"
        },
        {
          label: "Returns",
          value: formatInteger(state.performance?.returns_count),
          detail: "equity samples"
        },
        {
          label: "Sharpe",
          value: formatRatio(state.performance?.sharpe),
          detail: state.performance?.sharpe_status ?? "InsufficientData"
        },
        {
          label: "Latest Return",
          value: formatPercent(state.performance?.latest_return),
          detail: state.performance?.latest_return_status ?? "InsufficientData"
        },
        {
          label: "Volatility",
          value: formatPercent(state.performance?.volatility),
          detail: state.performance?.volatility_status ?? "InsufficientData"
        },
        {
          label: "Fill Rate",
          value: formatPercent(state.performance?.fill_rate),
          detail: state.performance?.fill_rate_status ?? "InsufficientData"
        },
        {
          label: "Risk Approval",
          value: formatPercent(state.performance?.risk_approval_rate),
          detail:
            state.performance?.risk_approval_rate_status ?? "InsufficientData"
        },
        {
          label: "Conversion",
          value: formatPercent(state.performance?.intent_conversion_rate),
          detail:
            state.performance?.intent_conversion_rate_status ??
            "InsufficientData"
        },
        {
          label: "Turnover",
          value: formatRatio(state.performance?.turnover),
          detail: state.performance?.turnover_status ?? "InsufficientData"
        },
        {
          label: "Reports",
          value: formatInteger(state.performance?.execution_reports_observed)
        },
        {
          label: "Filled Orders",
          value: formatInteger(activeFilledOrders(state).length),
          tone: "good"
        }
      ])
    );
    page.appendChild(element("h2", undefined, "Terminal Guaranteed PnL"));
    page.appendChild(terminalPnlTable(state));
    page.appendChild(element("h2", undefined, "Open PnL by Filled Order"));
    page.appendChild(openPnlTable(state));
    return page;
  }
};
