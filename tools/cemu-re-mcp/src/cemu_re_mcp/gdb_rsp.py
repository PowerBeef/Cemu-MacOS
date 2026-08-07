"""GDB Remote Serial Protocol client for Cemu's built-in GDB stub.

Pure-stdlib implementation. Speaks the subset of GDB RSP that Cemu supports.
Cemu listens on port 1337 by default when launched with "Debug -> Launch with
GDB stub".

Protocol primer:
- Each packet is `$<data>#<checksum>` where checksum is (sum of data bytes) % 256
  formatted as two lowercase hex digits.
- Ack: server sends `+` for received-ok or `-` for retry-please. We must read
  those before/after our packets (unless we negotiated no-ack mode).
- Interrupt: send a single 0x03 byte (no packet framing) to halt the target.
- RLE: the protocol supports run-length-encoding bytes via `*` markers in
  responses (e.g. `0*~` = `0` repeated 0x7e+29=... actually n = byte_value-29+3).
  We must decode this in incoming packets.
"""
from __future__ import annotations

import socket
import threading
import time
from dataclasses import dataclass
from typing import Optional


class GdbRspError(Exception):
    """Protocol-level error from the GDB stub."""


@dataclass
class StopReason:
    """Reply from a `?` query or after `c`/`s` completes."""

    signal: int  # POSIX signal that halted the target (5 = SIGTRAP is typical)
    raw: str    # full reply for caller to parse extras (e.g. `T05thread:1;...`)


class GdbRsp:
    """Thin GDB Remote Serial Protocol client.

    Single-threaded request/reply (Cemu's stub does not support multi-packet
    pipelining). Caller is responsible for not calling memory-read while a
    `wait_for_stop` is in flight.
    """

    def __init__(self, host: str = "127.0.0.1", port: int = 1337, timeout: float = 5.0):
        self.host = host
        self.port = port
        self.timeout = timeout
        self._sock: Optional[socket.socket] = None
        self._rxbuf = bytearray()
        self._lock = threading.Lock()
        self._no_ack = False  # set after qStartNoAckMode succeeds
        # GDB thread-id (hex string) of the thread that produced the most recent
        # stop reply. Cemu's stub services `g`/`p` against a *default* thread, not
        # the one that hit the breakpoint, so we must `Hg<tid>` before reading
        # registers or we get a parked coreinit thread's context. None until first stop.
        self._stop_thread: Optional[str] = None
        # Whether the stub honors Hg thread selection (probed lazily; assume yes).
        self._hg_ok = True

    # ----- connection lifecycle -----

    def connect(self) -> None:
        if self._sock is not None:
            return
        s = socket.create_connection((self.host, self.port), timeout=self.timeout)
        s.settimeout(self.timeout)
        self._sock = s
        self._rxbuf.clear()
        # Cemu's GDB stub does not support QStartNoAckMode — confirmed empirically
        # 2026-05-27. Skipping negotiation avoids a 5-second timeout on connect when
        # the target is running (the stub doesn't service queries while not halted).
        self._no_ack = False

    def close(self) -> None:
        if self._sock is not None:
            try:
                self._sock.close()
            finally:
                self._sock = None
                self._rxbuf.clear()
                self._no_ack = False

    def is_connected(self) -> bool:
        return self._sock is not None

    # ----- low-level I/O -----

    def _send_raw(self, data: bytes) -> None:
        assert self._sock is not None
        self._sock.sendall(data)

    def _recv_byte(self) -> int:
        assert self._sock is not None
        while not self._rxbuf:
            chunk = self._sock.recv(4096)
            if not chunk:
                raise GdbRspError("connection closed by remote")
            self._rxbuf.extend(chunk)
        b = self._rxbuf[0]
        del self._rxbuf[0]
        return b

    def _peek_byte(self, timeout: float = 0.1) -> Optional[int]:
        """Non-blocking peek for the next byte, if any."""
        assert self._sock is not None
        if self._rxbuf:
            return self._rxbuf[0]
        old = self._sock.gettimeout()
        self._sock.settimeout(timeout)
        try:
            chunk = self._sock.recv(4096)
            if not chunk:
                return None
            self._rxbuf.extend(chunk)
            return self._rxbuf[0]
        except (socket.timeout, BlockingIOError):
            return None
        finally:
            self._sock.settimeout(old)

    # ----- packet framing -----

    @staticmethod
    def _checksum(data: bytes) -> str:
        return f"{sum(data) & 0xff:02x}"

    def _build_packet(self, payload: str) -> bytes:
        body = payload.encode("ascii")
        return b"$" + body + b"#" + self._checksum(body).encode("ascii")

    def _send_packet(self, payload: str) -> None:
        pkt = self._build_packet(payload)
        # Retry on NAK up to 3 times
        for _ in range(3):
            self._send_raw(pkt)
            if self._no_ack:
                return
            ack = self._recv_byte()
            if ack == ord('+'):
                return
            if ack == ord('-'):
                continue
            raise GdbRspError(f"unexpected ack byte: {ack!r}")
        raise GdbRspError("repeated NAK from stub")

    def _read_packet(self) -> str:
        """Read one $<data>#cs packet, send + ack, return decoded data."""
        # Skip until we see $
        while True:
            b = self._recv_byte()
            if b == ord('$'):
                break
        body = bytearray()
        while True:
            b = self._recv_byte()
            if b == ord('#'):
                break
            body.append(b)
        # Two checksum bytes follow
        cs = bytes([self._recv_byte(), self._recv_byte()]).decode("ascii")
        # Validate
        if cs.lower() != self._checksum(body):
            if not self._no_ack:
                self._send_raw(b"-")
            raise GdbRspError(f"bad checksum: got {cs}, expected {self._checksum(body)}")
        if not self._no_ack:
            self._send_raw(b"+")
        return self._expand_rle(body.decode("ascii", errors="replace"))

    @staticmethod
    def _expand_rle(s: str) -> str:
        """Expand GDB RSP run-length encoding (`X*Y` → X repeated (Y-29+3) times)."""
        out = []
        i = 0
        while i < len(s):
            c = s[i]
            if c == '*' and i + 1 < len(s) and out:
                count = ord(s[i + 1]) - 29 + 3
                out.append(out[-1] * count)
                i += 2
                continue
            out.append(c)
            i += 1
        return "".join(out)

    # ----- high-level command -----

    def cmd(self, payload: str) -> str:
        """Send a packet and read the reply. Thread-safe via internal lock."""
        with self._lock:
            self._send_packet(payload)
            reply = self._read_packet()
            if reply.startswith("E") and len(reply) == 3 and all(c in "0123456789abcdefABCDEF" for c in reply[1:]):
                raise GdbRspError(f"stub returned error: {reply}")
            return reply

    def interrupt(self) -> None:
        """Send Ctrl-C (0x03) to halt the running target. No reply expected
        directly — the next `wait_for_stop()` will receive the T-packet."""
        with self._lock:
            self._send_raw(b"\x03")

    def wait_for_stop(self, timeout: Optional[float] = None) -> StopReason:
        """Wait for an unsolicited stop reply (after `c`, `s`, or interrupt)."""
        with self._lock:
            assert self._sock is not None
            old = self._sock.gettimeout()
            if timeout is not None:
                self._sock.settimeout(timeout)
            try:
                reply = self._read_packet()
            finally:
                self._sock.settimeout(old)
        stop = self._parse_stop_reply(reply)
        self._capture_stop_thread(stop.raw)
        return stop

    def _capture_stop_thread(self, raw: str) -> None:
        """Extract `thread:<hex>` from a T-packet so register reads can target it."""
        marker = "thread:"
        idx = raw.find(marker)
        if idx == -1:
            return
        rest = raw[idx + len(marker):]
        tid = ""
        for ch in rest:
            if ch in "0123456789abcdefABCDEF":
                tid += ch
            else:
                break
        if tid:
            self._stop_thread = tid

    def _select_stop_thread(self) -> None:
        """`Hg<tid>` — point subsequent g/p reads at the thread that stopped.

        Cemu's stub answers `g`/`p` against a default thread (often a parked
        coreinit thread), NOT the breakpointed one, so without this the PC and
        GPRs are from the wrong context. Best-effort: if the stub rejects Hg we
        stop trying and fall back to the default thread."""
        if not self._hg_ok or self._stop_thread is None:
            return
        try:
            reply = self.cmd(f"Hg{self._stop_thread}")
            if reply not in ("OK", ""):
                # Some stubs echo nothing useful; treat non-OK as unsupported only
                # if it's an error packet (cmd would have raised on E##).
                pass
        except GdbRspError:
            self._hg_ok = False

    @staticmethod
    def _parse_stop_reply(reply: str) -> StopReason:
        # Format: T<sig><key>:<value>;... or S<sig> or W<exit> or X<sig>
        if reply.startswith("T") and len(reply) >= 3:
            return StopReason(signal=int(reply[1:3], 16), raw=reply)
        if reply.startswith("S") and len(reply) >= 3:
            return StopReason(signal=int(reply[1:3], 16), raw=reply)
        raise GdbRspError(f"unexpected stop reply: {reply!r}")

    # ----- common GDB packets (convenience) -----

    def query_halt_reason(self) -> StopReason:
        """`?` — what was the most recent halt reason?"""
        reply = self.cmd("?")
        stop = self._parse_stop_reply(reply)
        self._capture_stop_thread(stop.raw)
        return stop

    def cont(self) -> None:
        """`vCont;c` — resume ALL threads. Does NOT block waiting for stop.

        Cemu's stub requires vCont (not bare `c`) because Wii U has multiple
        CPU threads — bare `c` only resumes the current thread and the others
        stay halted, leaving the game frozen even though we got an OK ack."""
        with self._lock:
            self._send_packet("vCont;c")
        # caller should `wait_for_stop` if they want to block

    def step(self) -> StopReason:
        """`vCont;s` — single-step one instruction. Blocks for the resulting stop.

        Uses vCont for the same reason as `cont()` — Cemu needs vCont semantics
        for multi-thread targets."""
        with self._lock:
            self._send_packet("vCont;s")
            reply = self._read_packet()
        stop = self._parse_stop_reply(reply)
        self._capture_stop_thread(stop.raw)
        return stop

    def read_mem(self, addr: int, length: int) -> bytes:
        """`m <addr>,<len>` — read raw bytes from target memory."""
        reply = self.cmd(f"m{addr:x},{length:x}")
        return bytes.fromhex(reply)

    def write_mem(self, addr: int, data: bytes) -> None:
        """`M <addr>,<len>:<hex>` — write hex-encoded bytes to target memory."""
        hex_data = data.hex()
        reply = self.cmd(f"M{addr:x},{len(data):x}:{hex_data}")
        if reply != "OK":
            raise GdbRspError(f"write_mem failed: {reply}")

    def read_register(self, regnum: int) -> bytes:
        """`p <n>` — read one register. Returns raw bytes (target endian).

        Selects the stopped thread first so the value comes from the
        breakpointed context, not the stub's default thread."""
        self._select_stop_thread()
        reply = self.cmd(f"p{regnum:x}")
        return bytes.fromhex(reply)

    def read_all_registers(self) -> bytes:
        """`g` — read full register file as raw bytes (target endian).

        Selects the stopped thread first (see read_register)."""
        self._select_stop_thread()
        reply = self.cmd("g")
        return bytes.fromhex(reply)

    def write_register(self, regnum: int, value: bytes) -> None:
        reply = self.cmd(f"P{regnum:x}={value.hex()}")
        if reply != "OK":
            raise GdbRspError(f"write_register failed: {reply}")

    def add_breakpoint(self, addr: int, kind: int = 4) -> None:
        """`Z0` — software breakpoint. `kind` is the PowerPC instruction width (4)."""
        reply = self.cmd(f"Z0,{addr:x},{kind}")
        if reply != "OK":
            raise GdbRspError(f"add_breakpoint failed: {reply}")

    def remove_breakpoint(self, addr: int, kind: int = 4) -> None:
        reply = self.cmd(f"z0,{addr:x},{kind}")
        if reply != "OK":
            raise GdbRspError(f"remove_breakpoint failed: {reply}")

    def add_watchpoint(self, addr: int, length: int, kind: str = "w") -> None:
        """`Z2/3/4` — write/read/access watchpoint. `kind` in {'w','r','a'}."""
        z = {"w": 2, "r": 3, "a": 4}[kind]
        reply = self.cmd(f"Z{z},{addr:x},{length:x}")
        if reply != "OK":
            raise GdbRspError(f"add_watchpoint failed: {reply}")

    def remove_watchpoint(self, addr: int, length: int, kind: str = "w") -> None:
        z = {"w": 2, "r": 3, "a": 4}[kind]
        reply = self.cmd(f"z{z},{addr:x},{length:x}")
        if reply != "OK":
            raise GdbRspError(f"remove_watchpoint failed: {reply}")
