#!/usr/bin/env python3
"""External open-source tool adapter used by Helm 1.7.0.

Each subcommand writes a compact UTF-8 result to stdout and diagnostics to
stderr. The C++ host owns timeouts/cancellation and never executes arbitrary
shell strings.
"""
from __future__ import annotations

import argparse
import json
import os
import random
import shutil
import subprocess
import sys
import time
import urllib.parse
from pathlib import Path


def fail(message: str, code: int = 2) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(code)


def _configure_playwright() -> None:
    if os.environ.get("PLAYWRIGHT_BROWSERS_PATH"):
        return
    try:
        root = Path(sys.executable).resolve().parents[2]
    except IndexError:
        return
    candidate = root / "Playwright-Browsers"
    if candidate.exists():
        os.environ["PLAYWRIGHT_BROWSERS_PATH"] = str(candidate)


def _compact_text(text: str, max_chars: int) -> str:
    return " ".join((text or "").replace("\x00", " ").split())[:max_chars]


def _validate_web_url(url: str) -> str:
    parsed = urllib.parse.urlparse(url.strip())
    if parsed.scheme not in {"http", "https"} or not parsed.netloc:
        raise ValueError("URL must be an absolute http or https address")
    return parsed.geturl()


def _extract_html_text(html: str, max_chars: int) -> str:
    text = ""
    try:
        import trafilatura
        text = trafilatura.extract(
            html,
            include_comments=False,
            include_tables=True,
            include_links=False,
            favor_recall=True,
        ) or ""
    except Exception:
        text = ""
    if len(text.strip()) < 200:
        try:
            from bs4 import BeautifulSoup
            soup = BeautifulSoup(html, "html.parser")
            for node in soup(["script", "style", "noscript", "svg", "template"]):
                node.decompose()
            text = soup.get_text(" ", strip=True)
        except Exception:
            pass
    return _compact_text(text, max_chars)


class _BrowserPool:
    """One Chromium launch shared by every fallback in a single tool call.

    The previous shape launched a whole browser per URL. Three fallbacks in one
    search meant three launches, three teardowns, and three full timeout budgets
    stacked end to end — which is what made a search look like it had hung.
    """

    def __init__(self) -> None:
        self._pw = None
        self._browser = None
        self.error = ""

    def available(self) -> bool:
        return self.error == ""

    def _ensure(self):
        if self._browser is not None:
            return self._browser
        _configure_playwright()
        try:
            from playwright.sync_api import sync_playwright
        except Exception as exc:
            self.error = f"Playwright is not installed: {exc}"
            return None
        try:
            self._pw = sync_playwright().start()
            self._browser = self._pw.chromium.launch(headless=True)
        except Exception as exc:
            # Almost always the Chromium binaries were never downloaded.
            self.error = (f"headless Chromium could not start ({exc}). "
                          "Run install_helm_tools.cmd to download it.")
            self.close()
            return None
        return self._browser

    def fetch(self, url: str, max_chars: int, budget_seconds: float = 25.0) -> dict:
        browser = self._ensure()
        if browser is None:
            return {"url": url, "text": "", "method": "browser", "error": self.error}
        page = None
        try:
            page = browser.new_page(
                viewport={"width": 1440, "height": 1000},
                user_agent=("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                            "HelmLocalAI/1.7.0 Chrome/126 Safari/537.36"),
            )
            # Budget split: most of it on first paint, a short tail for late XHR.
            page.goto(url, wait_until="domcontentloaded", timeout=int(budget_seconds * 1000 * 0.7))
            try:
                page.wait_for_load_state("networkidle", timeout=int(budget_seconds * 1000 * 0.2))
            except Exception:
                pass
            page.wait_for_timeout(250)
            title = page.title()
            final_url = page.url
            try:
                body_text = page.locator("body").inner_text(timeout=4_000)
            except Exception:
                body_text = _extract_html_text(page.content(), max_chars)
            return {
                "url": final_url or url,
                "title": _compact_text(title, 500),
                "text": _compact_text(body_text, max_chars),
                "method": "browser",
                "error": "",
            }
        except Exception as exc:
            return {"url": url, "text": "", "method": "browser", "error": str(exc)}
        finally:
            if page is not None:
                try:
                    page.close()
                except Exception:
                    pass

    def close(self) -> None:
        try:
            if self._browser is not None:
                self._browser.close()
        except Exception:
            pass
        try:
            if self._pw is not None:
                self._pw.stop()
        except Exception:
            pass
        self._browser = None
        self._pw = None


def _needs_browser(text: str) -> bool:
    """True when a static fetch clearly did not get the real content."""
    lowered = text.lower()
    markers = ("enable javascript", "javascript is required", "checking your browser",
               "please turn on javascript", "requires javascript")
    return len(text) < 500 or any(marker in lowered for marker in markers)


def _static_fetch(url: str, max_chars: int, client=None, timeout: float = 12.0) -> dict:
    try:
        url = _validate_web_url(url)
    except ValueError as exc:
        return {"url": url, "text": "", "method": "static", "error": str(exc)}
    try:
        import httpx
    except Exception as exc:
        return {"url": url, "text": "", "method": "static", "error": f"httpx unavailable: {exc}"}
    headers = {
        "User-Agent": ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                       "HelmLocalAI/1.7.0 Chrome/126 Safari/537.36"),
        "Accept": "text/html,application/xhtml+xml,text/plain;q=0.9,*/*;q=0.6",
        "Accept-Language": "en-US,en;q=0.8",
    }
    own = client is None
    http = client or httpx.Client(headers=headers, follow_redirects=True, timeout=timeout)
    try:
        response = http.get(url, timeout=timeout)
        response.raise_for_status()
        content_type = response.headers.get("content-type", "").lower()
        text = ""
        if any(kind in content_type for kind in ("html", "xml", "text")) or not content_type:
            text = _extract_html_text(response.text, max_chars)
        title = ""
        if response.text and ("html" in content_type or "<html" in response.text[:1000].lower()):
            try:
                from bs4 import BeautifulSoup
                soup = BeautifulSoup(response.text, "html.parser")
                title = _compact_text(soup.title.get_text(" ", strip=True), 500) if soup.title else ""
            except Exception:
                title = ""
        return {"url": str(response.url), "title": title, "text": text,
                "method": "static", "error": ""}
    except Exception as exc:
        return {"url": url, "text": "", "method": "static", "error": str(exc)}
    finally:
        if own:
            http.close()


def fetch_page(url: str, max_chars: int = 12000, client=None, pool=None,
               browser_budget: float = 25.0) -> dict:
    """Static fetch, escalating to a real browser only when the page needs one."""
    result = _static_fetch(url, max_chars, client)
    if result.get("text") and not _needs_browser(result["text"]):
        return result

    own_pool = pool is None
    pool = pool or _BrowserPool()
    try:
        rendered = pool.fetch(url, max_chars, browser_budget)
    finally:
        if own_pool:
            pool.close()

    if rendered.get("text"):
        return rendered
    # Both paths failed: keep whatever static managed, and report both reasons so
    # a missing Chromium is visible instead of looking like an empty page.
    combined = "; ".join(part for part in (result.get("error", ""), rendered.get("error", "")) if part)
    result["error"] = combined or "no readable text found"
    result["browser_attempted"] = True
    return result


def web_fetch(args: argparse.Namespace) -> None:
    print(json.dumps(fetch_page(args.url, max(1000, min(args.max_chars, 100000))),
                     ensure_ascii=False))


# How many search results get opened, and how many of those may escalate to a
# browser. Both are deliberately small: every browser fallback is seconds the
# user spends watching a progress bar.
_AUTO_OPEN = 3
_MAX_BROWSER_FALLBACKS = 2
_SEARCH_DEADLINE_SECONDS = 75.0


def github_search(args: argparse.Namespace) -> None:
    """Search GitHub repositories through the REST API.

    Returns fully-formed records: real url, stars, license, pushed_at, language,
    archived flag, open issue count, and the description. This exists because a
    model told to "find repos and record their details" will otherwise invent
    plausible-looking github.com/<owner>/<name> URLs from training memory and
    fetch 404s. Here there is nothing to invent - the API is the source.
    """
    try:
        import httpx
    except Exception as exc:
        fail(f"github search dependency missing: {exc}")

    per_page = max(1, min(args.max_results, 50))
    sort = args.sort if args.sort in ("stars", "updated", "forks", "help-wanted-issues") else "best-match"
    params = {"q": args.query, "per_page": per_page}
    if sort != "best-match":
        params["sort"] = sort
    params["order"] = "desc"

    headers = {
        "Accept": "application/vnd.github+json",
        "X-GitHub-Api-Version": "2022-11-28",
        "User-Agent": "HelmLocalAI/1.6",
    }
    # An optional token lifts the rate limit from 10/min to 30/min. Read-only.
    token = os.environ.get("GITHUB_TOKEN", "").strip()
    if token:
        headers["Authorization"] = f"Bearer {token}"

    try:
        with httpx.Client(headers=headers, follow_redirects=True, timeout=25) as client:
            resp = client.get("https://api.github.com/search/repositories", params=params)
            if resp.status_code == 403 and "rate limit" in resp.text.lower():
                fail("github rate limit reached (unauthenticated is 10 searches/min). "
                     "Wait a minute, or set GITHUB_TOKEN. This is not a failure of the query.")
            resp.raise_for_status()
            payload = resp.json()
    except SystemExit:
        raise
    except Exception as exc:
        fail(f"github search failed: {exc}")

    repos = []
    for item in payload.get("items", []):
        lic = item.get("license") or {}
        repos.append({
            "name": item.get("full_name", ""),
            "url": item.get("html_url", ""),
            "description": _compact_text(item.get("description") or "", 300),
            "stars": item.get("stargazers_count", 0),
            "language": item.get("language") or "unknown",
            "license": lic.get("spdx_id") or lic.get("name") or "none",
            "pushed_at": (item.get("pushed_at") or "")[:10],
            "open_issues": item.get("open_issues_count", 0),
            "archived": bool(item.get("archived", False)),
            "is_fork": bool(item.get("fork", False)),
            "topics": item.get("topics", [])[:8],
        })

    print(json.dumps({
        "query": args.query,
        "total_found": payload.get("total_count", 0),
        "returned": len(repos),
        "repositories": repos,
        "note": ("These are verified GitHub API results. Use the url and other fields exactly as "
                 "given - do NOT construct or guess repository URLs. Every field here is authoritative; "
                 "only fetch a page if you need README detail the description does not cover."),
    }, ensure_ascii=False))


def web_search(args: argparse.Namespace) -> None:
    try:
        import httpx
        from bs4 import BeautifulSoup
    except Exception as exc:
        fail(f"web search dependencies missing: {exc}")

    started = time.time()
    headers = {
        "User-Agent": ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                       "HelmLocalAI/1.7.0 Chrome/126 Safari/537.36"),
        "Accept-Language": "en-US,en;q=0.8",
    }
    url = "https://html.duckduckgo.com/html/?" + urllib.parse.urlencode({"q": args.query})
    requested = max(1, min(args.max_results, 12))
    results: list[dict] = []
    pool = _BrowserPool()

    try:
        with httpx.Client(headers=headers, follow_redirects=True, timeout=15) as client:
            response = client.get(url)
            response.raise_for_status()
            soup = BeautifulSoup(response.text, "html.parser")
            for row in soup.select(".result"):
                link = row.select_one("a.result__a")
                if not link:
                    continue
                href = link.get("href", "")
                parsed = urllib.parse.urlparse(href)
                if parsed.netloc.endswith("duckduckgo.com"):
                    href = urllib.parse.parse_qs(parsed.query).get("uddg", [href])[0]
                if not href.startswith(("http://", "https://")):
                    continue
                snippet = row.select_one(".result__snippet")
                results.append({
                    "title": " ".join(link.get_text(" ", strip=True).split()),
                    "url": href,
                    "snippet": " ".join(snippet.get_text(" ", strip=True).split()) if snippet else "",
                })
                if len(results) >= max(requested, _AUTO_OPEN):
                    break

            targets = results[:_AUTO_OPEN]

            # Static fetches run concurrently. They are almost all network wait,
            # so three at once costs about what one used to.
            from concurrent.futures import ThreadPoolExecutor
            if targets:
                with ThreadPoolExecutor(max_workers=len(targets)) as pool_exec:
                    statics = list(pool_exec.map(
                        lambda r: _static_fetch(str(r["url"]), 9000, timeout=12.0), targets))
            else:
                statics = []

            for result, fetched in zip(targets, statics):
                result["page_text"] = fetched.get("text", "")
                result["fetch_method"] = fetched.get("method", "")
                if fetched.get("error"):
                    result["fetch_error"] = fetched["error"]
                if fetched.get("title") and not result.get("title"):
                    result["title"] = fetched["title"]
                if fetched.get("url"):
                    result["final_url"] = fetched["url"]

            # Only pages that actually need JavaScript escalate, capped, and only
            # while there is time left in the budget.
            escalations = 0
            for result in targets:
                if escalations >= _MAX_BROWSER_FALLBACKS:
                    break
                remaining = _SEARCH_DEADLINE_SECONDS - (time.time() - started)
                if remaining < 12:
                    result.setdefault("fetch_note", "skipped browser rendering: time budget spent")
                    continue
                if not _needs_browser(str(result.get("page_text", ""))):
                    continue
                escalations += 1
                rendered = pool.fetch(str(result["url"]), 9000, min(25.0, remaining - 4))
                if rendered.get("text"):
                    result["page_text"] = rendered["text"]
                    result["fetch_method"] = "browser"
                    result.pop("fetch_error", None)
                    if rendered.get("url"):
                        result["final_url"] = rendered["url"]
                elif rendered.get("error"):
                    result["fetch_error"] = rendered["error"]
    finally:
        pool.close()

    usable = sum(1 for r in results[:_AUTO_OPEN] if len(str(r.get("page_text", ""))) >= 300)
    payload = {
        "query": args.query,
        "requested_max_results": requested,
        "results": results[:requested] if requested >= len(results[:_AUTO_OPEN]) else results,
        "auto_fetched_pages": usable,
        "elapsed_seconds": round(time.time() - started, 1),
        "javascript_rendering": "available" if pool.available() else "unavailable",
        "research_note": (
            "Helm already opened the top results with static extraction and escalated to headless "
            "Chromium where the page needed JavaScript. Use page_text directly. If evidence is still "
            "incomplete, call fetch_web_page or search_web again with a refined query in the same "
            "turn; do not ask the user to approve read-only research."
        ),
    }
    if not pool.available() and pool.error:
        payload["javascript_rendering_error"] = pool.error
    print(json.dumps(payload, ensure_ascii=False))


def extract_document(args: argparse.Namespace) -> None:
    path = Path(args.path)
    if not path.is_file():
        fail("document does not exist")
    ext = path.suffix.lower()
    chunks: list[str] = []
    try:
        if ext == ".pdf":
            from pypdf import PdfReader
            for index, page in enumerate(PdfReader(str(path)).pages, 1):
                text = page.extract_text() or ""
                if text.strip():
                    chunks.append(f"\n--- Page {index} ---\n{text}")
        elif ext == ".docx":
            from docx import Document
            doc = Document(str(path))
            chunks.extend(p.text for p in doc.paragraphs if p.text.strip())
            for table in doc.tables:
                for row in table.rows:
                    chunks.append("\t".join(cell.text for cell in row.cells))
        elif ext == ".xlsx":
            from openpyxl import load_workbook
            wb = load_workbook(str(path), read_only=True, data_only=True)
            for ws in wb.worksheets:
                chunks.append(f"\n--- Sheet: {ws.title} ---")
                for row in ws.iter_rows(values_only=True):
                    values = ["" if value is None else str(value) for value in row]
                    if any(values):
                        chunks.append("\t".join(values))
        elif ext == ".pptx":
            from pptx import Presentation
            prs = Presentation(str(path))
            for index, slide in enumerate(prs.slides, 1):
                chunks.append(f"\n--- Slide {index} ---")
                for shape in slide.shapes:
                    text = getattr(shape, "text", "")
                    if text and text.strip():
                        chunks.append(text)
        else:
            fail(f"unsupported document type: {ext}")
    except SystemExit:
        raise
    except Exception as exc:
        fail(f"document extraction failed: {exc}")
    text = "\n".join(chunks)
    sys.stdout.write(text[: args.max_chars])


def desktop_screenshot(args: argparse.Namespace) -> None:
    try:
        import mss
        from PIL import Image
    except Exception as exc:
        fail(f"screenshot dependencies missing: {exc}")
    target = Path(args.output)
    target.parent.mkdir(parents=True, exist_ok=True)
    with mss.mss() as capture:
        monitor = capture.monitors[args.monitor if 0 <= args.monitor < len(capture.monitors) else 0]
        shot = capture.grab(monitor)
        Image.frombytes("RGB", shot.size, shot.rgb).save(target)
    print(str(target))


def desktop_click(args: argparse.Namespace) -> None:
    try:
        import pyautogui
    except Exception as exc:
        fail(f"desktop automation dependency missing: {exc}")
    pyautogui.click(args.x, args.y, clicks=args.clicks, interval=max(0, args.interval_ms) / 1000, button=args.button)
    print(f"clicked {args.button} at ({args.x}, {args.y}) x{args.clicks}")


def desktop_type(args: argparse.Namespace) -> None:
    try:
        import pyautogui
    except Exception as exc:
        fail(f"desktop automation dependency missing: {exc}")
    pyautogui.write(args.text, interval=max(0, args.interval_ms) / 1000)
    print(f"typed {len(args.text)} characters")


def desktop_hotkey(args: argparse.Namespace) -> None:
    try:
        import pyautogui
    except Exception as exc:
        fail(f"desktop automation dependency missing: {exc}")
    keys = [key.strip().lower() for key in args.keys.split(",") if key.strip()]
    if not keys:
        fail("no keys supplied")
    pyautogui.hotkey(*keys)
    print("pressed " + "+".join(keys))


def _replace_workflow_values(workflow: dict, prompt: str, negative: str, width: int, height: int, prefix: str) -> None:
    text_nodes = []
    for node in workflow.values():
        if not isinstance(node, dict):
            continue
        class_type = str(node.get("class_type", ""))
        inputs = node.setdefault("inputs", {})
        if class_type == "CLIPTextEncode" and "text" in inputs:
            text_nodes.append(node)
        elif class_type in {"EmptyLatentImage", "EmptySD3LatentImage"}:
            inputs["width"] = width
            inputs["height"] = height
        elif class_type == "KSampler":
            inputs["seed"] = random.SystemRandom().randint(0, 2**63 - 1)
        elif class_type == "SaveImage":
            inputs["filename_prefix"] = prefix
    if text_nodes:
        text_nodes[0]["inputs"]["text"] = prompt
    if len(text_nodes) > 1:
        text_nodes[1]["inputs"]["text"] = negative


def _ensure_comfyui(base: str, start_command: str, wait_seconds: int = 180) -> None:
    import httpx

    def ready() -> bool:
        try:
            response = httpx.get(base + "/system_stats", timeout=3)
            return response.status_code == 200
        except Exception:
            return False

    if ready():
        return
    start_path = Path(start_command) if start_command else None
    if not start_path or not start_path.is_file():
        fail("ComfyUI is not running and START_COMFYUI.cmd was not found; run Install / repair tools")

    # Launch ComfyUI's own interpreter directly rather than going through
    # cmd.exe on the .cmd file. Routing through cmd gives the server a console
    # window, and START_COMFYUI.cmd ends in `pause`, so that window sticks
    # around after the server stops. CREATE_NO_WINDOW keeps it headless.
    root = start_path.parent
    venv_python = root / "ComfyUI" / ".venv" / "Scripts" / "python.exe"
    comfy_dir = root / "ComfyUI"
    if venv_python.is_file() and (comfy_dir / "main.py").is_file():
        command = [str(venv_python), "main.py", "--listen", "127.0.0.1", "--port", "8188"]
        workdir = str(comfy_dir)
    else:
        command = ["cmd.exe", "/d", "/c", str(start_path)]
        workdir = str(start_path.parent)

    try:
        flags = 0
        if os.name == "nt":
            flags = (getattr(subprocess, "CREATE_NO_WINDOW", 0)
                     | getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0))
        subprocess.Popen(
            command,
            cwd=workdir,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            creationflags=flags,
        )
    except Exception as exc:
        fail(f"could not start ComfyUI: {exc}")

    deadline = time.time() + max(10, wait_seconds)
    while time.time() < deadline:
        if ready():
            return
        time.sleep(2)
    fail("ComfyUI did not become ready after it was started")


def comfy_generate(args: argparse.Namespace) -> None:
    try:
        import httpx
    except Exception as exc:
        fail(f"ComfyUI dependency missing: {exc}")
    workflow_path = Path(args.workflow)
    if not workflow_path.is_file():
        fail("ComfyUI API workflow JSON is not configured")
    try:
        workflow = json.loads(workflow_path.read_text(encoding="utf-8"))
    except Exception as exc:
        fail(f"invalid ComfyUI workflow JSON: {exc}")
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    prefix = f"helm_{int(time.time())}"
    _replace_workflow_values(workflow, args.prompt, args.negative_prompt, args.width, args.height, prefix)

    base = args.url.rstrip("/")
    _ensure_comfyui(base, args.start_command)
    with httpx.Client(timeout=30) as client:
        response = client.post(base + "/prompt", json={"prompt": workflow, "client_id": "helm-local"})
        response.raise_for_status()
        prompt_id = response.json().get("prompt_id")
        if not prompt_id:
            fail("ComfyUI did not return a prompt id")
        deadline = time.time() + args.timeout_seconds
        history = None
        while time.time() < deadline:
            history_response = client.get(base + f"/history/{prompt_id}")
            if history_response.status_code == 200:
                history = history_response.json().get(prompt_id)
                if history and history.get("outputs"):
                    break
            time.sleep(1)
        if not history or not history.get("outputs"):
            fail("ComfyUI generation timed out")

        saved: list[str] = []
        for output in history["outputs"].values():
            for image in output.get("images", []):
                params = {"filename": image["filename"], "subfolder": image.get("subfolder", ""), "type": image.get("type", "output")}
                data = client.get(base + "/view", params=params)
                data.raise_for_status()
                target = output_dir / Path(image["filename"]).name
                target.write_bytes(data.content)
                saved.append(str(target))
        if not saved:
            fail("ComfyUI completed without an image output")
        print(json.dumps({"prompt_id": prompt_id, "files": saved}, ensure_ascii=False))


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="helm_tools")
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("web-search")
    p.add_argument("--query", required=True)
    p.add_argument("--max-results", type=int, default=8)
    p.set_defaults(func=web_search)

    p = sub.add_parser("github-search")
    p.add_argument("--query", required=True)
    p.add_argument("--max-results", type=int, default=15)
    p.add_argument("--sort", default="best-match")
    p.set_defaults(func=github_search)

    p = sub.add_parser("web-fetch")
    p.add_argument("--url", required=True)
    p.add_argument("--max-chars", type=int, default=12000)
    p.set_defaults(func=web_fetch)

    p = sub.add_parser("extract-document")
    p.add_argument("--path", required=True)
    p.add_argument("--max-chars", type=int, default=200000)
    p.set_defaults(func=extract_document)

    p = sub.add_parser("desktop-screenshot")
    p.add_argument("--output", required=True)
    p.add_argument("--monitor", type=int, default=0)
    p.set_defaults(func=desktop_screenshot)

    p = sub.add_parser("desktop-click")
    p.add_argument("--x", type=int, required=True)
    p.add_argument("--y", type=int, required=True)
    p.add_argument("--button", choices=["left", "right", "middle"], default="left")
    p.add_argument("--clicks", type=int, default=1)
    p.add_argument("--interval-ms", type=int, default=100)
    p.set_defaults(func=desktop_click)

    p = sub.add_parser("desktop-type")
    p.add_argument("--text", required=True)
    p.add_argument("--interval-ms", type=int, default=10)
    p.set_defaults(func=desktop_type)

    p = sub.add_parser("desktop-hotkey")
    p.add_argument("--keys", required=True)
    p.set_defaults(func=desktop_hotkey)

    p = sub.add_parser("comfy-generate")
    p.add_argument("--url", required=True)
    p.add_argument("--workflow", required=True)
    p.add_argument("--prompt", required=True)
    p.add_argument("--negative-prompt", default="")
    p.add_argument("--width", type=int, default=1024)
    p.add_argument("--height", type=int, default=1024)
    p.add_argument("--output-dir", required=True)
    p.add_argument("--timeout-seconds", type=int, default=900)
    p.add_argument("--start-command", default="")
    p.set_defaults(func=comfy_generate)
    return parser


def _force_utf8_streams() -> None:
    """Make stdout/stderr UTF-8 regardless of the Windows code page.

    The C++ host reads this script's stdout through a pipe. Python sees a pipe,
    not a console, so it picks the locale encoding - cp1252 on a US Windows box -
    and any non-ASCII character in fetched web text kills the process:

        UnicodeEncodeError: 'charmap' codec can't encode character '\u2010'

    U+2010 is an ordinary typographic hyphen. Wikipedia and half the web are full
    of them, so this is not an edge case; it is every other page. nlohmann::json
    on the receiving side expects UTF-8, which is what this produces.
    """
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass


def main() -> None:
    _force_utf8_streams()
    args = build_parser().parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
