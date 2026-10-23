@echo off
REM ===========================================================================
REM build_all.bat - development build into dev\ (the running miner uses bin\),
REM then the correctness tests (logs\test.log).
REM     build_all.bat          build dev\ + tests
REM     build_all.bat nobuild  tests only
REM   Extra nvcc flags for an experiment: run\nvcc_extra.txt (one line)
REM Logs: logs\build_gpu_dll.log, logs\build_host.log, logs\test.log,
REM       summary logs\build_all.log
REM ===========================================================================
setlocal EnableExtensions
title KrgnMiner - build
cd /d "%~dp0"
if not exist logs mkdir logs
if not exist dev mkdir dev
set "S=%CD%\logs\build_all.log"
echo [%date% %time%] build_all start %* > "%S%"
if /i "%~1"=="nobuild" goto tests

set "KM_NVCC_EXTRA="
if exist run\nvcc_extra.txt set /p KM_NVCC_EXTRA=<run\nvcc_extra.txt
echo [%date% %time%] nvcc extra flags: "%KM_NVCC_EXTRA%" >> "%S%"

echo === dev\kgpu.dll (nvcc) ===
call windows\build_gpu_dll.bat dev
if errorlevel 1 (
    echo [%date% %time%] GPU DLL BUILD FAILED >> "%S%"
    echo BUILD FAILED - see logs\build_gpu_dll.log
    goto end
)
echo [%date% %time%] kgpu.dll OK >> "%S%"

echo === dev\krgnminer.exe (g++) ===
set MSYSTEM=MINGW64
set CHERE_INVOKING=1
C:\msys64\usr\bin\bash.exe -lc "./windows/build_host.sh dev"
if errorlevel 1 (
    echo [%date% %time%] HOST BUILD FAILED >> "%S%"
    echo BUILD FAILED - see logs\build_host.log
    goto end
)
echo [%date% %time%] host OK >> "%S%"

:tests
call run_test.bat
echo [%date% %time%] tests done >> "%S%"

:end
echo [%date% %time%] build_all end >> "%S%"
echo TERMINE - voir logs\build_all.log
