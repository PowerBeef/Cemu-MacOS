#!/usr/bin/env bash
# Launch the project-local cemu-re-mcp GDB MCP server (TesseraEmu).
# Requires: tools/cemu-re-mcp setup (see tools/cemu-re-mcp/README.md).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
VENV_PY="$ROOT/cemu-re-mcp/.venv/bin/python"
if [[ ! -x "$VENV_PY" ]]; then
  echo "cemu-re-mcp venv missing. Run: tools/cemu-re-mcp/setup.sh" >&2
  exit 1
fi
export PYTHONPATH="${ROOT}/cemu-re-mcp/src${PYTHONPATH:+:$PYTHONPATH}"
exec "$VENV_PY" -m cemu_re_mcp.server "$@"
