@echo off
setlocal EnableExtensions EnableDelayedExpansion
title Helm installer build
cd /d "%~dp0.."

rem ---------------------------------------------------------------------------
rem Builds Helm in Release with CUDA, stages the CUDA runtime DLLs the binary
rem needs at load time, then compiles the Inno Setup script into
rem dist\Helm-Setup-<version>.exe
rem
rem Usage:
rem     make_installer.cmd            build for Ada (RTX 4090)
rem     make_installer.cmd 120        build for Blackwell (RTX PRO 5000 / 5090)
rem
rem Requires Inno Setup 6 (https://jrsoftware.org/isdl.php). Everything else is
rem already needed to build Helm at all.
rem ---------------------------------------------------------------------------

set "ARCH=%~1"
if "%ARCH%"=="" set "ARCH=89"

echo.
echo ============================================================
echo  Building Helm installer
echo  CUDA architecture: sm_%ARCH%
echo ============================================================
echo.

rem -- 1. Build ---------------------------------------------------------------
echo [1/4] Configuring and building Release...
cmake -S . -B build -DGGML_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=%ARCH% || goto :failed
cmake --build build --config Release || goto :failed

if not exist "build\Release\Helm.exe" (
  echo ERROR: build\Release\Helm.exe was not produced.
  goto :failed
)

rem -- 2. Stage the CUDA runtime --------------------------------------------
rem llama.cpp is statically linked, but the CUDA runtime and cuBLAS never are.
rem A machine without the CUDA Toolkit installed cannot start the binary without
rem these sitting next to it.
echo.
echo [2/4] Staging CUDA runtime libraries...
set "STAGE=packaging\staging\cuda"
if exist "%STAGE%" rmdir /s /q "%STAGE%"
mkdir "%STAGE%" 2>nul

set "CUDA_BIN="
if defined CUDA_PATH if exist "%CUDA_PATH%\bin" set "CUDA_BIN=%CUDA_PATH%\bin"
if not defined CUDA_BIN (
  rem CUDA_PATH is normally set by the toolkit installer. Fall back to the
  rem newest versioned directory if it is missing.
  for /f "delims=" %%D in ('dir /b /ad /o-n "%ProgramFiles%\NVIDIA GPU Computing Toolkit\CUDA" 2^>nul') do (
    if not defined CUDA_BIN set "CUDA_BIN=%ProgramFiles%\NVIDIA GPU Computing Toolkit\CUDA\%%D\bin"
  )
)

if not defined CUDA_BIN (
  echo WARNING: CUDA bin directory not found. The installer will be built
  echo          without the runtime DLLs, so it will only work on machines
  echo          that already have the CUDA Toolkit installed.
) else (
  echo   Source: !CUDA_BIN!
  for %%N in (cudart64 cublas64 cublasLt64) do (
    for %%F in ("!CUDA_BIN!\%%N_*.dll") do (
      copy /y "%%~F" "%STAGE%\" >nul && echo   + %%~nxF
    )
  )
)

rem -- 3. Locate Inno Setup -------------------------------------------------
echo.
echo [3/4] Locating Inno Setup...
set "ISCC="
for %%P in (
  "%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
  "%ProgramFiles%\Inno Setup 6\ISCC.exe"
) do if not defined ISCC if exist %%P set "ISCC=%%~P"

if not defined ISCC (
  echo ERROR: Inno Setup 6 not found.
  echo        Install it from https://jrsoftware.org/isdl.php and re-run.
  goto :failed
)
echo   Using: !ISCC!

rem -- 4. Compile the installer ---------------------------------------------
echo.
echo [4/4] Compiling installer...
if not exist dist mkdir dist
"!ISCC!" /Q "packaging\helm.iss" || goto :failed

echo.
echo ============================================================
echo  DONE
for %%F in (dist\Helm-Setup-*.exe) do echo  %%~nxF  (%%~zF bytes)
echo ============================================================
echo.
pause
exit /b 0

:failed
echo.
echo ============================================================
echo  BUILD FAILED - review the error above
echo ============================================================
pause
exit /b 1
