// @ts-nocheck
import { execFileSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { defineConfig } from "vite";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(__dirname, "../..");
const defaultRunDir = path.join(
  repoRoot,
  "research/runs/btc5m_full_markets_200_20260630_224709_utc"
);

function readJson(filePath) {
  try {
    return JSON.parse(fs.readFileSync(filePath, "utf8"));
  } catch {
    return null;
  }
}

function fileMtimeMs(filePath) {
  try {
    return fs.statSync(filePath).mtimeMs;
  } catch {
    return null;
  }
}

function splitCsvLine(line) {
  return line.split(",");
}

function parseProgress(samplesPath) {
  try {
    const text = fs.readFileSync(samplesPath, "utf8").trimEnd();
    if (!text) {
      return emptyProgress();
    }
    const lines = text.split(/\r?\n/);
    const header = splitCsvLine(lines[0]);
    const marketIdx = header.indexOf("market_slug");
    const recvIdx = header.indexOf("recv_utc");
    const tauIdx = header.indexOf("tau_sec");
    const markets = new Map();
    let rows = 0;
    let first = null;
    let last = null;

    for (let i = 1; i < lines.length; i += 1) {
      const line = lines[i];
      if (!line) {
        continue;
      }
      const cells = splitCsvLine(line);
      if (cells.length <= Math.max(marketIdx, recvIdx, tauIdx)) {
        continue;
      }
      const market = cells[marketIdx];
      const recvUtc = cells[recvIdx];
      const tau = Number(cells[tauIdx]);
      if (!market || !Number.isFinite(tau)) {
        continue;
      }
      rows += 1;
      const existing =
        markets.get(market) ??
        {
          rows: 0,
          maxTau: Number.NEGATIVE_INFINITY,
          minTau: Number.POSITIVE_INFINITY
        };
      existing.rows += 1;
      existing.maxTau = Math.max(existing.maxTau, tau);
      existing.minTau = Math.min(existing.minTau, tau);
      markets.set(market, existing);
      const sample = { market, recvUtc, tau };
      first ??= sample;
      last = sample;
    }

    let completeMarkets = 0;
    for (const market of markets.values()) {
      if (market.minTau <= 1.5 && market.maxTau >= 295) {
        completeMarkets += 1;
      }
    }

    const mtime = fileMtimeMs(samplesPath);
    return {
      rows,
      marketsSeen: markets.size,
      completeMarkets,
      incompleteMarkets: Math.max(0, markets.size - completeMarkets),
      firstMarket: first?.market ?? "",
      firstUtc: first?.recvUtc ?? "",
      lastMarket: last?.market ?? "",
      lastUtc: last?.recvUtc ?? "",
      lastTauSec: last?.tau ?? null,
      sampleMtimeMs: mtime,
      sampleAgeSec: mtime == null ? null : Math.max(0, (Date.now() - mtime) / 1000)
    };
  } catch {
    return emptyProgress();
  }
}

function emptyProgress() {
  return {
    rows: 0,
    marketsSeen: 0,
    completeMarkets: 0,
    incompleteMarkets: 0,
    firstMarket: "",
    firstUtc: "",
    lastMarket: "",
    lastUtc: "",
    lastTauSec: null,
    sampleMtimeMs: null,
    sampleAgeSec: null
  };
}

function tmuxSession(runDir) {
  try {
    return fs.readFileSync(path.join(runDir, "tmux_session.txt"), "utf8").trim();
  } catch {
    return "";
  }
}

function tmuxActive(session) {
  if (!session) {
    return false;
  }
  try {
    execFileSync("tmux", ["has-session", "-t", session], { stdio: "ignore" });
    return true;
  } catch {
    return false;
  }
}

function countSettlements(settlements) {
  const sources = {};
  const winners = {};
  for (const settlement of Object.values(settlements ?? {})) {
    const source = settlement?.source ?? "unknown";
    sources[source] = (sources[source] ?? 0) + 1;
    const winner =
      settlement?.up === 1 ? "up" : settlement?.down === 1 ? "down" : "unknown";
    winners[winner] = (winners[winner] ?? 0) + 1;
  }
  return { sources, winners };
}

function btc5mApiPlugin() {
  return {
    name: "btc5m-strategy-api",
    configureServer(server) {
      server.middlewares.use("/api/btc5m/current", (_req, res) => {
        const runDir = process.env.BTC5M_RUN_DIR ?? defaultRunDir;
        const auditPath = path.join(
          runDir,
          "sigma65_fixed_versions_trade_audit_partial_current.json"
        );
        const metadataPath = path.join(runDir, "partial_complete_markets_metadata.json");
        const settlementsPath = path.join(runDir, "settlements_partial_complete_markets.json");
        const samplesPath = path.join(runDir, "samples.csv");
        const session = tmuxSession(runDir);
        const settlements = readJson(settlementsPath) ?? {};
        const settlementCounts = countSettlements(settlements);
        const progress = parseProgress(samplesPath);
        const body = {
          nowUtc: new Date().toISOString(),
          runDir,
          progress: {
            ...progress,
            tmuxSession: session,
            tmuxActive: tmuxActive(session)
          },
          audit: readJson(auditPath),
          metadata: readJson(metadataPath),
          settlementSources: settlementCounts.sources,
          settlementWinners: settlementCounts.winners,
          auditMtimeMs: fileMtimeMs(auditPath),
          files: {
            audit: auditPath,
            metadata: metadataPath,
            settlements: settlementsPath,
            samples: samplesPath
          }
        };
        res.statusCode = 200;
        res.setHeader("content-type", "application/json; charset=utf-8");
        res.setHeader("cache-control", "no-store");
        res.end(JSON.stringify(body));
      });
    },
    configurePreviewServer(server) {
      server.middlewares.use("/api/btc5m/current", (_req, res) => {
        const runDir = process.env.BTC5M_RUN_DIR ?? defaultRunDir;
        const auditPath = path.join(
          runDir,
          "sigma65_fixed_versions_trade_audit_partial_current.json"
        );
        const metadataPath = path.join(runDir, "partial_complete_markets_metadata.json");
        const settlementsPath = path.join(runDir, "settlements_partial_complete_markets.json");
        const samplesPath = path.join(runDir, "samples.csv");
        const session = tmuxSession(runDir);
        const settlements = readJson(settlementsPath) ?? {};
        const settlementCounts = countSettlements(settlements);
        const progress = parseProgress(samplesPath);
        res.statusCode = 200;
        res.setHeader("content-type", "application/json; charset=utf-8");
        res.setHeader("cache-control", "no-store");
        res.end(
          JSON.stringify({
            nowUtc: new Date().toISOString(),
            runDir,
            progress: {
              ...progress,
              tmuxSession: session,
              tmuxActive: tmuxActive(session)
            },
            audit: readJson(auditPath),
            metadata: readJson(metadataPath),
            settlementSources: settlementCounts.sources,
            settlementWinners: settlementCounts.winners,
            auditMtimeMs: fileMtimeMs(auditPath),
            files: {
              audit: auditPath,
              metadata: metadataPath,
              settlements: settlementsPath,
              samples: samplesPath
            }
          })
        );
      });
    }
  };
}

export default defineConfig({
  plugins: [btc5mApiPlugin()],
  server: {
    host: "127.0.0.1",
    port: 5173,
    proxy: { "/api/v1": "http://127.0.0.1:8080", "/stream/v1": "http://127.0.0.1:8080" }
  },
  preview: {
    host: "127.0.0.1",
    port: 4173,
    proxy: { "/api/v1": "http://127.0.0.1:8080", "/stream/v1": "http://127.0.0.1:8080" }
  },
  build: {
    outDir: "dist",
    sourcemap: false
  }
});
