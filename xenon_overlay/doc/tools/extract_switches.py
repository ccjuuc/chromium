"""Extract all Chromium command-line switches across the repo.

Scans every *switches*.cc / command_line_switches.cc file in the src tree,
parses the switch name / "switch-string" value / preceding // comment and the
surrounding #if BUILDFLAG(...) context, filters out mobile/ChromeOS/Fuchsia/Cast
only switches, then writes a structured JSON file for later rendering.

Usage:
  python assets/tools/extract_switches.py            # writes assets/switches.json
"""

from __future__ import annotations

import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]  # H:\chromium_142\src
OUT = ROOT / "xenon_overlay" / "doc" / "switches.json"

# ---------------------------------------------------------------------------
# File discovery
# ---------------------------------------------------------------------------

SWITCH_GLOBS = [
    "**/*switches.cc",
    "**/*switches_*.cc",
    "**/command_line_switches.cc",
    "**/*command_line_switches.cc",
    # Some switches are defined inline in headers with `inline constexpr char`.
    "**/*switches.h",
    "**/*switches_*.h",
    "**/command_line_switches.h",
    "**/*command_line_switches.h",
]

# Drop entire subtrees that are not relevant for desktop-Windows builds.
EXCLUDE_DIR_PREFIXES = (
    "android_webview/",
    "ash/",
    "chromeos/",
    "chromecast/",
    "fuchsia_web/",
    "ios/",
    "remoting/",
    "out/",
    # third_party/ is mostly excluded, but see ALLOW_PATHS below for exceptions.
    "third_party/",
)

# Explicit allow-list that overrides EXCLUDE_DIR_PREFIXES.
ALLOW_PATHS = (
    "third_party/blink/common/switches.cc",
    "third_party/blink/public/common/switches.h",
)

# Drop files whose path itself screams mobile / non-desktop.
EXCLUDE_PATH_SUBSTRINGS = (
    "/android/",
    "/ios/",
    "/ash/",
    "/chromeos/",
    "_android.",
    "_ios.",
    "_chromeos.",
    "_fuchsia.",
    "_cast.",
    "/cast/",
    "/borealis/",
    "/crosier/",
)


def collect_files() -> list[Path]:
    seen: set[Path] = set()
    for pat in SWITCH_GLOBS:
        for p in ROOT.glob(pat):
            seen.add(p.resolve())
    out: list[Path] = []
    for p in sorted(seen):
        rel = p.relative_to(ROOT).as_posix()
        if rel in ALLOW_PATHS:
            out.append(p)
            continue
        if any(rel.startswith(pref) for pref in EXCLUDE_DIR_PREFIXES):
            continue
        lower = "/" + rel.lower()
        if any(s in lower for s in EXCLUDE_PATH_SUBSTRINGS):
            continue
        out.append(p)
    return out


# ---------------------------------------------------------------------------
# Parser
# ---------------------------------------------------------------------------

DECL_RE = re.compile(
    r"""
    ^\s*
    (?:[A-Z][A-Z0-9_]*_EXPORT\s+ | COMPONENT_EXPORT\([^)]*\)\s+)?   # optional *_EXPORT / COMPONENT_EXPORT(...)
    (?:extern\s+)?
    (?:inline\s+)?
    (?:constexpr\s+)?
    const\s+char\s+
    (k[A-Za-z0-9_]+)             # (1) C++ identifier
    \s*\[\]\s*=\s*
    (.*)$                        # (2) rest of line (may continue)
    """,
    re.VERBOSE,
)

# Alternative form: `inline constexpr char kXxx[] = "...";` (no leading `const`).
DECL_RE_ALT = re.compile(
    r"""
    ^\s*
    (?:[A-Z][A-Z0-9_]*_EXPORT\s+ | COMPONENT_EXPORT\([^)]*\)\s+)?
    (?:inline\s+)?
    constexpr\s+char\s+
    (k[A-Za-z0-9_]+)
    \s*\[\]\s*=\s*
    (.*)$
    """,
    re.VERBOSE,
)

MOBILE_FLAGS = {
    "IS_ANDROID",
    "IS_IOS",
    "IS_CHROMEOS",
    "IS_CHROMEOS_ASH",
    "IS_CHROMEOS_LACROS",
    "IS_FUCHSIA",
    "IS_CASTOS",
    "IS_CAST_ANDROID",
    "IS_CAST_RECEIVER",
}
DESKTOP_FLAGS = {"IS_WIN", "IS_MAC", "IS_LINUX", "IS_POSIX", "IS_CHROMEOS_DESKTOP"}


def is_mobile_only(conds: list[str]) -> bool:
    """Return True if every active condition restricts us to mobile/CrOS/etc.

    We look at BUILDFLAG(...) tokens in the joined condition string:
      * if any `!MOBILE_FLAG` (e.g. `!BUILDFLAG(IS_ANDROID)`) appears => desktop, keep.
      * if any DESKTOP_FLAG (non-negated) appears => desktop, keep.
      * if at least one MOBILE_FLAG is present non-negated AND no desktop hint
        is present => mobile-only, drop.
    """
    if not conds:
        return False
    joined = " && ".join(conds)
    flags = re.findall(r"BUILDFLAG\s*\(\s*(!?\s*[A-Za-z_][A-Za-z0-9_]*)\s*\)", joined)
    has_mobile_pos = False
    for raw in flags:
        tok = raw.replace(" ", "")
        neg = tok.startswith("!")
        name = tok[1:] if neg else tok
        if neg and name in MOBILE_FLAGS:
            return False  # e.g. !IS_ANDROID -> desktop
        if (not neg) and name in DESKTOP_FLAGS:
            return False
        if (not neg) and name in MOBILE_FLAGS:
            has_mobile_pos = True
    return has_mobile_pos


def parse_file(path: Path) -> list[dict]:
    try:
        text = path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        text = path.read_text(encoding="utf-8", errors="ignore")
    lines = text.splitlines()

    if_stack: list[str] = []
    comment_buf: list[str] = []
    results: list[dict] = []

    i = 0
    N = len(lines)
    while i < N:
        raw = lines[i]
        stripped = raw.strip()

        # -- preprocessor directives ------------------------------------------------
        if stripped.startswith("#if"):
            cond = stripped[3:].lstrip("ndef ").strip() if stripped.startswith("#ifdef") or stripped.startswith("#ifndef") else stripped[3:].strip()
            if stripped.startswith("#ifdef"):
                cond = "defined(" + stripped[6:].strip() + ")"
            elif stripped.startswith("#ifndef"):
                cond = "!defined(" + stripped[7:].strip() + ")"
            if_stack.append(cond)
            comment_buf.clear()
            i += 1
            continue
        if stripped.startswith("#elif"):
            if if_stack:
                if_stack[-1] = stripped[5:].strip()
            comment_buf.clear()
            i += 1
            continue
        if stripped.startswith("#else"):
            if if_stack:
                if_stack[-1] = "NOT(" + if_stack[-1] + ")"
            comment_buf.clear()
            i += 1
            continue
        if stripped.startswith("#endif"):
            if if_stack:
                if_stack.pop()
            comment_buf.clear()
            i += 1
            continue

        # -- comments ---------------------------------------------------------------
        if stripped.startswith("//"):
            body = stripped[2:]
            if body.startswith(" "):
                body = body[1:]
            # skip file-divider banners full of dashes / "=========="
            if not re.fullmatch(r"[-=# ]*", body):
                comment_buf.append(body)
            i += 1
            continue

        if not stripped:
            comment_buf.clear()
            i += 1
            continue

        # -- switch declaration -----------------------------------------------------
        m = DECL_RE.match(raw) or DECL_RE_ALT.match(raw)
        if m:
            name = m.group(1)
            rest = m.group(2)
            collected = rest
            j = i
            while ";" not in collected and j + 1 < N:
                j += 1
                collected += " " + lines[j].strip()
            vm = re.search(r'"((?:[^"\\]|\\.)*)"', collected)
            value = vm.group(1) if vm else ""
            comment = " ".join(c.strip() for c in comment_buf if c.strip())
            buildflag = " && ".join(if_stack) if if_stack else ""
            results.append(
                {
                    "name": name,
                    "value": value,
                    "comment": comment,
                    "buildflag": buildflag,
                }
            )
            comment_buf.clear()
            i = j + 1
            continue

        # anything else (function body, etc.) drops pending comments so we don't
        # attach them to an unrelated declaration below.
        comment_buf.clear()
        i += 1

    return results


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> None:
    files = collect_files()
    records: list[dict] = []
    per_file: dict[str, int] = {}

    for p in files:
        rel = p.relative_to(ROOT).as_posix()
        for sw in parse_file(p):
            if is_mobile_only([sw["buildflag"]] if sw["buildflag"] else []):
                continue
            if not sw["value"]:
                continue
            sw["file"] = rel
            records.append(sw)
        per_file[rel] = per_file.get(rel, 0) + 1

    # Deduplicate by switch string value, keeping the first occurrence but
    # recording additional file locations.
    by_value: dict[str, dict] = {}
    for r in records:
        v = r["value"]
        if v in by_value:
            by_value[v].setdefault("also_in", []).append(r["file"])
            if not by_value[v]["comment"] and r["comment"]:
                by_value[v]["comment"] = r["comment"]
        else:
            by_value[v] = r

    final = list(by_value.values())
    final.sort(key=lambda r: (r["file"], r["name"]))

    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(
        json.dumps({"count": len(final), "files_scanned": len(files), "switches": final}, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    print(f"scanned files: {len(files)}")
    print(f"unique switches: {len(final)}")
    print(f"output: {OUT}")


if __name__ == "__main__":
    main()
