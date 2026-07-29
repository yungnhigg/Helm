# Helm 1.x

A native Windows local-AI workspace. One process, one window, one binary.
C++20, Win32, WebView2, and llama.cpp linked in-process — no model server, no
interpreter in the runtime, no background service.

The model runs on the graphics card and can be unloaded without closing the
app, so the card is free for a game while the window stays open.

---

## Build

CUDA is not optional in practice, and a build without it fails silently: it
compiles, links, loads a model, and runs entirely on the CPU with no error
anywhere. Always configure with the flags.

```
cmake -S . -B build -DGGML_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=89
cmake --build build --config Release
```

`89` is Ada (RTX 4090). Blackwell (RTX PRO 5000, 5090) is `120`, Ampere `86`.

Delete `build\CMakeCache.txt` before reconfiguring **only** when switching
backend or CUDA architecture — stale toolchain detection survives otherwise.
It is not needed routinely; `-D` flags override cached values.

Output lands in `build\Release\`. The post-build step copies `web`, `config`,
`tools_runtime`, and `install_helm_tools.cmd` next to the executable.

### Prerequisites

| | |
|---|---|
| Visual Studio 2022+ | Desktop development with C++ workload |
| CUDA Toolkit | Install after Visual Studio so it detects the compiler |
| Git | llama.cpp is fetched at configure time |
| Disk | ~30 GB for build artifacts, before model files |

llama.cpp is pinned by tag in `CMakeLists.txt`. Updating is a deliberate act:
change the tag, rebuild, fix `engine/` if `llama.h` moved.

---

## External tools

Web search, image generation, speech, and document reading shell out to a
Python runtime that lives outside the repo. Without it those tools report a
missing runtime and everything else still works.

```
install_helm_tools.cmd
install_helm_tools.cmd "D:\AI Tools"    :: custom root
```

Run from Command Prompt, not PowerShell. Needs `git`, `curl.exe`, and the `py`
launcher on PATH. First run is 30–60 minutes and ~20 GB: a Python venv,
headless Chromium, ComfyUI with its own CUDA torch stack, an SDXL checkpoint,
whisper.cpp compiled with CUDA, a Piper voice, FFmpeg, ripgrep, yt-dlp.

The root must match **Tool root** in Settings. The detection chips there show
what actually installed — check them before assuming a tool is broken.

---

## Layout

Nine layers, one process. Each talks only to its neighbours, so any one can be
replaced without disturbing the rest.

```
src/
  main.cpp        Win32 entry, window, message pump
  ui/             bridge between the web view and the core
  engine/         llama.cpp wrapper, sampling, GBNF grammar
  agent/          loop, tool registry, prompt builders, job manager
  session/        conversations, long-term memory
  workspace/      RAG files, agent profiles, imported tool packs
  tools/          one file per tool
  common/         config, logging, paths, JSON
web/              interface — HTML/CSS/JS, no build step
config/app.json   shipped defaults, copied to the user directory on first run
tools_runtime/    Python adapter for the external tools
```

State lives in `%LOCALAPPDATA%\Helm`: `app.json`, `models.json`,
`runtime.json`, `memory.md`, `sessions\`, `workspace\`, `generated\`,
`helm.log`. Deleting that directory resets the app without touching the build.

---

## How a turn works

The model's output is constrained by a GBNF grammar to exactly one of two
byte-exact JSON envelopes:

```
{"type":"reply","thinking":"...","content":"..."}
{"type":"tool_call","thinking":"...","note":"...","name":"...","arguments":{...}}
```

Malformed calls are impossible rather than merely unlikely — the sampler
forbids any token that would produce one. `thinking` streams to a collapsible
pane and is never persisted; feeding reasoning back compounds across turns and
crowds out real history. `note` exists because a reply *ends the turn*: without
it, a model that wanted to narrate before acting had to stop in order to do it.

**GPT-OSS models are driven in native Harmony instead.** The format is detected
from the weights at load — watch for `prompt=harmony` in the log — and the
grammar is dropped, because constraining a model to an envelope it was not
trained on wastes the reasoning it does natively. Analysis, commentary, and
final channels route to the same thinking/note/reply panes. The model's own
analysis trace is preserved across a tool chain and discarded once a final
answer closes it.

### Tools

Two classes. **Synchronous** tools return a value and the turn continues in the
same breath. **Jobs** return an id immediately, report progress on their own
channel, and notify the loop on completion — a rip or a crawl occupies a slot
for many minutes while the window stays responsive.

Built in: `read_text_file`, `write_text_file`, `list_directory`, `run_process`,
`remember`, `recall_memory`, `forget`.

Via the Python runtime: `search_web`, `fetch_web_page`, `crawl_site`,
`extract_document`, `generate_image`, `speak_text`, `desktop_screenshot`,
`desktop_click`, `desktop_type`, `desktop_hotkey`.

Web research escalates on its own. A static fetch that comes back thin or
carries a "requires JavaScript" marker is retried through headless Chromium.
One browser is launched per tool call and shared, escalation is capped, and the
whole search sits under a time budget — otherwise three JS-rendered results
stack three full browser lifecycles end to end and the tool looks hung.

### Memory and compression

`memory.md` is one Markdown file injected into every system prompt, in both
modes. Writes are explicit — `/remember`, `/forget`, the `remember` tool, or
editing it in Settings. Nothing is captured implicitly: a model that decides
what is worth keeping records "ok" and its own unaccepted suggestions, and the
file rots within a week. It is budgeted, and over budget writes are refused
rather than silently truncated.

When history outgrows the context window, everything except the most recent
messages is folded into one model-written summary and persisted in its place.
Summaries chain. The old behaviour — dropping the oldest turns silently —
remains only as a fallback when compression is off or fails.

### Slash commands

Handled before anything reaches the model, so they work identically in Chat
mode where no tools are registered.

`/remember` `/forget` `/memory` `/tools` `/model` `/new` `/help`

---

## Known rough edges

- Some GPT-OSS conversions mark `<|end|>` as end-of-generation, which stops the
  turn after the analysis channel. Symptom is "did not return a valid Harmony
  final message"; the raw output is in `helm.log`.
- ComfyUI's first image of a session autostarts the server and loads ~7 GB into
  VRAM. Expect 60–90 seconds. Launching `START_COMFYUI.cmd` beforehand avoids
  it.
- The tool installer pulls a second full PyTorch CUDA stack for ComfyUI,
  independent of anything already on the machine. That is most of its footprint.

---

## Direction

The next substantial change splits the core out of the window: a headless
server holding sessions, memory, and workspace, with the desktop app as one
client among several and the same `web/` assets served to phones on the LAN.
The bridge already funnels all interface traffic through a single JSON channel,
which is the seam that makes it tractable.
