#!/bin/bash
#
# Build script for IPC2581 Converter macOS application bundle.
# Creates a self-contained .app that can be dragged into /Applications.
#
# Usage: ./package/build_macos_app.sh
#
# Prerequisites:
#   - The C++ converter must already be built in build/ipc2581-to-kicad
#   - macOS with python3, sips, and iconutil available
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

APP_NAME="IPC2581 Converter"
BUNDLE_ID="com.ipc2581-to-kicad.converter"
VERSION="1.0.0"
BINARY="ipc2581-to-kicad"

DIST_DIR="$PROJECT_DIR/dist"
APP_DIR="$DIST_DIR/${APP_NAME}.app"
CONTENTS_DIR="$APP_DIR/Contents"
MACOS_DIR="$CONTENTS_DIR/MacOS"
RESOURCES_DIR="$CONTENTS_DIR/Resources"

CONVERTER_BIN="$PROJECT_DIR/build/$BINARY"
GUI_SCRIPT="$PROJECT_DIR/gui/ipc2581_gui.py"

# --- Verify prerequisites ---

if [ ! -f "$CONVERTER_BIN" ]; then
    echo "ERROR: Converter binary not found at $CONVERTER_BIN"
    echo "Build it first:  cd build && cmake .. && make"
    exit 1
fi

if [ ! -x "$CONVERTER_BIN" ]; then
    echo "ERROR: $CONVERTER_BIN is not executable"
    exit 1
fi

if [ ! -f "$GUI_SCRIPT" ]; then
    echo "ERROR: GUI script not found at $GUI_SCRIPT"
    exit 1
fi

if ! command -v python3 &>/dev/null; then
    echo "ERROR: python3 not found"
    exit 1
fi

echo "=== Building ${APP_NAME}.app ==="

# --- Clean previous build ---

if [ -d "$APP_DIR" ]; then
    echo "Removing previous build..."
    rm -rf "$APP_DIR"
fi

# --- Create directory structure ---

echo "Creating app bundle structure..."
mkdir -p "$MACOS_DIR"
mkdir -p "$RESOURCES_DIR"

# --- Copy resources ---

echo "Copying converter binary..."
cp "$CONVERTER_BIN" "$RESOURCES_DIR/"

echo "Copying GUI script..."
cp "$GUI_SCRIPT" "$RESOURCES_DIR/"

# --- Generate app icon ---

echo "Generating app icon..."
generate_icon() {
    local ICONSET_DIR
    ICONSET_DIR=$(mktemp -d)/AppIcon.iconset
    mkdir -p "$ICONSET_DIR"

    # Create a simple icon using Python + tkinter
    python3 << 'PYEOF'
import tkinter as tk
import os, sys

sizes = [16, 32, 64, 128, 256, 512]
iconset_dir = sys.argv[1] if len(sys.argv) > 1 else None

# We'll generate PNGs by drawing on a tkinter canvas and saving via PostScript,
# then converting with sips. But a simpler approach: generate a single large PNG
# and let sips resize it.

# Create a hidden root window
root = tk.Tk()
root.withdraw()

# Draw at 512x512
size = 512
canvas = tk.Canvas(root, width=size, height=size, bg='white', highlightthickness=0)
canvas.pack()

# Background - rounded rectangle (approximate with oval corners)
pad = 20
r = 60
# Fill background with a gradient-like blue
canvas.create_rectangle(pad, pad, size-pad, size-pad, fill='#2563EB', outline='#1D4ED8', width=4)

# Draw "PCB" text large in center
canvas.create_text(size//2, size//2 - 40, text='PCB', fill='white',
                   font=('Helvetica', 120, 'bold'), anchor='center')

# Draw "IPC→KiCad" below
canvas.create_text(size//2, size//2 + 80, text='IPC→KiCad', fill='#BFDBFE',
                   font=('Helvetica', 48, 'bold'), anchor='center')

# Draw some trace-like lines
canvas.create_line(pad+20, pad+20, pad+80, pad+20, fill='#FCD34D', width=4)
canvas.create_line(pad+80, pad+20, pad+80, pad+60, fill='#FCD34D', width=4)
canvas.create_line(size-pad-20, size-pad-20, size-pad-80, size-pad-20, fill='#FCD34D', width=4)
canvas.create_line(size-pad-80, size-pad-20, size-pad-80, size-pad-60, fill='#FCD34D', width=4)

# Small via dots
canvas.create_oval(pad+70, pad+10, pad+90, pad+30, fill='#FCD34D', outline='#D97706', width=2)
canvas.create_oval(size-pad-90, size-pad-30, size-pad-70, size-pad-10, fill='#FCD34D', outline='#D97706', width=2)

# Update to render
root.update()

# Save as PostScript then we'll convert
ps_file = '/tmp/ipc2581_icon.ps'
canvas.postscript(file=ps_file, colormode='color', width=size, height=size)

root.destroy()
PYEOF

    # Convert PostScript to PNG using sips (via Preview/CoreGraphics)
    # First, use python to convert PS to a proper PNG
    python3 << 'PYEOF2'
import subprocess, os

ps_file = '/tmp/ipc2581_icon.ps'
png_file = '/tmp/ipc2581_icon_512.png'

# Try using sips with a PDF intermediate, or use the built-in macOS tools
# Convert PS to PDF first
pdf_file = '/tmp/ipc2581_icon.pdf'
try:
    subprocess.run(['pstopdf', ps_file, '-o', pdf_file], check=True, capture_output=True)
    # Convert PDF to PNG using sips
    subprocess.run(['sips', '-s', 'format', 'png', pdf_file, '--out', png_file,
                    '-z', '512', '512'], check=True, capture_output=True)
except Exception:
    # Fallback: create a simple PNG using tkinter screenshot approach
    # We'll use a simpler approach - create icon from scratch with raw bitmap
    import tkinter as tk
    root = tk.Tk()
    root.withdraw()

    size = 512
    canvas = tk.Canvas(root, width=size, height=size, bg='white', highlightthickness=0)
    canvas.pack()

    pad = 20
    canvas.create_rectangle(pad, pad, size-pad, size-pad, fill='#2563EB', outline='#1D4ED8', width=4)
    canvas.create_text(size//2, size//2 - 40, text='PCB', fill='white',
                       font=('Helvetica', 120, 'bold'), anchor='center')
    canvas.create_text(size//2, size//2 + 80, text='IPC>KiCad', fill='#BFDBFE',
                       font=('Helvetica', 48, 'bold'), anchor='center')

    root.update()

    # Use screencapture as last resort or just create the PS and convert manually
    ps2 = '/tmp/ipc2581_icon2.ps'
    canvas.postscript(file=ps2, colormode='color', width=size, height=size)
    root.destroy()

    # Try alternative conversion
    try:
        subprocess.run(['pstopdf', ps2, '-o', pdf_file], check=True, capture_output=True)
        subprocess.run(['sips', '-s', 'format', 'png', pdf_file, '--out', png_file,
                        '-z', '512', '512'], check=True, capture_output=True)
    except Exception as e2:
        print(f"Warning: Could not generate icon PNG: {e2}")
        print("The app will work but will use the default macOS icon.")
        import sys
        sys.exit(1)
PYEOF2

    local png_512="/tmp/ipc2581_icon_512.png"

    if [ ! -f "$png_512" ]; then
        echo "WARNING: Could not generate icon. Using default macOS icon."
        return 1
    fi

    # Generate all required icon sizes using sips
    for sz in 16 32 64 128 256 512; do
        sips -z $sz $sz "$png_512" --out "$ICONSET_DIR/icon_${sz}x${sz}.png" >/dev/null 2>&1
    done

    # Generate @2x variants
    sips -z 32 32 "$png_512" --out "$ICONSET_DIR/icon_16x16@2x.png" >/dev/null 2>&1
    sips -z 64 64 "$png_512" --out "$ICONSET_DIR/icon_32x32@2x.png" >/dev/null 2>&1
    sips -z 256 256 "$png_512" --out "$ICONSET_DIR/icon_128x128@2x.png" >/dev/null 2>&1
    sips -z 512 512 "$png_512" --out "$ICONSET_DIR/icon_256x256@2x.png" >/dev/null 2>&1
    sips -z 1024 1024 "$png_512" --out "$ICONSET_DIR/icon_512x512@2x.png" >/dev/null 2>&1

    # Convert iconset to icns
    iconutil -c icns "$ICONSET_DIR" -o "$RESOURCES_DIR/AppIcon.icns" 2>/dev/null

    # Cleanup
    rm -rf "$(dirname "$ICONSET_DIR")"
    rm -f /tmp/ipc2581_icon.ps /tmp/ipc2581_icon.pdf /tmp/ipc2581_icon_512.png /tmp/ipc2581_icon2.ps

    if [ -f "$RESOURCES_DIR/AppIcon.icns" ]; then
        echo "Icon generated successfully."
        return 0
    else
        echo "WARNING: Could not generate .icns file. Using default macOS icon."
        return 1
    fi
}

ICON_GENERATED=false
if generate_icon; then
    ICON_GENERATED=true
fi

# --- Create Info.plist ---

echo "Creating Info.plist..."
ICON_ENTRY=""
if [ "$ICON_GENERATED" = true ]; then
    ICON_ENTRY="<key>CFBundleIconFile</key>
	<string>AppIcon</string>"
fi

cat > "$CONTENTS_DIR/Info.plist" << PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleName</key>
	<string>${APP_NAME}</string>
	<key>CFBundleDisplayName</key>
	<string>${APP_NAME}</string>
	<key>CFBundleIdentifier</key>
	<string>${BUNDLE_ID}</string>
	<key>CFBundleVersion</key>
	<string>${VERSION}</string>
	<key>CFBundleShortVersionString</key>
	<string>${VERSION}</string>
	<key>CFBundlePackagetype</key>
	<string>APPL</string>
	<key>CFBundleSignature</key>
	<string>????</string>
	<key>CFBundleExecutable</key>
	<string>launcher</string>
	${ICON_ENTRY}
	<key>CFBundleInfoDictionaryVersion</key>
	<string>6.0</string>
	<key>LSMinimumSystemVersion</key>
	<string>10.15</string>
	<key>NSHighResolutionCapable</key>
	<true/>
	<key>CFBundleDocumentTypes</key>
	<array>
		<dict>
			<key>CFBundleTypeExtensions</key>
			<array>
				<string>xml</string>
			</array>
			<key>CFBundleTypeName</key>
			<string>IPC-2581 XML File</string>
			<key>CFBundleTypeRole</key>
			<string>Viewer</string>
		</dict>
	</array>
</dict>
</plist>
PLIST

# --- Create launcher script ---

echo "Creating launcher script..."
cat > "$MACOS_DIR/launcher" << 'LAUNCHER'
#!/bin/bash
#
# Launcher script for IPC2581 Converter.app
# Resolves paths inside the .app bundle and launches the Python GUI.
#

# macOS GUI apps don't inherit the user's shell PATH, so source their profile
# to pick up python3 from Homebrew, conda, radioconda, etc.
if [ -f "$HOME/.zprofile" ]; then
    source "$HOME/.zprofile" 2>/dev/null
fi
if [ -f "$HOME/.zshrc" ]; then
    source "$HOME/.zshrc" 2>/dev/null
fi

# Get the directory where this script lives (Contents/MacOS/)
MACOS_DIR="$(cd "$(dirname "$0")" && pwd)"
CONTENTS_DIR="$(dirname "$MACOS_DIR")"
RESOURCES_DIR="$CONTENTS_DIR/Resources"

# Add Resources to PATH so the GUI can find the converter binary
export PATH="$RESOURCES_DIR:$PATH"

# Launch the GUI using python3 from PATH (prefer user-installed over system)
exec python3 "$RESOURCES_DIR/ipc2581_gui.py" "$@"
LAUNCHER

chmod +x "$MACOS_DIR/launcher"

# --- Done ---

echo ""
echo "=== Build complete! ==="
echo "Application bundle: $APP_DIR"
echo ""
echo "To run:     open \"$APP_DIR\""
echo "To install: drag \"${APP_NAME}.app\" from dist/ into /Applications"
echo ""
echo "NOTE: On first launch, macOS may block the app because it is unsigned."
echo "      Right-click the app and select 'Open' to bypass Gatekeeper."
