@echo off
REM stop_krgn.bat - stops KrgnMiner: the auto-restart loop(s) (krgn_pool.bat),
REM then any remaining krgnminer.exe.   stop_krgn.bat nopause  (no key press)
REM Other miners are not touched.
taskkill /fi "WINDOWTITLE eq KrgnMiner - miner*" /t /f >nul 2>&1
powershell -NoProfile -Command "Get-CimInstance Win32_Process -Filter \"Name='cmd.exe'\" | Where-Object { $_.CommandLine -match 'krgn_pool\.bat' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }" >nul 2>&1
timeout /t 2 /nobreak >nul
taskkill /im krgnminer.exe /t /f >nul 2>&1
cd /d "%~dp0"
if not exist logs mkdir logs
echo [%date% %time%] miner stopped by stop_krgn.bat>> logs\pool.log
tasklist /fi "imagename eq krgnminer.exe" 2>nul | find /i "krgnminer.exe" >nul
if errorlevel 1 (echo Mineur arrete.) else (echo krgnminer.exe tourne encore.)
if /i not "%~1"=="nopause" pause
