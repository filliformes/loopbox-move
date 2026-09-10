# The Schwung Overtake SDK — Reverse-Engineered Reference

> **Status:** reference doc, compiled 2026-09-09 from source analysis of six shipping
> Overtake modules. There is **no official Overtake SDK, header, or scaffold** — this file
> *is* the documentation. Every fact below was read out of real code; the source module is
> cited in parentheses.
>
> **Sources analyzed:**
> - `charlesvestal/schwung-performance-fx` (v0.1.2) — 32 pressure punch-in FX. The framework
>   author's reference; ancestor of our own `punchfx-move`.
> - `timncox/schwung-mark` (v0.5.1) — **5-track RC-505 live looper. Our primary template.**
> - `timncox/schwung-smack` / `oversmack` (v0.15.2) — input loop glitcher/slicer.
> - `timncox/schwung-work` → `overwork` + `overwork-mix` (v0.12.0) — 8-track engine + p-lock
>   step sequencer; and an `end_of_chain` finalizer sibling.
> - `jrucho/schwung-twinsampler` (v0.3.7) — dual-grid sampler; lift-ready loop-voice engine.

---

## 1. What an Overtake module *is*

An Overtake module takes over the Move's **entire surface**: the 128×64 screen, all 32 pad
LEDs, the step buttons, knobs, and transport buttons. Unlike a regular `sound_generator` /
`audio_fx` / `midi_fx` (whose UI is host-drawn knob pages), an Overtake module draws its own
screen and lights its own pads.

**Crucially, there is no special Overtake C API.** A module is Overtake because of two things:
1. `module.json` says `"component_type": "overtake"` and names a `"ui"` file.
2. It ships a QuickJS `ui.js` that the host runs full-screen.

The DSP half is a **completely ordinary plugin** — the same ABI a `sound_generator` or
`audio_fx` uses. (Confirmed identical `plugin_api_v1.h` across all repos; none mentions
"overtake".)

### Three files, one install dir
```
/data/UserData/schwung/modules/overtake/<id>/
├── module.json     # manifest
├── dsp.so          # the audio engine (ordinary plugin ABI)
├── ui.js           # QuickJS UI: screen + LEDs + input + DSP bridge
├── help.json       # in-app help (optional)
└── web_ui.html     # browser editor on the Manager "Tool" tab (optional)
```

### Role is decided by the exported symbol, not by `component_type`
The host `dlsym`s the `.so` to decide how to feed it audio (`schwung-work/CLAUDE.md`:
*"Role is decided by the .so's exports, not by component_type"*):

| Exported init symbol | ABI | Role | Fed by |
|---|---|---|---|
| `move_plugin_init_v2` | `plugin_api_v2_t` (`render_block`) | **generator / jack** | mic/line-in via `mapped_memory + audio_in_offset` |
| `move_audio_fx_init_v2` | `audio_fx_api_v2_t` (`process_block`, in-place) | **end-of-chain FX** | the whole Move+Schwung mix, in place (needs `end_of_chain:true`) |

**A live looper that records the input jack uses the generator role → `move_plugin_init_v2`**
(same as Mark, Smack, Overwork, TwinSampler).

---

## 2. `module.json`

Canonical shape (merging Mark + Performance FX; the fields we'll use):
```json
{
  "id": "loopbox",
  "name": "LoopBox",
  "abbrev": "LBX",
  "version": "0.4.0",
  "description": "...",
  "author": "Filliformes",
  "dsp": "dsp.so",
  "ui": "ui.js",
  "api_version": 2,
  "component_type": "overtake",
  "capabilities": {
    "audio_in": true,
    "audio_out": true,
    "midi_in": true,
    "midi_out": true,
    "aftertouch": true,
    "claims_master_knob": false,
    "suspend_keeps_js": true,
    "button_passthrough": [ 85 ]
  }
}
```

| Field | Meaning |
|---|---|
| `component_type: "overtake"` | **The switch.** Host gives this module the whole surface. |
| `ui: "ui.js"` | Names the JS module run full-screen. (Regular plugins have no `ui`; a chain build uses `ui_chain`.) |
| `dsp: "dsp.so"` | The engine `.so`. |
| `api_version: 2` | Manifest/ABI version. **Never** use v1. |
| `audio_in` / `audio_out` | Reads/writes the audio bus. `true` for a looper. |
| `midi_in` / `midi_out` | Receive pad/CC; `midi_out:true` to echo to external gear. |
| `aftertouch: true` | Deliver poly (`0xA0`) + channel (`0xD0`) pressure. |
| `claims_master_knob: false` | Leave the Move volume/master encoder (CC 79) to the host. |
| **`suspend_keeps_js: true`** | **Essential for a looper** — leaving the module *suspends* (Back) rather than tears down; JS + DSP stay alive so loops keep playing. Drives `onResume`. |
| **`button_passthrough: [85]`** | Note/CC **85 = Move Play** still reaches Move firmware, so **transport/clock keep running** under your UI. |
| `end_of_chain: true` | *(FX role only)* run on the final Move mix. Needs host ≥ 0.12.0. Not for the looper's generator role. |
| `requires_continuous_processing: true` | Keep the DSP's `process_block` running even when no audio is passing (Forgetful) — needed for tails, decaying memories, and free-running glitch/stumble steps. Relevant if we add the Stumble page or "forgetting" loops. |

- **`min_host_version` is NOT in module.json** — it lives in the catalog entry. For a new
  Overtake module targeting current features, recommend `"1.3.0"` in the catalog.
- **Divergence to know:** `jrucho/twinsampler` uses `"raw_ui": true` and omits
  `suspend_keeps_js`; `charlesvestal`/`timncox` use `suspend_keeps_js` and omit `raw_ui`.
  **Follow the framework author (charlesvestal): `component_type:"overtake"` is the real
  switch; use `suspend_keeps_js`.**

---

## 3. Pad & button LEDs (driven from JS)

LEDs are lit from `ui.js`, not from C. Import the host helpers:
```js
import { setLED, setButtonLED, decodeDelta } from '/data/UserData/schwung/shared/input_filter.mjs';
import { Black, White, Red, BrightRed, Green, BrightGreen, Cyan, Purple, YellowGreen,
         OrangeRed, LightGrey /* … */ } from '/data/UserData/schwung/shared/constants.mjs';
```

- **`setLED(note, color, force)`** — lights a pad. Internally a framed pad note-on
  `move_midi_internal_send([0x09, 0x90, note, color])` where **`color` is the velocity byte =
  a Move palette index 0–127** (NOT RGB). `Black = 0`. `force` bypasses the LED diff-cache
  (needed on resume/exit).
- **`setButtonLED(cc, level)`** — transport/utility button LED via CC (`0x0B, 0xB0, cc, level`),
  white levels `Off/Dim/Medium/Bright`.

### The 32-pad note map (4 rows × 8 cols) — notes **68–99**
```
Row 4 (top):    92 93 94 95 96 97 98 99
Row 3:          84 85 86 87 88 89 90 91
Row 2:          76 77 78 79 80 81 82 83
Row 1 (bottom): 68 69 70 71 72 73 74 75
```
Column→section helper (TwinSampler `padNoteFor`): `note = 68 + row*8 + col`. So **left 4×4 =
columns 0–3, right 4×4 = columns 4–7** — exactly our "left 16 loops / right 16 FX" split.

### Step buttons = notes **16–31** (16 of them)
A ready-made 16-cell selector row (used by Mark/Overwork/Smack for track/bank/scene select).

### LED discipline (non-obvious; you WILL get ghost LEDs without it)
The host LED queue drains only ~8–16 writes/tick. Every module implements:
- **Batched painting:** push `[note,color]` pairs, drain **~8 per tick** (`LEDS_PER_TICK`/
  `LEDS_PER_FRAME = 8`). A full 32-pad repaint takes several ticks.
- **Resume repaint:** on `onResume`, LEDs come back cleared and the host queues an all-off
  sweep — so **force-repaint + schedule ~3 spaced resync passes** (`LED_RESYNC_PASSES=3`,
  Mark/Smack/Overwork all do this).
- **Clear on exit:** the host does *not* restore surfaces an Overtake module lit — call a
  `clearAllModuleLEDs()` (all to Black, `force=true`) in `onUnload` and before
  `host_exit_module()`.
- Colors reflect state (looper mapping): `dim (0x10)` = empty, `BrightRed` = recording,
  `Green` = playing, `YellowGreen` = overdub, `White` = stopped/has-material; **blink**
  scheduled actions via a tick counter (`(tickCount % 8) < 4`).

---

## 4. Screen — 128×64 monochrome, immediate mode

Host globals (no import needed), called every tick, gated behind a dirty flag:
```js
clear_screen();
print(x, y, text, color);          // color 0/1; ~21 chars wide per row; rows at y=0,10,20,30,40,50
draw_line(x0,y0,x1,y1, color);
draw_rect(x,y,w,h, color);
fill_rect(x,y,w,h, color);
const w = text_width(text);
host_flush_display();              // push buffer to hardware — call at end of tick()
```
- **Multi-track strip trick (Overwork):** draw the track row *inside the 1px header rule*
  (`fill_rect(0,9,128,1,1)`), `cell = Math.floor(128/nTracks)`, selected = solid block,
  has-material = underline. At **16 tracks → 8px cells, still legible** (`drawTrackStrip`
  bails only when `cell < 3`). Philosophy (Overwork `DESIGN-8TRACK.md`): *"Tracks live on the
  screen, pads are for doing."*
- View state machine: `boot | menu | help | browser | fx | main | park` — dispatch in `draw()`.
- Boot without a freeze: a **cooperative boot job queue** (`BOOT_JOBS_PER_TICK = 2`,
  TwinSampler) spreads heavy startup (session/sample load) across ticks.
- Strict-mode JS: an undeclared assignment throws and the host treats a handler exception
  as fatal (Overwork). QuickJS ES modules only.

---

## 5. Input

All input arrives at JS callbacks as `[status, d1, d2]` (or a 4-byte framed variant):
- **`globalThis.onMidiMessageInternal(data)`** — Move's own surface.
- **`globalThis.onMidiMessageExternal(data)`** — **USB controllers (our LaunchControl XL).**

Dispatch:
- **Pads:** `0x90 v>0` note-on / `0x80` or `0x90 v==0` note-off, notes **68–99**.
- **Pressure:** poly aftertouch `0xA0` (per pad) / channel `0xD0` (broadcast). Throttle to
  ~30 ms/pad so clock `0xF8`s aren't starved.
- **Knobs/encoders:** CC `0xB0`, `d1 ∈ MoveKnob1..MoveKnob8`, **relative** — decode with
  `decodeDelta(d2)` (1–63 = +1, 65–127 = −1). Shift held → a second knob layer.
- **Knob touch ("peek"):** capacitive touch = note-on on notes **0–9** (0=E1..7=E8,
  8=Master, 9=Jog) — show a value overlay without changing it.
- **Buttons (named CCs from `constants.mjs`):** `MoveShift 49`, `MoveMenu 50`, `MoveBack 51`,
  `MoveCapture 52`, `MoveUndo 56`, `MoveCopy 60`, `MoveMainKnob 14` (jog), `MovePlay 85`,
  `MoveRec 86`, `MoveLoop 87`, `MoveMute 88`, `MoveMaster 79`, arrows 44–47/`MoveLeft/Right`.
- **Clock:** `0xF8`/`0xFA`/`0xFB` — drop in JS; get tempo from the DSP/host instead.
- The C DSP's `on_midi` is often unused or handles only aftertouch — **control logic lives in
  JS**, which pushes params to the DSP.

---

## 6. The JS ↔ DSP bridge (string params) — and its traps

In Overtake mode the host shims these JS globals to `shadow_set_param(0, "overtake_dsp:"+key, v)`:
```js
host_module_set_param(key, val);                       // fire-and-forget, ONE mailbox slot
host_module_set_param_blocking(key, val, timeoutMs);   // ordered/critical
const v = host_module_get_param(key);                  // single read (~one SPI frame, slow)
const obj = host_module_get_params([k1,k2,...]);       // BULK read, up to 48 keys (Mark)
```

**Traps (every module works around these):**
1. **The mailbox is a single slot — rapid same-tick writes clobber.** Send **critical**
   transitions (record/play/clear) via `..._blocking` + a small retry list (the mailbox is
   busiest exactly when a pad is held hard and pressure is streaming). **Queue** continuous
   params (level/pan/feedback) and drain ~2/tick.
2. **Self-heal:** reconcile UI state against DSP truth every ~2 s (read a `state`/`active`
   param) to repair a dropped `*_off`.
3. **The DSP must answer `get_param("module_id")`** with the module id or the Manager shows
   "no tool loaded."
4. **`get_param` has a 16 KB return ceiling** and a low single-key read rate (~44/s vs the
   ~224/s a 16-track UI needs). → use **`host_module_get_params` bulk reads**, and serialize
   large state as a **base64-windowed blob** (Overwork `DESIGN-8TRACK.md`).
5. `get_param` returns **raw values, not display strings** (`"0.5000"` not `"50%"`) or state
   round-trips break.

---

## 7. Tempo, clock & quantized launch

- `host->get_bpm()` — with an arming pattern: an explicit `bpm` param (tap/jog) disables
  host-follow; `bpm_follow_host=1` re-arms.
- **`host->get_beat_position()`** — 24-PPQN interpolated beats, **returns < 0 when stopped**.
  Guard for NULL (older hosts). **This is the clock for quantized loop start/stop to the bar.**
- `button_passthrough:[85]` keeps Move transport/clock alive under the UI; loops sync to the
  Move's own transport.

---

## 8. Memory & threading (audio-thread safety)

- **Allocate ALL buffers in `create_instance`** — never on a transport event, never on the
  audio thread. `render_block`/`process_block` = lock-free arithmetic + `memcpy` only.
- **Lockless publish:** set up all of a voice's state, then flip `active = 1` **last**; the
  audio thread reads `active` without a lock.
- **Shrinking-capacity fallback ladder (Mark) — de-risks big allocations:** request the
  largest buffer `calloc` grants and step down until one succeeds, instead of hard-failing.
  This is how a 16-track / ~127 MB looper degrades gracefully rather than refusing to load.
- **If hosting other modules' FX (Mark):** a `SCHED_OTHER` worker (cores 0–2, **never core
  3**) does `dlopen`/`dlsym(move_audio_fx_init_v2)`/`create_instance`; hand the instance to
  the audio thread with a lock-free block-boundary `atomic_exchange` + a reader-count guard.
- Per-frame constants everywhere: **44100 Hz, 128 frames/block, stereo interleaved int16.**

---

## 9. Recording the input

Two proven approaches:
- **Direct (Mark/Smack/Overwork):** read `mapped_memory + audio_in_offset` (int16 stereo)
  each block, write into RAM loop buffers. Simplest for live looping — **this is LoopBox's
  existing design.**
- **Input-swap (TwinSampler):** temporarily overwrite the shared-memory input region
  (`save → overwrite → restore`) to choose the capture *source* — line-in / the Move mix bus
  (`audio_out_offset`) / a −3 dB summed mix (`0.70710678f` legs) with TPDF dither — then let
  the engine record from `audio_in` as usual. Useful if we later want "record what the Move
  is playing," not just the jack.

Offsets (`plugin_api_v1.h`): `MOVE_AUDIO_OUT_OFFSET 256`, `MOVE_AUDIO_IN_OFFSET 2048+256`.

---

## 10. State persistence

- Round-trip: JS/host calls `get_param("state")` (serialize all params to JSON) and, after
  `create_instance`, `set_param("state", saved_json)` to restore. Autosaved ~every 10 s + on
  slot/set change + shutdown.
- JSON parser must **tolerate pretty-printed whitespace** after colons (Genera bug) or enum/
  string params silently reset while ints survive.
- Store presets/sessions **outside** the module dir (e.g.
  `/data/UserData/schwung/presets/loopbox` or `…/loopbox-sessions`) so reinstalls preserve
  them.

---

## 11. Build & deploy

- **Toolchain:** Docker `debian:bookworm` + `gcc-aarch64-linux-gnu`,
  `-O3 -g -shared -fPIC -Wall -Wextra -Iinclude -lm` (add `-lpthread -ldl` if threading/
  hosting FX; link with `g++` if any C++ dep). On Windows use the `docker create` +
  `docker cp` + explicit exit-code check pattern (Git Bash `set -e` does **not** propagate
  docker failures — else you deploy a stale `.so`).
- **Package with GNU `tar` inside the container** (avoids macOS `._*` AppleDouble entries that
  break the installer). Tarball must extract to `<id>/` containing `module.json`.
- **Install path:** `/data/UserData/schwung/modules/overtake/<id>/`.
- **⚠️ Deploy safety (prevents a crash + power cycle):**
  1. **Overwrite `dsp.so` only with the tool CLOSED** (overtake mode off). Overwriting a
     mapped Overtake `.so` crashes MoveOriginal.
  2. **Full-exit before relaunch** (Shift+Back / Shift+Vol+Jog) — `suspend_keeps_js` will
     otherwise resume the *old* code.
  3. **Deploy by atomic rename, not overwrite** (Forgetful's install trap): unpack to a staging
     dir on `/data`, then `mv -f` the `.so`/`.json` into place. Writing straight over a `.so`
     a running process has mapped truncates it under the live mapping and the next page fault
     takes Move's audio process down. A `mv` within `/data` is an atomic rename, so a loaded
     instance keeps its old mapping until you reload.
- **Entry symbols on device:** DSP `move_plugin_init_v2`; UI globals
  `init/tick/onResume/onUnload/onMidiMessageInternal/onMidiMessageExternal`.
- **CI/catalog:** tag `v*` → Docker build → GitHub Release → commit `release.json`; then a PR
  adding the module to `charlesvestal/schwung` `module-catalog.json`.

---

## 12. Minimal Overtake shell (the smoke test to build first)

A tiny module that proves the whole SDK + the safe deploy loop, before porting any engine:
- **`module.json`** — as §2, `component_type:"overtake"`, `suspend_keeps_js:true`,
  `button_passthrough:[85]`.
- **`dsp.so`** — `move_plugin_init_v2` returning an engine whose `render_block` passes audio
  through (or silence); answers `get_param("module_id")="loopbox"`; a couple of test params.
- **`ui.js`** — `init` (paint pads by a fake per-slot state, batched), `tick`
  (`clear_screen`→`print` a header + a 16-cell strip→`host_flush_display`), `onMidiMessage
  Internal` (light a pad on press, read a knob via `decodeDelta`), `onResume` (force-repaint
  + resync), `onUnload` (clear all LEDs), clean Back-button exit via `host_exit_module()`.

Once this loads, lights pads, reads input, and re-flashes cleanly through the CLOSED-tool
deploy loop, the risky unknowns are gone and we drop `loopbox.c` in behind it.

---

## 13. Quick "steal-from" index

| Want | Take from | What |
|---|---|---|
| Looper skeleton | **Mark** | record→play→overdub SM, grid scheduler, undo-by-swap, per-track pad colors+blink |
| Per-track hostable FX | Mark | `dlopen` any Schwung audio_fx as a track insert (future: Palette/Magnéto per track) |
| Punch-in FX + pressure | **Performance FX** | 32 pressure FX, SEND-vs-INSERT wet topology (dry survives) |
| P-lock step sequencer | Overwork | hold-step + turn-knob locks a value on that step |
| Glitch/slice re-roll | Smack | seeded auto-slice + reordered playback, reproducible from seed |
| Loop-voice DSP | **TwinSampler** | `source_loop_voice_t`: varispeed `2^(semis/12)`, lerp, loop + ping-pong, per-voice gain/pan; load-time reverse/rate cache |
| MIDI looper layer | TwinSampler | record/quantize/replay pad events (overdub without audio) |
| Whole-mix finalizer | Overwork Mix | `end_of_chain:true` + `move_audio_fx_init_v2` |
