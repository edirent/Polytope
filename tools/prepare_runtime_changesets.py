#!/usr/bin/env python3
"""Create review patches against the captured dirty baseline without staging Git."""
import collections
import difflib
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / 'runs/runtime_v020'

def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()

def main():
    baseline = BASE / 'baseline_20261106/source'
    output = BASE / 'evidence_20261106/changesets'
    output.mkdir(parents=True, exist_ok=True)
    spec = importlib.util.spec_from_file_location('freeze', ROOT / 'tools/freeze_runtime_release.py')
    freeze = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(freeze)
    prior = {e['path']: e for e in json.loads((BASE / 'prechange_20261106/dirty_inventory.json').read_text())}
    tracked = set(subprocess.check_output(['git', 'ls-files', '-z'], cwd=ROOT).decode().split('\0'))
    groups = collections.defaultdict(list)
    changes = []
    for path in freeze.collect():
        rel = str(path.relative_to(ROOT))
        before = baseline / rel
        if before.exists() and sha(before) == sha(path):
            continue
        if not before.exists() and rel in prior and prior[rel]['sha256'] == sha(path):
            continue
        if not before.exists() and rel in tracked:
            committed = subprocess.check_output(['git', 'show', 'HEAD:' + rel], cwd=ROOT)
            if committed == path.read_bytes():
                continue
            raise RuntimeError('Missing dirty baseline file: ' + rel)
        if rel == 'CMakeLists.txt' or rel.startswith(('engine/runtime/', 'engine/memory/', 'engine/signal/', 'config/trading_runtime.')):
            group = '01_runtime_feed_durability'
        elif rel.startswith(('engine/execution/', 'engine/paper/')):
            group = '02_oms_accounting'
        elif rel.startswith(('apps/paper_backend/', 'frontend/dashboard/')):
            group = '03_operations'
        elif rel.startswith('tests/integration/runtime/'):
            group = '04_integration_faults'
        elif rel.startswith(('docs/', 'tools/')):
            group = '05_documents_tools'
        else:
            raise RuntimeError('Unclassified change requires review: ' + rel)
        old = before.read_text().splitlines(keepends=True) if before.exists() else []
        new = path.read_text().splitlines(keepends=True)
        header = 'diff --git a/' + rel + ' b/' + rel + '\n'
        if not before.exists():
            header += 'new file mode 100644\n'
        delta = ''.join(difflib.unified_diff(old, new, fromfile='a/' + rel if before.exists() else '/dev/null', tofile='b/' + rel))
        groups[group].append(header + delta)
        changes.append(dict(path=rel, group=group, before_sha256=sha(before) if before.exists() else None,
                            after_sha256=sha(path), bytes=path.stat().st_size))
    patches = []
    with tempfile.TemporaryDirectory(prefix='polytope-review-patches-') as temp:
        check_root = Path(temp) / 'source'
        shutil.copytree(baseline, check_root)
        for group in sorted(groups):
            patch = output / (group + '.patch')
            patch.write_text(''.join(groups[group]))
            subprocess.run(['git', 'apply', '--check', str(patch)], cwd=check_root, check=True)
            # Only a disposable copy is changed. User worktree and Git index are untouched.
            subprocess.run(['git', 'apply', str(patch)], cwd=check_root, check=True)
            patches.append(dict(path=str(patch.relative_to(ROOT)), sha256=sha(patch), apply_check='PASS'))
        assert all(sha(check_root / r['path']) == r['after_sha256'] for r in changes)
    changed = {r['path'] for r in changes}
    unrelated = [r for rel, r in prior.items() if rel not in changed]
    assert all((ROOT / r['path']).exists() and sha(ROOT / r['path']) == r['sha256'] for r in unrelated), 'Unrelated prior change modified'
    result = dict(baseline='runs/runtime_v020/baseline_20261106/source',
                  changes=changes, patches=patches, git_index_modified=False, commits_created=False,
                  preexisting_unrelated_dirty_files_verified_unchanged=len(unrelated),
                  exclusions='Preexisting tracked fixtures omitted by baseline snapshot are not new V0.2.0 work; no prior research edits folded into patches.')
    (output / 'manifest.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(dict(changed_files=len(changes), patches=len(patches), apply_check='PASS', unrelated_preserved=len(unrelated))))

if __name__ == '__main__':
    main()
