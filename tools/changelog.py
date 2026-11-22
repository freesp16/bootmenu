#!/usr/bin/env python3
"""Print the CHANGELOG.md section of a version (the release notes), or check that it exists.

    tools/changelog.py 1.0.0            # the "## [1.0.0]" section, without its heading
    tools/changelog.py --check 1.0.0    # exit 1 if there is no such section
"""
import re
import sys
from pathlib import Path

CHANGELOG = Path(__file__).resolve().parent.parent / "CHANGELOG.md"


def section(version):
    text = CHANGELOG.read_text(encoding="utf-8")
    m = re.search(rf"^## \[{re.escape(version)}\][^\n]*\n(.*?)(?=^## \[|\Z)", text, re.S | re.M)
    return m.group(1).strip() if m else None


def main():
    args = sys.argv[1:]
    check = args[:1] == ["--check"]
    if check:
        args = args[1:]
    if len(args) != 1:
        sys.exit(__doc__)
    body = section(args[0])
    if body is None:
        sys.exit(f"CHANGELOG.md has no '## [{args[0]}]' section")
    if not check:
        print(body)


if __name__ == "__main__":
    main()
