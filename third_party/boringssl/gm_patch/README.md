# Local national-crypto patch bundle

`third_party/boringssl/src` is a separate Git checkout. Committing Chromium's
outer repository does not record modifications inside it. This directory keeps
the reviewed local SM2/SM3/SM4, RFC 8998, TLCP client and libpki changes replayable
alongside the Chromium integration changes.

The only accepted baseline is
`52ba6a143e03e9ac84369f79ac3d06dea0128dab`, matching `../README.chromium`.
The patch is a maintenance artifact, not a substitute for upstream security
updates or the ordinary-TLS and national-TLS regression suites.

## Contents

- `files.txt`: explicit, reviewed source paths. Export rejects unexpected source
  changes and missing changes. Review additions before changing this list.
- `current.patch`: full-index binary-capable Git diff against the fixed baseline,
  including new, previously untracked source/test files.
- `manifest.json`: patch SHA-256, baseline, replayed Git tree and each changed
  file's Git blob, mode, byte count and SHA-256. File hashes describe normalized
  Git blob contents, not necessarily the checkout's CRLF bytes.
- `export_patch.py`: Python standard-library maintenance tool. Requires Git with
  `--pathspec-from-file` support (Git 2.25 or newer).

## Export and verify

From the Chromium workspace root, after relevant source edits have stopped:

```powershell
python third_party/boringssl/gm_patch/export_patch.py export
python third_party/boringssl/gm_patch/export_patch.py check --check-worktree
```

The exporter creates a temporary index from the exact baseline, stages only the
reviewed paths there, and exports its diff. A second snapshot checks that the
working sources did not change during capture. Another fresh baseline index
first checks and then applies the patch with `git apply --cached`; its complete
tree must exactly match the exported tree, and all manifest file hashes must
match. `check` repeats the replay and hash verification without rewriting the
bundle. `--check-worktree` additionally compares the currently modified sources.

The real Git index, HEAD and working source files are never written. The tool
checks the real index hash and HEAD before and after the operation. Git may add
unreachable blobs/trees to its object database while constructing temporary
indexes. It runs no Ninja, Go, gclient, checkout, reset or commit commands.
Ignored files and `.git` are not included; common top-level build output
directories are excluded. Other unexpected untracked sources abort export.
An explicit path list prevents an unrelated file from silently entering the
patch, but reviewers must still examine edits within each listed source file.

## Restore onto a fresh checkout

After obtaining the exact baseline and reviewing the bundle, ensure the nested
checkout is clean. The following is an explicit manual restoration step; it is
not run by GN, gclient or the export tool:

```powershell
git -C third_party/boringssl/src rev-parse HEAD
git -C third_party/boringssl/src status --short
python third_party/boringssl/gm_patch/export_patch.py check
git -C third_party/boringssl/src apply --check ../gm_patch/current.patch
git -C third_party/boringssl/src apply ../gm_patch/current.patch
python third_party/boringssl/gm_patch/export_patch.py check --check-worktree
```

`HEAD` must equal the baseline above and `status` must be empty before applying.
Do not apply on top of an already modified checkout, resolve failures by force,
or silently use a different revision. The commands do not commit or stage the
restored sources. Keep the outer integration changes and this bundle together.

## Build and upgrade boundary

Ordinary GN/Ninja builds use the applied source checkout and checked-in generated
source lists. They do not run this exporter and do not require Go or Tongsuo.
Go is needed only when maintainers regenerate BoringSSL's upstream generated
lists/test inputs after changing their declarations. The existing Chromium
compiler/toolchain requirements still apply.

When Chromium updates BoringSSL, first review and port each local extension to
the new baseline, resolve semantic conflicts, regenerate affected source lists,
and run both ordinary and national-crypto tests. Only then update the baseline,
reviewed file list and bundle together. Do not auto-apply this patch with fuzz,
`--3way` or ignored failures during an autoroll. See
`../../../net/gm/BORINGSSL_TEST_RESULTS.md` for the actual validation state and
`../../../net/gm/SINGLE_LIBRARY_DESIGN.md` for feature/security boundaries.
