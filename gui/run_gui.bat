@echo off
REM Launcher script for IPC-2581 to KiCad Converter GUI (Windows)

cd /d "%~dp0"

where python >nul 2>nul
if %ERRORLEVEL% EQU 0 (
    python ipc2581_gui.py %*
) else (
    echo Error: Python not found. Please install Python 3.
    pause
    exit /b 1
)
