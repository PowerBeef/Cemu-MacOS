"""Single-session full probe of Cemu's GDB stub.

ONE connection, never disconnects until end. Sequence:
  1. Connect
  2. Confirm halted at splash
  3. vCont;c to let game start
  4. Wait 3 sec (let game advance past splash)
  5. Probe: do queries work while target is RUNNING?
  6. Try vCtrlC for clean pause
  7. If paused: read mem + regs, then vCont;c again
  8. Disconnect

Run this ONCE per Cemu launch. After disconnect, stub stops listening.
"""
import socket
import struct
import time
from cemu_re_mcp.gdb_rsp import GdbRsp, GdbRspError

c = GdbRsp(host="127.0.0.1", port=1337, timeout=2.0)
print("[1] connecting ...")
c.connect()
print("    connected\n")

print("[2] confirming halted at splash ...")
stop = c.query_halt_reason()
print(f"    halted: signal={stop.signal} raw={stop.raw!r}\n")

print("[3] vCont;c (resume from splash halt) ...")
c.cont()
print("    resume sent — game should be advancing now\n")

print("[4] waiting 3s for game to settle ...")
time.sleep(3.0)
print()

def try_cmd(label, payload, ignore_timeout=False):
    print(f"    [{label}] {payload!r}")
    try:
        reply = c.cmd(payload)
        print(f"      reply: {reply[:200]!r}")
        return reply
    except socket.timeout:
        if ignore_timeout:
            print(f"      TIMEOUT (expected — stub doesn't service while running)")
        else:
            print(f"      TIMEOUT")
        return None
    except GdbRspError as e:
        print(f"      RSP error: {e}")
        return None
    except Exception as e:
        print(f"      {type(e).__name__}: {e}")
        return None

print("[5] probing what works while target is RUNNING ...")
try_cmd("halt-reason", "?", ignore_timeout=True)
try_cmd("read 16B @ 0x02000000", "m02000000,10", ignore_timeout=True)
try_cmd("read PC", "p40", ignore_timeout=True)
print()

print("[6] trying vCtrlC for clean pause ...")
vctrlc_reply = try_cmd("vCtrlC", "vCtrlC")
if vctrlc_reply is not None:
    print("    vCtrlC accepted — checking if target is now halted ...")
    try:
        stop = c.wait_for_stop(timeout=2.0)
        print(f"    halted via vCtrlC: signal={stop.signal} raw={stop.raw!r}")
    except Exception as e:
        print(f"    no stop reply: {e}")
else:
    print("    vCtrlC not supported, trying raw 0x03 interrupt byte ...")
    c.interrupt()
    try:
        stop = c.wait_for_stop(timeout=2.0)
        print(f"    halted via 0x03: signal={stop.signal} raw={stop.raw!r}")
    except Exception as e:
        print(f"    0x03 also didn't halt the target: {e}")
print()

print("[7] attempting memory + register reads (will succeed only if halted) ...")
try:
    data = c.read_mem(0x02000000, 16)
    print(f"    bytes @ 0x02000000: {data.hex()}")
    for i in range(0, 16, 4):
        word = struct.unpack(">I", data[i:i+4])[0]
        print(f"      0x{0x02000000 + i:08x}: 0x{word:08x}")
except Exception as e:
    print(f"    mem read failed: {e}")
try:
    pc_raw = c.read_register(64)
    pc = struct.unpack(">I", pc_raw)[0]
    print(f"    PC: 0x{pc:08x}")
except Exception as e:
    print(f"    PC read failed: {e}")
print()

print("[8] resuming + disconnecting ...")
try:
    c.cont()
    print("    vCont;c sent")
except Exception as e:
    print(f"    resume failed (target may already be running): {e}")
c.close()
print("    disconnected")
