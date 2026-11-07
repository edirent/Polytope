# Reproducible engineering releases

Date: 2026-11-06 (America/Chicago). No reset, checkout, cleanup, index staging, commit or remote push is part of this workflow.

## Source and change ownership

`runs/runtime_v020/prechange_20261106/` inventories tracked modifications and untracked files before restructuring. `baseline_20261106/source/` preserves the prior source state. Existing PMXT/BTC research, market-making/risk experiments, previous cross-venue research and unrelated frontend work were retained. The shared Git HEAD remains the old baseline; a commit ID alone is not this release's source identity.

`tools/freeze_runtime_release.py` produces an allowlisted source snapshot and SHA-256 tree manifest, including untracked source needed for native builds. It excludes credentials.local files, .env files, keys, generated runs/logs/caches, binaries from build directories, node_modules and unrelated research data. Public/synthetic test fixtures are allowed. The secret check rejects private-key PEM markers and populated credential-like JSON fields, without printing matching values. This automated scan is an exclusion/check mechanism, not a claim of universal secret detection.

The source snapshot includes CMake, native libraries, tools, tests and relevant configuration. Reviewable changesets are generated against the captured prechange state, grouped into runtime/feed/durability, OMS/accounting, operations, tests and documentation/tooling. They are local patch artifacts, not an automatic commit of the user's dirty worktree. Pre-existing files omitted by an older census are not misrepresented as newly authored changes.

## Release identity

Each release manifest records:

- File paths, sizes and hashes; an ordered source-tree hash.
- Git baseline separately from actual source identity.
- Compiler/CMake/OpenSSL and package dependency versions.
- Release build profile and optimization flags.
- Frozen executable SHA-256.
- Separate configuration, public metadata/Oracle artifact and recorded-input hashes.

The runtime additionally freezes configuration, artifact, input and executable identity in its journal. Live append-only inputs receive a final recording hash and journal head. Public metadata selection is frozen separately; it is not embedded credentials or permission to trade a cross-venue pair.

A fixed source snapshot and matching executable are copied under the release directory. Subsequent report/document changes do not silently replace that executable. The release-ready manifest refers to the immutable binary/configuration hashes checked by the secure local launcher.

## Independent rebuild

Build with the recorded compiler/dependencies and Release profile. On this workspace, rebuilding from a nested frozen source requires mapping its source path back to the original build prefix to avoid path-only differences from __FILE__/source-location metadata:

```bash
cmake -S PATH_TO_RELEASE/source -B PATH_TO_NEW_BUILD \
  -DCMAKE_BUILD_TYPE=Release -DENABLE_LTO=OFF -DENABLE_NATIVE_ARCH=OFF \
  '-DCMAKE_CXX_FLAGS=-ffile-prefix-map=ABSOLUTE_FROZEN_SOURCE=/home/edirent/Polytope'
cmake --build PATH_TO_NEW_BUILD --target trading_runtime -j 4
sha256sum PATH_TO_RELEASE/trading_runtime PATH_TO_NEW_BUILD/trading_runtime
```

Use an empty build directory for independent verification. Record the exact prefix, compiler/dependency versions, configure/build output and output hash. Absolute operational input paths in a profile are a separate runtime configuration issue; changing them changes its configuration identity and cannot masquerade as the old frozen run.

The initial candidate was rebuilt from its frozen source and matched its executable byte-for-byte. Final-release rebuild results and exact hashes are in the release evidence, rather than assumed from an incremental build. Reproducing on a different compiler/libc/OpenSSL/Boost stack is not guaranteed byte-identical; compare manifest differences explicitly.

The frozen source is sufficient for the native runtime build. Runtime validation also requires the separately hashed public market metadata, Oracle artifact and input journals. A live feed cannot be reproduced by fetching today's metadata and calling it the old snapshot; use the immutable recording and same-executable REPLAY.

## Verification procedure

1. Recompute every source manifest entry and ordered tree hash.
2. Verify no excluded environment/key/log/runtime files entered the source snapshot or patch set.
3. Rebuild the executable in a new build directory and compare its SHA-256.
4. Verify the frozen configuration/artifact/input identities.
5. Run the preserved full tests and operational fault suite; keep the known unrelated failure visible.
6. Run `tools/audit_operational_run.py --with-replay` and compare every transaction's decision trace and account digest.
7. Inspect raw snapshot/delta evidence and per-market admission/epoch transitions; socket status alone is not acceptance.

The automated audit never copies terminal environment values or the local Kalshi key into a manifest. Logs contain public data, simulated receipts and generic error types only. Source/data archives and patch files remain local; publishing or pushing them is not authorized by this procedure.

## Final verified release

`runs/runtime_v020/release_v020_20261106/` contains the actual executable and 1,326 frozen source files. Source tree SHA-256 is `355cd6fd686a196200e53113fa1d498a1515c5b6fe5b003d04dd276ab3402ba2`; executable SHA-256 is `e43d3e3444789c3304009667017851120bdd6c542bc1f0faa26952cfcda477b1`. Independent empty-directory builds in `rebuild_final_a/` and `rebuild_final_b/` both matched byte-for-byte. Every frozen file and the ordered source-tree digest were recomputed successfully. Compiler/dependency/flag details remain in the release manifest; configure/build logs are copied to final evidence.

The final analysis scripts and result appendices were updated after the runtime freeze. They are a separately hashed analysis overlay, not retroactively part of the binary's frozen source. `tools/summarize_runtime_validation.py` refuses native-source drift and assembles exact final metrics. `tools/prepare_runtime_changesets.py` creates five scoped patches against the captured dirty baseline; patches apply and reproduce changed files in a disposable copy. It does not stage, commit or apply changes to the user's checkout. The changeset manifest records preserved unrelated pre-existing files and exact before/after hashes.

Each experiment's own `recording_manifest.json` and `operational_audit.json` bind the complete input, financial journal, original configuration, compact configuration snapshot, Oracle artifact and executable separately. A configuration snapshot's compact byte hash is not mislabeled as the original profile hash. Both final credential-free replays passed all transaction/account comparisons. The final evidence inventory hashes analysis tools, test logs, configuration/Oracle artifacts and original/replay recordings; changing a report does not silently alter run identity.
