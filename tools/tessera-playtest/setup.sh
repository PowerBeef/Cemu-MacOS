#!/usr/bin/env bash
# Bootstrap tessera-playtest MCP venv (mcp 1.6 for FastMCP compatibility).
set -euo pipefail
cd "$(dirname "$0")"
command -v uv >/dev/null || { echo "uv is required (https://github.com/astral-sh/uv)" >&2; exit 1; }
uv venv .venv
uv pip install -e . 'mcp==1.6.0'
.venv/bin/python -c "from tessera_playtest.server import playtest_health; print(playtest_health())"
echo "tessera-playtest OK"
