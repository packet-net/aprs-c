#!/usr/bin/env python3
"""Checks the repository's own files: no em dash (U+2014) or en dash (U+2013)
anywhere, and source, build and workflow files in plain ASCII. The vectors
submodule is someone else's record and is not checked."""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DASHES = {chr(0x2013), chr(0x2014)}
ASCII_ONLY = {".c", ".h", ".cpp", ".py", ".cmake", ".in", ".txt", ".yml", ".yaml", ".gitmodules", ""}


def main():
    files = subprocess.run(["git", "-C", str(ROOT), "ls-files"], capture_output=True, text=True,
                           check=True).stdout.split("\n")
    problems = 0
    for name in files:
        path = ROOT / name
        if not name or not path.is_file() or name.startswith("fuzz/regressions/"):
            continue  # fuzz regressions are arbitrary bytes, not text
        data = path.read_bytes()
        try:
            text = data.decode("utf-8")
        except UnicodeDecodeError:
            print(f"{name}: not UTF-8")
            problems += 1
            continue
        for lineno, line in enumerate(text.split("\n"), 1):
            if DASHES & set(line):
                print(f"{name}:{lineno}: em or en dash")
                problems += 1
            if Path(name).suffix in ASCII_ONLY and any(ord(ch) > 127 for ch in line):
                print(f"{name}:{lineno}: not plain ASCII")
                problems += 1
    print(f"{len([f for f in files if f])} files checked, {problems} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
