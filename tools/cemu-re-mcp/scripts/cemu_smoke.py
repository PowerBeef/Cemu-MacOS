"""Live end-to-end smoke test against a running Cemu + MH3U session.

Sequence:
  1. Connect
  2. Interrupt (pause)
  3. Read 16 bytes from 0x02000000 (start of mh3g_cafe_us module)
  4. Read PC + LR
  5. Resume (vCont;c)
  6. Disconnect

This is the canonical "does the MCP layer actually work end-to-end" test.
"""
import struct
from cemu_re_mcp.gdb_rsp import GdbRsp

c = GdbRsp(host="127.0.0.1", port=1337, timeout=5.0)
print("connecting ...")
c.connect()
print("connected")

print("\n[1] interrupting target ...")
c.interrupt()
stop = c.wait_for_stop(timeout=2.0)
print(f"  halted: signal={stop.signal} raw={stop.raw!r}")

print("\n[2] reading 16 bytes from 0x02000000 (mh3g_cafe_us start) ...")
data = c.read_mem(0x02000000, 16)
print(f"  bytes: {data.hex()}")
print(f"  as PPC instructions (BE u32):")
for i in range(0, 16, 4):
    word = struct.unpack(">I", data[i:i+4])[0]
    print(f"    0x{0x02000000 + i:08x}: 0x{word:08x}")

print("\n[3] reading PC + LR via register packets ...")
try:
    pc_raw = c.read_register(64)
    pc = struct.unpack(">I", pc_raw)[0]
    print(f"  PC (reg 64): 0x{pc:08x}")
except Exception as e:
    print(f"  PC read failed: {e}")
try:
    lr_raw = c.read_register(67)
    lr = struct.unpack(">I", lr_raw)[0]
    print(f"  LR (reg 67): 0x{lr:08x}")
except Exception as e:
    print(f"  LR read failed: {e}")

print("\n[4] resuming target ...")
c.cont()
print("  resumed")

c.close()
print("\ndone — connection closed cleanly")
