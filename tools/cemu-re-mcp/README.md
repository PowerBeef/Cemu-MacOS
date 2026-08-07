# cemu-re-mcp (TesseraEmu packaging)

Upstream: [Matt-Wood-23/cemu-re-mcp](https://github.com/Matt-Wood-23/cemu-re-mcp) — MCP tools over
Cemu/TesseraEmu's GDB stub for live guest PowerPC inspection.

## Why a local checkout

1. Upstream pins `mcp>=1.0.0`, which today resolves to **mcp 2.x** without `FastMCP` and breaks import.
   We pin **`mcp==1.6.0`**.
2. The **pymem** scanner is Windows-only (`Cemu.exe`). On macOS it is skipped so GDB tools still load.
3. Tessera enables the stub with `--enable-gdbstub` (port **1337**, same as upstream Cemu).

## Setup

```sh
# from repo root, if this directory is empty/missing:
git clone https://github.com/Matt-Wood-23/cemu-re-mcp.git tools/cemu-re-mcp
# re-apply Tessera patches if you re-cloned (see git status / setup.sh notes)
./tools/cemu-re-mcp/setup.sh
```

Grok project config (`.grok/config.toml`) launches `tools/run-cemu-re-mcp.sh`.

## Use with TesseraEmu

```sh
./bin/TesseraEmu_relwithdebinfo --enable-gdbstub --verbose -g "Roms/<title>.wux"
# then from the agent: connect_tool → resume_tool (required after splash halt) → read_*/BP tools
```

Hardware watchpoints may be unsupported on macOS arm64 (see porting notes). Prefer software BPs.
