#!/usr/bin/env python3
# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Rewrite Chromium product name strings for custom branding builds.

The generated .grd lives under gen/. Keep grit base_dir as "." so -f
first_ids_file resolution stays correct, rewrite relative inputs to point at
//chrome/app, and generate branded copies of the locale XTB files.
"""

import argparse
import io
import pathlib
import re
import sys

sys.path.insert(
    0, str(pathlib.Path(__file__).resolve().parents[2] / "tools/grit"))
from grit import grd_reader  # pylint: disable=wrong-import-position

_TRANSLATION_FILE_RE = re.compile(
    r'<file\s+path="(resources/chromium_strings_[^"]+\.xtb)"')
_SETTINGS_STRINGS_PART = "settings_chromium_strings.grdp"


def _message_translation_id(message_xml: str) -> str:
    name_match = re.search(r'<message\s+name="([^"]+)"', message_xml)
    if not name_match:
        raise ValueError("GRIT message has no name")
    document = ('<grit latest_public_release="0" current_release="1" '
                'source_lang_id="en" base_dir="."><release seq="1"><messages>'
                f"{message_xml}</messages></release></grit>")
    root = grd_reader.Parse(io.StringIO(document), ".")
    node = root.GetNodeById(name_match.group(1))
    return node.GetCliques()[0].GetId()


def _translation_id_changes(before: str, after: str) -> dict[str, str]:
    message_pattern = re.compile(r"<message\b.*?</message>", re.DOTALL)
    before_messages = message_pattern.findall(before)
    after_messages = message_pattern.findall(after)
    if len(before_messages) != len(after_messages):
        raise ValueError("Custom branding changed the GRIT message structure")

    changes = {}
    for before_message, after_message in zip(before_messages, after_messages):
        if before_message == after_message or 'translateable="false"' in before_message:
            continue
        old_id = _message_translation_id(before_message)
        new_id = _message_translation_id(after_message)
        if old_id != new_id:
            changes[old_id] = new_id
    return changes


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True, type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument("--product-name")
    # Path from the generated .grd directory to //chrome/app (POSIX separators).
    parser.add_argument("--source-dir")
    args = parser.parse_args()

    source_text = args.input.read_text(encoding="utf-8")
    translation_files = _TRANSLATION_FILE_RE.findall(source_text)
    if not translation_files:
        print("No Chromium translation files found", file=sys.stderr)
        return 1

    if not args.output or not args.product_name or not args.source_dir:
        parser.error("--output, --product-name and --source-dir are required")

    product = args.product_name
    if not product or any(c in product for c in "<>&\r\n"):
        print(f"Invalid product name: {product!r}", file=sys.stderr)
        return 1

    source_dir = args.source_dir.replace("\\", "/").rstrip("/")
    if not source_dir or any(c in source_dir for c in "<>&\r\n\""):
        print(f"Invalid source dir: {args.source_dir!r}", file=sys.stderr)
        return 1

    text = source_text

    # <part file> is resolved vs the .grd directory; <file path> uses base_dir
    # ("."), which for the generated file is also the .grd directory.
    def prefix_part_input(match: re.Match) -> str:
        if match.group(2) == _SETTINGS_STRINGS_PART:
            return match.group(0)
        return f"{match.group(1)}{source_dir}/{match.group(2)}{match.group(3)}"

    def rewrite_file_input(match: re.Match) -> str:
        path = match.group(2)
        if path in translation_files:
            return match.group(0)
        return f"{match.group(1)}{source_dir}/{path}{match.group(3)}"

    text, part_count = re.subn(r'(<part\s+file=")([^"]+)(")',
                               prefix_part_input, text)
    text, file_count = re.subn(r'(<file\s+path=")([^"]+)(")',
                               rewrite_file_input, text)
    if part_count < 1 or file_count < 1:
        print(
            f"Failed to rewrite inputs (part={part_count}, file={file_count})",
            file=sys.stderr,
        )
        return 1

    required_replacements = [
        (
            r'(<message name="IDS_PRODUCT_NAME" desc="The Chrome application name" translateable="false">\s*)Chromium(\s*</message>)',
            rf"\g<1>{product}\g<2>",
        ),
        (
            r'(<message name="IDS_SHORT_PRODUCT_NAME" desc="The Chrome application short name." translateable="false">\s*)Chromium(\s*</message>)',
            rf"\g<1>{product}\g<2>",
        ),
        (
            r'(<message name="IDS_APP_MENU_PRODUCT_NAME"[^>]*>\s*)Chromium(\s*</message>)',
            rf"\g<1>{product}\g<2>",
        ),
        (
            r'(<message name="IDS_HELPER_NAME"[^>]*>\s*)Chromium Helper(\s*</message>)',
            rf"\g<1>{product} Helper\g<2>",
        ),
        (
            r'(<message name="IDS_SHORT_HELPER_NAME"[^>]*>\s*)Chromium Helper(\s*</message>)',
            rf"\g<1>{product} Helper\g<2>",
        ),
    ]

    # Window titles and other formats embed the product name literally instead of
    # referencing IDS_PRODUCT_NAME (see brave_strings.grd for the branded pattern).
    branding_replacements = [
        (
            r'(<ph name="PAGE_TITLE">\$1<ex>Google</ex></ph>) - Chromium(\s*</message>)',
            rf"\g<1> - {product}\g<2>",
        ),
        (
            r'Chromium - (<ph name="PAGE_TITLE">\$1<ex>Google</ex></ph>)',
            rf"{product} - \g<1>",
        ),
        (
            r'(<ph name="PAGE_TITLE">\$1<ex>Google</ex></ph> - Network Sign-in - )Chromium(\s*</message>)',
            rf"\g<1>{product}\g<2>",
        ),
        (
            r'Chromium - (Network Sign-in - <ph name="PAGE_TITLE">\$1<ex>Google</ex></ph>)',
            rf"{product} - \g<1>",
        ),
        (
            r'(Task Manager - )Chromium(\s*</message>)',
            rf"\g<1>{product}\g<2>",
        ),
    ]

    for pattern, repl in required_replacements:
        text, count = re.subn(pattern, repl, text, count=1, flags=re.DOTALL)
        if count != 1:
            print(f"Failed to apply pattern: {pattern}", file=sys.stderr)
            return 1

    for pattern, repl in branding_replacements:
        text, count = re.subn(pattern, repl, text)
        if count == 0:
            print(f"Failed to apply pattern: {pattern}", file=sys.stderr)
            return 1

    def brand_message_body(message_name: str, old: str, new: str) -> None:
        nonlocal text
        pattern = re.compile(
            rf'(<message\s+name="{message_name}"[^>]*>)(.*?)(</message>)',
            re.DOTALL,
        )

        def replace_body(match: re.Match) -> str:
            return (f"{match.group(1)}{match.group(2).replace(old, new)}"
                    f"{match.group(3)}")

        text, count = pattern.subn(replace_body, text)
        if count == 0:
            raise ValueError(f"Message not found: {message_name}")

    brand_message_body("IDS_ABOUT", "About &amp;Chromium",
                       f"About &amp;{product}")
    brand_message_body(
        "IDS_SET_BROWSER_AS_DEFAULT_MENU_ITEM",
        "Set Chromium as your default browser",
        f"Set {product} as your default browser",
    )

    original_settings_text = (args.input.parent /
                              _SETTINGS_STRINGS_PART).read_text(
                                  encoding="utf-8")
    settings_text = original_settings_text
    settings_pattern = re.compile(
        r'(<message\s+name="IDS_SETTINGS_ABOUT_PROGRAM"[^>]*>)(.*?)'
        r'(</message>)',
        re.DOTALL,
    )

    def brand_settings_about(match: re.Match) -> str:
        return (
            f"{match.group(1)}"
            f"{match.group(2).replace('About Chromium', f'About {product}')}"
            f"{match.group(3)}")

    settings_text, settings_count = settings_pattern.subn(
        brand_settings_about, settings_text)
    if settings_count == 0:
        print("Failed to brand IDS_SETTINGS_ABOUT_PROGRAM", file=sys.stderr)
        return 1

    translation_id_map = _translation_id_changes(source_text, text)
    translation_id_map.update(
        _translation_id_changes(original_settings_text, settings_text))

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(text, encoding="utf-8", newline="\n")
    (args.output.parent / _SETTINGS_STRINGS_PART).write_text(settings_text,
                                                             encoding="utf-8",
                                                             newline="\n")

    for relative_path in translation_files:
        translation = (args.input.parent /
                       relative_path).read_text(encoding="utf-8")
        for old_id, new_id in translation_id_map.items():
            pattern = re.compile(
                rf'(<translation\s+id="){old_id}("[^>]*>)(.*?)(</translation>)',
                re.DOTALL,
            )

            def brand_translation(match: re.Match) -> str:
                return (f"{match.group(1)}{new_id}{match.group(2)}"
                        f"{match.group(3).replace('Chromium', product)}"
                        f"{match.group(4)}")

            translation = pattern.sub(brand_translation, translation)
        output_path = args.output.parent / relative_path
        output_path.parent.mkdir(parents=True, exist_ok=True)
        output_path.write_text(translation, encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
