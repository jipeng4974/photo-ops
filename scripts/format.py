#!/usr/bin/env python3
"""Apply clang-format plus declaration spacing; --check is the build gate."""
import argparse
import difflib
import os
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]

# Comments and literals are indivisible, so punctuation inside them never changes scope.
TOKENS = re.compile(
    r'//[^\n]*|/\*[\s\S]*?\*/|R"(?P<raw>[^ ()\\\t\n]{0,16})\([\s\S]*?\)(?P=raw)"'
    r'|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[A-Za-z_]\w*|[^\s]'
)


def separate_declarations(source):
    """Space sibling declarations, never local statements or aggregate members.

    clang-format handles definition blocks. This small lexical pass fills its gap for
    declarations/global constants and method declarations. Braced initializers, lambdas,
    comments, strings and parameter-list semicolons do not act as scope boundaries.
    """
    lines = source.splitlines(keepends=True)
    stack = [{"kind": "namespace", "tokens": [], "parens": 0}]
    boundaries = set()
    for match in TOKENS.finditer(source):
        token = match.group()
        if token.startswith(("//", "/*")):
            continue
        line_number = source.count("\n", 0, match.start())
        if lines[line_number].lstrip().startswith("#"):
            continue
        scope = stack[-1]
        if token == "(":
            scope["parens"] += 1
        elif token == ")":
            scope["parens"] -= 1
        if token == "{":
            words = scope["tokens"]
            if "namespace" in words and scope["kind"] == "namespace":
                kind = "namespace"
            elif scope["kind"] in ("namespace", "class") and any(
                word in words for word in ("class", "struct", "union")
            ) and ")" not in words:
                kind = "class"
            elif scope["kind"] in ("namespace", "class") and ")" in words:
                kind = "function"
            else:
                kind = "initializer"
            stack.append({"kind": kind, "tokens": [], "parens": 0})
            continue
        if token == "}":
            if len(stack) == 1:
                continue
            finished = stack.pop()
            scope = stack[-1]
            if finished["kind"] in ("function", "namespace"):
                if finished["kind"] == "function":
                    boundaries.add(line_number)
                scope["tokens"] = []
            continue
        if token == ";" and scope["parens"] == 0:
            words = scope["tokens"]
            global_declaration = scope["kind"] == "namespace" and not any(
                word in words for word in ("module", "import", "using")
            )
            method_declaration = scope["kind"] == "class" and ")" in words
            if global_declaration or method_declaration:
                boundaries.add(line_number)
            scope["tokens"] = []
        else:
            scope["tokens"].append(token)

    for number in sorted(boundaries, reverse=True):
        if number + 1 >= len(lines) or not lines[number + 1].strip():
            continue
        following = lines[number + 1].lstrip()
        if following.startswith(("}", ";", "#", "public:", "private:", "protected:")):
            continue
        lines.insert(number + 1, "\n")
    return "".join(lines)


def formatted(source, path, executable):
    command = [executable, "--style=file", f"--assume-filename={path}"]
    first = subprocess.run(command, input=source, text=True, capture_output=True, check=True).stdout
    spaced = separate_declarations(first)
    return subprocess.run(command, input=spaced, text=True, capture_output=True, check=True).stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    executable = shutil.which(os.environ.get("CLANG_FORMAT", "clang-format"))
    if not executable and shutil.which("brew"):
        prefix = subprocess.run(["brew", "--prefix", "llvm"], text=True,
                                capture_output=True, check=False)
        candidate = Path(prefix.stdout.strip()) / "bin/clang-format"
        if prefix.returncode == 0 and candidate.is_file():
            executable = str(candidate)
    if not executable:
        raise SystemExit("clang-format is required; brew install llvm, or set CLANG_FORMAT")

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
        for number, line in enumerate(result.splitlines(), 1):
            if len(line.expandtabs(8)) > 96:
                print(f"{path}:{number}: exceeds 96 columns")
                failed = True
    if failed:
        raise SystemExit("format check failed; run xmake format")
    print(f"format {'checked' if args.check else 'applied'}: {len(paths)} C++ files")


if __name__ == "__main__":
    main()
