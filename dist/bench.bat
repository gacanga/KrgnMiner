@echo off
REM KrgnMiner - 60 s Sol/s benchmark on the GPUs of krgnminer.conf. Stop mining first.
setlocal EnableExtensions
title KrgnMiner - bench
cd /d "%~dp0"
set CUDA_DEVICE_ORDER=PCI_BUS_ID
"%~dp0krgnminer.exe" --config "%~dp0krgnminer.conf" --bench 60
pause
