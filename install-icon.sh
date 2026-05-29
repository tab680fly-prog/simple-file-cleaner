#!/usr/bin/env bash
# Installs the File Cleaner icon into the user icon theme at all standard sizes.
# Requires: inkscape (for PNG rasterisation) or rsvg-convert (librsvg).
# Run from the same directory as io.github.filecleaner.svg

set -e
APP_ID="io.github.filecleaner"
SVG="${APP_ID}.svg"
ICON_DIR="${HOME}/.local/share/icons/hicolor"

if [[ ! -f "$SVG" ]]; then
  echo "Error: $SVG not found. Run this script from the same folder as the SVG."
  exit 1
fi

# Pick a renderer
if command -v rsvg-convert &>/dev/null; then
  RENDERER="rsvg"
elif command -v inkscape &>/dev/null; then
  RENDERER="inkscape"
else
  echo "Error: install either librsvg2-tools (rsvg-convert) or inkscape."
  exit 1
fi

render_png() {
  local size=$1 dest=$2
  mkdir -p "$(dirname "$dest")"
  if [[ "$RENDERER" == "rsvg" ]]; then
    rsvg-convert -w "$size" -h "$size" "$SVG" -o "$dest"
  else
    inkscape --export-filename="$dest" -w "$size" -h "$size" "$SVG" 2>/dev/null
  fi
  echo "  wrote $dest"
}

for size in 16 24 32 48 64 128 256 512; do
  dest="${ICON_DIR}/${size}x${size}/apps/${APP_ID}.png"
  render_png "$size" "$dest"
done

# Also install the scalable SVG
mkdir -p "${ICON_DIR}/scalable/apps"
cp "$SVG" "${ICON_DIR}/scalable/apps/${APP_ID}.svg"
echo "  wrote ${ICON_DIR}/scalable/apps/${APP_ID}.svg"

# Refresh the icon cache
if command -v gtk-update-icon-cache &>/dev/null; then
  gtk-update-icon-cache -f -t "${ICON_DIR}" 2>/dev/null || true
fi

echo ""
echo "Icon installed. If the app grid doesn't update immediately, log out and back in."
