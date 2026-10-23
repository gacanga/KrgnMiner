@echo off
REM ===========================================================================
REM build_variants.bat - layout experiments of the GPU solver.
REM   run\variants.txt : one line per variant  "NAME TPB nvcc-flags..."
REM     e.g.  B12 512 -DEQ_BUCKBITS=12 -DEQ_SHARE_CHECK=0
REM   For each line: dev\var\NAME\kgpu.dll (+ krgnminer.exe, cudart) then
REM   krgnminer.exe --profile --tpb TPB (all GPUs).  Uses the current dev\krgnminer.exe.
REM   Results: logs\variants.log (appended to logs\variants_history.log)
REM ===========================================================================
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0.."
if not exist logs mkdir logs
set "V=%CD%\logs\variants.log"
echo [%date% %time%] variants start> "%V%"
if not exist dev\krgnminer.exe (
    echo ERROR: dev\krgnminer.exe missing, run build_all.bat first>> "%V%"
    exit /b 1
)
set CUDA_DEVICE_ORDER=PCI_BUS_ID
for /f "usebackq tokens=1,2,*" %%a in ("run\variants.txt") do (
    set "D=dev\var\%%a"
    if not exist "!D!" mkdir "!D!"
    echo ---- %%a: tpb %%b, flags %%c>> "%V%"
    set "KM_NVCC_EXTRA=%%c"
    call windows\build_gpu_dll.bat "!D!" >nul 2>&1
    if exist "!D!\kgpu.dll" (
        copy /y dev\krgnminer.exe "!D!\" >nul
        "!D!\krgnminer.exe" --profile --tpb %%b 2>&1 | findstr "PROFILE" >> "%V%"
    ) else (
        echo BUILD FAILED>> "%V%"
        findstr /i "error" logs\build_gpu_dll.log >> "%V%"
    )
)
echo [%date% %time%] variants end>> "%V%"
type "%V%" >> logs\variants_history.log
exit /b 0
