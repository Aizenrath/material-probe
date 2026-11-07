@echo off
cd /d "%~dp0"
py -3 -c "import sys" 1>nul 2>nul
if errorlevel 1 (
  powershell -NoProfile -Command "Add-Type -AssemblyName PresentationFramework; [System.Windows.MessageBox]::Show('Install Python from python.org and tick Add python.exe to PATH. Then run this installer again.')"
  exit /b 1
)
py -3 -m pip install --user -r "%~dp0requirements.txt"
if errorlevel 1 (
  echo Could not install pyserial.
  pause
  exit /b 1
)
powershell -NoProfile -Command "$w = New-Object -ComObject WScript.Shell; $s = $w.CreateShortcut([Environment]::GetFolderPath('Startup') + '\Material Probe.lnk'); $s.TargetPath = '%~dp0Material Probe.bat'; $s.WorkingDirectory = '%~dp0'; $s.Save(); $d = $w.CreateShortcut([Environment]::GetFolderPath('Desktop') + '\Material Probe.lnk'); $d.TargetPath = '%~dp0Material Probe.bat'; $d.WorkingDirectory = '%~dp0'; $d.Save()"
echo.
echo Installed. A desktop shortcut is ready, and the window opens when Windows starts.
echo Leave the probe plugged into USB.
echo.
start "" "%~dp0Material Probe.bat"
pause
