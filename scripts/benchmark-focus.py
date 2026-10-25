#!/usr/bin/env python3
"""Measure one focus-stack workload with per-child RSS, CPU/wall time and native stage reports."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import shutil
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


def publish_json(path, value):
    with tempfile.NamedTemporaryFile(dir=path.parent, prefix='.summary-', delete=False) as stream:
        temporary = Path(stream.name)
        try:
            stream.write((json.dumps(value, indent=2, ensure_ascii=False) + '\n').encode())
            stream.flush()
            os.fsync(stream.fileno())
            os.link(temporary, path)  # Atomic no-replace; never clobber an earlier measurement.
        finally:
            temporary.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('inputs', nargs='+', type=Path)
    parser.add_argument('--binary', type=Path, default=ROOT/'build/linux/x86_64/release/photo-ops')
    parser.add_argument('--output', required=True, type=Path, help='New measurement directory')
    parser.add_argument('--repeat', type=int, default=1, help='Repeat a single source for identity/resource workloads')
    parser.add_argument('--memory-budget-mib', type=int, default=2048)
    parser.add_argument('--tile-size', type=int, default=256)
    parser.add_argument('--proxy-long-edge', type=int, default=1024)
    parser.add_argument('--scratch-directory', type=Path)
    parser.add_argument('--aligned', action='store_true')
    args = parser.parse_args()
    if args.repeat < 1 or (args.repeat != 1 and len(args.inputs) != 1):
        parser.error('--repeat requires exactly one source and a positive count')
    inputs = [str(path.resolve(strict=True)) for path in args.inputs] * args.repeat
    if not 2 <= len(inputs) <= 256:
        parser.error('workload must have 2..256 frames')
    binary = args.binary.resolve(strict=True)
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=False)
    scratch = args.scratch_directory.resolve() if args.scratch_directory else root/'scratch'
    if not args.scratch_directory:
        scratch.mkdir()
    snapshot = root/'photo-ops.binary'
    shutil.copy2(binary, snapshot)
    with snapshot.open('rb') as stream:
        binary_hash = hashlib.file_digest(stream, 'sha256').hexdigest()
    command = [str(snapshot), 'focus-stack', *inputs, '-o', str(root/'stack.tif'),
               '--diagnostics', str(root/'diagnostics'), '--scratch-directory', str(scratch),
               '--memory-budget-mib', str(args.memory_budget_mib), '--tile-size', str(args.tile_size),
               '--proxy-long-edge', str(args.proxy_long_edge)]
    if args.aligned:
        command.append('--aligned')
    start = time.monotonic()
    with (root/'.stdout-running.log').open('xb') as stdout, (root/'.stderr-running.log').open('xb') as stderr:
        process = subprocess.Popen(command, stdout=stdout, stderr=stderr)
        try:
            _, status, usage = os.wait4(process.pid, 0)
            process.returncode = os.waitstatus_to_exitcode(status)
        except BaseException:
            process.terminate()
            process.wait()
            raise
    wall = time.monotonic() - start
    for name in ('stdout', 'stderr'):
        temporary = root/f'.{name}-running.log'
        os.link(temporary, root/f'{name}.txt')
        temporary.unlink()
    report_path = root/'diagnostics/report.json'
    report = json.loads(report_path.read_text()) if report_path.exists() else None
    summary = dict(binary=str(binary), binary_snapshot=str(snapshot), binary_sha256=binary_hash, command=command,
                   workload_kind='repeated-single-input' if args.repeat != 1 else 'sequence',
                   note='Repeated inputs measure resources/identity, not focus-bracket photographic quality.' if args.repeat != 1 else '',
                   frames=len(inputs), exit_code=process.returncode, wall_seconds=wall,
                   user_seconds=usage.ru_utime, system_seconds=usage.ru_stime,
                   peak_rss_kib=usage.ru_maxrss, report=report)
    publish_json(root/'summary.json', summary)
    print(json.dumps({key: summary[key] for key in ('frames', 'exit_code', 'wall_seconds', 'peak_rss_kib')}), flush=True)
    if process.returncode:
        raise SystemExit(process.returncode if process.returncode > 0 else 1)


if __name__ == '__main__':
    main()
