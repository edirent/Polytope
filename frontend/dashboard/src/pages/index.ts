import { executionPage } from "./execution";
import { latencyPage } from "./latency";
import { marketsPage } from "./markets";
import { overviewPage } from "./overview";
import { pnlPage } from "./pnl";
import { regimePage } from "./regime";
import { riskPage } from "./risk";
import { signalsPage } from "./signals";
import type { PageDefinition } from "./pageTypes";

export const pages: PageDefinition[] = [
  overviewPage,
  marketsPage,
  signalsPage,
  riskPage,
  executionPage,
  pnlPage,
  regimePage,
  latencyPage
];
