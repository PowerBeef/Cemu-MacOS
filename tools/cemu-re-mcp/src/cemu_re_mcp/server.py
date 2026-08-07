"""MCP server entrypoint for cemu_re_mcp.

Exposes Cemu's GDB stub as a set of MCP tools for runtime reverse engineering.

Start Cemu with: Debug -> Launch with GDB stub (default port 1337). Then
register this server in your project's `.mcp.json` and call `connect_tool`
once per session.

PowerPC register mapping (Cemu / Espresso, Wii U):
    0-31   r0-r31 (GPRs)
    32-63  f0-f31 (FPRs, 64-bit each)
    64     PC
    65     MSR
    66     CR
    67     LR
    68     CTR
    69     XER
The actual numbering is stub-dependent; verify via Cemu's PPC debugger window
if a register read returns unexpected bytes.
"""
from __future__ import annotations

import struct
from typing import Optional

from mcp.server.fastmcp import FastMCP

from .gdb_rsp import GdbRsp, GdbRspError, StopReason

mcp = FastMCP("cemu")

# Module-level singleton — the MCP server lives in a single process so one
# client connection is enough. If we ever need parallel sessions, this becomes
# a connection pool.
_client: Optional[GdbRsp] = None


def _require_client() -> GdbRsp:
    if _client is None or not _client.is_connected():
        raise RuntimeError("not connected — call connect_tool first")
    return _client


# ---------- connection lifecycle ----------

@mcp.tool()
def health_check_tool() -> dict:
    """Confirm the MCP server is alive. Does NOT require a Cemu connection."""
    return {
        "status": "ok",
        "connected_to_cemu": _client is not None and _client.is_connected(),
    }


@mcp.tool()
def connect_tool(host: str = "127.0.0.1", port: int = 1337, timeout: float = 5.0,
                 force: bool = False) -> dict:
    """Open a TCP connection to Cemu's GDB stub. Default port 1337.

    NOTE: is_connected() only checks the local socket — it can't detect that the
    remote stub died (e.g. Cemu was restarted). If you restarted Cemu and this
    returns 'already_connected' against a dead stub, call reconnect_tool (or pass
    force=True) to drop the stale client and connect fresh — no MCP-server
    restart needed."""
    global _client
    if _client is not None and _client.is_connected() and not force:
        return {"status": "already_connected", "host": _client.host, "port": _client.port,
                "hint": "if Cemu was restarted this may be stale — use reconnect_tool"}
    if _client is not None:
        try:
            _client.close()
        except Exception:
            pass
        _client = None
    _client = GdbRsp(host=host, port=port, timeout=timeout)
    _client.connect()
    return {"status": "connected", "host": host, "port": port, "no_ack_mode": _client._no_ack}


@mcp.tool()
def reconnect_tool(host: str = "127.0.0.1", port: int = 1337, timeout: float = 5.0) -> dict:
    """Force-drop any existing GDB client and connect fresh to Cemu's stub.

    Use this after restarting Cemu: the one-shot stub means the old socket is
    dead, but is_connected() can't tell, so plain connect_tool short-circuits on
    the stale client and resume/read fail with WinError 10054. This rebuilds the
    connection in-process — NO MCP-server restart required."""
    global _client
    if _client is not None:
        try:
            _client.close()
        except Exception:
            pass
        _client = None
    _client = GdbRsp(host=host, port=port, timeout=timeout)
    _client.connect()
    return {"status": "reconnected", "host": host, "port": port}


@mcp.tool()
def disconnect_tool() -> dict:
    """Close the GDB connection. Cemu and the game keep running."""
    global _client
    if _client is None:
        return {"status": "not_connected"}
    _client.close()
    _client = None
    return {"status": "disconnected"}


# ---------- execution control ----------

@mcp.tool()
def pause_tool() -> dict:
    """Attempt to halt the running target.

    NOTE: Cemu's GDB stub does NOT implement programmatic pause. Both `vCtrlC`
    and the raw 0x03 interrupt byte are accepted but do not actually halt the
    target. To halt Cemu, the USER must press Pause in Cemu's UI, or you must
    set a breakpoint/watchpoint and `wait_for_hit_tool`. This tool sends the
    interrupt as a best-effort but will likely return a timeout."""
    c = _require_client()
    c.interrupt()
    try:
        stop = c.wait_for_stop(timeout=1.0)
        return {"status": "paused", "signal": stop.signal, "raw": stop.raw}
    except Exception as e:
        return {
            "status": "interrupt_ignored",
            "note": "Cemu does not implement programmatic pause. Use BP/WP + wait_for_hit, or pause via Cemu's UI.",
            "error": str(e),
        }


@mcp.tool()
def resume_tool() -> dict:
    """Continue execution. Does NOT block — use wait_for_hit to block until next stop."""
    c = _require_client()
    c.cont()
    return {"status": "resumed"}


@mcp.tool()
def step_tool() -> dict:
    """Single-step one PPC instruction. Blocks for the resulting stop."""
    c = _require_client()
    stop = c.step()
    return {"status": "stopped", "signal": stop.signal, "raw": stop.raw}


@mcp.tool()
def wait_for_hit_tool(timeout: float = 30.0) -> dict:
    """Block until the target hits a breakpoint, watchpoint, or other stop event."""
    c = _require_client()
    try:
        stop = c.wait_for_stop(timeout=timeout)
        return {"status": "stopped", "signal": stop.signal, "raw": stop.raw}
    except Exception as e:
        return {"status": "timeout", "error": str(e)}


@mcp.tool()
def is_paused_tool() -> dict:
    """Query halt reason. Note: GDB stubs always have a 'last halt reason' even
    if the target is currently running — this is best-effort."""
    c = _require_client()
    try:
        stop = c.query_halt_reason()
        return {"signal": stop.signal, "raw": stop.raw}
    except GdbRspError as e:
        return {"error": str(e)}


# ---------- memory: raw ----------

@mcp.tool()
def read_mem_tool(addr: str, length: int) -> dict:
    """Read `length` bytes from `addr`. addr is hex string like '0x02000000' or decimal."""
    c = _require_client()
    a = int(addr, 0)
    data = c.read_mem(a, length)
    return {"addr": f"0x{a:08x}", "length": length, "hex": data.hex()}


@mcp.tool()
def write_mem_tool(addr: str, hex_data: str) -> dict:
    """Write raw bytes (hex-encoded string) to `addr`."""
    c = _require_client()
    a = int(addr, 0)
    data = bytes.fromhex(hex_data)
    c.write_mem(a, data)
    return {"addr": f"0x{a:08x}", "wrote": len(data)}


# ---------- memory: typed (big-endian) ----------

@mcp.tool()
def read_u8_tool(addr: str) -> dict:
    c = _require_client()
    a = int(addr, 0)
    return {"addr": f"0x{a:08x}", "value": c.read_mem(a, 1)[0]}


@mcp.tool()
def read_u16_tool(addr: str) -> dict:
    c = _require_client()
    a = int(addr, 0)
    v = struct.unpack(">H", c.read_mem(a, 2))[0]
    return {"addr": f"0x{a:08x}", "value": v, "hex": f"0x{v:04x}"}


@mcp.tool()
def read_u32_tool(addr: str) -> dict:
    c = _require_client()
    a = int(addr, 0)
    v = struct.unpack(">I", c.read_mem(a, 4))[0]
    return {"addr": f"0x{a:08x}", "value": v, "hex": f"0x{v:08x}"}


@mcp.tool()
def read_s32_tool(addr: str) -> dict:
    c = _require_client()
    a = int(addr, 0)
    v = struct.unpack(">i", c.read_mem(a, 4))[0]
    return {"addr": f"0x{a:08x}", "value": v}


@mcp.tool()
def read_f32_tool(addr: str) -> dict:
    c = _require_client()
    a = int(addr, 0)
    v = struct.unpack(">f", c.read_mem(a, 4))[0]
    return {"addr": f"0x{a:08x}", "value": v}


@mcp.tool()
def write_u32_tool(addr: str, value: int) -> dict:
    c = _require_client()
    a = int(addr, 0)
    c.write_mem(a, struct.pack(">I", value & 0xFFFFFFFF))
    return {"addr": f"0x{a:08x}", "wrote": value}


@mcp.tool()
def write_f32_tool(addr: str, value: float) -> dict:
    c = _require_client()
    a = int(addr, 0)
    c.write_mem(a, struct.pack(">f", value))
    return {"addr": f"0x{a:08x}", "wrote": value}


# ---------- registers ----------

@mcp.tool()
def get_gprs_tool() -> dict:
    """Read all 32 general-purpose registers (r0-r31) as big-endian u32s."""
    c = _require_client()
    raw = c.read_all_registers()
    # First 32 registers * 4 bytes each = 128 bytes for GPRs
    gprs = struct.unpack(">32I", raw[:128])
    return {f"r{i}": f"0x{v:08x}" for i, v in enumerate(gprs)}


@mcp.tool()
def get_pc_tool() -> dict:
    """Read program counter (PPC reg #64 in standard mapping; verify against Cemu)."""
    c = _require_client()
    raw = c.read_register(64)
    v = struct.unpack(">I", raw)[0]
    return {"pc": f"0x{v:08x}"}


@mcp.tool()
def get_lr_tool() -> dict:
    """Read link register (PPC reg #67 in standard mapping)."""
    c = _require_client()
    raw = c.read_register(67)
    v = struct.unpack(">I", raw)[0]
    return {"lr": f"0x{v:08x}"}


# ---------- thread / raw-protocol diagnostics ----------

@mcp.tool()
def raw_cmd_tool(packet: str) -> dict:
    """Send a raw GDB RSP packet (no `$`/checksum framing — just the payload,
    e.g. 'qfThreadInfo', 'qC', 'Hg2EE285C0', 'g') and return the raw reply.

    Escape hatch for protocol-level probing the higher-level tools don't cover."""
    c = _require_client()
    try:
        return {"packet": packet, "reply": c.cmd(packet)}
    except Exception as e:
        return {"packet": packet, "error": str(e)}


@mcp.tool()
def list_threads_tool() -> dict:
    """List the stub's thread IDs via qfThreadInfo/qsThreadInfo, plus the thread
    that produced the most recent stop (the one register reads now target)."""
    c = _require_client()
    threads = []
    try:
        reply = c.cmd("qfThreadInfo")
        while reply and reply[0] == "m":
            threads.extend(reply[1:].split(","))
            reply = c.cmd("qsThreadInfo")
    except Exception as e:
        return {"error": str(e), "stop_thread": c._stop_thread}
    return {"threads": threads, "stop_thread": c._stop_thread, "hg_ok": c._hg_ok}


@mcp.tool()
def select_thread_tool(thread: Optional[str] = None) -> dict:
    """Force the thread used for subsequent register reads (`Hg<tid>`). With no
    arg, selects the most recent stop thread. Returns the stub's reply."""
    c = _require_client()
    tid = thread if thread is not None else c._stop_thread
    if not tid:
        return {"error": "no thread specified and no stop thread recorded"}
    try:
        reply = c.cmd(f"Hg{tid}")
        return {"selected": tid, "reply": reply}
    except Exception as e:
        return {"thread": tid, "error": str(e)}


# ---------- breakpoints / watchpoints ----------

@mcp.tool()
def add_breakpoint_tool(addr: str) -> dict:
    """Set a software breakpoint at `addr`. PPC instruction width is 4 bytes."""
    c = _require_client()
    a = int(addr, 0)
    c.add_breakpoint(a, kind=4)
    return {"status": "set", "addr": f"0x{a:08x}"}


@mcp.tool()
def remove_breakpoint_tool(addr: str) -> dict:
    c = _require_client()
    a = int(addr, 0)
    c.remove_breakpoint(a, kind=4)
    return {"status": "removed", "addr": f"0x{a:08x}"}


@mcp.tool()
def add_watchpoint_tool(addr: str, length: int = 4, kind: str = "w") -> dict:
    """Set a watchpoint. kind in {'w' write, 'r' read, 'a' access (rw)}."""
    c = _require_client()
    a = int(addr, 0)
    c.add_watchpoint(a, length, kind)
    return {"status": "set", "addr": f"0x{a:08x}", "length": length, "kind": kind}


@mcp.tool()
def remove_watchpoint_tool(addr: str, length: int = 4, kind: str = "w") -> dict:
    c = _require_client()
    a = int(addr, 0)
    c.remove_watchpoint(a, length, kind)
    return {"status": "removed", "addr": f"0x{a:08x}", "length": length, "kind": kind}


# ---------- high-level helpers ----------

@mcp.tool()
def capture_on_hit_tool(
    bp_addr: str,
    mem_addr: Optional[str] = None,
    mem_len: int = 0,
    capture_regs: bool = True,
    timeout: float = 30.0,
    auto_resume: bool = False,
    auto_remove_bp: bool = True,
) -> dict:
    """Set a breakpoint, wait for it to fire, snapshot context, return.

    Workhorse for runtime tracing — e.g. "set BP at the damage-apply function,
    wait for it to hit, snapshot all GPRs + the entity struct it points at."

    Args:
        bp_addr: address (hex string or decimal) to set the BP at
        mem_addr: optional address to snapshot bytes from when BP hits
        mem_len: how many bytes to snapshot at mem_addr (ignored if mem_addr None)
        capture_regs: include all GPRs + PC + LR in the snapshot
        timeout: seconds to wait for the BP to fire before giving up
        auto_resume: if True, send vCont;c after capture (BP will re-fire on next hit if not removed)
        auto_remove_bp: if True, remove the BP after the hit (so it's one-shot)

    Returns dict with: hit (bool), signal, raw_stop, regs (if requested),
    memory (hex, if requested), and a note describing what was captured.
    """
    c = _require_client()
    a = int(bp_addr, 0)

    c.add_breakpoint(a, kind=4)
    try:
        try:
            stop = c.wait_for_stop(timeout=timeout)
        except Exception as e:
            return {
                "hit": False,
                "bp_addr": f"0x{a:08x}",
                "error": f"BP did not fire within {timeout}s: {e}",
            }

        result: dict = {
            "hit": True,
            "bp_addr": f"0x{a:08x}",
            "signal": stop.signal,
            "raw_stop": stop.raw,
        }

        if capture_regs:
            try:
                raw = c.read_all_registers()
                gprs = struct.unpack(">32I", raw[:128])
                result["regs"] = {f"r{i}": f"0x{v:08x}" for i, v in enumerate(gprs)}
                try:
                    pc_raw = c.read_register(64)
                    result["regs"]["pc"] = f"0x{struct.unpack('>I', pc_raw)[0]:08x}"
                except Exception:
                    pass
                try:
                    lr_raw = c.read_register(67)
                    result["regs"]["lr"] = f"0x{struct.unpack('>I', lr_raw)[0]:08x}"
                except Exception:
                    pass
            except Exception as e:
                result["regs_error"] = str(e)

        if mem_addr is not None and mem_len > 0:
            try:
                ma = int(mem_addr, 0)
                data = c.read_mem(ma, mem_len)
                result["memory"] = {"addr": f"0x{ma:08x}", "length": mem_len, "hex": data.hex()}
            except Exception as e:
                result["memory_error"] = str(e)

        return result
    finally:
        if auto_remove_bp:
            try:
                c.remove_breakpoint(a, kind=4)
            except Exception:
                pass
        if auto_resume:
            try:
                c.cont()
            except Exception:
                pass


# ---------- pymem fast backend + Cheat-Engine scanner ----------
# Independent of the GDB stub: attaches directly to Cemu.exe (Windows only).
# On macOS (TesseraEmu) these tools are omitted so the GDB path still loads.

try:
    from . import pymem_backend as _pb
    _PYMEM_OK = True
except Exception as _pymem_exc:  # ImportError, missing windll, etc.
    _pb = None  # type: ignore
    _PYMEM_OK = False
    _PYMEM_ERR = _pymem_exc


def _require_pymem():
    if not _PYMEM_OK:
        raise RuntimeError(
            f"pymem backend unavailable on this platform ({_PYMEM_ERR!r}). "
            "Use the GDB tools (connect_tool / read_mem_tool / …) instead."
        )
    return _pb


if _PYMEM_OK:

    @mcp.tool()
    def pymem_attach_tool() -> dict:
        """Attach to Cemu.exe and derive host_base (guest<->host linear map). Windows only."""
        return _pb.get_bridge().attach()

    @mcp.tool()
    def pm_read_tool(addr: str, length: int) -> dict:
        """Fast read of `length` bytes at guest `addr` (via pymem). Big-endian bytes."""
        b = _pb.get_bridge()
        a = int(addr, 0)
        return {"addr": f"0x{a:08x}", "length": length, "hex": b.read(a, length).hex()}

    @mcp.tool()
    def pm_read_typed_tool(addr: str, vtype: str = "u32") -> dict:
        """Fast typed read at guest `addr`. vtype: u8 u16 u32 s32 f32 (big-endian)."""
        b = _pb.get_bridge()
        a = int(addr, 0)
        return {"addr": f"0x{a:08x}", "vtype": vtype, "value": b.read_typed(a, vtype)}

    @mcp.tool()
    def pm_write_tool(addr: str, hex_data: str) -> dict:
        """Fast raw write of hex-encoded bytes at guest `addr` (via pymem)."""
        b = _pb.get_bridge()
        a = int(addr, 0)
        n = b.write(a, bytes.fromhex(hex_data))
        return {"addr": f"0x{a:08x}", "wrote": n}

    @mcp.tool()
    def pm_write_typed_tool(addr: str, vtype: str, value: float) -> dict:
        """Fast typed write at guest `addr`. vtype: u8 u16 u32 s32 f32 (big-endian)."""
        b = _pb.get_bridge()
        a = int(addr, 0)
        b.write_typed(a, vtype, value)
        return {"addr": f"0x{a:08x}", "vtype": vtype, "wrote": value}

    @mcp.tool()
    def scan_begin_tool(vtype: str = "u32", mode: str = "eq", a: Optional[float] = None,
                        b: Optional[float] = None, exclude_noise: bool = True) -> dict:
        """First scan over the whole guest region. Windows/pymem only."""
        av = None if a is None else (a if vtype == "f32" else int(a))
        bv = None if b is None else (b if vtype == "f32" else int(b))
        return _pb.get_bridge().scan_begin(vtype, mode, av, bv, exclude_noise)

    @mcp.tool()
    def scan_next_tool(op: str, val: Optional[float] = None) -> dict:
        """Refine the active scan. Windows/pymem only."""
        return _pb.get_bridge().scan_next(op, val)

    @mcp.tool()
    def scan_pair_tool(max_lo: int, max_hi: int, neighbor: str = "+") -> dict:
        return _pb.get_bridge().scan_pair(max_lo, max_hi, neighbor)

    @mcp.tool()
    def scan_region_tool(lo: str, hi: str, exclude: bool = False) -> dict:
        return _pb.get_bridge().scan_region(int(lo, 0), int(hi, 0), exclude)

    @mcp.tool()
    def scan_results_tool(limit: int = 40, addr_lo: str = "0", addr_hi: str = "0") -> dict:
        return _pb.get_bridge().scan_results(limit, int(addr_lo, 0), int(addr_hi, 0))

    @mcp.tool()
    def ptr_scan_tool(lo: str, hi: str, limit: int = 120) -> dict:
        return _pb.get_bridge().ptr_scan(int(lo, 0), int(hi, 0), limit)

    @mcp.tool()
    def read_ptr_chain_tool(base: str, offsets: list[int]) -> dict:
        return _pb.get_bridge().read_ptr_chain(int(base, 0), offsets)

    @mcp.tool()
    def hold_start_tool(addr: str, vtype: str, value: float, hz: float = 120.0) -> dict:
        return _pb.get_bridge().hold_start(int(addr, 0), vtype, value, hz)

    @mcp.tool()
    def hold_stop_tool(addr: str) -> dict:
        return _pb.get_bridge().hold_stop(int(addr, 0))

    @mcp.tool()
    def hold_list_tool() -> dict:
        return _pb.get_bridge().hold_list()

    @mcp.tool()
    def disasm_tool(addr: str, count: int = 16) -> dict:
        b = _pb.get_bridge()
        a = int(addr, 0)
        code = b.read(a, count * 4)
        return {"addr": f"0x{a:08x}", "insns": _pb.disasm(code, a, count)}

else:

    @mcp.tool()
    def pymem_status_tool() -> dict:
        """Report that the Windows pymem backend is not available (expected on TesseraEmu/macOS)."""
        return {
            "available": False,
            "error": repr(_PYMEM_ERR),
            "hint": "Use GDB tools after TesseraEmu --enable-gdbstub (port 1337).",
        }



# ---------- entrypoint ----------

def main() -> None:
    """Console-script entrypoint (`cemu-re-mcp`)."""
    mcp.run()


if __name__ == "__main__":
    main()
