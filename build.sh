#!/usr/bin/env bash
# Builds and installs File Cleaner as a user Flatpak.
# Run from the project root (where this script lives).
set -e

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "==> Checking dependencies..."

if ! command -v flatpak-builder &>/dev/null; then
  echo "flatpak-builder not found. Installing..."
  rpm-ostree install flatpak-builder
  echo ""
  echo "flatpak-builder installed. Please reboot, then run this script again."
  exit 0
fi

for runtime in "org.gnome.Platform//47" "org.gnome.Sdk//47"; do
  if ! flatpak info "$runtime" &>/dev/null; then
    echo "Installing Flatpak runtime: $runtime"
    flatpak install --user -y flathub "$runtime"
  fi
done

echo "==> Building Flatpak..."
mkdir -p "$PROJECT_DIR/build-dir"

flatpak-builder \
  --user \
  --install \
  --force-clean \
  "$PROJECT_DIR/build-dir" \
  "$PROJECT_DIR/io.github.filecleaner.json"

echo ""
echo "==> Done! File Cleaner is installed."
echo "    Run it with:  flatpak run io.github.filecleaner"
echo "    Or find it in your app grid."
