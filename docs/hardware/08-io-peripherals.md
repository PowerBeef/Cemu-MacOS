# 08 — I/O and peripherals

**Deliberately shallow.** This chapter exists to mark boundaries, not to document protocols. Almost
everything here is HLE'd at the library level, so the hardware beneath it is unreachable from a
title and never needs modelling. Where something *does* matter for accuracy, it is flagged and
carried into chapter 09.

## Starbuck and IOSU

The Wii U has a second processor: **Starbuck**, an ARM926EJ-S, the direct descendant of the Wii's
Starlet `[RE]`. It owns all I/O and all security. It has 96 KB of SRAM and access to MEM0's 3 MB of
1T-SRAM, and runs **IOSU** — a multithreaded microkernel supporting up to 180 threads (up from 100
on Wii), with driver modules for storage, networking, crypto and the disc drive `[RE]`.

Espresso does not touch hardware directly for any of this. It sends **IPC** messages to IOSU and
waits for replies. Every `FSReadFile`, every socket operation, every title-key decryption is an IPC
round trip on real hardware.

**We model none of it.** `coreinit_IPC.cpp` and `coreinit_IOS.cpp` present the IPC surface, but the
services behind it are implemented directly in C++ against the host filesystem and host sockets.
There is no IOSU. This is the right call — emulating a second OS to reach a `read()` would be
enormous and observationally pointless — but it has one real consequence: **IPC latency is absent**,
and a title that overlaps I/O with computation on the assumption that IPC is slow will see different
timing here.

`coreinit_IPC.cpp` carries the note *"we should wait for an event instead of busylooping"* at one of
its two `cemu_assert_unimplemented()` sites.

## Filesystem

`coreinit_FS.cpp` — 92 exports, 2800+ lines, the largest single library file in the tree. It
implements both layers:

- **`FS*`** — the title-facing API: open/read/write/seek/stat/dir/truncate/rename/remove/mkdir/
  chdir/getcwd/isEof/flushQuota/volumeState/userData, in synchronous and asynchronous forms.
- **`FSA*`** — the lower layer that would talk to IOSU: `FSAInit`, `FSAAddClient`, `FSAMount`,
  `FSAOpenFileEx`, `FSAReadFile`, `FSAGetStat`, `FSAGetDeviceInfo`, `FSAChangeMode`, …

Nine `cemu_assert_unimplemented()` sites live here — more than any other file. Storage media (8/32 GB
eMMC NAND, SD, USB, the 22.5 MB/s optical drive `[RE]`) are abstracted away entirely; there is no
seek-time or throughput model.

## The GamePad (DRC)

The most interesting peripheral on the system, and the least relevant to us. It is three computers
in a plastic shell `[RE]`:

| Component | Part | Role |
|---|---|---|
| SoC | DRC-WUP, ARM926EJ-S, 4 MB RAM | H.264 decode of the streamed video |
| Wi-Fi | Broadcom BCM4319, Cortex-M3 | 802.11n, **5 GHz**, custom WPA2-PSK |
| I/O | STMicro STM8 | buttons, sensors, 2 KB + 28 KB EEPROM |

The console encodes the DRC framebuffer to H.264 and streams it at 60 Hz over a dedicated 5 GHz
link. Screen resolution is 854×480.

For us this collapses to: **render a second view and show it in a window.** `DisplayDRCEnabled`
selects whether we do. No encoding, no radio, no latency model.

Input is `vpad` (36 exports). The `VPADStatus` structure is 0xAC bytes and is the complete
guest-visible pad state — buttons, sticks, touch, accelerometer, gyro, magnetometer, battery,
volume slider.

## Wii Remotes and other controllers

`padscore` (30 exports) implements **KPAD** (the cooked, calibrated API) and **WPAD** (the raw one),
covering Wii Remote, Nunchuk, Classic Controller and the Wii U Pro Controller. Bluetooth is not
modelled; host controllers are mapped through `src/input/`.

`controllerProfiles/` ships **empty**, which is why input appears not to work at all out of the box
— the profile must be created before any button does anything. Profiles are XML, and button values
are **macOS virtual key codes** for keyboard profiles, because `wxKeyEvent::GetRawKeyCode()` is a
pass-through outside Windows.

## USB and other devices

| Library | Exports | Notes |
|---|---|---|
| `nsyshid` | 9 (+18 backend files) | USB HID — Skylanders/Infinity portals and similar |
| `nsyskbd` | 2 | USB keyboard |
| `mic` | 7 | Microphone |
| `camera` | 7 | Front camera |
| `nfc` / `ntag` / `nn_nfp` | 13 / 11 / 25 | amiibo |

## Networking

`nsysnet` (42 exports) provides BSD sockets plus **NSSL**, the TLS layer. It carries **74 debug
asserts** — the highest count of any library — because it bails on most unhandled socket options and
flags. `nlibcurl` (25) is a libcurl shim; `nlibnss` (2) exports device certificates and its one real
function is unimplemented.

The `nn_*` online stack (`nn_act`, `nn_fp`, `nn_boss`, `nn_olv`, `nn_ec`, `nn_nim`, …) sits above
this and is out of scope for accuracy work on rendering and timing.

## Hardware register blocks

For completeness, the state of the raw MMIO blocks under `src/Cafe/HW/` (see chapter 02 for
addresses):

| Block | Status |
|---|---|
| `SI` (serial / controllers) | 4-channel interface partially implemented |
| `ACR` | VI-register indirection; reads/writes logged and discarded |
| `VI` (video interface) | **Empty stub** — the only registration is commented out |
| `AI` (audio interface) | **`AI.h` is an empty file** |

None of these are reached on a normal path, because VPAD, GX2 and AX are all HLE'd above them. They
would only matter for a title that programmed the hardware directly, which no retail Wii U title
does.

## Modelled / Approximated / Absent

| Behaviour | Status | Note |
|---|---|---|
| `FS*` / `FSA*` filesystem API | **Approximated** | 9 unimplemented sites; host FS underneath |
| Storage timing (NAND, disc, USB) | **Absent** | No seek or throughput model |
| IOSU / Starbuck | **Absent** | Services implemented directly; **no IPC latency** |
| IPC message surface | **Approximated** | Present, but nothing real behind it |
| DRC video streaming (H.264, 5 GHz) | **Absent** | Second render target, shown in a window |
| DRC input (`VPADStatus`, 0xAC bytes) | **Modelled** | |
| KPAD / WPAD | **Approximated** | No Bluetooth; host controllers mapped |
| Controller profiles | **Emulator-specific** | Ships empty — a first-run trap, not a hardware fact |
| USB HID, keyboard, mic, camera | **Approximated** | |
| NFC / amiibo | **Approximated** | |
| Sockets / TLS | **Approximated** | 74 asserts on unhandled paths |
| `VI` / `AI` register blocks | **Absent** | Empty; unreachable because GX2/AX are HLE'd |
| `SI` / `ACR` | **Approximated** | ACR discards writes |
