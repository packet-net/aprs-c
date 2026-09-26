#!/usr/bin/env python3
"""Checks that every code block in README.md marked with
<!-- example: examples/NAME.c --> is exactly that file, so the README's
examples are the ones CI compiles and runs."""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def main():
    readme = (ROOT / "README.md").read_text(encoding="utf-8")
    blocks = re.findall(r"<!-- example: (\S+) -->\s*```c\n(.*?)```", readme, re.S)
    if not blocks:
        print("README.md has no marked examples")
        return 1
    bad = 0
    for path, code in blocks:
        text = (ROOT / path).read_text(encoding="utf-8")
        if text != code:
            print(f"README.md's block for {path} differs from the file")
            bad += 1
    print(f"{len(blocks)} README examples checked, {bad} differ")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
