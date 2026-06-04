import type { DashboardUiState } from "../state/dashboardStore";

export interface PageDefinition {
  id:
    | "overview"
    | "markets"
    | "signals"
    | "risk"
    | "execution"
    | "market-maker"
    | "pnl"
    | "regime"
    | "latency";
  title: string;
  render: (state: DashboardUiState) => HTMLElement;
}
