#!/bin/bash
# Launcher script for IPC-2581 to KiCad Converter GUI

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Check if Python 3 is available
if command -v python3 &> /dev/null; then
    python3 ipc2581_gui.py "$@"
elif command -v python &> /dev/null; then
    python ipc2581_gui.py "$@"
else
    echo "Error: Python not found. Please install Python 3."
    exit 1
fi
