import { element } from "../components/dom";
import { formatInteger } from "../components/format";
import { metricGrid } from "../components/metricGrid";
import { objectTable } from "../components/tableView";
import type { PageDefinition } from "./pageTypes";

export const signalsPage: PageDefinition = {
  id: "signals",
  title: "Signals",
  render(state) {
    const page = element("div", "page");
    page.appendChild(element("h1", undefined, "Signals"));
    page.appendChild(
      metricGrid([
        {
          label: "Published",
          value: formatInteger(state.snapshot?.signal.intents_published)
        },
        {
          label: "Paper Opportunities",
          value: formatInteger(state.snapshot?.signal.paper_opportunities),
          tone: "good"
        },
        {
          label: "Rejected",
          value: formatInteger(state.snapshot?.signal.rejected),
          tone: "warn"
        },
        {
          label: "Output Hash",
          value: formatInteger(state.snapshot?.signal.output_hash)
        }
      ])
    );
    page.appendChild(objectTable(state.intents, "No intents reported yet."));
    return page;
  }
};
