#!/bin/bash
# Double-click this file on macOS to launch the GUI

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Launch the GUI
python3 ipc2581_gui.py &

# Close the terminal window after a short delay
sleep 1
osascript -e 'tell application "Terminal" to close first window' &> /dev/null || true
