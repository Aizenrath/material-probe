@echo off
cd /d "%~dp0"
py -3 -c "import sys" 1>nul 2>nul
if errorlevel 1 (
  powershell -NoProfile -Command "Add-Type -AssemblyName PresentationFramework; [System.Windows.MessageBox]::Show('Install Python from python.org and tick Add python.exe to PATH. Then open Material Probe again.')"
  exit /b 1
)
py -3 -c "import serial" 1>nul 2>nul
if errorlevel 1 py -3 -m pip install --user -r "%~dp0requirements.txt"
start "" pyw -3 "%~dp0preview.py"
