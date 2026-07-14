#!/bin/bash
# Build a TeXpresso.app bundle for macOS.
#
# Run AFTER `make texpresso` so the binaries exist under build/.
# Output: build/TeXpresso.app/ with proper Info.plist, padded AppIcon.icns,
# and the texpresso + engine binaries under Contents/MacOS/.
#
# Launch via `open build/TeXpresso.app --args path/to/doc.tex` or by
# pointing your editor plugin at build/TeXpresso.app/Contents/MacOS/texpresso.
# macOS will then show the bundle icon in the dock at the correct size.

set -e

REPO="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$REPO/build"
APP="$BUILD/TeXpresso.app"
SVG="$REPO/doc/texpresso_logo_v3.svg"

if [ ! -x "$BUILD/texpresso" ]; then
  echo "error: $BUILD/texpresso not found — run 'make texpresso' first" >&2
  exit 1
fi

for tool in rsvg-convert iconutil; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "error: $tool not found in PATH" >&2
    exit 1
  fi
done

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"

# Render the SVG at every size macOS expects, padding so the artwork
# occupies ~80% of the canvas (Apple HIG: icons sit inside a squircle
# that takes 824/1024 = ~80.5% of the canvas).
ICONSET="$(mktemp -d)"
trap 'rm -rf "$ICONSET"' EXIT

render() {
  size=$1
  name=$2
  # SVG already contains the 80% padding (viewBox extends beyond the
  # artwork). Just rasterize at the target size.
  rsvg-convert -w "$size" -h "$size" "$SVG" -o "$ICONSET/$name"
}

render 16    icon_16x16.png
render 32    icon_16x16@2x.png
render 32    icon_32x32.png
render 64    icon_32x32@2x.png
render 128   icon_128x128.png
render 256   icon_128x128@2x.png
render 256   icon_256x256.png
render 512   icon_256x256@2x.png
render 512   icon_512x512.png
render 1024  icon_512x512@2x.png

# iconutil expects the directory to end in .iconset.
mv "$ICONSET" "$ICONSET.iconset"
iconutil -c icns "$ICONSET.iconset" -o "$APP/Contents/Resources/AppIcon.icns"
rm -rf "$ICONSET.iconset"
trap - EXIT

# Copy texpresso and its engine binary. find_engine() looks for the
# engine in the same directory as the main binary, so both must
# sit together under Contents/MacOS/.
cp "$BUILD/texpresso" "$APP/Contents/MacOS/texpresso"
[ -x "$BUILD/texpresso-xetex" ]  && cp "$BUILD/texpresso-xetex"  "$APP/Contents/MacOS/"

cat > "$APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>
    <string>texpresso</string>
    <key>CFBundleIdentifier</key>
    <string>net.texpresso.texpresso</string>
    <key>CFBundleName</key>
    <string>TeXpresso</string>
    <key>CFBundleDisplayName</key>
    <string>TeXpresso</string>
    <key>CFBundleIconFile</key>
    <string>AppIcon</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>1.0</string>
    <key>CFBundleVersion</key>
    <string>1.0</string>
    <key>LSMinimumSystemVersion</key>
    <string>10.13</string>
    <key>NSHighResolutionCapable</key>
    <true/>
</dict>
</plist>
PLIST

echo "built: $APP"
