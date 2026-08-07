# Cemu's GDB stub: what works, what doesn't

Notes from building cemu-re-mcp against Cemu 2.6 (May 2026), targeting Monster Hunter 3 Ultimate (`mh3g_cafe_us`). Posted because the Cemu GDB stub is sparsely documented and behaves differently from Dolphin's and from a textbook GDB stub.

If you're writing tooling against Cemu's stub, this is what I wish I'd known on day one.

## Enabling the stub

`Debug → Launch with GDB stub` in Cemu's menu. Default port is **1337**. The setting must be active when the game is loaded — Cemu only opens the listening socket during game initialization. Toggling it after a game is running has no effect.

## The behaviors that matter

### 1. The stub halts the game at boot

Until you connect and issue continue, the target is frozen at the first instruction. Cemu's window shows the splash but never advances. This is standard GDB-stub behavior but takes people by surprise — easy to assume Cemu has hung.

Helper: connect, send `vCont;c`, disconnect. Game then runs normally.

### 2. The stub is one-shot per game session

After any TCP disconnect from the stub, it stops listening. Reconnecting requires restarting Cemu and reloading the game (the listener only opens during game init).

Practical consequence: long-lived clients (like an MCP server) should connect once on startup and hold the socket open for the entire session. Test scripts that connect → do work → disconnect → reconnect will fail on the second connect.

### 3. `QStartNoAckMode` is not supported

The standard GDB no-ack negotiation hangs the stub instead of returning `OK` or empty. If you don't time out cleanly, you eat 5 seconds at every connect. Skip the negotiation and stay in always-ack mode.

### 4. Bare `c` does not resume all threads

Wii U has multiple CPU threads (often three: the foreground game thread plus background DSP/IO threads). The classic GDB `c` packet continues only the current thread. The other threads stay halted, and the game stays frozen even though you got `OK` back.

Use `vCont;c` (continue all) instead. Same for stepping — use `vCont;s` not `s`.

Cemu's `vCont?` reply confirms what's supported: `vCont;c;C;s;S`. No per-thread targeting (`c:thread-id`), just all-or-nothing.

### 5. There is no programmatic pause

This is the biggest surprise vs other GDB stubs. Cemu accepts both:

- `vCtrlC` (returns an empty reply, no stop event)
- Raw `0x03` interrupt byte (silently ignored)

Neither actually halts the target. The game keeps running.

To halt the game, you must either:
- **Pause from Cemu's UI** (the play/pause button), or
- **Set a breakpoint or watchpoint and let the target stop itself when the event fires**

This rules out the "halt, inspect, resume" workflow common in other emulator debuggers. You can still trace using breakpoints, which is the more useful pattern in practice — but if you need an arbitrary mid-execution snapshot, you'll need to wait until the target naturally enters a known function.

### 6. Inspection works while the target is running

This is the most unusual feature, and it compensates a lot for the lack of programmatic pause. Cemu's stub services these packets while the game is actively executing:

- `?` (halt reason — returns the last halt info even while running)
- `m <addr>,<len>` (memory read)
- `p <regnum>` (single register read)
- `g` (all registers read)

You can poll memory and registers continuously without ever pausing. This is the opposite of how Dolphin's stub works, and it's actually preferable for most reverse-engineering workflows — most reads don't need a consistent halted snapshot.

Untested: whether memory **writes** are accepted while running. The MCP code attempts them and assumes they work.

## Mapping summary

| Operation | Standard GDB | Cemu | Notes |
|---|---|---|---|
| Connect to stub | `:port` | `:1337` | Default port; one-shot per game session |
| Negotiate no-ack | `QStartNoAckMode` | not supported | Stays in always-ack mode |
| Continue all threads | `c` | `vCont;c` | Bare `c` only continues current thread |
| Single step | `s` | `vCont;s` | Same reason as above |
| Pause running target | `vCtrlC` or `0x03` | **not supported** | Use BPs or UI pause |
| Read memory | `m a,l` | works | Even while running |
| Read register | `p n` | works | Even while running |
| Software BP | `Z0,a,k` | works | `k=4` for PPC |
| Hardware watchpoint | `Z2/Z3/Z4` | works | Untested across many BPs |
| Read full register file | `g` | works | Even while running |
| Query stop reason | `?` | works | Even while running |
| Wait for stop after `c` | (passive read) | works | T-packet arrives normally |

## Cemu is open-source — patch the stub if needed

The GDB stub lives in `bin/Cafe/HW/Espresso/Debugger/GDBStub.cpp` (or similar — check current Cemu source). If your tooling needs working `vCtrlC` or persistent listening across disconnects, those are reasonable patches to upstream. The dolphin-re-mcp project did something similar for Dolphin's stub.

## Address space gotcha (Wii U vs Wii)

Coming from Wii / GameCube tooling, you might expect MEM1/MEM2 split addresses (0x80000000 / 0x90000000 ranges). Wii U is different: virtual addresses are flat 32-bit. For most games the main module loads around `0x02000000` and coreinit around `0x00E00000`. There's no MEM1/MEM2 distinction at the GDB protocol level — just read any virtual address.

For MH3U US v1.3, the main module `mh3g_cafe_us` loads at `0x02000000`, size `0x11b2550`. First two instructions are `nop; blr` (a common entry-stub pattern).

## Attaching (pymem): process selection

`PymemBridge.attach()` (and therefore the MCP's `pymem_attach_tool`, which just calls it) picks the Cemu process like this, first match wins:

1. explicit `pid=` arg, or env **`CEMU_PID`** — attach to that exact PID;
2. explicit `process=` arg, or env **`CEMU_PROC`** — attach by exe name;
3. otherwise auto-try **`Cemu.exe`** then **`Cemu_release.exe`**.

The guest region is then located by its entry signature, so it works for any build regardless of how we attached.

Why this matters:

- **Stock Cemu is `Cemu.exe`; the MH3U online fork is `Cemu_release.exe`** (built at `E:\Cemu-src\bin`). `attach()` used to be hardcoded to `Cemu.exe` and threw `Cemu.exe not found` against the fork — the old "attach by PID" workaround. The auto-try now handles the online build with no config.
- **Multiple Cemu instances** (e.g. an online host + a guest on one machine, or a stock build + the fork side by side): the name-based attach grabs whichever pymem finds first, which is ambiguous. To target a specific one, set **`CEMU_PID`** (or `CEMU_PROC`) in the MCP server's environment *before it starts* — the bridge reads env at `attach()` time, and `pymem_attach_tool` takes no args. For the standalone cheat scripts use the `--pid` flag (e.g. `scripts/mh3u_trainer.py --list` then `--pid <n>`).

## Acknowledgements

- [Kuriimu2](https://github.com/FanTranslatorsInternational/Kuriimu2) — for confirming the MT Framework ARC `dsize_flag` encoding (an adjacent project but related to the same RE work)
- The GDB Remote Serial Protocol reference: <https://sourceware.org/gdb/current/onlinedocs/gdb.html/Remote-Protocol.html>
- Anyone who's documented Cemu internals on the cemu-project Discord — most of what's here was discovered by running into walls and probing
