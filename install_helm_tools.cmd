@echo off
setlocal EnableExtensions EnableDelayedExpansion
title Helm 1.9.1 AI Tools Installer

rem Tool root. Pass a path as the first argument to override; it must match
rem the "Tool root" value in Helm Settings.
set "ROOT=%~1"
if "%ROOT%"=="" set "ROOT=F:\AI Tools"
set "WORKFLOW_SOURCE=%~dp0tools_runtime\comfyui_sdxl_api.json"
set "WORKFLOW_TARGET=%ROOT%\Helm\workflows\sdxl-api.json"
set "SDXL_MODEL=%ROOT%\ComfyUI\models\checkpoints\sd_xl_base_1.0.safetensors"

echo.
echo ============================================================
echo  HELM 1.9.1 AI TOOLS INSTALLER
echo  Tools and models will be installed under:
echo  %ROOT%
echo ============================================================
echo.

where git >nul 2>&1
if errorlevel 1 (
  echo ERROR: Git is not available in this terminal.
  pause
  exit /b 1
)

where curl.exe >nul 2>&1
if errorlevel 1 (
  echo ERROR: Windows curl.exe is unavailable.
  pause
  exit /b 1
)

where py >nul 2>&1
if errorlevel 1 (
  echo ERROR: Python Launcher "py" is not installed.
  echo Install Python 3.12, then run this file again.
  pause
  exit /b 1
)

set "PY="
py -3.12 --version >nul 2>&1 && set "PY=py -3.12"
if not defined PY py -3.13 --version >nul 2>&1 && set "PY=py -3.13"
if not defined PY py -3 --version >nul 2>&1 && set "PY=py -3"
if not defined PY (
  echo ERROR: No usable Python 3 installation was found.
  pause
  exit /b 1
)

mkdir "%ROOT%" 2>nul
mkdir "%ROOT%\Models" 2>nul
mkdir "%ROOT%\Voices" 2>nul
mkdir "%ROOT%\Utilities" 2>nul
mkdir "%ROOT%\Helm\workflows" 2>nul

echo Using Python:
%PY% --version
echo.

echo [1/9] Installing the shared Helm tool runtime...
if not exist "%ROOT%\HelmToolRuntime\Scripts\python.exe" (
  %PY% -m venv "%ROOT%\HelmToolRuntime"
  if errorlevel 1 goto :failed
)
call "%ROOT%\HelmToolRuntime\Scripts\activate.bat"
python -m pip install --upgrade pip setuptools wheel
if errorlevel 1 goto :failed_active
python -m pip install ^
  playwright ^
  piper-tts ^
  pillow ^
  opencv-python-headless ^
  "rembg[cli]" ^
  onnxruntime ^
  pypdf ^
  python-docx ^
  openpyxl ^
  python-pptx ^
  pandas ^
  fastembed ^
  chromadb ^
  rank-bm25 ^
  beautifulsoup4 ^
  trafilatura ^
  httpx ^
  lxml ^
  psutil ^
  pyautogui ^
  pywinauto ^
  mss ^
  pyperclip ^
  watchdog ^
  fastapi ^
  uvicorn
if errorlevel 1 goto :failed_active

echo.
echo [2/9] Installing headless Chromium for JavaScript-rendered web pages...
set "PLAYWRIGHT_BROWSERS_PATH=%ROOT%\Playwright-Browsers"
python -m playwright install chromium
if errorlevel 1 goto :failed_active
call deactivate

echo.
echo [3/9] Downloading and setting up ComfyUI...
if not exist "%ROOT%\ComfyUI\.git" (
  git clone https://github.com/Comfy-Org/ComfyUI.git "%ROOT%\ComfyUI"
  if errorlevel 1 goto :failed
) else (
  git -C "%ROOT%\ComfyUI" pull --ff-only
)

if not exist "%ROOT%\ComfyUI\.venv\Scripts\python.exe" (
  %PY% -m venv "%ROOT%\ComfyUI\.venv"
  if errorlevel 1 goto :failed
)
call "%ROOT%\ComfyUI\.venv\Scripts\activate.bat"
rem torch pins setuptools<82; upgrading it here guarantees a conflict on install.
python -m pip install --upgrade pip wheel
if errorlevel 1 goto :failed_active
python -m pip install torch torchvision torchaudio --index-url https://download.pytorch.org/whl/cu128
if errorlevel 1 goto :failed_active
python -m pip install -r "%ROOT%\ComfyUI\requirements.txt"
if errorlevel 1 goto :failed_active
call deactivate

(
  echo @echo off
  echo title Helm ComfyUI
  echo cd /d "%ROOT%\ComfyUI"
  echo call ".venv\Scripts\activate.bat"
  echo python main.py --listen 127.0.0.1 --port 8188
  echo pause
) > "%ROOT%\START_COMFYUI.cmd"

echo.
echo [4/9] Installing the starter SDXL workflow and checkpoint...
if not exist "%WORKFLOW_SOURCE%" (
  echo ERROR: The bundled ComfyUI workflow is missing:
  echo   "%WORKFLOW_SOURCE%"
  goto :failed
)
copy /y "%WORKFLOW_SOURCE%" "%WORKFLOW_TARGET%" >nul
mkdir "%ROOT%\ComfyUI\models\checkpoints" 2>nul
if not exist "%SDXL_MODEL%" (
  echo Downloading SDXL base 1.0. This is a large download and can be resumed.
  curl.exe --fail --location --retry 5 --continue-at - --output "%SDXL_MODEL%" ^
    "https://huggingface.co/stabilityai/stable-diffusion-xl-base-1.0/resolve/main/sd_xl_base_1.0.safetensors?download=true"
  if errorlevel 1 goto :failed
)

echo.
echo [5/9] Downloading and compiling whisper.cpp with CUDA for RTX 4090...
if not exist "%ROOT%\whisper.cpp\.git" (
  git clone https://github.com/ggml-org/whisper.cpp.git "%ROOT%\whisper.cpp"
  if errorlevel 1 goto :failed
) else (
  git -C "%ROOT%\whisper.cpp" pull --ff-only
)

if not exist "%ROOT%\Models\ggml-base.en.bin" (
  curl.exe --fail --location --retry 5 --continue-at - --output "%ROOT%\Models\ggml-base.en.bin" ^
    "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.en.bin?download=true"
  if errorlevel 1 goto :failed
)

set "CMAKE_EXE="
for %%I in (cmake.exe) do set "CMAKE_EXE=%%~$PATH:I"
if not defined CMAKE_EXE if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" (
  for /f "usebackq delims=" %%I in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set "VSROOT=%%I"
  if exist "!VSROOT!\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
    set "CMAKE_EXE=!VSROOT!\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
  )
)

if not defined CMAKE_EXE (
  echo WARNING: CMake was not found. Whisper source and model are installed,
  echo          but whisper.cpp could not be compiled.
) else (
  echo Using CMake: "!CMAKE_EXE!"
  rem CMake stamps generator and platform into the cache on first configure and
  rem then refuses to reconfigure that directory. One interrupted run poisons it
  rem permanently, so this always starts clean.
  if exist "%ROOT%\whisper.cpp\build\CMakeCache.txt" rmdir /s /q "%ROOT%\whisper.cpp\build"
  "!CMAKE_EXE!" -S "%ROOT%\whisper.cpp" -B "%ROOT%\whisper.cpp\build" ^
    -G "Visual Studio 17 2022" -A x64 ^
    -DGGML_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=89 ^
    -DWHISPER_BUILD_TESTS=OFF -DWHISPER_BUILD_EXAMPLES=ON
  if errorlevel 1 goto :failed
  "!CMAKE_EXE!" --build "%ROOT%\whisper.cpp\build" --config Release --parallel 1
  if errorlevel 1 goto :failed
)

echo.
echo [6/9] Downloading the Piper English voice...
if not exist "%ROOT%\Voices\en_US-lessac-medium.onnx" (
  curl.exe --fail --location --retry 5 --continue-at - --output "%ROOT%\Voices\en_US-lessac-medium.onnx" ^
    "https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_US/lessac/medium/en_US-lessac-medium.onnx?download=true"
  if errorlevel 1 goto :failed
)
if not exist "%ROOT%\Voices\en_US-lessac-medium.onnx.json" (
  curl.exe --fail --location --retry 5 --continue-at - --output "%ROOT%\Voices\en_US-lessac-medium.onnx.json" ^
    "https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_US/lessac/medium/en_US-lessac-medium.onnx.json?download=true"
  if errorlevel 1 goto :failed
)

echo.
echo [7/9] Downloading yt-dlp...
curl.exe --fail --location --retry 5 --output "%ROOT%\Utilities\yt-dlp.exe" ^
  "https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp.exe"
if errorlevel 1 goto :failed

echo.
echo [8/9] Downloading portable FFmpeg...
if not exist "%ROOT%\FFmpeg\bin\ffmpeg.exe" (
  curl.exe --fail --location --retry 5 --output "%ROOT%\ffmpeg-release-essentials.zip" ^
    "https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip"
  if errorlevel 1 goto :failed

  powershell.exe -NoProfile -ExecutionPolicy Bypass -Command ^
    "$root='%ROOT%'; $tmp=Join-Path $root '_ffmpeg_unpack';" ^
    "Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue;" ^
    "Expand-Archive (Join-Path $root 'ffmpeg-release-essentials.zip') $tmp -Force;" ^
    "$src=Get-ChildItem $tmp -Directory | Select-Object -First 1;" ^
    "Remove-Item (Join-Path $root 'FFmpeg') -Recurse -Force -ErrorAction SilentlyContinue;" ^
    "Move-Item $src.FullName (Join-Path $root 'FFmpeg');" ^
    "Remove-Item $tmp -Recurse -Force;" ^
    "Remove-Item (Join-Path $root 'ffmpeg-release-essentials.zip') -Force"
  if errorlevel 1 goto :failed
)

echo.
echo [9/9] Downloading portable ripgrep...
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command ^
  "$root='%ROOT%';" ^
  "$release=Invoke-RestMethod 'https://api.github.com/repos/BurntSushi/ripgrep/releases/latest';" ^
  "$asset=$release.assets | Where-Object { $_.name -match 'x86_64-pc-windows-msvc\.zip$' } | Select-Object -First 1;" ^
  "if(-not $asset){throw 'Could not find the Windows x64 ripgrep package'};" ^
  "$zip=Join-Path $root 'ripgrep.zip';" ^
  "Invoke-WebRequest $asset.browser_download_url -OutFile $zip;" ^
  "Remove-Item (Join-Path $root 'ripgrep') -Recurse -Force -ErrorAction SilentlyContinue;" ^
  "Remove-Item (Join-Path $root 'ripgrep-unpack') -Recurse -Force -ErrorAction SilentlyContinue;" ^
  "Expand-Archive $zip (Join-Path $root 'ripgrep-unpack') -Force;" ^
  "$src=Get-ChildItem (Join-Path $root 'ripgrep-unpack') -Directory | Select-Object -First 1;" ^
  "Move-Item $src.FullName (Join-Path $root 'ripgrep');" ^
  "Remove-Item (Join-Path $root 'ripgrep-unpack') -Recurse -Force;" ^
  "Remove-Item $zip -Force"
if errorlevel 1 goto :failed

(
  echo @echo off
  echo set "HELM_AI_TOOLS=%ROOT%"
  echo set "PLAYWRIGHT_BROWSERS_PATH=%ROOT%\Playwright-Browsers"
  echo set "PATH=%ROOT%\Utilities;%ROOT%\FFmpeg\bin;%ROOT%\ripgrep;%%PATH%%"
  echo call "%ROOT%\HelmToolRuntime\Scripts\activate.bat"
  echo echo Helm tool environment is active.
) > "%ROOT%\OPEN_HELM_TOOL_TERMINAL.cmd"

echo.
echo ============================================================
echo  DONE - HELM TOOLS ARE READY
echo ============================================================
echo.
echo Installed under:
echo   %ROOT%
echo.
echo Starter image model:
echo   %SDXL_MODEL%
echo.
echo ComfyUI workflow:
echo   %WORKFLOW_TARGET%
echo.
echo Start ComfyUI with:
echo   "%ROOT%\START_COMFYUI.cmd"
echo.
pause
exit /b 0

:failed_active
call deactivate >nul 2>&1
:failed
echo.
echo ============================================================
echo  INSTALL FAILED - review the error above
echo ============================================================
echo.
pause
exit /b 1
