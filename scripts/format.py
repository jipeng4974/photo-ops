#!/usr/bin/env python3
"""Apply stock clang-format; --check is the build gate."""
import argparse
import difflib
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def formatted(source, path, executable):
    command = [executable, "--style=file", f"--assume-filename={path}"]
    return subprocess.run(command, input=source, text=True, capture_output=True, check=True).stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    executable = shutil.which(os.environ.get("CLANG_FORMAT", "clang-format"))
    if not executable:
        raise SystemExit("clang-format is required; install the Arch clang package, or set CLANG_FORMAT")

    failed = False
    paths = sorted(path for directory in ("src", "tests") for path in (ROOT / directory).rglob("*")
                   if path.suffix in (".cpp", ".cppm", ".hpp", ".h"))
    for path in paths:
        source = path.read_text()
        result = formatted(source, path, executable)
        if result != source:
            if args.check:
                print("".join(difflib.unified_diff(source.splitlines(True), result.splitlines(True),
                                                   fromfile=str(path), tofile="formatted")), end="")
                failed = True
            else:
                path.write_text(result)
    if failed:
        raise SystemExit("format check failed; run xmake format")
    print(f"format {'checked' if args.check else 'applied'}: {len(paths)} C++ files")


if __name__ == "__main__":
    main()
