#!/usr/bin/env bash
# Cross-compiles File Cleaner for 64-bit Windows and packages it as a
# portable folder plus a .zip in dist/. Run from the project root:
#
#   ./build-windows.sh
#
# Everything needed (the llvm-mingw toolchain and the MSYS2 builds of GTK 4
# and libadwaita) is downloaded into .win-build/ on the first run. The build
# itself runs inside the GNOME SDK Flatpak, so nothing is installed on the
# host system.
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$PROJECT_DIR"

# Re-run inside the GNOME SDK, which provides cmake, ninja, zstd,
# pkg-config, rsvg-convert and glib-compile-schemas.
if [ -z "${FC_IN_SDK:-}" ]; then
  if ! flatpak info org.gnome.Sdk//47 &>/dev/null; then
    echo "Installing the GNOME 47 SDK..."
    flatpak install --user -y flathub org.gnome.Sdk//47
  fi
  exec flatpak run --share=network --filesystem="$PROJECT_DIR" --env=FC_IN_SDK=1 \
    --command=bash org.gnome.Sdk//47 "$PROJECT_DIR/build-windows.sh" "$@"
fi

WORK="$PROJECT_DIR/.win-build"
LLVM_MINGW_VERSION=20260922
LLVM_MINGW="$WORK/llvm-mingw-$LLVM_MINGW_VERSION-ucrt-ubuntu-22.04-x86_64"
SYSROOT="$WORK/msys2/sysroot/ucrt64"
BUILD="$WORK/build"
DIST="$PROJECT_DIR/dist/FileCleaner-windows-x64"
mkdir -p "$WORK"

echo "==> Toolchain"
if [ ! -x "$LLVM_MINGW/bin/x86_64-w64-mingw32-clang++" ]; then
  curl -sSL -o "$WORK/llvm-mingw.tar.xz" \
    "https://github.com/mstorsjo/llvm-mingw/releases/download/$LLVM_MINGW_VERSION/$(basename "$LLVM_MINGW").tar.xz"
  tar -xf "$WORK/llvm-mingw.tar.xz" -C "$WORK"
  rm "$WORK/llvm-mingw.tar.xz"
fi

echo "==> GTK 4 and libadwaita for Windows (MSYS2)"
python3 tools/fetch_msys2.py "$WORK/msys2"

echo "==> Compiling"
export LLVM_MINGW
export PKG_CONFIG_SYSROOT_DIR="$WORK/msys2/sysroot"
export PKG_CONFIG_LIBDIR="$SYSROOT/lib/pkgconfig:$SYSROOT/share/pkgconfig"
export PKG_CONFIG_PATH=""
cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$PROJECT_DIR/tools/mingw-toolchain.cmake" >/dev/null
cmake --build "$BUILD"

echo "==> Packaging"
rm -rf "$DIST"
mkdir -p "$DIST"
cp "$BUILD/FileCleaner.exe" "$DIST/"

# gdk-pixbuf's SVG loader, used to draw the (SVG) icons of the Adwaita theme.
LOADER_REL="lib/gdk-pixbuf-2.0/2.10.0/loaders"
mkdir -p "$DIST/$LOADER_REL"
cp "$SYSROOT/$LOADER_REL/pixbufloader_svg.dll" "$DIST/$LOADER_REL/"
cat > "$DIST/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache" <<'EOF'
"lib\\gdk-pixbuf-2.0\\2.10.0\\loaders\\pixbufloader_svg.dll"
"svg" 6 "gdk-pixbuf" "Scalable Vector Graphics" "LGPL"
"image/svg+xml" "image/svg" "image/svg-xml" "image/vnd.adobe.svg+xml" "text/xml-svg" "image/svg+xml-compressed" ""
"svg" "svgz" "svg.gz" ""
" <svg" "*    " 100
" <!DOCTYPE svg" "*             " 100

EOF

# Copy every DLL the executable (and the loader) depends on, recursively.
# DLLs not found in the sysroot or toolchain belong to Windows itself.
OBJDUMP="$LLVM_MINGW/bin/llvm-objdump"
SEARCH=("$SYSROOT/bin" "$LLVM_MINGW/x86_64-w64-mingw32/bin")
declare -A SEEN=()
queue=("$DIST/FileCleaner.exe" "$DIST/$LOADER_REL/pixbufloader_svg.dll")
while [ ${#queue[@]} -gt 0 ]; do
  file="${queue[0]}"
  queue=("${queue[@]:1}")
  while read -r dll; do
    key="${dll,,}"
    [ -n "${SEEN[$key]:-}" ] && continue
    SEEN[$key]=1
    for dir in "${SEARCH[@]}"; do
      match=$(find "$dir" -maxdepth 1 -iname "$dll" -print -quit 2>/dev/null || true)
      if [ -n "$match" ]; then
        cp "$match" "$DIST/"
        queue+=("$DIST/$(basename "$match")")
        break
      fi
    done
  done < <("$OBJDUMP" -p "$file" | sed -n 's/^\s*DLL Name: //p')
done

# GSettings schemas (required by the GTK file chooser) and icon themes.
mkdir -p "$DIST/share/glib-2.0/schemas"
cp "$SYSROOT"/share/glib-2.0/schemas/*.xml "$DIST/share/glib-2.0/schemas/"
glib-compile-schemas "$DIST/share/glib-2.0/schemas"
mkdir -p "$DIST/share/icons"
cp -r "$SYSROOT/share/icons/Adwaita" "$SYSROOT/share/icons/hicolor" "$DIST/share/icons/"
mkdir -p "$DIST/share/icons/hicolor/scalable/apps"
cp io.github.filecleaner.svg "$DIST/share/icons/hicolor/scalable/apps/"

cp LICENSE "$DIST/LICENSE.txt"
cat > "$DIST/README.txt" <<'EOF'
File Cleaner for Windows
========================

Double-click FileCleaner.exe to start. No installation is required; keep
FileCleaner.exe together with the other files in this folder.

C:\Windows, Program Files and ProgramData are always protected and are never
scanned or deleted. Add your own important folders under
Preferences > Exclusions before cleaning.

Settings are stored in %APPDATA%\FileCleaner and scan history in
%LOCALAPPDATA%\FileCleaner.
EOF

(cd "$(dirname "$DIST")" && rm -f "$(basename "$DIST").zip" && \
  python3 -c "import shutil,sys; shutil.make_archive(sys.argv[1], 'zip', '.', sys.argv[1])" "$(basename "$DIST")")

echo ""
echo "==> Done"
echo "    Folder: $DIST"
echo "    Zip:    $DIST.zip ($(du -h "$DIST.zip" | cut -f1))"
