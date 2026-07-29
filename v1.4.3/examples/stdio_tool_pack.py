"""Minimal Helm stdio-json tool adapter example."""
from __future__ import annotations

import json
import sys


def main() -> int:
    request = json.loads(sys.stdin.readline())
    tool = request.get("tool")
    arguments = request.get("arguments", {})
    if tool == "word_count":
        count = len(str(arguments.get("text", "")).split())
        print(json.dumps({"result": f"{count} words"}))
        return 0
    print(json.dumps({"result": f"Unknown tool: {tool}"}))
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
