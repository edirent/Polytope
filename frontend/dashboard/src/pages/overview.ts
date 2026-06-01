import { sparkline } from "../charts/sparkline";
import { element } from "../components/dom";
import { formatAgo, formatInteger, formatMicros, formatTick } from "../components/format";
import { metricGrid } from "../components/metricGrid";
import { statusPill } from "../components/statusPill";
import type { PageDefinition } from "./pageTypes";

export const overviewPage: PageDefinition = {
  id: "overview",
  title: "Overview",
  render(state) {
    const page = element("div", "page page-overview");
    const header = element("div", "page-header");
    header.appendChild(element("h1", undefined, "Overview"));
    header.appendChild(
      element(
        "p",
        "page-subtitle",
        `Read-only paper runtime. Updated ${formatAgo(state.updatedAt)}.`
      )
    );
    page.appendChild(header);

    const statusRow = element("div", "status-row");
    statusRow.appendChild(statusPill("Backend", state.health?.mode ?? "unknown"));
    statusRow.appendChild(
      statusPill(
        "Paper",
        state.health?.paper_trading_running ? "Running" : "Stopped"
      )
    );
    statusRow.appendChild(statusPill("Stream", state.streamStatus));
    statusRow.appendChild(statusPill("Data", state.regime?.data ?? "Unknown"));
    statusRow.appendChild(statusPill("Risk", state.regime?.risk ?? "Unknown"));
    page.appendChild(statusRow);

    page.appendChild(
      metricGrid([
        {
          label: "Cash",
          value: formatTick(state.snapshot?.account.cash_balance_tick),
          detail: "paper account"
        },
        {
          label: "Unrealized PnL",
          value: formatTick(state.snapshot?.account.unrealized_pnl_tick),
          tone: (state.snapshot?.account.unrealized_pnl_tick ?? 0) >= 0 ? "good" : "bad"
        },
        {
          label: "Signals",
          value: formatInteger(state.snapshot?.signal.paper_opportunities),
          detail: "paper opportunities"
        },
        {
          label: "Approvals",
          value: formatInteger(state.snapshot?.risk.approved),
          detail: "risk accepted"
        },
        {
          label: "Plans Filled",
          value: formatInteger(state.snapshot?.execution.plans_filled),
          detail: "paper execution"
        },
        {
          label: "End to End",
          value: formatMicros(state.latency?.end_to_end_ns),
          detail: "latest latency"
        }
      ])
    );

    const chartBand = element("section", "panel");
    chartBand.appendChild(element("h2", undefined, "Latency Shape"));
    chartBand.appendChild(
      sparkline(
        [
          state.latency?.feed_to_state_ns ?? 0,
          state.latency?.state_to_signal_ns ?? 0,
          state.latency?.signal_to_risk_ns ?? 0,
          state.latency?.risk_to_execution_ns ?? 0,
          state.latency?.end_to_end_ns ?? 0
        ],
        "Latest latency components"
      )
    );
    page.appendChild(chartBand);

    if (state.lastError) {
      page.appendChild(element("p", "inline-error", state.lastError));
    }

    return page;
  }
};
