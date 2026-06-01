import { element } from "../components/dom";
import { formatInteger, formatTick } from "../components/format";
import { metricGrid } from "../components/metricGrid";
import { statusPill } from "../components/statusPill";
import {
  activeFilledOrders,
  displayMarket,
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

function filledOrdersTable(state: Parameters<PageDefinition["render"]>[0]): HTMLElement {
  const orders = activeFilledOrders(state);
  const wrapper = element("div", "table-wrap");
  if (orders.length === 0) {
    wrapper.appendChild(element("p", "empty", "No filled paper orders yet."));
    return wrapper;
  }

  const table = document.createElement("table");
  table.className = "data-table";
  const thead = document.createElement("thead");
  const headRow = document.createElement("tr");
  for (const label of [
    "Market",
    "Asset",
    "Side",
    "Qty",
    "Avg Fill",
    "Limit",
    "Mark",
    "Open PnL",
    "Plan"
  ]) {
    const th = document.createElement("th");
    th.textContent = label;
    headRow.appendChild(th);
  }
  thead.appendChild(headRow);
  table.appendChild(thead);

  const tbody = document.createElement("tbody");
  for (const order of orders.slice().reverse()) {
    const tr = document.createElement("tr");
    const cells = [
      displayMarket(order),
      shortAssetId(order.asset_id),
      order.side,
      formatInteger(order.filled_lots),
      formatTick(order.avg_fill_price_tick),
      formatTick(order.limit_price_tick),
      order.mark_price_tick === 0
        ? order.mark_quality
        : `${formatTick(order.mark_price_tick)} ${order.mark_quality}`,
      formatTick(order.unrealized_pnl_tick),
      `${order.plan_id}/${order.child_order_id}`
    ];

    for (let i = 0; i < cells.length; ++i) {
      const td = document.createElement("td");
      td.textContent = cells[i];
      if (i === 1 || i === 8) {
        td.className = "mono";
      }
      if (i === 7) {
        td.classList.add(pnlClass(order.unrealized_pnl_tick));
      }
      tr.appendChild(td);
    }
    tbody.appendChild(tr);
  }
  table.appendChild(tbody);
  wrapper.appendChild(table);
  return wrapper;
}

export const executionPage: PageDefinition = {
  id: "execution",
  title: "Execution",
  render(state) {
    const page = element("div", "page");
    page.appendChild(element("h1", undefined, "Execution"));
    const row = element("div", "status-row");
    row.appendChild(
      statusPill("Execution Regime", state.regime?.execution ?? "Unknown")
    );
    page.appendChild(row);
    page.appendChild(
      metricGrid([
        {
          label: "Plans Created",
          value: formatInteger(state.snapshot?.execution.plans_created)
        },
        {
          label: "Plans Filled",
          value: formatInteger(state.snapshot?.execution.plans_filled),
          tone: "good"
        },
        {
          label: "Plans Failed",
          value: formatInteger(state.snapshot?.execution.plans_failed),
          tone: "warn"
        },
        {
          label: "Output Hash",
          value: formatInteger(state.snapshot?.execution.output_hash)
        },
        {
          label: "Filled Orders",
          value: formatInteger(activeFilledOrders(state).length),
          tone: "good"
        }
      ])
    );
    page.appendChild(element("h2", undefined, "Filled Paper Orders"));
    page.appendChild(filledOrdersTable(state));
    return page;
  }
};
