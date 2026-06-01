import type {
  DashboardSnapshot,
  EquityResponse,
  ExecutionReportsResponse,
  FilledOrder,
  HealthResponse,
  IntentsResponse,
  LatencySnapshot,
  MarketsResponse,
  PerformanceSnapshot,
  RegimeSnapshot,
  RiskDecisionsResponse,
  StreamStatus
} from "../api/dashboardTypes";
import type { ReadOnlyDashboardClient } from "../api/readOnlyClient";

export interface DashboardUiState {
  health: HealthResponse | null;
  snapshot: DashboardSnapshot | null;
  markets: MarketsResponse["markets"];
  intents: IntentsResponse["intents"];
  riskDecisions: RiskDecisionsResponse["risk_decisions"];
  executionReports: ExecutionReportsResponse["execution_reports"];
  equity: EquityResponse | null;
  performance: PerformanceSnapshot | null;
  regime: RegimeSnapshot | null;
  latency: LatencySnapshot | null;
  streamStatus: StreamStatus;
  lastError: string | null;
  updatedAt: number | null;
}

export function filledOrdersFromSnapshot(
  snapshot: DashboardSnapshot | null
): FilledOrder[] {
  return snapshot?.filled_orders ?? [];
}

type Listener = (state: DashboardUiState) => void;

const initialState: DashboardUiState = {
  health: null,
  snapshot: null,
  markets: [],
  intents: [],
  riskDecisions: [],
  executionReports: [],
  equity: null,
  performance: null,
  regime: null,
  latency: null,
  streamStatus: "connecting",
  lastError: null,
  updatedAt: null
};

export class DashboardStore {
  private state: DashboardUiState = { ...initialState };
  private readonly listeners = new Set<Listener>();

  get(): DashboardUiState {
    return this.state;
  }

  subscribe(listener: Listener): () => void {
    this.listeners.add(listener);
    listener(this.state);
    return () => {
      this.listeners.delete(listener);
    };
  }

  update(patch: Partial<DashboardUiState>): void {
    this.state = {
      ...this.state,
      ...patch,
      updatedAt: Date.now()
    };
    for (const listener of this.listeners) {
      listener(this.state);
    }
  }
}

async function settle<T>(
  task: Promise<T>,
  onSuccess: (value: T) => Partial<DashboardUiState>
): Promise<Partial<DashboardUiState>> {
  try {
    return onSuccess(await task);
  } catch (error) {
    return {
      lastError: error instanceof Error ? error.message : "dashboard request failed"
    };
  }
}

export async function refreshDashboardData(
  client: ReadOnlyDashboardClient,
  store: DashboardStore
): Promise<void> {
  const patches = await Promise.all([
    settle(client.health(), (health) => ({ health })),
    settle(client.latestSnapshot(), (snapshot) => ({ snapshot })),
    settle(client.markets(), (markets) => ({ markets: markets.markets })),
    settle(client.intents(), (intents) => ({ intents: intents.intents })),
    settle(client.riskDecisions(), (riskDecisions) => ({
      riskDecisions: riskDecisions.risk_decisions
    })),
    settle(client.executionReports(), (executionReports) => ({
      executionReports: executionReports.execution_reports
    })),
    settle(client.equity(), (equity) => ({ equity })),
    settle(client.performance(), (performance) => ({ performance })),
    settle(client.regime(), (regime) => ({ regime })),
    settle(client.latency(), (latency) => ({ latency }))
  ]);

  const merged = Object.assign({}, ...patches) as Partial<DashboardUiState>;
  if (
    (merged.executionReports == null || merged.executionReports.length === 0) &&
    merged.snapshot != null &&
    filledOrdersFromSnapshot(merged.snapshot).length > 0
  ) {
    merged.executionReports = filledOrdersFromSnapshot(merged.snapshot);
  }

  store.update(merged);
}
