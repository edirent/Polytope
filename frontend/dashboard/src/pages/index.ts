import { executionPage } from "./execution";
import { latencyPage } from "./latency";
import { marketMakerPage } from "./marketMaker";
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
  marketMakerPage,
  pnlPage,
  regimePage,
  latencyPage
];
