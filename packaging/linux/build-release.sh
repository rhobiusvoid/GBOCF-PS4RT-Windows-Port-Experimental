#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build, audit, and package four private-free release assets from Git HEAD.
set -euo pipefail

fail() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)"
PROJECT="${PS4RT_BUILD_ID:-$(basename "$ROOT")}"
[[ "$PROJECT" =~ ^[A-Za-z0-9][A-Za-z0-9._-]{0,79}$ ]] || fail 'Invalid project name'
# Reject dirty tracked source BEFORE spending time compiling old HEAD.
git -C "$ROOT" diff --quiet HEAD || fail 'Commit tracked source changes before building a release'
PARENT="${PS4RT_LINUX_WORK_PARENT:-$HOME/Documents/PS4RT-Builds}"
WORK="$PARENT/$PROJECT"
RELEASE_ROOT="${PS4RT_RELEASE_ROOT:-$HOME/Documents/PS4RT-Releases}"
echo "Project: $PROJECT"
echo "Source: committed Git HEAD only"
echo "Private build workspace: $WORK"
echo "Release assets: $RELEASE_ROOT/$PROJECT"

# The Control Center invokes this script directly. Prepare missing builder images
# here, so GUI and CLI release builds have the same self-contained preflight.
command -v podman >/dev/null || fail 'Podman is missing. Install Podman before packaging.'
command -v python3 >/dev/null || fail 'Python 3 is missing.'
echo "Checking Podman release builder images..."

if ! podman image exists localhost/ps4rt-builder:linux; then
  echo "Base Linux builder is missing. Building it now..."
  bash "$ROOT/packaging/linux/build-images.sh" ||
    fail 'Could not build base Linux Podman image'
fi
podman image exists localhost/ps4rt-builder:linux ||
  fail 'Base Linux Podman image is still missing after the build'

NEEDS_NATIVE_PKG=0
if [[ -f "$ROOT/port_features.json" ]]; then
  NEEDS_NATIVE_PKG="$(python3 - "$ROOT/port_features.json" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as source:
    features = json.load(source)
if not isinstance(features, dict):
    raise SystemExit("Invalid port_features.json: expected a JSON object")
for key in ("supports_updates", "supports_asset_dlc", "needs_native_pkg"):
    if type(features.get(key)) is not bool:
        raise SystemExit(f"Invalid port_features.json: {key} must be boolean")
required = features["supports_updates"] or features["supports_asset_dlc"]
if features["needs_native_pkg"] != required:
    raise SystemExit("Invalid port_features.json: inconsistent native PKG capability")
print(int(required))
PY
  )" || fail 'Could not validate port_features.json'
fi

if [[ "$NEEDS_NATIVE_PKG" == 1 ]]; then
  if ! podman image exists localhost/ps4rt-builder:linux-pkg; then
    echo "Native PKG/Crypto++ builder is missing. Building it now..."
    bash "$ROOT/packaging/linux/build-pkg-image.sh" ||
      fail 'Could not build native PKG/Crypto++ Podman image'
  fi
  podman image exists localhost/ps4rt-builder:linux-pkg ||
    fail 'Native PKG/Crypto++ Podman image is still missing after the build'
fi

echo "Builder images ready. Reusing installed images where possible."
bash "$ROOT/packaging/linux/build-runtime.sh"
[[ -f "$WORK/source-revision.txt" && -d "$WORK/bundle" ]] ||
  fail 'Build did not create expected workspace'
[[ "$(cat "$WORK/source-revision.txt")" == "$(git -C "$ROOT" rev-parse HEAD)" ]] ||
  fail 'Source changed after build; refusing mixed-revision package'
python3 "$ROOT/packaging/linux/release.py" package \
  --bundle-from "$WORK/bundle" \
  --name "$PROJECT" \
  --output "$RELEASE_ROOT/$PROJECT"
echo 'Four archives/checksums created, not published. Inspect and play-test before releasing.'
