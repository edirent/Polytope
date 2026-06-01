import { element } from "../components/dom";
import { formatInteger } from "../components/format";
import { statusPill } from "../components/statusPill";
import type { PageDefinition } from "./pageTypes";

export const regimePage: PageDefinition = {
  id: "regime",
  title: "Regime",
  render(state) {
    const page = element("div", "page");
    page.appendChild(element("h1", undefined, "Regime"));
    const grid = element("div", "regime-grid");
    grid.appendChild(statusPill("Data", state.regime?.data ?? "Unknown"));
    grid.appendChild(statusPill("Liquidity", state.regime?.liquidity ?? "Unknown"));
    grid.appendChild(statusPill("Chain", state.regime?.chain ?? "Unknown"));
    grid.appendChild(statusPill("Signal", state.regime?.signal ?? "Unknown"));
    grid.appendChild(statusPill("Risk", state.regime?.risk ?? "Unknown"));
    grid.appendChild(
      statusPill("Execution", state.regime?.execution ?? "Unknown")
    );
    page.appendChild(grid);
    page.appendChild(
      element(
        "p",
        "muted",
        `Regime version ${formatInteger(state.regime?.version)}`
      )
    );
    return page;
  }
};
