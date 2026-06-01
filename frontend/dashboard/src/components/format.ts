export function formatInteger(value: number | null | undefined): string {
  if (value == null || !Number.isFinite(value)) {
    return "0";
  }
  return new Intl.NumberFormat("en-US", {
    maximumFractionDigits: 0
  }).format(value);
}

export function formatTick(value: number | null | undefined): string {
  if (value == null || !Number.isFinite(value)) {
    return "0.000000";
  }
  return (value / 1_000_000).toFixed(6);
}

export function formatMicros(ns: number | null | undefined): string {
  if (valueMissing(ns)) {
    return "0.000 us";
  }
  return `${(ns / 1_000).toFixed(3)} us`;
}

export function formatPercent(value: number | null | undefined): string {
  if (valueMissing(value)) {
    return "0.00%";
  }
  return `${(value * 100).toFixed(2)}%`;
}

export function formatRatio(value: number | null | undefined): string {
  if (valueMissing(value)) {
    return "0.0000";
  }
  return value.toFixed(4);
}

export function formatAgo(timestampMs: number | null): string {
  if (timestampMs == null) {
    return "never";
  }
  const seconds = Math.max(0, Math.round((Date.now() - timestampMs) / 1000));
  return `${seconds}s ago`;
}

function valueMissing(value: number | null | undefined): value is null | undefined {
  return value == null || !Number.isFinite(value);
}
