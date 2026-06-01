import { element } from "../components/dom";
import { formatMicros } from "../components/format";
import { metricGrid } from "../components/metricGrid";
import type { PageDefinition } from "./pageTypes";

export const latencyPage: PageDefinition = {
  id: "latency",
  title: "Latency",
  render(state) {
    const page = element("div", "page");
    page.appendChild(element("h1", undefined, "Latency"));
    page.appendChild(
      metricGrid([
        {
          label: "Feed to State",
          value: formatMicros(state.latency?.feed_to_state_ns)
        },
        {
          label: "State to Signal",
          value: formatMicros(state.latency?.state_to_signal_ns)
        },
        {
          label: "Signal to Risk",
          value: formatMicros(state.latency?.signal_to_risk_ns)
        },
        {
          label: "Risk to Execution",
          value: formatMicros(state.latency?.risk_to_execution_ns)
        },
        {
          label: "End to End",
          value: formatMicros(state.latency?.end_to_end_ns),
          tone: "good"
        }
      ])
    );
    return page;
  }
};
