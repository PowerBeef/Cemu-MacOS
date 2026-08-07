"""pymem-backed fast memory access + Cheat-Engine-style scanner for Cemu.

Independent of the GDB stub: attaches directly to ``Cemu.exe`` and treats the
guest 32-bit address space as one linear region inside the host process
(``host = host_base + guest``). host_base is per-launch (ASLR) and is derived by
locating the big committed region whose first 8 bytes are the PPC entry
signature ``60 00 00 00 4e 80 00 20`` at guest ``0x02000000``.

Why this exists: Cemu's GDB stub has no search primitive and bulk RSP reads are
slow. pymem reads the whole ~1.3 GB region in ~7 s and numpy vectorizes diffs in
seconds, so this is the fast path for value scanning, differential HP hunts, and
hold-writes. See ``E:\\cemu_re_mcp\\docs\\MCP_TOOLING_PLAN.md``.

Reference prototypes this consolidates: ``E:\\mh3ureversing\\scripts\\cemu_scan.py``
and ``cemu_hp_diff.py`` (proven live 2026-05-28: found + killed a Rathian).

All addresses exposed here are GUEST addresses. Values are big-endian.
"""
from __future__ import annotations

import ctypes
import os
import struct
import threading
import time
from ctypes import wintypes
from typing import Optional

import numpy as np
import pymem

GUEST_BASE = 0x02000000
ENTRY_SIG = bytes.fromhex("600000004e800020")

# numpy dtype + width per value type (big-endian, natural alignment)
DT = {
    "u8": (">u1", 1),
    "u16": (">u2", 2),
    "u32": (">u4", 4),
    "s32": (">i4", 4),
    "f32": (">f4", 4),
}

# struct-module format codes (numpy dtype strings above are NOT valid struct fmts)
STRUCT_FMT = {"u8": ">B", "u16": ">H", "u32": ">I", "s32": ">i", "f32": ">f"}

# Known-noise guest ranges on MH3U (audio / streaming buffers). Excluded by
# default in scans because they oscillate and repeat at fixed strides, burying
# real values. (lo, hi) half-open. See MCP_TOOLING_PLAN.md.
MH3U_NOISE_RANGES = [
    (0x0E100000, 0x0E330000),
    (0x15990000, 0x159A0000),
    (0x2CE00000, 0x2D900000),
]


# ---------- Win32 VirtualQueryEx (region enumeration) ----------

class _MBI(ctypes.Structure):
    _fields_ = [
        ("BaseAddress", ctypes.c_ulonglong),
        ("AllocationBase", ctypes.c_ulonglong),
        ("AllocationProtect", wintypes.DWORD),
        ("__a", wintypes.DWORD),
        ("RegionSize", ctypes.c_ulonglong),
        ("State", wintypes.DWORD),
        ("Protect", wintypes.DWORD),
        ("Type", wintypes.DWORD),
        ("__b", wintypes.DWORD),
    ]


_VQ = ctypes.windll.kernel32.VirtualQueryEx
_VQ.restype = ctypes.c_ulonglong
_VQ.argtypes = [wintypes.HANDLE, ctypes.c_ulonglong, ctypes.POINTER(_MBI), ctypes.c_ulonglong]
_MEM_COMMIT = 0x1000
_READABLE = {0x02, 0x04, 0x20, 0x40}  # PAGE_READONLY/READWRITE/EXECUTE_READ/EXECUTE_READWRITE


class CemuNotRunning(RuntimeError):
    pass


class PymemBridge:
    """Live handle on Cemu.exe with guest<->host translation and a scan state.

    One instance per MCP-server process. Re-``attach()`` after a Cemu relaunch
    (host_base changes). Scan state (offsets/values) lives in-process.
    """

    def __init__(self) -> None:
        self.pm: Optional[pymem.Pymem] = None
        self.host_region: int = 0   # host address of guest 0x02000000
        self.region_size: int = 0
        # scan state
        self._offs: Optional[np.ndarray] = None   # int64 guest-relative byte offsets (from GUEST_BASE)
        self._vals: Optional[np.ndarray] = None    # int64 last-seen values
        self._vtype: Optional[str] = None
        # hold-write threads, keyed by guest addr
        self._holds: dict[int, "_Holder"] = {}

    # ---- lifecycle ----

    def attach(self, process: Optional[str] = None, pid: Optional[int] = None) -> dict:
        """Attach to a Cemu process and derive host_base. Safe to call repeatedly.

        Process selection (first match wins):
          * explicit ``pid`` arg, or env ``CEMU_PID`` — attach to that exact process
            (needed when several Cemu instances run, e.g. a host + a guest for an
            online multiplayer test).
          * explicit ``process`` arg, or env ``CEMU_PROC`` — attach by exe name.
          * otherwise auto-try the known names: stock ``Cemu.exe`` then the online
            fork ``Cemu_release.exe``.
        The guest region is then found by its entry signature, so it works for any
        build regardless of how we attached.
        """
        pid = pid if pid is not None else (
            int(os.environ["CEMU_PID"]) if os.environ.get("CEMU_PID") else None)
        if pid is not None:
            try:
                self.pm = pymem.Pymem()
                self.pm.open_process_from_id(pid)
            except Exception as e:
                raise CemuNotRunning(f"Cemu pid={pid} not attachable: {e}")
        else:
            names = [process] if process else (
                [os.environ["CEMU_PROC"]] if os.environ.get("CEMU_PROC")
                else ["Cemu.exe", "Cemu_release.exe"])
            last = None
            self.pm = None
            for name in names:
                try:
                    self.pm = pymem.Pymem(name)
                    break
                except Exception as e:  # pymem.exception.ProcessNotFound etc.
                    last, self.pm = e, None
            if self.pm is None:
                raise CemuNotRunning(f"no Cemu process attachable (tried {names}): {last}")
        base, size = self._find_region()
        self.host_region = base
        self.region_size = size
        return {
            "status": "attached",
            "process": self.pm.process_base.name if hasattr(self.pm, "process_base") else "?",
            "pid": getattr(self.pm, "process_id", None),
            "host_region": f"0x{base:012x}",
            "host_base": f"0x{base - GUEST_BASE:012x}",
            "region_size": size,
            "region_size_mb": round(size / (1 << 20), 1),
        }

    def _require(self) -> pymem.Pymem:
        if self.pm is None or self.host_region == 0:
            self.attach()
        return self.pm  # type: ignore[return-value]

    def _find_region(self) -> tuple[int, int]:
        h = self.pm.process_handle  # type: ignore[union-attr]
        addr = 0
        best = None
        while addr < 0x7FFFFFFFFFFF:
            mbi = _MBI()
            if not _VQ(h, addr, ctypes.byref(mbi), ctypes.sizeof(mbi)):
                break
            if (mbi.State == _MEM_COMMIT and (mbi.Protect & 0xFF) in _READABLE
                    and mbi.RegionSize > (256 << 20)):
                try:
                    if self.pm.read_bytes(mbi.BaseAddress, 8) == ENTRY_SIG:  # type: ignore[union-attr]
                        return mbi.BaseAddress, mbi.RegionSize
                except Exception:
                    pass
                if best is None or mbi.RegionSize > best[1]:
                    best = (mbi.BaseAddress, mbi.RegionSize)
            addr = mbi.BaseAddress + mbi.RegionSize if mbi.RegionSize else addr + 0x1000
        if best is None:
            raise CemuNotRunning("could not locate Cemu guest region (entry sig not found)")
        return best

    # ---- typed read/write (guest addresses, big-endian) ----

    def _host(self, guest: int) -> int:
        return self.host_region + (guest - GUEST_BASE)

    def read(self, guest: int, length: int) -> bytes:
        self._require()
        return self.pm.read_bytes(self._host(guest), length)  # type: ignore[union-attr]

    def write(self, guest: int, data: bytes) -> int:
        self._require()
        self.pm.write_bytes(self._host(guest), data, len(data))  # type: ignore[union-attr]
        return len(data)

    def read_typed(self, guest: int, vtype: str):
        fmt = STRUCT_FMT[vtype]
        _, w = DT[vtype]
        return struct.unpack(fmt, self.read(guest, w))[0]

    def write_typed(self, guest: int, vtype: str, value) -> int:
        fmt = STRUCT_FMT[vtype]
        packed = struct.pack(fmt, float(value) if vtype == "f32" else int(value))
        return self.write(guest, packed)

    # ---- bulk region read ----

    def read_region(self) -> np.ndarray:
        """Read the whole committed guest region into a uint8 numpy array (~7 s)."""
        self._require()
        out = bytearray()
        pos = 0
        chunk = 16 << 20
        while pos < self.region_size:
            n = min(chunk, self.region_size - pos)
            out += self.pm.read_bytes(self.host_region + pos, n)  # type: ignore[union-attr]
            pos += n
        return np.frombuffer(bytes(out), dtype=np.uint8)

    def _gather(self, arr: np.ndarray, offs: np.ndarray, vtype: str) -> np.ndarray:
        """Read the values at `offs` (byte offsets from GUEST_BASE) as int64."""
        code, w = DT[vtype]
        cols = np.zeros((len(offs), w), dtype=np.uint8)
        for j in range(w):
            cols[:, j] = arr[offs + j]
        return np.frombuffer(cols.tobytes(), dtype=np.dtype(code)).astype(np.int64)

    # ---- Cheat-Engine-style scanner ----

    def scan_begin(self, vtype: str, mode: str, a=None, b=None,
                   exclude_noise: bool = True) -> dict:
        """First scan. mode 'eq' (a=value), 'range' (a=lo,b=hi), or 'unknown'.

        'unknown' snapshots every aligned slot (for later changed/dec/inc passes).
        """
        if vtype not in DT:
            raise ValueError(f"vtype must be one of {list(DT)}")
        code, w = DT[vtype]
        arr = self.read_region()
        m = (len(arr) // w) * w
        v = arr[:m].view(code).astype(np.int64)
        all_offs = np.arange(len(v), dtype=np.int64) * w
        if mode == "eq":
            idx = np.nonzero(v == int(a))[0]
        elif mode == "range":
            idx = np.nonzero((v >= int(a)) & (v <= int(b)))[0]
        elif mode == "unknown":
            idx = np.arange(len(v), dtype=np.int64)
        else:
            raise ValueError("mode must be eq|range|unknown")
        offs = all_offs[idx]
        vals = v[idx]
        if exclude_noise:
            offs, vals = self._drop_noise(offs, vals)
        self._offs, self._vals, self._vtype = offs, vals, vtype
        return {"survivors": int(len(offs)), "vtype": vtype, "mode": mode}

    def scan_next(self, op: str, val=None) -> dict:
        """Refine survivors. ops: dec inc changed unchanged eq lt gt."""
        if self._offs is None:
            raise RuntimeError("no active scan — call scan_begin first")
        arr = self.read_region()
        cur = self._gather(arr, self._offs, self._vtype)  # type: ignore[arg-type]
        prev = self._vals
        if op == "dec":
            keep = cur < prev
        elif op == "inc":
            keep = cur > prev
        elif op == "changed":
            keep = cur != prev
        elif op == "unchanged":
            keep = cur == prev
        elif op == "eq":
            keep = cur == int(val)
        elif op == "lt":
            keep = cur < int(val)
        elif op == "gt":
            keep = cur > int(val)
        else:
            raise ValueError("op: dec inc changed unchanged eq lt gt")
        before = len(self._offs)
        self._offs, self._vals = self._offs[keep], cur[keep]
        return {"survivors": int(len(self._offs)), "was": int(before), "op": op}

    def scan_pair(self, max_lo: int, max_hi: int, neighbor: str = "+") -> dict:
        """Keep survivors whose adjacent slot (same width) is a CONSTANT-shaped
        value in [max_lo,max_hi] and >= the survivor — the cur/max-HP trick.

        neighbor '+' = max sits right after cur (cur_hp,max_hp); '-' = before.
        Re-reads region for the neighbor values. Use after a few dec passes.
        """
        if self._offs is None:
            raise RuntimeError("no active scan — call scan_begin first")
        code, w = DT[self._vtype]  # type: ignore[index]
        arr = self.read_region()
        cur = self._gather(arr, self._offs, self._vtype)  # type: ignore[arg-type]
        noff = self._offs + w if neighbor == "+" else self._offs - w
        valid = (noff >= 0) & (noff + w <= len(arr))
        nb = np.full(len(self._offs), -1, dtype=np.int64)
        nb[valid] = self._gather(arr, noff[valid], self._vtype)  # type: ignore[arg-type]
        keep = valid & (nb >= int(max_lo)) & (nb <= int(max_hi)) & (nb >= cur)
        before = len(self._offs)
        self._offs, self._vals = self._offs[keep], cur[keep]
        self._pair_neighbor = nb[keep]
        return {"survivors": int(len(self._offs)), "was": int(before),
                "note": f"kept slots with neighbor in [{max_lo},{max_hi}] >= value"}

    def scan_region(self, lo: int, hi: int, exclude: bool = False) -> dict:
        """Restrict (or exclude) survivors to a guest address window [lo,hi)."""
        if self._offs is None:
            raise RuntimeError("no active scan — call scan_begin first")
        g = GUEST_BASE + self._offs
        inside = (g >= int(lo)) & (g < int(hi))
        keep = ~inside if exclude else inside
        before = len(self._offs)
        self._offs, self._vals = self._offs[keep], self._vals[keep]
        return {"survivors": int(len(self._offs)), "was": int(before),
                "window": f"[0x{int(lo):x},0x{int(hi):x}) exclude={exclude}"}

    def _drop_noise(self, offs: np.ndarray, vals: np.ndarray):
        g = GUEST_BASE + offs
        keep = np.ones(len(offs), dtype=bool)
        for lo, hi in MH3U_NOISE_RANGES:
            keep &= ~((g >= lo) & (g < hi))
        return offs[keep], vals[keep]

    def scan_results(self, limit: int = 40, addr_lo: int = 0, addr_hi: int = 0) -> dict:
        if self._offs is None:
            return {"survivors": 0, "results": []}
        g = GUEST_BASE + self._offs
        sel = np.ones(len(g), dtype=bool)
        if addr_lo:
            sel &= g >= int(addr_lo)
        if addr_hi:
            sel &= g < int(addr_hi)
        idx = np.nonzero(sel)[0][:limit]
        nb = getattr(self, "_pair_neighbor", None)
        results = []
        for k in idx:
            r = {"addr": f"0x{int(g[k]):08x}", "value": int(self._vals[k])}
            if nb is not None and len(nb) == len(self._offs):
                r["neighbor"] = int(nb[k])
            results.append(r)
        return {"survivors": int(len(self._offs)), "shown": len(results), "results": results}

    # ---- pointer scan / chase ----

    def ptr_scan(self, lo: int, hi: int, limit: int = 120) -> dict:
        """Find u32 BE slots whose value (a pointer) lands in [lo,hi). Recovers
        struct bases: point at a known live struct addr to find what references it."""
        arr = self.read_region()
        m = (len(arr) // 4) * 4
        v = arr[:m].view(">u4").astype(np.int64)
        idx = np.nonzero((v >= int(lo)) & (v < int(hi)))[0]
        hits = []
        for i in idx[:limit]:
            at = GUEST_BASE + int(i) * 4
            tgt = int(v[i])
            zone = (".data/.bss" if 0x10164640 <= at < 0x10470F40
                    else "rodata" if 0x10000000 <= at < 0x10164640 else "heap")
            hits.append({"ptr_at": f"0x{at:08x}", "points_to": f"0x{tgt:08x}", "zone": zone})
        return {"total": int(len(idx)), "shown": len(hits), "hits": hits}

    def read_ptr_chain(self, base: int, offsets: list[int]) -> dict:
        """Follow base -> [*+off0] -> [*+off1] ... Each deref reads a BE u32."""
        steps = [f"0x{int(base):08x}"]
        cur = int(base)
        for off in offsets:
            ptr = struct.unpack(">I", self.read(cur, 4))[0]
            cur = (ptr + int(off)) & 0xFFFFFFFF
            steps.append(f"[*0x{ptr:08x}+0x{int(off):x}] -> 0x{cur:08x}")
        return {"final": f"0x{cur:08x}", "steps": steps}

    # ---- hold-write (beat per-frame rewrites) ----

    def hold_start(self, guest: int, vtype: str, value, hz: float = 120.0) -> dict:
        self.hold_stop(guest)  # replace any existing holder on this addr
        holder = _Holder(self, int(guest), vtype, value, hz)
        holder.start()
        self._holds[int(guest)] = holder
        return {"status": "holding", "addr": f"0x{int(guest):08x}", "value": value, "hz": hz}

    def hold_stop(self, guest: int) -> dict:
        h = self._holds.pop(int(guest), None)
        if h:
            h.stop()
            return {"status": "stopped", "addr": f"0x{int(guest):08x}"}
        return {"status": "not_held", "addr": f"0x{int(guest):08x}"}

    def hold_list(self) -> dict:
        return {"holding": [f"0x{a:08x}" for a in self._holds]}


class _Holder(threading.Thread):
    def __init__(self, bridge: PymemBridge, guest: int, vtype: str, value, hz: float):
        super().__init__(daemon=True)
        self.bridge, self.guest, self.vtype, self.value = bridge, guest, vtype, value
        self.interval = 1.0 / max(hz, 1.0)
        self._stop = threading.Event()

    def run(self) -> None:
        while not self._stop.is_set():
            try:
                self.bridge.write_typed(self.guest, self.vtype, self.value)
            except Exception:
                pass
            time.sleep(self.interval)

    def stop(self) -> None:
        self._stop.set()


# ---- disassembly (Capstone PPC, big-endian 32-bit) ----

def disasm(code_bytes: bytes, addr: int, count: int = 0) -> list[dict]:
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN)
    out = []
    for ins in md.disasm(code_bytes, addr):
        out.append({"addr": f"0x{ins.address:08x}", "bytes": ins.bytes.hex(),
                    "mnemonic": ins.mnemonic, "op_str": ins.op_str})
        if count and len(out) >= count:
            break
    return out


# module-level singleton (one MCP-server process)
_bridge: Optional[PymemBridge] = None


def get_bridge() -> PymemBridge:
    global _bridge
    if _bridge is None:
        _bridge = PymemBridge()
    return _bridge
