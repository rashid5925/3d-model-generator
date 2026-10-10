#!/bin/sh
# Replaces the OWNER/shapeforge placeholder with your GitHub repository in every
# packaging file. Usage: scripts/set-repo.sh your-user/shapeforge
set -eu
[ $# -eq 1 ] || { echo "usage: $0 <github-owner>/<repo>"; exit 1; }
new="$1"
owner="${new%%/*}"
cd "$(dirname "$0")/.."
files=$(grep -rl "OWNER" --include='*.json' --include='*.sh' --include='*.ps1' --include='*.js' --include='*.mjs' \
        --include='*.md' --include='*.rb' --include='*.yml' . | grep -v -e '^./build' -e '^./out' -e '^./scripts/set-repo.sh' || true)
for f in $files; do
  # MCP registry name first (keeps the product name), then the repo slug, then bare owner fields.
  sed -i.bak -e "s#io\.github\.OWNER/#io.github.$owner/#g" -e "s#OWNER/shapeforge#$new#g" \
      -e "s#OWNER/homebrew-tap#$owner/homebrew-tap#g" -e "s#OWNER/tap#$owner/tap#g" -e "s#\"OWNER\"#\"$owner\"#g" "$f"
  rm -f "$f.bak"
  echo "updated $f"
done
