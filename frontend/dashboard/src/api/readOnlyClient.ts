import type {
  DashboardSnapshot,
  EquityResponse,
  ExecutionReportsResponse,
  HealthResponse,
  IntentsResponse,
  LatestSnapshotResponse,
  MarketsResponse,
  PerformanceSnapshot,
  RegimeSnapshot,
  RiskDecisionsResponse,
  LatencySnapshot
} from "./dashboardTypes";

export type DashboardStreamHandler = (snapshot: DashboardSnapshot) => void;
export type StreamStateHandler = (state: "live" | "reconnecting") => void;

export interface DashboardStreamHandlers {
  onDashboard: DashboardStreamHandler;
  onStateChange: StreamStateHandler;
  onError: (message: string) => void;
}

const API_PREFIX = "/api/v1/";
const STREAM_PATH = "/stream/v1/dashboard";

function normalizeBaseUrl(baseUrl: string): string {
  return baseUrl.endsWith("/") ? baseUrl.slice(0, -1) : baseUrl;
}

function assertReadOnlyPath(path: string): void {
  if (!path.startsWith(API_PREFIX) && path !== STREAM_PATH) {
    throw new Error(`Unsupported read-only dashboard path: ${path}`);
  }
}

function snapshotOrNull(payload: LatestSnapshotResponse): DashboardSnapshot | null {
  if ("snapshot" in payload) {
    return payload.snapshot;
  }
  return payload;
}

export class ReadOnlyDashboardClient {
  private readonly baseUrl: string;

  constructor(baseUrl = "") {
    this.baseUrl = normalizeBaseUrl(baseUrl);
  }

  async health(): Promise<HealthResponse> {
    return this.getJson<HealthResponse>("/api/v1/health");
  }

  async latestSnapshot(): Promise<DashboardSnapshot | null> {
    const payload = await this.getJson<LatestSnapshotResponse>(
      "/api/v1/snapshot/latest"
    );
    return snapshotOrNull(payload);
  }

  async markets(): Promise<MarketsResponse> {
    return this.getJson<MarketsResponse>("/api/v1/markets");
  }

  async intents(limit = 100): Promise<IntentsResponse> {
    return this.getJson<IntentsResponse>(
      `/api/v1/intents?limit=${encodeURIComponent(String(limit))}`
    );
  }

  async riskDecisions(limit = 100): Promise<RiskDecisionsResponse> {
    return this.getJson<RiskDecisionsResponse>(
      `/api/v1/risk-decisions?limit=${encodeURIComponent(String(limit))}`
    );
  }

  async executionReports(limit = 100): Promise<ExecutionReportsResponse> {
    return this.getJson<ExecutionReportsResponse>(
      `/api/v1/execution-reports?limit=${encodeURIComponent(String(limit))}`
    );
  }

  async equity(): Promise<EquityResponse> {
    return this.getJson<EquityResponse>("/api/v1/pnl/equity");
  }

  async performance(): Promise<PerformanceSnapshot> {
    return this.getJson<PerformanceSnapshot>("/api/v1/performance");
  }

  async regime(): Promise<RegimeSnapshot> {
    return this.getJson<RegimeSnapshot>("/api/v1/regime");
  }

  async latency(): Promise<LatencySnapshot> {
    return this.getJson<LatencySnapshot>("/api/v1/latency");
  }

  connectDashboardStream(handlers: DashboardStreamHandlers): () => void {
    const path = STREAM_PATH;
    assertReadOnlyPath(path);

    const source = new EventSource(`${this.baseUrl}${path}`);
    source.onopen = () => handlers.onStateChange("live");
    source.onerror = () => {
      handlers.onStateChange("reconnecting");
      handlers.onError("dashboard stream disconnected");
    };
    source.addEventListener("dashboard", (event) => {
      const message = event as MessageEvent<string>;
      try {
        handlers.onDashboard(JSON.parse(message.data) as DashboardSnapshot);
      } catch (error) {
        handlers.onError(
          error instanceof Error ? error.message : "invalid dashboard event"
        );
      }
    });

    return () => {
      source.close();
    };
  }

  private async getJson<T>(path: string): Promise<T> {
    assertReadOnlyPath(path);

    const response = await fetch(`${this.baseUrl}${path}`, {
      method: "GET",
      headers: {
        Accept: "application/json"
      },
      cache: "no-store"
    });

    if (!response.ok) {
      throw new Error(`GET ${path} failed with ${response.status}`);
    }

    return (await response.json()) as T;
  }
}
