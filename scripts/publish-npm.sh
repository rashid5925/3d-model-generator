#!/bin/sh
# Publishes the npm packages for an existing GitHub Release from this machine,
# using your own npm login (2FA prompts are answered in the terminal/browser).
#
# Needed once for brand-new packages: npm Trusted Publishing can only be set up
# for packages that already exist. After that, releases publish from CI.
#
#   scripts/publish-npm.sh 0.2.0
set -eu

VERSION="${1:?usage: scripts/publish-npm.sh <version, e.g. 0.2.0>}"
REPO="${SHAPEFORGE_REPO:-rashid5925/3d-model-generator}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

npm whoami >/dev/null 2>&1 || { echo "Not logged in to npm. Run 'npm login' first."; exit 1; }
echo "Publishing shapeforge $VERSION to npm as $(npm whoami)"

mkdir -p "$WORK/dist"
base="https://github.com/$REPO/releases/download/v$VERSION"
for f in shapeforge-macos-universal shapeforge-linux-x64 shapeforge-linux-arm64 shapeforge-windows-x64.exe SHA256SUMS; do
  curl -fsSL -o "$WORK/dist/$f" "$base/$f"
done
(cd "$WORK/dist" && if command -v sha256sum >/dev/null 2>&1; then sha256sum -c SHA256SUMS; else shasum -a 256 -c SHA256SUMS; fi)

node "$ROOT/packaging/npm/build-packages.mjs" "$VERSION" "$WORK/dist" "$WORK/npm"

# Platform packages first so the main package's optionalDependencies resolve.
for p in shapeforge-darwin shapeforge-linux-x64 shapeforge-linux-arm64 shapeforge-win32-x64 shapeforge; do
  if npm view "$p@$VERSION" version >/dev/null 2>&1; then
    echo "$p@$VERSION already published, skipping"
  else
    (cd "$WORK/npm/$p" && npm publish --access public)
  fi
done

echo
echo "Done. Check: npx -y shapeforge@$VERSION version"
