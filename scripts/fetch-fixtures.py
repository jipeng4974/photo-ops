#!/usr/bin/env python3
"""Fetch opt-in photographic fixtures, pinned by revision and SHA-256."""
import argparse
import hashlib
import json
from pathlib import Path
import urllib.request

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--group", choices=["all", "cr3", "tiff", "jpeg"], default="all")
    parser.add_argument("--manifest", type=Path, default=ROOT / "tests/data/manifest.json",
                        help="Use a separate opt-in fixture manifest")
    args = parser.parse_args()
    manifest = json.loads(args.manifest.read_text())
    destination = ROOT / "tests/data/downloads"
    for entry in manifest["files"]:
        if entry["group"] not in ("notice", args.group) and args.group != "all":
            continue
        target = destination / entry["path"]
        target.parent.mkdir(parents=True, exist_ok=True)
        if target.exists() and digest(target) == entry["sha256"]:
            print(f"verified {entry['path']}", flush=True)
            continue
        partial = target.with_suffix(target.suffix + ".part")
        try:
            print(f"fetching {entry['path']}", flush=True)
            request = urllib.request.Request(entry["url"], headers={"User-Agent": "photo-ops-fixtures/1"})
            with urllib.request.urlopen(request, timeout=90) as response, partial.open("wb") as output:
                while block := response.read(1024 * 1024):
                    output.write(block)
            if digest(partial) != entry["sha256"]:
                raise RuntimeError(f"SHA-256 mismatch: {entry['path']}")
            partial.replace(target)
        finally:
            partial.unlink(missing_ok=True)
    print(f"fixtures: {destination}")


if __name__ == "__main__":
    main()
