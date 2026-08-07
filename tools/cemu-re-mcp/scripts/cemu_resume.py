"""Connect-and-resume helper.

Connects to Cemu GDB stub, queries state, issues vCont;c (resume all threads),
disconnects cleanly. Run this when Cemu is frozen on splash because the GDB
stub halted on launch.

Usage:
    python scripts/cemu_resume.py [port]
"""
import sys
from cemu_re_mcp.gdb_rsp import GdbRsp

port = int(sys.argv[1]) if len(sys.argv) > 1 else 1337

c = GdbRsp(host="127.0.0.1", port=port, timeout=3.0)
print(f"connecting to 127.0.0.1:{port} ...")
c.connect()
print("connected")

# Query halt reason
try:
    stop = c.query_halt_reason()
    print(f"halted: signal={stop.signal}, raw={stop.raw!r}")
except Exception as e:
    print(f"halt query failed: {e}")

# Try vCont? to see what the stub supports
try:
    vcont_supported = c.cmd("vCont?")
    print(f"vCont supported actions: {vcont_supported!r}")
except Exception as e:
    print(f"vCont? failed (stub may not advertise): {e}")

# Send vCont;c (continue ALL threads, no thread-specific binding)
print("\nsending vCont;c (continue all threads) ...")
try:
    # vCont;c expects no reply until next stop event, so just send the packet
    with c._lock:
        c._send_packet("vCont;c")
    print("vCont;c sent — game should resume now")
except Exception as e:
    print(f"vCont;c errored: {e}")
    print("falling back to bare 'c' ...")
    try:
        c.cont()
        print("'c' sent")
    except Exception as e2:
        print(f"'c' also failed: {e2}")

# Disconnect cleanly (do NOT send 'k' kill packet)
c.close()
print("disconnected cleanly")
