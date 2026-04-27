"""Temp helper: dump a subset of switches (value + English comment) so we can
hand-write Chinese translations for them. Prints tab-separated data to stdout.

Usage:
  python xenon_overlay/doc/tools/_dump_to_translate.py <file_pattern_substring>...
  python xenon_overlay/doc/tools/_dump_to_translate.py --untranslated   # only show entries missing zh_comments
  python xenon_overlay/doc/tools/_dump_to_translate.py --files          # list distinct file paths with switch counts
"""
from __future__ import annotations

import json
import sys
from collections import Counter
from pathlib import Path

HERE = Path(__file__).resolve()
ROOT = HERE.parents[3]
JSON_PATH = ROOT / "xenon_overlay" / "doc" / "switches.json"
ZH_PATH = ROOT / "xenon_overlay" / "doc" / "tools" / "zh_comments.json"


def load_zh() -> dict[str, str]:
    if not ZH_PATH.exists():
        return {}
    return json.loads(ZH_PATH.read_text(encoding="utf-8"))


def main() -> None:
    args = sys.argv[1:]
    only_untranslated = False
    if "--untranslated" in args:
        only_untranslated = True
        args.remove("--untranslated")
    list_files = False
    if "--files" in args:
        list_files = True
        args.remove("--files")
    wanted = args  # empty = all files
    data = json.loads(JSON_PATH.read_text(encoding="utf-8"))
    zh = load_zh() if only_untranslated else {}

    if list_files:
        counter: Counter[str] = Counter()
        for s in data["switches"]:
            counter[s["file"]] += 1
        for path, n in counter.most_common():
            print(f"{n:5d}  {path}")
        print(f"\n# distinct files: {len(counter)}", file=sys.stderr)
        return

    total = 0
    for s in data["switches"]:
        if wanted and not any(w in s["file"] for w in wanted):
            continue
        if only_untranslated and s["value"] in zh:
            continue
        comment = " ".join(s["comment"].split())
        print(f"{s['file']}\t{s['value']}\t{comment}")
        total += 1
    print(f"\n# total: {total}", file=sys.stderr)


if __name__ == "__main__":
    main()
