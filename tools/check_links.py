#!/usr/bin/env python3
"""Verify every relative markdown link in the repository resolves.

DOC-04. Documentation diverging from reality is RISK-020, and dead cross-references are
the first symptom. Runs in CI on every push.

Usage:  python3 tools/check_links.py [repo_root]
"""

from __future__ import annotations

import os
import re
import sys

LINK_RE = re.compile(r"\[([^\]]*)\]\(([^)]+)\)")
SKIP_DIRS = {".git", "build", "node_modules", ".gradle", ".cxx", "out"}
EXTERNAL_PREFIXES = ("http://", "https://", "mailto:", "#")


def main() -> int:
    root = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.dirname(
        os.path.abspath(__file__)))

    broken: list[tuple[str, str]] = []
    total_links = 0
    total_files = 0

    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for fn in filenames:
            if not fn.endswith(".md"):
                continue
            total_files += 1
            path = os.path.join(dirpath, fn)
            with open(path, encoding="utf-8") as f:
                text = f.read()

            for _label, target in LINK_RE.findall(text):
                if target.startswith(EXTERNAL_PREFIXES):
                    continue
                total_links += 1
                rel = target.split("#", 1)[0]
                if not rel:
                    continue
                resolved = os.path.normpath(os.path.join(dirpath, rel))
                if not os.path.exists(resolved):
                    broken.append((os.path.relpath(path, root), target))

    print(f"checked {total_files} markdown files, {total_links} relative links")
    if broken:
        print(f"\nBROKEN LINKS: {len(broken)}")
        for src, tgt in broken:
            print(f"  {src}  ->  {tgt}")
        return 1
    print("all relative links resolve")
    return 0


if __name__ == "__main__":
    sys.exit(main())
