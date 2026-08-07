#!/usr/bin/env bash
# Bootstrap cemu-re-mcp for TesseraEmu (macOS-friendly: GDB tools only).
set -euo pipefail
cd "$(dirname "$0")"
if [[ ! -d .git ]]; then
  echo "Expected a git checkout of Matt-Wood-23/cemu-re-mcp in $(pwd)" >&2
  exit 1
fi
# Ensure local patches (mcp pin + optional pymem) are present
if ! grep -q 'mcp>=1.2,<1.7' pyproject.toml 2>/dev/null; then
  echo "warning: pyproject.toml mcp pin missing; install may pull mcp 2.x and break" >&2
fi
command -v uv >/dev/null || { echo "uv is required (https://github.com/astral-sh/uv)" >&2; exit 1; }
uv venv .venv
uv pip install -e . 'mcp==1.6.0'
.venv/bin/python -c "import cemu_re_mcp.server as s; assert s.health_check_tool()['status']=='ok'; print('cemu-re-mcp OK (pymem=', s._PYMEM_OK, ')')"
