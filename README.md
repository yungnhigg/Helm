# Helm 1.2 merged source

Helm is a native Windows local-AI workspace built with C++20, llama.cpp, Win32,
and WebView2. This archive merges the stable patched runtime/UI branch with the
1.2 tooling revision and the later inference-memory and streaming-scroll changes.

## What is merged

- Saved conversations, Chat / Agent modes, GGUF model selection, effort controls,
  attachments, RAG files, local/task/web-scraper agents, and imported stdio-json
  tool packs.
- Built-in 1.2 adapters for current web search, ComfyUI image generation, Whisper
  microphone transcription, Piper speech, PDF/DOCX/XLSX/PPTX extraction, desktop
  screenshots, clicking, typing, and hotkeys.
- A microphone button at the end of the composer. WebView2 grants microphone
  access only to Helm's private `https://app.local` UI origin; the recording is
  converted locally with FFmpeg and transcribed locally with whisper.cpp.
- Runtime settings for context size, GPU layers, logical and physical batches,
  CPU threads, Flash Attention, KV-cache location, and F16/Q8_0/Q4_0 KV cache.
- Presets for a fast RTX 4090 setup, a larger-context balanced 4090 setup, and a
  96 GB system-RAM hybrid setup.
- Streaming that follows new tokens only while the transcript is already near the
  bottom. Scrolling upward stays put, and a **New output** button returns to live
  output.
- The full registered tool set is callable from both Chat and Agent modes. Agent
  mode adds reusable profiles, task instructions, and multi-step worker behavior.
- Tool detection and enable/disable controls under the gear menu and Agent → AI
  Tooling.

## Source layout

```text
src/main.cpp                  Win32/WebView2 host and microphone permission
src/ui/bridge.*               validated UI/core bridge, settings, transcription
src/engine/                   llama.cpp model/context runtime
src/agent/                    prompt, tool grammar, loop, cancellable jobs
src/session/                  saved conversations
src/workspace/                agents, RAG, attachments, imported tool packs
src/tools/external_tools.*    native process boundary for 1.2 tool adapters
tools_runtime/helm_tools.py   web, ComfyUI, documents, and desktop helper
web/                          Windows 11-style UI
config/app.json               immutable first-run defaults
install_helm_tools.cmd        installs the optional stack under F:\AI Tools
```

## Build

Requirements:

- Windows 11 or current Windows 10
- Visual Studio with **Desktop development with C++**
- CMake 3.24+
- Git
- WebView2 Runtime
- CUDA toolkit for the NVIDIA build

From an x64 Native Tools terminal:

```powershell
cmake -B build -S . -DGGML_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=89
cmake --build build --config Release --parallel 1
ctest --test-dir build -C Release --output-on-failure
```

The `--parallel 1` build is intentional for machines that hit Windows error 5
while many CUDA template compiler processes write into the same build tree.

Output:

```text
build\Release\Helm.exe
build\Release\web\
build\Release\config\
build\Release\tools_runtime\
build\Release\install_helm_tools.cmd
```

## Set up the optional tools

Run this from the source folder or beside `Helm.exe`:

```text
install_helm_tools.cmd
```

It installs the shared helper environment, Playwright browsers, ComfyUI,
whisper.cpp plus an English model, Piper plus an English voice, FFmpeg, yt-dlp,
and ripgrep under `F:\AI Tools`. ComfyUI model weights are intentionally not
included; add your chosen checkpoint and export an API-format workflow JSON,
then select that workflow in Helm's gear menu.

The tool-root path and individual executable overrides live in:

```text
%LOCALAPPDATA%\Helm\runtime.json
```

The UI writes this file. It does not overwrite the shipped `config\app.json`.

## Models and memory settings

Use the **+** button beside the model selector to add a GGUF. Helm stores the
reference instead of copying the model. Saving runtime settings unloads and
reloads the active model so the new context/KV configuration takes effect.

Preset intent:

- **4090 Fast:** 8K context, full GPU offload, F16 KV in VRAM.
- **4090 Balanced:** 32K context, full GPU offload, Q8_0 KV in VRAM.
- **96 GB Hybrid:** 64K context, full GPU model offload, Q8_0 KV in system RAM. Lower GPU layers manually when the model itself does not fit.

The exact usable context still depends on the GGUF architecture, quantization,
VRAM use, and backend support. Lower context, use Q8_0/Q4_0 KV, or move KV to
RAM when allocation fails.

## RAG and attachments

Text/source files are ranked locally and inserted as bounded context. PDF, DOCX,
XLSX, and PPTX files are extracted through the local helper and cached beside
the imported workspace copy. Images remain attachments/metadata unless a model
or future multimodal adapter can consume them.

## Safety boundaries

- Navigation and WebView messages are restricted to `https://app.local`.
- Microphone permission is granted only to that local UI origin.
- External tools run as argument arrays, not arbitrary shell command strings.
- Tool jobs have cancellation and timeouts.
- `run_process` remains disabled by default and supports an executable allowlist.
- Desktop controls are separately switchable in Settings.

## Verification in this archive

- JavaScript syntax checked with Node.
- HTML IDs and local asset references checked.
- Default and example JSON parsed.
- Python helper byte-compiled.
- Portable grammar and stream-filter C++ tests compiled and run.

A complete Windows/WebView2/CUDA compile must still be run on the target Windows
machine because this archive was assembled in a Linux container without the
Windows SDK or MSVC.

## Operators

Typed in the composer, handled before anything reaches the model — so they work
in Chat mode too, where no tools are registered.

| Command | Effect |
| --- | --- |
| `/remember <fact>` | Append a fact to long-term memory |
| `/forget <text>` | Remove matching memory entries |
| `/memory` | Show memory and open its editor |
| `/tools` | List registered tools and which groups are enabled |
| `/model <name>` | Switch model by partial name |
| `/new` | Start a new conversation |
| `/help` | List these |

Type `/` for a filterable menu; Tab completes.

## Long-term memory

Plain markdown at `%LOCALAPPDATA%\Helm\memory.md`, injected into every system
prompt in both modes. Budgeted (`memory_budget_bytes`, default 8192): over
budget, writes are refused rather than silently truncated. Edit it directly in
Settings, or with `/remember` and `/forget`. In agent mode the model can also
call `remember`, `recall_memory`, and `forget`.

Writes are always explicit. Nothing is captured implicitly.
