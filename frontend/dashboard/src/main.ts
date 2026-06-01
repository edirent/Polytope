import { ReadOnlyDashboardClient } from "./api/readOnlyClient";
import { AppShell } from "./components/appShell";
import { pages } from "./pages";
import {
  DashboardStore,
  refreshDashboardData
} from "./state/dashboardStore";
import "./styles.css";

const root = document.querySelector<HTMLElement>("#app");

if (root == null) {
  throw new Error("missing dashboard root");
}

const apiBase = import.meta.env.VITE_DASHBOARD_API_BASE ?? "";
const client = new ReadOnlyDashboardClient(apiBase);
const store = new DashboardStore();
const shell = new AppShell(root, pages);

store.subscribe((state) => shell.render(state));
window.addEventListener("dashboard:navigate", () => shell.render(store.get()));

void refreshDashboardData(client, store);

const closeStream = client.connectDashboardStream({
  onDashboard(snapshot) {
    store.update({
      snapshot,
      executionReports: snapshot.filled_orders ?? [],
      performance: snapshot.performance,
      regime: snapshot.regime,
      latency: snapshot.latency,
      streamStatus: "live",
      lastError: null
    });
  },
  onStateChange(streamStatus) {
    store.update({ streamStatus });
  },
  onError(message) {
    store.update({ lastError: message });
  }
});

const refreshTimer = window.setInterval(() => {
  void refreshDashboardData(client, store);
}, 5_000);

window.addEventListener("beforeunload", () => {
  window.clearInterval(refreshTimer);
  closeStream();
});
