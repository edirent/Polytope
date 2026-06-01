import type { DashboardUiState } from "../state/dashboardStore";

export interface PageDefinition {
  id:
    | "overview"
    | "markets"
    | "signals"
    | "risk"
    | "execution"
    | "pnl"
    | "regime"
    | "latency";
  title: string;
  render: (state: DashboardUiState) => HTMLElement;
}
