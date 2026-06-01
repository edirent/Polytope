import { clear, element } from "../components/dom";
import { formatInteger } from "../components/format";
import { metricGrid } from "../components/metricGrid";
import { objectTable } from "../components/tableView";
import type { DashboardUiState } from "../state/dashboardStore";
import type { PageDefinition } from "./pageTypes";

function filteredMarkets(
  state: DashboardUiState,
  filterValue: string
): Array<Record<string, unknown>> {
  const filter = filterValue.trim().toLowerCase();
  if (filter.length === 0) {
    return state.markets;
  }
  return state.markets.filter((market) =>
    JSON.stringify(market).toLowerCase().includes(filter)
  );
}

export const marketsPage: PageDefinition = {
  id: "markets",
  title: "Markets",
  render(state) {
    const page = element("div", "page");
    page.appendChild(element("h1", undefined, "Markets"));
    page.appendChild(
      metricGrid([
        {
          label: "Markets",
          value: formatInteger(state.markets.length),
          detail: "read-only universe"
        }
      ])
    );

    const toolbar = element("div", "toolbar");
    const filter = document.createElement("input");
    filter.id = "market-filter";
    filter.placeholder = "Filter local rows";
    filter.type = "search";
    filter.className = "input";
    toolbar.appendChild(filter);
    page.appendChild(toolbar);

    const tableHost = element("div");
    const renderTable = () => {
      clear(tableHost);
      tableHost.appendChild(
        objectTable(filteredMarkets(state, filter.value), "No markets reported yet.")
      );
    };
    filter.addEventListener("input", renderTable);
    renderTable();
    page.appendChild(tableHost);
    return page;
  }
};
