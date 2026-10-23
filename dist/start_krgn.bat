@echo off
REM KrgnMiner - mines Kerrigan (KRGN) with the settings of krgnminer.conf (same folder).
REM Edit krgnminer.conf first: replace YOUR_KRGN_ADDRESS with your address.
REM Restarts the miner 10 s after any exit; close this window to stop.
setlocal EnableExtensions
title KrgnMiner - miner
cd /d "%~dp0"
findstr /c:"YOUR_KRGN_ADDRESS" "%~dp0krgnminer.conf" >nul
if not errorlevel 1 (
    echo.
    echo  Edit krgnminer.conf: replace YOUR_KRGN_ADDRESS with your payout address.
    echo  Modifiez krgnminer.conf : remplacez YOUR_KRGN_ADDRESS par votre adresse de paiement.
    echo.
    pause
    exit /b 1
)
set CUDA_DEVICE_ORDER=PCI_BUS_ID
:loop
"%~dp0krgnminer.exe" --config "%~dp0krgnminer.conf" --log "%~dp0krgnminer.log"
echo KrgnMiner exited (code %ERRORLEVEL%), restarting in 10 s - close this window to stop.
timeout /t 10 /nobreak >nul
goto loop
