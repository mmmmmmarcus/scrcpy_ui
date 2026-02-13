#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 3 ]]; then
    echo "Usage: $0 <build-dir> [output-dir] [stamp-file]" >&2
    exit 1
fi

BUILD_DIR="$1"
OUTPUT_DIR="${2:-$BUILD_DIR}"
STAMP_FILE="${3:-}"
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

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

if [[ -f "$PROJECT_ROOT/app/data/icon.png" ]]; then
    cp "$PROJECT_ROOT/app/data/icon.png" "$APP_RESOURCES/icon.png"

    ICONSET_DIR="$APP_RESOURCES/AppIcon.iconset"
    rm -rf "$ICONSET_DIR"
    mkdir -p "$ICONSET_DIR"

    for size in 16 32 128 256 512; do
        sips -s format png -z "$size" "$size" \
            "$PROJECT_ROOT/app/data/icon.png" \
            --out "$ICONSET_DIR/icon_${size}x${size}.png" >/dev/null
        size2=$((size * 2))
        sips -s format png -z "$size2" "$size2" \
            "$PROJECT_ROOT/app/data/icon.png" \
            --out "$ICONSET_DIR/icon_${size}x${size}@2x.png" >/dev/null
    done

    if command -v iconutil >/dev/null 2>&1; then
        iconutil -c icns "$ICONSET_DIR" -o "$APP_RESOURCES/AppIcon.icns" \
            >/dev/null
    fi

    rm -rf "$ICONSET_DIR"
fi

if [[ -f "$PROJECT_ROOT/app/data/img/photo_camera_wght300_24.png" ]]; then
    cp "$PROJECT_ROOT/app/data/img/photo_camera_wght300_24.png" \
       "$APP_RESOURCES/photo_camera_wght300_24.png"
fi

if [[ -f "$PROJECT_ROOT/app/data/img/copy_screenshot_figma.svg" ]]; then
    # Rasterize SVG at high resolution to keep icon crisp on HiDPI displays.
    sips -s format png -z 256 256 \
        "$PROJECT_ROOT/app/data/img/copy_screenshot_figma.svg" \
        --out "$APP_RESOURCES/copy_screenshot_figma.png" >/dev/null
elif [[ -f "$PROJECT_ROOT/app/data/img/copy_screenshot_figma.png" ]]; then
    cp "$PROJECT_ROOT/app/data/img/copy_screenshot_figma.png" \
       "$APP_RESOURCES/copy_screenshot_figma.png"
fi

if [[ -f "$PROJECT_ROOT/app/data/img/screenshot_success_check.svg" ]]; then
    sips -s format png -z 256 256 \
        "$PROJECT_ROOT/app/data/img/screenshot_success_check.svg" \
        --out "$APP_RESOURCES/screenshot_success_check.png" >/dev/null
fi

if [[ -f "$PROJECT_ROOT/app/data/img/screenshot_button_bg.png" ]]; then
    cp "$PROJECT_ROOT/app/data/img/screenshot_button_bg.png" \
       "$APP_RESOURCES/screenshot_button_bg.png"
fi

if [[ -f "$PROJECT_ROOT/app/data/img/input_toggle_on.svg" ]]; then
    sips -s format png -z 256 256 \
        "$PROJECT_ROOT/app/data/img/input_toggle_on.svg" \
        --out "$APP_RESOURCES/input_toggle_on.png" >/dev/null
fi

if [[ -f "$PROJECT_ROOT/app/data/img/input_toggle_button_bg.png" ]]; then
    cp "$PROJECT_ROOT/app/data/img/input_toggle_button_bg.png" \
       "$APP_RESOURCES/input_toggle_button_bg.png"
fi

if [[ -f "$PROJECT_ROOT/app/data/img/settings_wght300_24.svg" ]]; then
    sips -s format png -z 256 256 \
        "$PROJECT_ROOT/app/data/img/settings_wght300_24.svg" \
        --out "$APP_RESOURCES/settings_wght300_24.png" >/dev/null
elif [[ -f "$PROJECT_ROOT/app/data/img/settings_wght300_24.png" ]]; then
    cp "$PROJECT_ROOT/app/data/img/settings_wght300_24.png" \
       "$APP_RESOURCES/settings_wght300_24.png"
fi

if [[ -f "$PROJECT_ROOT/app/data/img/Ndot-55.otf" ]]; then
    cp "$PROJECT_ROOT/app/data/img/Ndot-55.otf" "$APP_RESOURCES/Ndot-55.otf"
fi

cat > "$APP_MACOS/ScrcpyUI" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

SELF_DIR="$(cd "$(dirname "$0")" && pwd)"
RESOURCE_DIR="$(cd "$SELF_DIR/../Resources" && pwd)"

if [[ -f "$RESOURCE_DIR/icon.png" ]]; then
    export SCRCPY_ICON_PATH="$RESOURCE_DIR/icon.png"
fi

if [[ -f "$RESOURCE_DIR/copy_screenshot_figma.png" ]]; then
    export SCRCPY_SCREENSHOT_ICON_PATH="$RESOURCE_DIR/copy_screenshot_figma.png"
elif [[ -f "$RESOURCE_DIR/photo_camera_wght300_24.png" ]]; then
    export SCRCPY_SCREENSHOT_ICON_PATH="$RESOURCE_DIR/photo_camera_wght300_24.png"
fi

if [[ -f "$RESOURCE_DIR/screenshot_success_check.png" ]]; then
    export SCRCPY_SCREENSHOT_CHECK_ICON_PATH="$RESOURCE_DIR/screenshot_success_check.png"
fi

if [[ -f "$RESOURCE_DIR/screenshot_button_bg.png" ]]; then
    export SCRCPY_SCREENSHOT_BUTTON_BG_PATH="$RESOURCE_DIR/screenshot_button_bg.png"
fi

if [[ -f "$RESOURCE_DIR/input_toggle_on.png" ]]; then
    export SCRCPY_INPUT_TOGGLE_ICON_PATH="$RESOURCE_DIR/input_toggle_on.png"
fi

if [[ -f "$RESOURCE_DIR/input_toggle_button_bg.png" ]]; then
    export SCRCPY_INPUT_TOGGLE_BUTTON_BG_PATH="$RESOURCE_DIR/input_toggle_button_bg.png"
fi

if [[ -f "$RESOURCE_DIR/settings_wght300_24.png" ]]; then
    export SCRCPY_SETTINGS_ICON_PATH="$RESOURCE_DIR/settings_wght300_24.png"
fi

if [[ -f "$RESOURCE_DIR/Ndot-55.otf" ]]; then
    export SCRCPY_UI_FONT_PATH="$RESOURCE_DIR/Ndot-55.otf"
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
  <key>CFBundleIconFile</key>
  <string>AppIcon</string>
  <key>NSHighResolutionCapable</key>
  <true/>
</dict>
</plist>
EOF

echo "Created app bundle: $APP_DIR"

if [[ -n "$STAMP_FILE" ]]; then
    mkdir -p "$(dirname "$STAMP_FILE")"
    : > "$STAMP_FILE"
fi
