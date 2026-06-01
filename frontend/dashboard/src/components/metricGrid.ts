import { element } from "./dom";

export interface MetricItem {
  label: string;
  value: string;
  detail?: string;
  tone?: "neutral" | "good" | "warn" | "bad";
}

export function metricGrid(items: MetricItem[]): HTMLElement {
  const grid = element("div", "metric-grid");
  for (const item of items) {
    const tile = element("section", `metric metric-${item.tone ?? "neutral"}`);
    tile.appendChild(element("span", "metric-label", item.label));
    tile.appendChild(element("strong", "metric-value", item.value));
    if (item.detail) {
      tile.appendChild(element("span", "metric-detail", item.detail));
    }
    grid.appendChild(tile);
  }
  return grid;
}
