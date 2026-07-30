#!/usr/bin/env bash
set -euo pipefail

PACKAGE_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
EXTENSION_DIR="$PACKAGE_ROOT/integrations/browser-extension"
OUTPUT_DIR="$PACKAGE_ROOT/dist"
OUTPUT_FILE="$OUTPUT_DIR/quizpane-course-companion.zip"
extension_version="${1:-}"

mkdir -p "$OUTPUT_DIR"
rm -f "$OUTPUT_FILE"

if [[ -n "$extension_version" ]]; then
  [[ "$extension_version" =~ ^[0-9]+(\.[0-9]+){0,3}$ ]] || {
    echo "Extension version must contain 1-4 numeric segments, for example 0.1.1" >&2
    exit 2
  }
  staging_dir="$(mktemp -d)"
  trap 'rm -rf "$staging_dir"' EXIT
  cp -R "$EXTENSION_DIR/src" "$EXTENSION_DIR/icons" "$staging_dir/"
  jq --arg version "$extension_version" '.version = $version' \
    "$EXTENSION_DIR/manifest.json" > "$staging_dir/manifest.json"
  package_dir="$staging_dir"
else
  package_dir="$EXTENSION_DIR"
fi

(cd "$package_dir" && zip -qr "$OUTPUT_FILE" manifest.json src icons \
    -x '*/.DS_Store' -x 'icons/icon.svg' -x 'icons/shiba-source.png')

echo "Created $OUTPUT_FILE"
