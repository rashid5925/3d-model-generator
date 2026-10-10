#!/bin/sh
# shapeforge installer for macOS and Linux.
#   curl -fsSL https://raw.githubusercontent.com/rashid5925/3d-model-generator/main/scripts/install.sh | sh
# Options (environment variables):
#   SHAPEFORGE_VERSION=v0.2.0      install a specific release (default: latest)
#   SHAPEFORGE_INSTALL_DIR=DIR     where to put the binary (default: ~/.local/bin)
#   SHAPEFORGE_CLIENT=cursor       also register with an agent (see `shapeforge install`)
set -eu

REPO="${SHAPEFORGE_REPO:-rashid5925/3d-model-generator}"
VERSION="${SHAPEFORGE_VERSION:-latest}"
INSTALL_DIR="${SHAPEFORGE_INSTALL_DIR:-$HOME/.local/bin}"

say() { printf '%s\n' "$*"; }
fail() { say "error: $*" >&2; exit 1; }

os=$(uname -s)
arch=$(uname -m)
case "$os" in
  Darwin) asset="shapeforge-macos-universal" ;;
  Linux)
    case "$arch" in
      x86_64 | amd64) asset="shapeforge-linux-x64" ;;
      aarch64 | arm64) asset="shapeforge-linux-arm64" ;;
      *) fail "unsupported CPU architecture: $arch" ;;
    esac ;;
  *) fail "unsupported OS: $os (on Windows, use install.ps1)" ;;
esac

if [ "$VERSION" = "latest" ]; then
  base="https://github.com/$REPO/releases/latest/download"
else
  base="https://github.com/$REPO/releases/download/$VERSION"
fi

download() {
  if command -v curl >/dev/null 2>&1; then curl -fsSL "$1" -o "$2"
  elif command -v wget >/dev/null 2>&1; then wget -q "$1" -O "$2"
  else fail "curl or wget is required"; fi
}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
say "Downloading $asset ($VERSION)..."
download "$base/$asset" "$tmp/shapeforge" || fail "download failed: $base/$asset"
download "$base/SHA256SUMS" "$tmp/SHA256SUMS" || fail "download failed: $base/SHA256SUMS"

expected=$(grep " $asset\$" "$tmp/SHA256SUMS" | awk '{print $1}')
[ -n "$expected" ] || fail "no checksum for $asset in SHA256SUMS"
if command -v sha256sum >/dev/null 2>&1; then actual=$(sha256sum "$tmp/shapeforge" | awk '{print $1}')
else actual=$(shasum -a 256 "$tmp/shapeforge" | awk '{print $1}'); fi
[ "$expected" = "$actual" ] || fail "checksum mismatch for $asset"

mkdir -p "$INSTALL_DIR"
chmod +x "$tmp/shapeforge"
mv "$tmp/shapeforge" "$INSTALL_DIR/shapeforge"
say "Installed $INSTALL_DIR/shapeforge ($("$INSTALL_DIR/shapeforge" version))"

if [ -n "${SHAPEFORGE_CLIENT:-}" ]; then
  "$INSTALL_DIR/shapeforge" install "$SHAPEFORGE_CLIENT"
fi

case ":$PATH:" in
  *":$INSTALL_DIR:"*) ;;
  *) say ""; say "Note: $INSTALL_DIR is not on your PATH. Add it, e.g.:"
     say "  echo 'export PATH=\"$INSTALL_DIR:\$PATH\"' >> ~/.profile" ;;
esac

say ""
say "Next: connect it to your AI app, e.g."
say "  $INSTALL_DIR/shapeforge install claude-code"
say "Agents: claude-code, claude-desktop, cursor, windsurf, vscode, copilot-cli, opencode, codex, all"
say "Then ask: \"Model a coffee mug and export it as OBJ.\""
