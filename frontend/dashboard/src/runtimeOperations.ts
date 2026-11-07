import { ReadOnlyDashboardClient } from "./api/readOnlyClient";

// Uses the existing read-only backend snapshot, independently of research views.
export function startRuntimeOperations(root: HTMLElement): void {
  const client = new ReadOnlyDashboardClient();
  function node(tag: string, text: string): HTMLElement {
    const el = document.createElement(tag);
    el.textContent = text;
    return el;
  }
  async function refresh(): Promise<void> {
    try {
      const snapshot = await client.latestSnapshot();
      const data = snapshot as unknown as Record<string, unknown> | null;
      const runtime = data?.runtime as Record<string, unknown> | undefined;
      root.replaceChildren(node("h1", "Polytope Runtime Operations"));
      root.appendChild(node("p", "Read-only · REPLAY / PAPER / SHADOW · Live orders disabled"));
      if (!runtime) {
        root.appendChild(node("p", "Runtime evidence unavailable. Check the backend dashboard file."));
        return;
      }
      root.appendChild(node("p", `Release ${String(runtime.release_version ?? "foundation")} · ${String(runtime.mode)} · Health ${String(runtime.health ?? "unknown")} · Snapshot received: ${new Date().toISOString()}. Feed is ${String(runtime.feed_kind)}; runtime ${String(runtime.lifecycle)}.`));
      const groups: Array<[string, unknown]> = [
        ["Runtime, feed, risk and recovery", runtime],
        ["Strategies and configuration epochs", data?.strategies],
        ["Actual maker orders owned by OMS", data?.maker_orders],
        ["Shared portfolio reservations", data?.portfolio_reservations],
        ["Global portfolio limits and encumbrances", data?.portfolio_risk],
        ["Portfolio and strategy paper PnL", { portfolio: data?.portfolio_pnl, strategies: data?.strategy_pnl_attribution }],
        ["Book integrity", data?.markets],
        ["Strategy intents", data?.intents],
        ["Risk decisions", data?.risk_decisions],
        ["Order states", data?.order_states],
        ["Account and PnL attribution", { account: data?.account, performance: data?.performance }],
        ["Processing latency percentiles", data?.latency_metrics ?? data?.latency],
        ["Recovery and reconciliation incidents", data?.incidents]
      ];
      for (const [title, value] of groups) {
        const section = node("section", "");
        section.className = "panel";
        section.append(node("h2", title), node("pre", JSON.stringify(value ?? null, null, 2)));
        root.appendChild(section);
      }
    } catch (error) {
      root.replaceChildren(node("h1", "Runtime telemetry unavailable"), node("p", String(error)));
    }
  }
  void refresh();
  window.setInterval(() => void refresh(), 2000);
}
