#!/usr/bin/env python3
# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Export or check the reviewed local BoringSSL patch using disposable indexes.

This maintenance tool never applies a patch to the working tree, changes HEAD,
or writes the real Git index. Git may add unreachable blobs/trees to its object
database. Python 3 and Git supporting --pathspec-from-file are required.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import subprocess
import tempfile


BASELINE = "52ba6a143e03e9ac84369f79ac3d06dea0128dab"
BUNDLE = Path(__file__).resolve().parent
SOURCE = BUNDLE.parent / "src"


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def git(*args, index=None, data=None, accepted_codes=(0,)):
    environment = os.environ.copy()
    # Prevent status/diff's optional index refresh from touching the real index.
    environment["GIT_OPTIONAL_LOCKS"] = "0"
    for variable in ("GIT_INDEX_FILE", "GIT_DIR", "GIT_WORK_TREE", "GIT_COMMON_DIR"):
        environment.pop(variable, None)
    if index is not None:
        environment["GIT_INDEX_FILE"] = str(index)
    completed = subprocess.run(
        ["git", "-c", "core.quotePath=false", "-C", str(SOURCE), *args],
        input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        env=environment, check=False)
    if completed.returncode not in accepted_codes:
        raise RuntimeError("git " + " ".join(args) + ":\n" +
                           completed.stderr.decode("utf-8", errors="replace"))
    return completed.stdout


def output_path(path):
    first = PurePosixPath(path).parts[0]
    return (first in (".git", "out", "build", "CMakeFiles") or
            first.startswith(("cmake-build-", "build-")))


def reviewed_files():
    paths = [line.strip() for line in
             (BUNDLE / "files.txt").read_text(encoding="utf-8").splitlines()
             if line.strip() and not line.startswith("#")]
    if not paths or paths != sorted(set(paths)):
        raise RuntimeError("files.txt must contain unique, sorted source paths")
    for path in paths:
        parsed = PurePosixPath(path)
        if (parsed.is_absolute() or ".." in parsed.parts or "\\" in path or
                ":" in path or ".git" in parsed.parts or output_path(path)):
            raise RuntimeError("Unacceptable source path: " + path)
    return paths


def split_paths(data):
    return set(data.decode("utf-8").rstrip("\0").split("\0")) if data else set()


def current_changes():
    changed = split_paths(git("diff", "--name-only", "-z", BASELINE, "--"))
    untracked = split_paths(git("ls-files", "--others", "--exclude-standard", "-z"))
    # Git already excludes .git and ignored files. Only known output roots are
    # skipped here; any other unexpected source file must be reviewed explicitly.
    return changed | {path for path in untracked if not output_path(path)}


def require_exact_files(actual, expected):
    missing = set(expected) - set(actual)
    unexpected = set(actual) - set(expected)
    if missing or unexpected:
        raise RuntimeError("Changed-file list differs from files.txt.\nMissing: " +
                           ", ".join(sorted(missing)) + "\nUnexpected: " +
                           ", ".join(sorted(unexpected)))


def snapshot_source(index, paths):
    require_exact_files(current_changes(), paths)
    ignored = git("check-ignore", "--no-index", "--stdin", "-z",
                  data=("\0".join(paths) + "\0").encode("utf-8"),
                  accepted_codes=(0, 1))
    if ignored:
        raise RuntimeError("Reviewed list contains ignored paths: " +
                           ignored.decode(errors="replace"))
    git("read-tree", BASELINE, index=index)
    git("--literal-pathspecs", "add", "--all", "--pathspec-from-file=-",
        "--pathspec-file-nul", index=index,
        data=("\0".join(paths) + "\0").encode("utf-8"))
    return git("write-tree", index=index).decode().strip()


def file_records(index, paths):
    entries = {}
    raw = git("ls-files", "--stage", "-z", index=index)
    for entry in raw.rstrip(b"\0").split(b"\0"):
        metadata, path = entry.split(b"\t", 1)
        mode, object_id, stage = metadata.decode().split()
        if stage != "0":
            raise RuntimeError("Unexpected unmerged temporary index")
        entries[path.decode("utf-8")] = (mode, object_id)
    records = []
    for path in paths:
        if path not in entries:
            records.append({"path": path, "deleted": True})
            continue
        mode, object_id = entries[path]
        if mode not in ("100644", "100755"):
            raise RuntimeError("Patch may contain only regular source files: " + path)
        content = git("cat-file", "blob", object_id)
        records.append({"path": path, "mode": mode, "git_blob": object_id,
                        "bytes": len(content), "sha256": sha256(content)})
    return records


def verify_patch(index, patch, manifest, paths):
    if manifest["schema_version"] != 1 or manifest["baseline"] != BASELINE:
        raise RuntimeError("Unsupported manifest or unexpected BoringSSL baseline")
    if manifest["patch_sha256"] != sha256(patch):
        raise RuntimeError("current.patch SHA-256 does not match manifest")
    if [entry["path"] for entry in manifest["files"]] != paths:
        raise RuntimeError("Manifest files do not match the reviewed list")
    git("read-tree", BASELINE, index=index)
    git("apply", "--cached", "--check", "--whitespace=nowarn", "-",
        index=index, data=patch)
    git("apply", "--cached", "--whitespace=nowarn", "-", index=index, data=patch)
    changed = split_paths(git("diff", "--cached", "--name-only", "-z",
                              BASELINE, "--", index=index))
    require_exact_files(changed, paths)
    tree = git("write-tree", index=index).decode().strip()
    if tree != manifest["result_tree"]:
        raise RuntimeError("Replayed tree differs from exported source tree")
    if file_records(index, paths) != manifest["files"]:
        raise RuntimeError("Replayed source blobs differ from manifest")
    return tree


def repository_state():
    head = git("rev-parse", "HEAD").decode().strip()
    index_path = Path(git("rev-parse", "--git-path", "index").decode().strip())
    if not index_path.is_absolute():
        index_path = SOURCE / index_path
    return head, sha256(index_path.read_bytes()) if index_path.exists() else None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("export", "check"))
    parser.add_argument("--check-worktree", action="store_true",
                        help="also require current modified sources to match the bundle")
    args = parser.parse_args()
    paths = reviewed_files()
    original_state = repository_state()
    if original_state[0] != BASELINE:
        raise RuntimeError("HEAD must equal the reviewed baseline " + BASELINE +
                           "; rebase and review explicitly before updating this bundle")
    try:
        with tempfile.TemporaryDirectory(prefix="boringssl-gm-patch-") as temporary:
            directory = Path(temporary)
            if args.command == "export":
                source_index = directory / "source.index"
                tree = snapshot_source(source_index, paths)
                patch = git("diff", "--cached", "--binary", "--full-index",
                            "--no-ext-diff", "--no-textconv", "--no-color",
                            "--no-renames", "--diff-algorithm=myers",
                            "--no-indent-heuristic",
                            "--src-prefix=a/", "--dst-prefix=b/", BASELINE, "--",
                            index=source_index)
                manifest = {
                    "schema_version": 1,
                    "baseline": BASELINE,
                    "result_tree": tree,
                    "patch_sha256": sha256(patch),
                    "files": file_records(source_index, paths),
                }
                # A second independently populated index catches source edits
                # during export without changing the real index or source files.
                if snapshot_source(directory / "second.index", paths) != tree:
                    raise RuntimeError("Sources changed during export; retry when stable")
            else:
                patch = (BUNDLE / "current.patch").read_bytes()
                manifest = json.loads((BUNDLE / "manifest.json").read_text(encoding="utf-8"))
            tree = verify_patch(directory / "replay.index", patch, manifest, paths)
            if args.check_worktree and args.command == "check":
                if snapshot_source(directory / "current.index", paths) != tree:
                    raise RuntimeError("Current working sources differ from the bundle")
            if repository_state() != original_state:
                raise RuntimeError("Real HEAD/index changed during operation; review concurrent activity")
            if args.command == "export":
                (BUNDLE / "current.patch").write_bytes(patch)
                (BUNDLE / "manifest.json").write_text(
                    json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
            print("PASS: %d reviewed files; baseline %s; replay tree %s" %
                  (len(paths), BASELINE, tree))
            print("Patch SHA-256: " + manifest["patch_sha256"])
            print("Real HEAD/index unchanged; working sources were not written.")
    finally:
        if repository_state() != original_state:
            raise RuntimeError("Real HEAD/index changed during operation; review concurrent activity")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError, KeyError) as error:
        raise SystemExit(str(error))
