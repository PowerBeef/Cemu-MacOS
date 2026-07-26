# 07 — Audio: the DSP and AX

Latte carries a custom **DSP** on-die, inherited in spirit from the GameCube/Wii audio processor. It
handles voice decoding, resampling, mixing and effects, feeding three independent output endpoints
`[RE]`. The guest talks to it through **AX** (`snd_core` / `sndcore2`), with a sequencer layer
(`snd_user` / `snduser2`) on top.

We do not emulate the DSP. AX is HLE'd: voices are mixed in C++ and handed to the host audio backend
(cubeb). What matters for accuracy is therefore AX's *observable model* — frame cadence, voice
limits, mix topology — not the DSP's microcode.

## The frame model

AX runs on a **3 ms frame**, which is the single most important number here `[SRC ax.h:15-17]`:

```cpp
const int AX_RENDERER_FREQ_32KHZ = 0;
const int AX_RENDERER_FREQ_48KHZ = 1;
const int AX_FRAMELENGTH_3MS     = 0;

const int AX_SAMPLES_PER_3MS_48KHZ = 144;   // 48000 * 3 / 1000
const int AX_SAMPLES_PER_3MS_32KHZ = 96;    // 32000 * 3 / 1000
const int AX_SAMPLES_MAX           = AX_SAMPLES_PER_3MS_48KHZ;
```

So a title picks 32 kHz or 48 kHz at `AXInit` time, and thereafter the DSP produces a block every
**3 ms** — roughly **333 Hz**, or about 5.5 audio frames per 60 Hz video frame. Titles register
frame callbacks (`AX_APP_FRAME_CALLBACK_MAX = 64`) and do their per-frame voice bookkeeping there.

> **This cadence is not driven by a timer in this emulator.** `AXOut_update()` is called from
> `__OSCheckSystemEvents()`, which runs on the main core's **idle loop**
> `[SRC coreinit_Thread.cpp:1218-1226]`. The idle loop parks with a 250 µs bound, so audio is
> serviced at idle-loop granularity rather than on a 3 ms clock. Under heavy CPU load, when the
> main core is rarely idle, audio update rate degrades — a plausible and unverified explanation for
> audio glitching correlating with frame drops. See chapter 09.

## Output endpoints

Three devices, with different channel counts `[SRC ax.h:19-38]`:

| Device | Constant | Channels |
|---|---|---|
| TV | `AX_DEV_TV = 0` | **6** (`AX_TV_CHANNEL_COUNT`) |
| GamePad / DRC | `AX_DEV_DRC = 1` | **4** (`AX_DRC_CHANNEL_COUNT`) |
| Wii Remote | `AX_DEV_RMT = 2` | **1** (`AX_RMT_CHANNEL_COUNT`) |

The Wiimote endpoint is low-quality by design — it feeds the remote's small speaker.

Output modes `[SRC ax.h:42-46]`: `AX_MODE_STEREO`, `SURROUND`, `DPL2` (Dolby Pro Logic II),
`6CH`, `MONO`.

## Voices

| | Value |
|---|---|
| Maximum voices | **96** (`AX_MAX_VOICES`) |
| Priority range | 0–32; `AX_PRIORITY_FREE = 0`, `AX_PRIORITY_LOWEST = 1`, `AX_PRIORITY_NODROP = 31` |
| Sample formats | `ADPCM (0x0)`, `PCM16 (0xA)`, `PCM8 (0x19)` |
| Aux buses | 3 (`AX_AUX_BUS_COUNT`), 4 total buses (`AX_MAX_NUM_BUS`) |

`[SRC ax.h:48-59]`

The priority system is a voice-stealing scheme: when all 96 voices are in use, a new voice can
displace a lower-priority one, and `AX_PRIORITY_NODROP` marks a voice that must never be stolen.

Two upsample-stage placements exist — `AX_UPSAMPLE_STAGE_BEFORE_FINALMIX` and `..._AFTER_FINALMIX`
`[SRC ax.h:24-25]` — which matters when a title mixes at 32 kHz but outputs at 48 kHz.

## Implementation layout

`src/Cafe/OS/libs/snd_core/`:

| File | Role |
|---|---|
| `ax_exports.cpp` | RPL export registration (`snd_core` **and** `sndcore2`) |
| `ax_voice.cpp` | Voice state, ADPCM/PCM decode, sample-rate conversion |
| `ax_mix.cpp` | The mixer |
| `ax_aux.cpp` | Aux bus routing |
| `ax_out.cpp` | Device output, `AXOut_update` |
| `ax_ist.cpp` | Frame callbacks / interrupt service |
| `ax_multivoice.cpp` | Multi-voice (stereo/surround source) handling |

139 registration sites across `snd_core`+`sndcore2` (≈70 distinct functions, registered under both
RPL names). `snd_user`/`snduser2` adds 61 sites (≈31 distinct) for the MIDI/sequencer layer.

**33 debug asserts** in `snd_core` and 23 `todo` markers — the fourth-most-incomplete library in the
tree after nsysnet, coreinit and h264.

## Host side

Output goes through **cubeb** (`src/audio/`). One known defect is recorded in the porting plan: the
cubeb realtime callback used a `std::vector` plus a mutex — a mutex on the CoreAudio HAL thread,
which runs above every QoS class, plus an O(n) `erase` from the front. The fix (a lock-free SPSC
ring, and latency clamped to [480, 1920] frames) is planned but not landed
`[SRC ../porting/01-foundation-platform-packaging.md]`.

## Modelled / Approximated / Absent

| Behaviour | Status | Note |
|---|---|---|
| AX voice model (96 voices, priorities) | **Modelled** | |
| ADPCM / PCM16 / PCM8 decode | **Modelled** | |
| Mixing, aux buses, final mix | **Modelled** | |
| Three output endpoints (TV/DRC/RMT) | **Modelled** | |
| Output modes incl. DPL2 | **Approximated** | |
| Sample-rate conversion | **Approximated** | |
| **3 ms frame cadence** | **Approximated** | Driven by the idle loop at ≥250 µs granularity, not a 3 ms clock → ch. 09 |
| Frame callbacks | **Modelled** | Up to 64 |
| DSP as hardware (microcode, cycle cost) | **Absent** | Fully HLE'd; a title cannot overrun the DSP here the way it could on console |
| DSP-side effects (reverb etc.) | **Approximated** | `[HW]` notes filtering/effects are software on hardware too, at CPU expense |
| Wiimote speaker output | **Approximated** | |
| Audio-thread realtime safety (host) | **Approximated** | Mutex in the cubeb callback — known defect, fix planned |
