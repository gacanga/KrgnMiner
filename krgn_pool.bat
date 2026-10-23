@echo off
REM ===========================================================================
REM krgn_pool.bat - KrgnMiner on rplant (Kerrigan, Equihash 192,7).  Settings:
REM   krgnminer.conf, extra arguments in run\pool_args.txt (e.g. --gpus 0,1).
REM   Automatic restart 10 s after any exit.
REM   Log: logs\pool.log (appended).  Stop: stop_krgn.bat or close the window.
REM ===========================================================================
setlocal EnableExtensions
title KrgnMiner - miner
cd /d "%~dp0"
if not exist logs mkdir logs
set "PATH=%CD%\bin;C:\msys64\mingw64\bin;%PATH%"
set CUDA_DEVICE_ORDER=PCI_BUS_ID
:loop
set "ARGS="
if exist run\pool_args.txt set "ARGS=--args-file run\pool_args.txt"
echo [%date% %time%] miner start %ARGS%>> logs\pool.log
bin\krgnminer.exe --config krgnminer.conf --log logs\pool.log %ARGS%
echo [%date% %time%] miner exited with code %ERRORLEVEL% - restart in 10 s>> logs\pool.log
echo [%date% %time%] arret du mineur (code %ERRORLEVEL%), relance dans 10 s
timeout /t 10 /nobreak >nul
goto loop
