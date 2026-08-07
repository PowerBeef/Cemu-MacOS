"""Probe Cemu's GDB stub capabilities when the target is running.

Tries each query, reports response or timeout. Run while MH3U is in-game (NOT
halted at splash) to see what the stub services while target runs.
"""
import socket
from cemu_re_mcp.gdb_rsp import GdbRsp, GdbRspError

c = GdbRsp(host="127.0.0.1", port=1337, timeout=1.5)
print("connecting (target should be running) ...")
c.connect()
print("connected\n")

def try_cmd(label, payload):
    print(f"[{label}] {payload!r}")
    try:
        reply = c.cmd(payload)
        print(f"  reply: {reply[:200]!r}")
    except socket.timeout:
        print(f"  TIMEOUT (stub didn't reply)")
    except GdbRspError as e:
        print(f"  RSP error: {e}")
    except Exception as e:
        print(f"  other error: {type(e).__name__}: {e}")
    print()

# Capability/feature queries — usually safe while running
try_cmd("qSupported", "qSupported:multiprocess+;swbreak+;hwbreak+;vContSupported+")
try_cmd("vCont?", "vCont?")
try_cmd("?", "?")

# Memory read while running — does Cemu allow this?
try_cmd("read 16B @ 0x02000000", "m02000000,10")

# Register read while running?
try_cmd("read PC (reg 64)", "p40")
try_cmd("read all regs (g)", "g")

# Try the alternate interrupt: vCtrlC packet (some stubs use this)
try_cmd("vCtrlC", "vCtrlC")

c.close()
print("disconnected")
