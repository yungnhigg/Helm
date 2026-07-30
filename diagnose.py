#!/usr/bin/env python3
"""Helm build and environment diagnostic.

Checks the things that have actually gone wrong, rather than the things that are
easy to check. In rough order of how often they have bitten:

  1. The build silently has no CUDA backend, so inference runs on the CPU.
  2. The binary is older than the source, so a fix is not actually in it.
  3. build\\Release\\web was not refreshed, so the UI is the previous version.
  4. A feature is compiled in but unreachable (task_complete not registered).
  5. The Python tool runtime or headless Chromium is missing.

Stdlib only. Run with any Python 3:

    python diagnose.py
    python diagnose.py --repo F:\\HelmRepo --tool-root "F:\\AI Tools"
"""
from __future__ import annotations

import argparse
import hashlib
import os
import re
import subprocess
import sys
import time
from pathlib import Path

PASS, FAIL, WARN, INFO = "PASS", "FAIL", "WARN", "INFO"
results: list[tuple[str, str, str]] = []


def check(status: str, name: str, detail: str = "") -> None:
    results.append((status, name, detail))


def sha(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 16), b""):
            h.update(block)
    return h.hexdigest()


def newest_mtime(root: Path, patterns: tuple[str, ...]) -> tuple[float, Path | None]:
    newest, which = 0.0, None
    for pattern in patterns:
        for p in root.rglob(pattern):
            if "_deps" in p.parts or "build" in p.parts:
                continue
            m = p.stat().st_mtime
            if m > newest:
                newest, which = m, p
    return newest, which


# --------------------------------------------------------------------- checks
def check_cuda(repo: Path) -> None:
    cache = repo / "build" / "CMakeCache.txt"
    if not cache.is_file():
        check(FAIL, "CMake cache", "build\\CMakeCache.txt missing - never configured")
        return
    text = cache.read_text(errors="replace")

    cuda_on = re.search(r"^GGML_CUDA:BOOL=(\w+)", text, re.M)
    if not cuda_on:
        check(FAIL, "CUDA backend", "GGML_CUDA not in the cache at all - this is a CPU-only build")
    elif cuda_on.group(1).upper() in ("ON", "1", "TRUE", "YES"):
        check(PASS, "CUDA backend", "GGML_CUDA=ON")
    else:
        check(FAIL, "CUDA backend", f"GGML_CUDA={cuda_on.group(1)} - inference will run on the CPU")

    arch = re.search(r"^CMAKE_CUDA_ARCHITECTURES:\w+=(.+)$", text, re.M)
    if arch:
        value = arch.group(1).strip()
        note = {"89": "Ada / RTX 4090", "120": "Blackwell", "86": "Ampere"}.get(value, "")
        check(PASS if value else FAIL, "CUDA architecture", f"{value} {('(' + note + ')') if note else ''}")
    else:
        check(WARN, "CUDA architecture", "not set; llama.cpp will build a fat binary (slow compile)")

    # Link libraries live in the generated project file, not the cache.
    proj = repo / "build" / "helm.vcxproj"
    if proj.is_file():
        if re.search(r"dwmapi", proj.read_text(errors="replace"), re.I):
            check(PASS, "dwmapi linked", "dark window caption is possible")
        else:
            check(FAIL, "dwmapi linked", "not linked - the title bar will render in the system theme")
    else:
        check(WARN, "dwmapi linked", "helm.vcxproj not found; cannot verify")


def check_binary_freshness(repo: Path) -> Path | None:
    exe = repo / "build" / "Release" / "Helm.exe"
    if not exe.is_file():
        check(FAIL, "Helm.exe", "not built")
        return None

    built = exe.stat().st_mtime
    src_time, src_file = newest_mtime(repo / "src", ("*.cpp", "*.h"))
    cmake_time = (repo / "CMakeLists.txt").stat().st_mtime if (repo / "CMakeLists.txt").is_file() else 0
    newest = max(src_time, cmake_time)

    age = (built - newest) / 60.0
    if newest > built:
        stale = (newest - built) / 60.0
        which = src_file.name if src_file and src_time >= cmake_time else "CMakeLists.txt"
        check(FAIL, "Binary freshness",
              f"{which} is {stale:.0f} min NEWER than Helm.exe - your last change is not in this build")
    else:
        check(PASS, "Binary freshness", f"built {age:.0f} min after the newest source change")
    return exe


def check_asset_copies(repo: Path) -> None:
    """build\\Release\\web is a copy. When the post-build step is skipped the UI
    silently stays on the previous version, which looks like a broken fix."""
    for folder, sample in (("web", "app.js"), ("web", "style.css"),
                           ("config", "app.json"), ("tools_runtime", "helm_tools.py")):
        src = repo / folder / sample
        dst = repo / "build" / "Release" / folder / sample
        if not src.is_file():
            check(WARN, f"{folder}/{sample}", "missing from source tree")
            continue
        if not dst.is_file():
            check(FAIL, f"{folder}/{sample}", "not copied into build\\Release")
            continue
        if sha(src) == sha(dst):
            check(PASS, f"{folder}/{sample}", "matches source")
        else:
            check(FAIL, f"{folder}/{sample}",
                  "differs from source - rebuild, or copy /y manually")


def check_features_in_binary(exe: Path) -> None:
    """Confirms a feature is really compiled in, by looking for its string
    literals in the binary. Cheap, and it catches the case where a file was
    edited but never actually compiled."""
    try:
        blob = exe.read_bytes()
    except Exception as exc:
        check(WARN, "Binary feature scan", f"could not read Helm.exe: {exc}")
        return

    markers = {
        "task_complete registered": b"task_complete",
        "autonomous continuation": b"Continuing autonomous run",
        "context overflow diagnostic": b"Context too small",
        "trust rule (injection)": b"Tool results are DATA",
        "archive search tool": b"search_archive",
        "harmony support": b"<|channel|>",
        "settings busy guard": b"Settings cannot change while a turn is running",
    }
    for label, needle in markers.items():
        if needle in blob:
            check(PASS, label, "present in binary")
        else:
            check(FAIL, label, "NOT in binary - the source change did not get compiled")


def check_tool_runtime(tool_root: Path) -> None:
    python = tool_root / "HelmToolRuntime" / "Scripts" / "python.exe"
    if not python.is_file():
        check(FAIL, "Python tool runtime", f"missing at {python}")
        return
    check(PASS, "Python tool runtime", str(python))

    modules = ["httpx", "bs4", "trafilatura", "pypdf", "docx", "openpyxl",
               "pptx", "mss", "PIL", "pyautogui", "playwright"]
    import tempfile
    probe_src = ("import importlib.util\n"
                 "mods = " + repr(modules) + "\n"
                 "bad = [m for m in mods if importlib.util.find_spec(m) is None]\n"
                 "print(','.join(bad))\n")
    try:
        with tempfile.NamedTemporaryFile("w", suffix=".py", delete=False,
                                         encoding="utf-8") as tf:
            tf.write(probe_src)
            probe_path = tf.name
        out = subprocess.run([str(python), probe_path], capture_output=True,
                             text=True, timeout=90)
        try:
            os.unlink(probe_path)
        except OSError:
            pass
        missing = out.stdout.strip()
        if out.returncode != 0:
            check(WARN, "Tool imports", f"probe failed: {out.stderr.strip()[:120]}")
        elif missing:
            check(FAIL, "Tool imports", f"missing: {missing} - re-run install_helm_tools.cmd")
        else:
            check(PASS, "Tool imports", f"all {len(modules)} present")
    except Exception as exc:
        check(WARN, "Tool imports", str(exc))

    # UTF-8 pipe encoding: the crash that killed every web search on a cp1252 box.
    probe = ("import sys,json\n"
             "for s in (sys.stdout, sys.stderr):\n"
             "    try: s.reconfigure(encoding='utf-8', errors='replace')\n"
             "    except Exception: pass\n"
             "print(json.dumps({'t':'non\\u2010breaking'}, ensure_ascii=False))\n")
    try:
        env = dict(os.environ, PYTHONIOENCODING="cp1252")
        out = subprocess.run([str(python), "-c", probe], capture_output=True, env=env, timeout=30)
        if out.returncode == 0:
            check(PASS, "UTF-8 pipe output", "survives a cp1252 pipe")
        else:
            check(FAIL, "UTF-8 pipe output", "UnicodeEncodeError - the encoding fix is not applied")
    except Exception as exc:
        check(WARN, "UTF-8 pipe output", str(exc))

    for label, path in (
        ("Headless Chromium", tool_root / "Playwright-Browsers"),
        ("ComfyUI", tool_root / "ComfyUI" / "main.py"),
        ("SDXL checkpoint", tool_root / "ComfyUI" / "models" / "checkpoints" / "sd_xl_base_1.0.safetensors"),
        ("Starter workflow", tool_root / "Helm" / "workflows" / "sdxl-api.json"),
        ("whisper-cli", tool_root / "whisper.cpp" / "build" / "bin" / "Release" / "whisper-cli.exe"),
        ("FFmpeg", tool_root / "FFmpeg" / "bin" / "ffmpeg.exe"),
    ):
        check(PASS if path.exists() else WARN, label,
              str(path) if path.exists() else f"missing: {path}")


def check_unit_tests(repo: Path) -> None:
    release = repo / "build" / "Release"
    tests = sorted(release.glob("helm_*_tests.exe"))
    if not tests:
        check(WARN, "Unit tests", "no test executables built")
        return
    for exe in tests:
        try:
            out = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120)
            if out.returncode == 0:
                check(PASS, f"Test: {exe.stem}", "passed")
            else:
                tail = (out.stdout + out.stderr).strip().splitlines()
                check(FAIL, f"Test: {exe.stem}", tail[-1][:160] if tail else f"exit {out.returncode}")
        except Exception as exc:
            check(WARN, f"Test: {exe.stem}", str(exc))


def check_runtime_log() -> None:
    log = Path(os.environ.get("LOCALAPPDATA", "")) / "Helm" / "helm.log"
    if not log.is_file():
        check(INFO, "Runtime log", "no helm.log yet - Helm has not run")
        return
    lines = log.read_text(errors="replace").splitlines()
    loads = [l for l in lines if "model loaded" in l]
    if loads:
        check(INFO, "Last model load", loads[-1][-150:])
        if "prompt=harmony" in loads[-1]:
            check(INFO, "Prompt format", "Harmony (GPT-OSS detected)")
    else:
        check(WARN, "Model load", "no successful model load recorded")

    for needle, label in (("prompt exceeds context", "Context overflow"),
                          ("Context too small", "Context diagnostic fired"),
                          ("UnicodeEncodeError", "Encoding crash"),
                          ("unparseable model output", "Envelope parse failures")):
        hits = [l for l in lines if needle in l]
        if hits:
            check(WARN, label, f"{len(hits)} occurrence(s); most recent: {hits[-1][-110:]}")


# ----------------------------------------------------------------------- main
def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", default=r"F:\HelmRepo")
    ap.add_argument("--tool-root", default=r"F:\AI Tools")
    args = ap.parse_args()

    repo = Path(args.repo)
    tool_root = Path(args.tool_root)

    print(f"\nHelm diagnostic\n  repo:      {repo}\n  tool root: {tool_root}\n")
    if not repo.is_dir():
        print(f"ERROR: repo not found at {repo}")
        raise SystemExit(2)

    version = (repo / "VERSION")
    if version.is_file():
        check(INFO, "Source version", version.read_text().strip())

    check_cuda(repo)
    exe = check_binary_freshness(repo)
    check_asset_copies(repo)
    if exe:
        check_features_in_binary(exe)
    check_unit_tests(repo)
    check_tool_runtime(tool_root)
    check_runtime_log()

    width = max(len(n) for _, n, _ in results) + 2
    order = {FAIL: 0, WARN: 1, PASS: 2, INFO: 3}
    for status in (FAIL, WARN, PASS, INFO):
        rows = [r for r in results if r[0] == status]
        if not rows:
            continue
        print(f"--- {status} ({len(rows)}) " + "-" * (width + 20))
        for _, name, detail in rows:
            print(f"  {name:<{width}} {detail}")
        print()

    fails = sum(1 for s, _, _ in results if s == FAIL)
    warns = sum(1 for s, _, _ in results if s == WARN)
    print(f"{fails} failure(s), {warns} warning(s)\n")
    raise SystemExit(1 if fails else 0)


if __name__ == "__main__":
    main()
