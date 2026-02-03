#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
    echo "Usage: $0 <build-dir> [output-dir]" >&2
    exit 1
fi

BUILD_DIR="$1"
OUTPUT_DIR="${2:-$BUILD_DIR}"

SCRCPY_BIN="$BUILD_DIR/app/scrcpy"
SERVER_BIN="$BUILD_DIR/server/scrcpy-server"

if [[ ! -x "$SCRCPY_BIN" ]]; then
    echo "Missing executable: $SCRCPY_BIN" >&2
    exit 1
fi

APP_DIR="$OUTPUT_DIR/ScrcpyUI.app"
APP_CONTENTS="$APP_DIR/Contents"
APP_MACOS="$APP_CONTENTS/MacOS"
APP_RESOURCES="$APP_CONTENTS/Resources"

rm -rf "$APP_DIR"
mkdir -p "$APP_MACOS" "$APP_RESOURCES"

cp "$SCRCPY_BIN" "$APP_MACOS/scrcpy"
if [[ -f "$SERVER_BIN" ]]; then
    cp "$SERVER_BIN" "$APP_MACOS/scrcpy-server"
fi

if command -v adb >/dev/null 2>&1; then
    cp "$(command -v adb)" "$APP_MACOS/adb"
fi

if [[ -f "app/data/icon.png" ]]; then
    cp "app/data/icon.png" "$APP_RESOURCES/icon.png"
fi

cat > "$APP_MACOS/ScrcpyUI" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

SELF_DIR="$(cd "$(dirname "$0")" && pwd)"
RESOURCE_DIR="$(cd "$SELF_DIR/../Resources" && pwd)"

if [[ -f "$RESOURCE_DIR/icon.png" ]]; then
    export SCRCPY_ICON_PATH="$RESOURCE_DIR/icon.png"
fi

if [[ -f "$SELF_DIR/scrcpy-server" ]]; then
    export SCRCPY_SERVER_PATH="$SELF_DIR/scrcpy-server"
fi

if [[ -f "$SELF_DIR/adb" ]]; then
    export ADB="$SELF_DIR/adb"
elif [[ -x /opt/homebrew/bin/adb ]]; then
    export ADB="/opt/homebrew/bin/adb"
elif [[ -x /usr/local/bin/adb ]]; then
    export ADB="/usr/local/bin/adb"
fi

export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"

exec "$SELF_DIR/scrcpy" "$@"
EOF

chmod +x "$APP_MACOS/ScrcpyUI"

cat > "$APP_CONTENTS/Info.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
  "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key>
  <string>ScrcpyUI</string>
  <key>CFBundleDisplayName</key>
  <string>ScrcpyUI</string>
  <key>CFBundleIdentifier</key>
  <string>com.genymobile.scrcpyui</string>
  <key>CFBundleVersion</key>
  <string>1.0</string>
  <key>CFBundleShortVersionString</key>
  <string>1.0</string>
  <key>CFBundlePackageType</key>
  <string>APPL</string>
  <key>CFBundleExecutable</key>
  <string>ScrcpyUI</string>
  <key>NSHighResolutionCapable</key>
  <true/>
</dict>
</plist>
EOF

echo "Created app bundle: $APP_DIR"
