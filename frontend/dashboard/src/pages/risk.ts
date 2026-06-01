import { element } from "../components/dom";
import { formatInteger } from "../components/format";
import { metricGrid } from "../components/metricGrid";
import { statusPill } from "../components/statusPill";
import { objectTable } from "../components/tableView";
import type { PageDefinition } from "./pageTypes";

export const riskPage: PageDefinition = {
  id: "risk",
  title: "Risk",
  render(state) {
    const page = element("div", "page");
    page.appendChild(element("h1", undefined, "Risk"));
    const row = element("div", "status-row");
    row.appendChild(statusPill("Risk Regime", state.regime?.risk ?? "Unknown"));
    page.appendChild(row);
    page.appendChild(
      metricGrid([
        {
          label: "Decisions",
          value: formatInteger(state.snapshot?.risk.decisions)
        },
        {
          label: "Approved",
          value: formatInteger(state.snapshot?.risk.approved),
          tone: "good"
        },
        {
          label: "Rejected",
          value: formatInteger(state.snapshot?.risk.rejected),
          tone: "warn"
        },
        {
          label: "Output Hash",
          value: formatInteger(state.snapshot?.risk.output_hash)
        }
      ])
    );
    page.appendChild(
      objectTable(state.riskDecisions, "No risk decisions reported yet.")
    );
    return page;
  }
};
