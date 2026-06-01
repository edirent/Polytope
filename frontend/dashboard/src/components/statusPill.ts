import { element } from "./dom";

export function statusPill(label: string, state: string): HTMLElement {
  const normalized = state.toLowerCase();
  const tone =
    normalized.includes("healthy") || normalized === "live"
      ? "good"
      : normalized.includes("stale") ||
          normalized.includes("degraded") ||
          normalized.includes("reconnecting") ||
          normalized.includes("partial")
        ? "warn"
        : normalized.includes("error") ||
            normalized.includes("crossed") ||
            normalized.includes("kill")
          ? "bad"
          : "neutral";

  const pill = element("span", `status-pill status-${tone}`);
  pill.appendChild(element("span", "status-label", label));
  pill.appendChild(element("strong", "status-value", state));
  return pill;
}
