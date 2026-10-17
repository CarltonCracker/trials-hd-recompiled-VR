#!/usr/bin/env bash
# Regenerate patches/rexglue-sdk/trials-hd.patch from the working tree of the
# thirdparty/rexglue-sdk submodule (all local changes vs. the pinned commit).
# CMake applies this patch to a clean SDK checkout at configure time.
#
# The description at the top of the patch (everything before the first
# "diff --git") is kept as it is: edit it in the patch file when adding a
# change. Only the diff below it is regenerated.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
sdk="$root/thirdparty/rexglue-sdk"
out="$root/patches/rexglue-sdk/trials-hd.patch"

# Include new files in the diff without staging their contents.
git -C "$sdk" ls-files --others --exclude-standard -z | xargs -0 -r git -C "$sdk" add -N --

header="$(mktemp)"
trap 'rm -f "$header"' EXIT
if [[ -f "$out" ]]; then
  sed '/^diff --git /,$d' "$out" >"$header"
fi
{
  cat "$header"
  git -C "$sdk" diff
} >"$out.new"
mv "$out.new" "$out"
echo "wrote $out ($(grep -c '^diff --git' "$out") files)"
