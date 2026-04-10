#!/bin/bash
#
# Prepares the LittleFS data directory for the ESP32-C6 bridge.
# Copies the WebUI build output and gzip-compresses the large files
# so they fit comfortably on the ESP32's flash.
#
# Usage:
#   cd esp32c6-bridge
#   bash prepare_data.sh
#
# After running, upload the data/ directory to LittleFS using
# the ESP32 Sketch Data Upload tool or arduino-cli.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DATA_DIR="$SCRIPT_DIR/data/www"
BUILD_DIR="$SCRIPT_DIR/../webui/build"

if [ ! -d "$BUILD_DIR" ]; then
  echo "ERROR: WebUI build directory not found at $BUILD_DIR"
  echo "Run 'npm run build' inside webui/ first."
  exit 1
fi

echo "Cleaning data directory..."
rm -rf "$SCRIPT_DIR/data"
mkdir -p "$DATA_DIR/static/css" "$DATA_DIR/static/js"

echo "Copying and compressing WebUI files..."

# index.html — gzip
gzip -9 -c "$BUILD_DIR/index.html" > "$DATA_DIR/index.html.gz"

# manifest.json — gzip
if [ -f "$BUILD_DIR/manifest.json" ]; then
  gzip -9 -c "$BUILD_DIR/manifest.json" > "$DATA_DIR/manifest.json.gz"
fi

# robots.txt — small, copy as-is
if [ -f "$BUILD_DIR/robots.txt" ]; then
  cp "$BUILD_DIR/robots.txt" "$DATA_DIR/robots.txt"
fi

# CSS files — gzip
for f in "$BUILD_DIR/static/css/"*.css; do
  [ -f "$f" ] || continue
  gzip -9 -c "$f" > "$DATA_DIR/static/css/$(basename "$f").gz"
done

# JS files — gzip
for f in "$BUILD_DIR/static/js/"*.js; do
  [ -f "$f" ] || continue
  gzip -9 -c "$f" > "$DATA_DIR/static/js/$(basename "$f").gz"
done

# JS LICENSE files — copy as-is (small)
for f in "$BUILD_DIR/static/js/"*.LICENSE.txt; do
  [ -f "$f" ] || continue
  cp "$f" "$DATA_DIR/static/js/$(basename "$f")"
done

echo ""
echo "=== Data directory summary ==="
du -sh "$SCRIPT_DIR/data"
find "$SCRIPT_DIR/data" -type f -exec ls -lh {} \;
echo ""
echo "Done! Upload data/ to ESP32-C6 LittleFS, then flash the sketch."
