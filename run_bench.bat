@echo off
REM run_bench.bat - Sol/s benchmark of dev\krgnminer.exe (60 s by default).
REM Extra arguments (e.g. --gpus 2 --tpb 256 --bench 120) in run\bench_args.txt
REM Log: logs\bench.log (overwritten), history appended to logs\bench_history.log
setlocal EnableExtensions
cd /d "%~dp0"
if not exist logs mkdir logs
set "PATH=%CD%\dev;C:\msys64\mingw64\bin;%PATH%"
set CUDA_DEVICE_ORDER=PCI_BUS_ID
if exist run\variants.txt (
    call windows\build_variants.bat
    goto :eof
)
set "ARGS="
if exist run\bench_args.txt set "ARGS=--args-file run\bench_args.txt"
echo [%date% %time%] bench start> logs\bench.log
if exist run\bench_args.txt type run\bench_args.txt>> logs\bench.log
dev\krgnminer.exe --bench 60 %ARGS% >> logs\bench.log 2>&1
echo [%date% %time%] bench exit=%ERRORLEVEL%>> logs\bench.log
type logs\bench.log >> logs\bench_history.log
