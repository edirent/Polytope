import "./styles.css";

type Tone = "good" | "bad" | "warn" | "neutral";

interface ProgressSnapshot {
  rows: number;
  marketsSeen: number;
  completeMarkets: number;
  incompleteMarkets: number;
  firstMarket: string;
  firstUtc: string;
  lastMarket: string;
  lastUtc: string;
  lastTauSec: number | null;
  sampleMtimeMs: number | null;
  sampleAgeSec: number | null;
  tmuxSession: string;
  tmuxActive: boolean;
}

interface SummaryMetrics {
  trades: number;
  trade_rate: number | null;
  win_rate: number | null;
  avg_entry: number | null;
  avg_edge: number | null;
  mean_pnl: number | null;
  median_pnl: number | null;
  p05: number | null;
  min: number | null;
  max: number | null;
  max_drawdown: number | null;
  longest_loss_streak: number;
  sum_pnl: number | null;
}

interface WinnerGroup {
  count: number;
  mean_pnl: number | null;
  avg_entry: number | null;
  avg_z: number | null;
  avg_spot_bps: number | null;
}

interface WinningSplit {
  up_winning_trades: WinnerGroup;
  down_winning_trades: WinnerGroup;
  up_down_count_ratio: number | null;
}

interface CostStress {
  cost_per_share: number;
  mean_pnl: number | null;
  median_pnl: number | null;
  p05: number | null;
  min: number | null;
  max_drawdown: number | null;
  longest_loss_streak: number;
  sum_pnl: number | null;
}

interface HourlyBucket {
  bucket_start_utc: string;
  trade_count: number;
  pnl: number;
}

interface StrategyTrade {
  market: string;
  entry_ts: string;
  tau: number;
  entry_q50_vwap: number;
  edge: number;
  fair_sigma65: number;
  z_score_sigma65: number;
  spot_vs_K_bps: number;
  poly_up_mid_delta_3s: number;
  settlement_winner: string;
  pnl: number;
}

interface VersionAudit {
  summary: SummaryMetrics;
  cost_stress: Record<string, CostStress>;
  winning_split: WinningSplit;
  trades: StrategyTrade[];
  time_distribution_by_cost: Record<
    string,
    { hourly: { buckets: HourlyBucket[] } }
  >;
}

interface AuditSnapshot {
  market_count: number;
  run_dir: string;
  versions: Record<string, VersionAudit>;
}

interface MetadataSnapshot {
  completed_market_count: number;
  row_count: number;
  excluded_incomplete_markets: string[];
}

interface ApiSnapshot {
  nowUtc: string;
  runDir: string;
  progress: ProgressSnapshot;
  audit: AuditSnapshot | null;
  metadata: MetadataSnapshot | null;
  settlementSources: Record<string, number>;
  settlementWinners: Record<string, number>;
  auditMtimeMs: number | null;
  files: Record<string, string>;
  error?: string;
}

interface MetricSpec {
  label: string;
  value: string;
  detail?: string;
  tone?: Tone;
}

const root = document.querySelector<HTMLElement>("#app");

if (root == null) {
  throw new Error("missing dashboard root");
}

const appRoot: HTMLElement = root;

let latest: ApiSnapshot | null = null;
let lastError: string | null = null;

function el<K extends keyof HTMLElementTagNameMap>(
  tag: K,
  className?: string,
  text?: string
): HTMLElementTagNameMap[K] {
  const node = document.createElement(tag);
  if (className) {
    node.className = className;
  }
  if (text != null) {
    node.textContent = text;
  }
  return node;
}

function clear(node: HTMLElement): void {
  while (node.firstChild) {
    node.removeChild(node.firstChild);
  }
}

function numberValue(value: number | null | undefined): number | null {
  return value == null || !Number.isFinite(value) ? null : value;
}

function formatInteger(value: number | null | undefined): string {
  const clean = numberValue(value);
  if (clean == null) {
    return "-";
  }
  return new Intl.NumberFormat("en-US", { maximumFractionDigits: 0 }).format(clean);
}

function formatPct(value: number | null | undefined): string {
  const clean = numberValue(value);
  return clean == null ? "-" : `${(clean * 100).toFixed(1)}%`;
}

function formatCents(value: number | null | undefined): string {
  const clean = numberValue(value);
  return clean == null ? "-" : `${(clean * 100).toFixed(2)}c`;
}

function formatSignedCents(value: number | null | undefined): string {
  const clean = numberValue(value);
  if (clean == null) {
    return "-";
  }
  const sign = clean > 0 ? "+" : "";
  return `${sign}${(clean * 100).toFixed(2)}c`;
}

function formatBps(value: number | null | undefined): string {
  const clean = numberValue(value);
  return clean == null ? "-" : `${clean.toFixed(3)} bps`;
}

function formatUtc(value: string | null | undefined): string {
  if (!value) {
    return "-";
  }
  const date = new Date(value);
  if (Number.isNaN(date.getTime())) {
    return value;
  }
  return date.toISOString().replace(".000", "");
}

function pnlTone(value: number | null | undefined): Tone {
  const clean = numberValue(value);
  if (clean == null || clean === 0) {
    return "neutral";
  }
  return clean > 0 ? "good" : "bad";
}

function statusTone(progress: ProgressSnapshot | null): Tone {
  if (progress == null) {
    return "warn";
  }
  if (!progress.tmuxActive) {
    return "bad";
  }
  if ((progress.sampleAgeSec ?? 9999) > 120) {
    return "warn";
  }
  return "good";
}

function versionLabel(version: string): string {
  return version
    .replace("V2.1_spot_gt_K", "V2.1 spot>K")
    .replace("V2.2C_spot_gt_K_poly_d3_ge_0", "V2.2C spot>K + poly d3>=0")
    .replace("V2.3a_V2.2C_z_gt_0", "V2.3a z>0")
    .replace("V2.3b_V2.2C_z_gt_0p02", "V2.3b z>0.02")
    .replace("V2.3c_V2.2C_z_gt_0p03", "V2.3c z>0.03")
    .replace("V2.3d_V2.2C_z_gt_0p05", "V2.3d z>0.05")
    .replace("V2.4_candidate_family", "V2.4 candidate")
    .replaceAll("_", " ");
}

function metricGrid(metrics: MetricSpec[]): HTMLElement {
  const grid = el("div", "metric-grid");
  for (const metric of metrics) {
    const tile = el("section", `metric metric-${metric.tone ?? "neutral"}`);
    tile.appendChild(el("span", "metric-label", metric.label));
    tile.appendChild(el("strong", "metric-value", metric.value));
    if (metric.detail) {
      tile.appendChild(el("span", "metric-detail", metric.detail));
    }
    grid.appendChild(tile);
  }
  return grid;
}

function table(headers: string[], rows: Array<Array<string | HTMLElement>>): HTMLElement {
  const wrap = el("div", "table-wrap");
  const node = el("table", "data-table");
  const thead = document.createElement("thead");
  const headRow = document.createElement("tr");
  for (const header of headers) {
    headRow.appendChild(el("th", undefined, header));
  }
  thead.appendChild(headRow);
  node.appendChild(thead);

  const tbody = document.createElement("tbody");
  for (const row of rows) {
    const tr = document.createElement("tr");
    for (const cell of row) {
      const td = document.createElement("td");
      if (typeof cell === "string") {
        td.textContent = cell;
      } else {
        td.appendChild(cell);
      }
      tr.appendChild(td);
    }
    tbody.appendChild(tr);
  }
  node.appendChild(tbody);
  wrap.appendChild(node);
  return wrap;
}

function pnlCell(value: number | null | undefined): HTMLElement {
  const node = el("span", pnlTone(value) === "good" ? "pnl-positive" : pnlTone(value) === "bad" ? "pnl-negative" : "");
  node.textContent = formatSignedCents(value);
  return node;
}

function renderHeader(snapshot: ApiSnapshot): HTMLElement {
  const header = el("section", "page-header");
  const titleRow = el("div", "title-row");
  titleRow.appendChild(el("h1", undefined, "BTC 5m Strategy Monitor"));
  const badge = el("span", `status-pill status-${statusTone(snapshot.progress)}`);
  badge.appendChild(el("span", "status-label", "capture"));
  badge.appendChild(
    el(
      "span",
      "status-value",
      snapshot.progress.tmuxActive ? "running" : "stopped"
    )
  );
  titleRow.appendChild(badge);
  header.appendChild(titleRow);
  header.appendChild(
    el(
      "p",
      "page-subtitle",
      `${snapshot.runDir} | last sample ${formatUtc(snapshot.progress.lastUtc)}`
    )
  );
  if (lastError) {
    header.appendChild(el("div", "inline-error", lastError));
  }
  return header;
}

function renderTopMetrics(snapshot: ApiSnapshot): HTMLElement {
  const v22c = snapshot.audit?.versions["V2.2C_spot_gt_K_poly_d3_ge_0"];
  const v2 = snapshot.audit?.versions.V2;
  return metricGrid([
    {
      label: "Live Complete",
      value: formatInteger(snapshot.progress.completeMarkets),
      detail: `${formatInteger(snapshot.progress.marketsSeen)} seen / ${formatInteger(snapshot.progress.rows)} rows`,
      tone: snapshot.progress.completeMarkets >= 100 ? "good" : "neutral"
    },
    {
      label: "Audit Markets",
      value: formatInteger(snapshot.metadata?.completed_market_count ?? snapshot.audit?.market_count),
      detail: `${formatInteger(snapshot.settlementWinners.up ?? 0)} UP / ${formatInteger(snapshot.settlementWinners.down ?? 0)} DOWN`
    },
    {
      label: "V2.2C Trades",
      value: formatInteger(v22c?.summary.trades),
      detail: `${formatPct(v22c?.summary.trade_rate)} trade rate`
    },
    {
      label: "V2.2C Win",
      value: formatPct(v22c?.summary.win_rate),
      detail: `UP/DOWN ${formatInteger(v22c?.winning_split.up_winning_trades.count)}/${formatInteger(v22c?.winning_split.down_winning_trades.count)}`,
      tone: (v22c?.summary.win_rate ?? 0) > 0.55 ? "good" : "warn"
    },
    {
      label: "V2.2C Mean PnL",
      value: formatSignedCents(v22c?.summary.mean_pnl),
      detail: `median ${formatSignedCents(v22c?.summary.median_pnl)}`,
      tone: pnlTone(v22c?.summary.mean_pnl)
    },
    {
      label: "V2.2C Sum PnL",
      value: formatSignedCents(v22c?.summary.sum_pnl),
      detail: "per-share total",
      tone: pnlTone(v22c?.summary.sum_pnl)
    },
    {
      label: "V2 Drawdown",
      value: formatCents(v2?.summary.max_drawdown),
      detail: `${formatInteger(v2?.summary.longest_loss_streak)} loss streak`,
      tone: (v2?.summary.longest_loss_streak ?? 0) > 4 ? "bad" : "neutral"
    },
    {
      label: "Audit Freshness",
      value:
        snapshot.auditMtimeMs == null
          ? "-"
          : `${Math.max(0, Math.round((Date.now() - snapshot.auditMtimeMs) / 1000))}s`,
      detail: `sample age ${formatInteger(snapshot.progress.sampleAgeSec)}s`,
      tone: (snapshot.progress.sampleAgeSec ?? 9999) < 120 ? "good" : "warn"
    }
  ]);
}

function renderVersionTable(snapshot: ApiSnapshot): HTMLElement {
  const versions = snapshot.audit?.versions ?? {};
  const rows = Object.entries(versions).map(([name, version]) => {
    const summary = version.summary;
    const split = version.winning_split;
    return [
      versionLabel(name),
      formatInteger(summary.trades),
      formatPct(summary.win_rate),
      pnlCell(summary.mean_pnl),
      pnlCell(summary.median_pnl),
      pnlCell(summary.p05),
      pnlCell(summary.min),
      formatCents(summary.max_drawdown),
      formatInteger(summary.longest_loss_streak),
      pnlCell(summary.sum_pnl),
      `${formatInteger(split.up_winning_trades.count)}/${formatInteger(split.down_winning_trades.count)}`
    ];
  });
  return table(
    [
      "Version",
      "Trades",
      "Win",
      "Mean",
      "Median",
      "P05",
      "Min",
      "Drawdown",
      "Streak",
      "Sum",
      "UP/DOWN"
    ],
    rows
  );
}

function renderCostStress(version: VersionAudit | undefined): HTMLElement {
  const costs = version?.cost_stress ?? {};
  const rows = Object.values(costs).map((cost) => [
    formatCents(cost.cost_per_share),
    pnlCell(cost.mean_pnl),
    pnlCell(cost.median_pnl),
    pnlCell(cost.p05),
    pnlCell(cost.min),
    formatCents(cost.max_drawdown),
    formatInteger(cost.longest_loss_streak),
    pnlCell(cost.sum_pnl)
  ]);
  return table(
    ["Cost", "Mean", "Median", "P05", "Min", "Drawdown", "Streak", "Sum"],
    rows
  );
}

function renderWinnerSplit(version: VersionAudit | undefined): HTMLElement {
  if (!version) {
    return table(["Winner", "Count", "Mean", "Avg Entry", "Avg Z", "Avg Spot"], []);
  }
  const split = version.winning_split;
  const rows: Array<Array<string | HTMLElement>> = [
    [
      "UP-winning",
      formatInteger(split.up_winning_trades.count),
      pnlCell(split.up_winning_trades.mean_pnl),
      formatCents(split.up_winning_trades.avg_entry),
      split.up_winning_trades.avg_z?.toFixed(4) ?? "-",
      formatBps(split.up_winning_trades.avg_spot_bps)
    ],
    [
      "DOWN-winning",
      formatInteger(split.down_winning_trades.count),
      pnlCell(split.down_winning_trades.mean_pnl),
      formatCents(split.down_winning_trades.avg_entry),
      split.down_winning_trades.avg_z?.toFixed(4) ?? "-",
      formatBps(split.down_winning_trades.avg_spot_bps)
    ]
  ];
  return table(["Winner", "Count", "Mean", "Avg Entry", "Avg Z", "Avg Spot"], rows);
}

function renderTrades(version: VersionAudit | undefined): HTMLElement {
  const trades = version?.trades ?? [];
  return table(
    [
      "Market",
      "Entry",
      "Winner",
      "Tau",
      "Entry",
      "Edge",
      "Fair",
      "Z",
      "Spot",
      "Poly d3",
      "PnL"
    ],
    trades.map((trade) => [
      trade.market,
      formatUtc(trade.entry_ts),
      trade.settlement_winner.toUpperCase(),
      `${trade.tau.toFixed(0)}s`,
      formatCents(trade.entry_q50_vwap),
      formatSignedCents(trade.edge),
      formatCents(trade.fair_sigma65),
      trade.z_score_sigma65.toFixed(4),
      formatBps(trade.spot_vs_K_bps),
      formatSignedCents(trade.poly_up_mid_delta_3s),
      pnlCell(trade.pnl)
    ])
  );
}

function renderHourly(version: VersionAudit | undefined): HTMLElement {
  const buckets =
    version?.time_distribution_by_cost["cost_0.0000"]?.hourly.buckets ?? [];
  return table(
    ["Hour", "Trades", "PnL"],
    buckets.map((bucket) => [
      formatUtc(bucket.bucket_start_utc),
      formatInteger(bucket.trade_count),
      pnlCell(bucket.pnl)
    ])
  );
}

function renderPanel(title: string, child: HTMLElement): HTMLElement {
  const panel = el("section", "panel");
  panel.appendChild(el("h2", undefined, title));
  panel.appendChild(child);
  return panel;
}

function render(snapshot: ApiSnapshot | null): void {
  clear(appRoot);
  const page = el("main", "strategy-page");
  if (!snapshot) {
    page.appendChild(el("h1", undefined, "BTC 5m Strategy Monitor"));
    page.appendChild(el("p", "page-subtitle", "Loading current strategy metrics."));
    appRoot.appendChild(page);
    return;
  }

  const v22c = snapshot.audit?.versions["V2.2C_spot_gt_K_poly_d3_ge_0"];
  page.appendChild(renderHeader(snapshot));
  page.appendChild(renderTopMetrics(snapshot));
  page.appendChild(renderPanel("Version PnL", renderVersionTable(snapshot)));

  const split = el("div", "two-column");
  split.appendChild(renderPanel("V2.2C Cost Stress", renderCostStress(v22c)));
  split.appendChild(renderPanel("V2.2C UP/DOWN Split", renderWinnerSplit(v22c)));
  page.appendChild(split);

  const lower = el("div", "two-column");
  lower.appendChild(renderPanel("V2.2C Trades", renderTrades(v22c)));
  lower.appendChild(renderPanel("V2.2C Hourly PnL", renderHourly(v22c)));
  page.appendChild(lower);
  appRoot.appendChild(page);
}

async function refresh(): Promise<void> {
  try {
    const response = await fetch(`/api/btc5m/current?ts=${Date.now()}`, {
      cache: "no-store"
    });
    if (!response.ok) {
      throw new Error(`strategy API ${response.status}`);
    }
    latest = (await response.json()) as ApiSnapshot;
    lastError = latest.error ?? null;
  } catch (error) {
    lastError = error instanceof Error ? error.message : String(error);
  }
  render(latest);
}

if (new URLSearchParams(window.location.search).get("view") === "runtime") {
  void import("./runtimeOperations").then(({ startRuntimeOperations }) => startRuntimeOperations(appRoot));
} else {
  render(null);
  void refresh();
  window.setInterval(() => { void refresh(); }, 10_000);
}
