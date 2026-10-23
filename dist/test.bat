@echo off
REM KrgnMiner - correctness tests: CPU checks, then every GPU against the reference
REM solutions. Stop mining first (each card needs about 5.4 GB of free memory).
setlocal EnableExtensions
title KrgnMiner - test
cd /d "%~dp0"
set CUDA_DEVICE_ORDER=PCI_BUS_ID
"%~dp0krgnminer.exe" --config "%~dp0krgnminer.conf" --test
echo.
echo Test exit code %ERRORLEVEL% (0 = all OK).
pause
