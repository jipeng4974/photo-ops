#!/usr/bin/env python3
"""Opt-in real-image smoke checks; requires fetched fixtures and a built CLI."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "build/linux/x86_64/debug/photo-ops")
    parser.add_argument("--manifest", type=Path, default=ROOT / "tests/data/manifest.json")
    args = parser.parse_args()
    manifest = json.loads(args.manifest.read_text())
    sources = []
    for entry in manifest["files"]:
        if entry["group"] == "notice":
            continue
        path = ROOT / "tests/data/downloads" / entry["path"]
        if path.exists():
            with path.open("rb") as stream:
                if hashlib.file_digest(stream, "sha256").hexdigest() != entry["sha256"]:
                    raise RuntimeError(f"fixture hash mismatch: {path}")
            sources.append(path)
    if not sources:
        raise SystemExit("No fixtures found; run scripts/fetch-fixtures.py first")

    def run(*arguments):
        subprocess.run([str(args.binary.resolve()), *map(str, arguments)], check=True)

    for source in sources:
        run("inspect", source, "--half-size")
    raw = next((p for p in sources if p.suffix == ".CR3"), None)
    if raw:
        with tempfile.TemporaryDirectory(prefix="photo-ops-smoke-") as directory:
            tiff = Path(directory) / "raw.tif"
            jpeg = Path(directory) / "preview.jpg"
            run("convert", raw, "--half-size", "-o", tiff)
            run("convert", tiff, "-o", jpeg)
            run("inspect", jpeg)
    print(f"real-image smoke passed: {len(sources)} input fixtures")


if __name__ == "__main__":
    main()
