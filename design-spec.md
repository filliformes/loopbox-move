# LoopBox — Design Spec (Overtake edition)

> **Status:** DESIGN — revising the v0.3.0 `sound_generator` implementation into a full
> **Overtake** module. No code changed yet; this doc is the plan to review before building.
> **Plugin type:** `overtake` (was `sound_generator`)
> **Module ID:** `loopbox` — evolve the existing `loopbox-move` repo in place
> **DSP role:** generator/jack (`move_plugin_init_v2`, records mic/line-in)
> **Last updated:** 2026-09-09
> **Companion doc:** [`OVERTAKE-SDK.md`](OVERTAKE-SDK.md) — the reverse-engineered SDK reference.

---

## 0. Why Overtake (what changed and why)

The v0.3.0 spec was a `sound_generator`: it could *read* pad presses but **could not light the
pads** — only Overtake modules can drive pad LEDs (framework limitation, confirmed across all
analyzed modules). For a live-looping instrument modeled on the 1010music BlackBox, illuminated
per-track pads (empty/rec/play/overdub by color) are core to the workflow, not a nicety. Going
Overtake also lets us:

- Own the whole 128×64 screen for a 16-track meter/detail view.
- Put **loops on the left 16 pads and Punch-in FX on the right 16 pads** (merged into one
  module — see §5).
- Take external USB-MIDI (the **LaunchControl XL**) directly in JS and keep the Move's clock
  running underneath (`button_passthrough:[85]`).
- Keep loops playing while the module is suspended (`suspend_keeps_js:true`).

The **DSP engine from v0.3.0 (`loopbox.c`) is largely reusable** — it already records to
per-voice stereo buffers, plays back at variable rate, and has the full per-voice FX chain and
sends. The Overtake move is mostly: (a) relocate all *control logic* into `ui.js`, (b) drive
the engine through string params, (c) light pads from JS, (d) merge the punch-fx bank, (e)
harden the big allocation. See §11 for the migration map.

---

## 1. What it is

A **16-track asynchronous stereo tape looper + performance-FX instrument** for Ableton Move,
inspired by the 1010music BlackBox and the user's LaunchControl-XL BlackBox template. Records
mic/line-in into 16 independent stereo loop buffers. Each track has variable-rate (pitch)
playback, a DJ filter, tape character, per-voice EQ, pan, volume, and post-fader sends. A shared
send section (tape delay + plate reverb, extensible). Three overdub modes. External control via
LaunchControl XL (two templates → 16 tracks). **The right 16 pads are a Punch-in FX bank** (PO-33
style, pressure-controlled) applied to the master or the selected track.

### Surface model (Overtake)
```
 Move 4×8 pad grid (notes 68–99)          Step buttons 16–31        8 knobs + jog
 ┌───────────────┬───────────────┐        ┌───────────────────┐
 │  LEFT 4×4     │  RIGHT 4×4    │        │ 16 = track/scene  │    knobs = params of the
 │  16 LOOP      │  16 PUNCH-IN  │        │      select        │    selected track (+Shift
 │  TRACKS       │  FX           │        └───────────────────┘    layer); jog = data/BPM
 │  (state LEDs) │  (momentary,  │
 │               │   pressure)   │
 └───────────────┴───────────────┘
 Screen: 16-track strip (state/level) + selected-track detail page + FX page.
```

Loop track note layout (from `padNoteFor`, columns 0–3): the left 16 = notes
`68,69,70,71, 76,77,78,79, 84,85,86,87, 92,93,94,95`. Punch-FX (columns 4–7) = the +4 siblings.

---

## 2. Sonic intent

Unchanged from v0.3.0. A creative tape looper that treats recordings as living, degradable
material — preamp models color the input, the per-voice chain sculpts playback, Stability
compounds degradation, Disintegration dissolves loops into abstraction. Plus a punch-in FX layer
for live performance gestures.

**References:** 1010music BlackBox; Chase Bliss Blooper / Mood mk2 / Generation Loss mk2;
Kinotone Ribbons; Studer 962; Airwindows; Chowdhury DSP; Teenage Engineering PO-33 (punch-in);
the user's own Magnéto (tape), Palette (FX bank), and Punch-in FX modules.

**Not:** a clean digital looper; a sampler with slicing/MIDI-note triggering; a granular
processor (that's Verglas).

---

## 3. Architecture split (Overtake)

| Layer | File | Responsibility |
|---|---|---|
| **DSP engine** | `src/dsp/loopbox.c` → `dsp.so` | `plugin_api_v2`, `move_plugin_init_v2`. 16 loop voices, record/varispeed playback, per-voice FX, sends, punch-in FX bank, master chain. Reads input from `mapped_memory+audio_in_offset`. **No control logic, no UI.** Answers `set_param`/`get_param`/`get_param("state")`/`get_param("module_id")`. |
| **UI** | `src/ui.js` | QuickJS. Owns screen + all 32 pad LEDs + step LEDs + input. Translates pads/knobs/buttons/**external LCXL MIDI** into engine params. Holds transport/gesture state; drives the DSP via `host_module_set_param[_blocking]` / bulk `get_params`. |
| **Manifest** | `src/module.json` | `component_type:"overtake"`, capabilities (§4 of SDK doc). |
| **Help** | `src/help.json` | In-app manual. |
| **LCXL** | `BlackBox 1-8.syx`, `BlackBox 9-16.syx` | LaunchControl XL templates (kept). Now consumed by `ui.js` `onMidiMessageExternal`. |

All heavy work (buffer alloc, WAV I/O, session save) happens in `create_instance` or on the JS
tick — **never on the audio thread or on a transport event** (see SDK §8).

---

## 4. DSP engine (per-track, unchanged core from v0.3.0)

**Voice architecture:** 16 independent stereo loop voices, identical chains, no voice stealing.

**Signal flow:**
```
Input (mic/line-in) → Preamp Model (12 types) → [Stereo Record Buffer per voice]

Per-voice playback (×16):
  Buffer readout (variable-rate pitch, start/end, reverse)
  → DJ Filter (Isolator3 3-stage biquad) → Saturation → Wow/Flutter (Flutter2)
  → Glitch/Beat-Repeat → Tilt EQ (800Hz) → Studer 962 EQ → Pan → Volume
  → post-fader Send A + Send B → the two send buses

Send bus A → Palette effect (selectable from all 24)   ┐  (§6)
Send bus B → Palette effect (selectable from all 24)   ┘

All voices + sends → Global Sat (IronOxide) → Clock SR decimation → Master Comp
  → Lo/Hi Cut → soft limiter → PUNCH-IN FX BANK (master insert, §5) → int16 out
```

**Overdub modes:** Replace / Multiply (additive w/ Decay) / Disintegration (FX re-applied per
pass). **Preamp models (12):** Clean, Cass1/2, VHS1/2, Reel15/7/3, 4trk, Porta, Dub, Warp.
(Full parameter tables carried over from v0.3.0 — see git history of this file / CLAUDE.md.)

**New/changed for Overtake:**
- **Quantized launch:** **default OFF** — LoopBox is unquantized, tape-style by design (free
  loop lengths, no grid snapping). Optional per-need: when enabled, record/play/overdub
  transitions snap using `host->get_beat_position()` (24-PPQN, <0 when stopped), with a
  **selectable grid of 1–16 bars**. A per-track "pending" state blinks the pad LED until the
  boundary fires. Off = transitions happen the instant you hit the pad (current behavior).
- **Punch-in FX bank** merged into the master insert (§5).

---

## 5. Merged Punch-in FX (right 16 pads)

Fold the `punchfx-move` engine (PO-33-style, 16 effects, pressure/aftertouch) into `loopbox.c`
as a **master-insert performance bank** triggered by the right 4×4 pads (columns 4–7):

- **Momentary:** hold a pad → effect on; release → off. **Shift+hold → latch.**
- **Pressure:** poly aftertouch `0xA0` per pad modulates the effect (throttled ~30 ms).
- **Wet topology = SEND-style** (from Performance FX `apply_wet`): scale only the *added*
  signal so the dry loop mix never drops out on a throw.
- **Target: master bus by default.** A mode switch retargets the *whole* bank to the
  currently-selected track — **one target at a time, never both simultaneously** (master OR
  the selected track).
- 16 effect slots map 1:1 to the right pads; effect list starts from `punchfx.c` (repeat/
  stutter/reverse/tape-stop/filter/crush/gate/delay/…).

> **Decision made:** merge into a single module (not two chained modules). The Mark-style
> "`dlopen` a hosted FX per track" route stays available as a *future* option for per-track
> Palette/Magnéto inserts, but v1 bakes the punch-in bank in.

---

## 6. Sends — two selectable Palette send buses (DECIDED)

Replaces the old fixed tape-delay + plate-reverb sends. **Two shared post-fader send buses,
Send A and Send B; each hosts one Palette effect selectable from the FULL 24** (not the curated
insert list — the sends get everything). This directly answers the BlackBox "only one
reverb+delay send" limitation and mirrors Schwung's own Send A / Send B model.

- **Per bus (on a Sends page):** `sendX_fx` (enum, all 24 Palette effects) + `sendX_amount` +
  `sendX_macro` + `sendX_drift` (Palette's per-slot controls). Effects are 100% wet on the bus.
- **Per voice (Loop page):** `v_sendA`, `v_sendB` — each track's post-fader send amount into
  the two buses (replaces the single `v_send`).
- **Main-page knob change:** the old `delayRate` / `delayFeedback` / `reverb` macro knobs are
  retired; Send A / Send B live on their own page with select + amount + macro. (Update the
  parameter tables accordingly when we code.)
- Uses the **same baked Palette DSP** as the per-track insert (§6.5) — one effect library
  serves both the 10-effect insert and the 24-effect sends.

---

## 6.5 Per-track integration proposals — Magnéto, Palette, Smack

Keep LoopBox's current feature set; these are *proposals* for weaving in the user's own modules.
Baking (copying the DSP into `loopbox.c`) vs hosting (`dlopen` the released `.so` per track,
Mark-style) is the one architectural fork — see the recommendation at the end. **Everything
here is gated by the hardware CPU/RAM test (§12 step 3); nothing is committed until we know how
many full-chain voices actually run.**

### Magnéto → the per-track *playback/tape* engine (not an insert)
LoopBox's per-voice playback already carries tape character (saturation, wow/flutter, HF
rolloff), so Magnéto isn't an add-on effect — it's the *soul of the loop voice*. Proposal:
promote the per-voice playback stage to Magnéto's **full** engine, reusing `magneto.c` (the
user's own single-file C tape looper) as the per-voice character + transport stage:
- **9 tape models** selectable per track (Type I/II/IV, Worn, Radio, VCR, Dictaphone,
  Microcass, Studio) → each loop can be a different tape.
- **Performance gestures per track:** reverse, scrub/jog, jump, scan, stutter, dropouts, and
  the tape-stop ramp — mapped to knobs and/or the right-hand pads on the selected track.
- Hiss (model-colored) + the ClipOnly2 output ceiling, already in `magneto.c`.
- **Cost:** mostly already present; this *deepens* the existing per-voice tape stage. RAM
  neutral (same loop buffers); modest extra CPU per playing voice — covered by the per-voice
  "FX enable" so empty/idle voices cost ~nothing.

### Palette → the per-track *insert* (the "one FX slot per track from a bank")
Each track gets **one insert slot** that selects one of Palette's 24 effects (Character /
Movement / Diffusion / Texture). Two build strategies:

| | **A — Bake `palette.c` in** | **B — Host `palette-move.so` (Mark pattern)** |
|---|---|---|
| How | copy the 24-effect DSP into `loopbox.c`; per-track slot picks 1 | `dlopen` the released module per track on a `SCHED_OTHER` worker, atomic block-boundary handoff |
| Pros | self-contained, predictable CPU, consistent with the punch-FX merge decision | reuse the *released* module verbatim + updates free; **any** Schwung audio_fx per track (Palette, Magnéto, Verglas…) |
| Cons | code duplication to keep in sync; bigger binary | up to 16 hosted instances (RAM/CPU); threading machinery; partly reopens "one module vs many" |
| CPU control | per-voice FX-enable; realistically only a few tracks run heavy FX at once | same, plus load/unload debounce |

**DECIDED — per-track insert bank (curated 10):** Drive, Sweeten, Fold, Doubler, Pitch,
Tremolo, Filter, Squash, Broken, Halo. Each track's single insert slot picks one of these.
**The two send buses (§6) draw from the FULL 24** — same baked Palette DSP, so the curated
insert and the full-palette sends are one body of code.

### Smack → a per-track "Scatter" / re-roll mode (you've never used Smack — here's the cool part)
Smack grabs a clock-synced loop, auto-slices it, and uses a **seeded RNG** to (a) assign a
playback effect per slice and (b) reorder the slices — repeating *identically* every pass until
you re-roll the seed. Ported into LoopBox as a **per-track mode** it becomes a live glitch
generator that is:
- **Deterministic & recallable** — the seed reproduces the exact mangle; store it with the loop,
  recall the good ones.
- **Re-rollable live** — one pad/knob bumps the seed → a fresh but coherent rhythmic variation.
- **Non-destructive** — a playback transform over the existing buffer; the clean loop is intact,
  so "Normal ↔ Scatter" is a toggle and "lock this roll" / "back to clean" are one gesture.

Turns a clean tape loop into a stutter/glitch performance element on demand. Controls: **Mode**
(Normal/Scatter), **Slices** (e.g. 2–32, or grid-derived), **Amount** (how much per-slice FX),
**Seed** (jog to re-roll). Non-v1, but reserve a per-track mode + a re-roll gesture in the UI now.

### Forgetful → a master "Stumble" glitch page (the complement to Scatter)
`kliegsablaze/forgetful`'s end-of-chain Glitch is a **dblue-Glitch-style probabilistic step
sequencer of destructive effects** on the whole mix (post-sends, pre-limiter). It is the
*opposite axis* to Scatter, and that's the point:

| | **Scatter (Smack)** | **Stumble (Forgetful)** |
|---|---|---|
| Determinism | seeded, **recallable** (repeats identically) | **stochastic**, never the same twice |
| Scope | **per-track**, one loop | **master / end-of-chain**, the whole mix |
| Feel | composed chaos you recall | live chaos you ride |

Add **Stumble** as a master glitch page (mirrors the punch-FX master-insert placement). Knobs
lifted from Forgetful: **Mix** (crossfade to dry — 0 = bypass), **Step** (20–1000 ms *or*
clock-synced via `get_beat_position`, since we have it), **Odds** (per-step fire probability —
0 = silent), **Size** (slice length), **Kind** (Tumble = re-roll each step, or pin Stutter/
Rewind/Tape/Gate/Crush), **Reach**, **Pitch** (±12 st), **Width** (stereo spread).

Reusable mechanics worth copying verbatim:
- **2-second capture ring + Reach**: at 0 it stutters the immediate past; up, it *quotes* a grain
  from anywhere in the last 2 s. **LoopBox enhancement:** Reach could quote from the **loop
  buffers themselves** (we already hold 16 × 45 s), giving a far deeper "memory" than a 2 s ring.
- **Odds + Mix = safe-to-arm** (either at 0 is bit-exact bypass) — the "always loaded, silent
  until wanted" design.
- **Tumble vs pinned Kind**; **every edge faded ~1.5 ms** (click-free); **capture the PRE-glitch
  signal** (never a feedback path); effect pool is extensible (add one, Tumble picks it up free).

Placement note: Stumble sits **after the punch-FX master insert, before the limiter** — so a
punch-in throw and a stumble step compose rather than fight. Non-v1, but it shares the master-
insert plumbing with punch-FX, so reserve the page now.

*(Bonus, further out: Forgetful's core concept — loops that "forget themselves," drifting out of
tune / hissing / breaking up the longer since you touched them — overlaps LoopBox's Disintegration/
Stability. A per-track "Age/Forget" that degrades an untouched loop over time is a natural cousin.)*

### Recommendation
For **v1: bake** — Magnéto as the per-voice tape engine, a curated Palette subset as the
per-track insert bank, sharing one effect table with the sends. It's self-contained, keeps CPU
predictable, and matches the "merge into one module" decision already made for punch-FX. Keep
**hosting (strategy B) documented as a post-v1 lever** for "any Schwung FX per track" once the
hardware CPU headroom is known. Smack-style **Scatter** (per-track, deterministic) and
Forgetful-style **Stumble** (master, stochastic) are fun post-v1 glitch modes — complementary
axes, both reserved in the UI now.

---

## 7. Controls & input mapping

### Move pads (internal MIDI, `onMidiMessageInternal`, notes 68–99)
- **Left 16 (loops):** single tap cycles Empty→Rec→Play⇄Pause; quick double-tap from Play/Pause
  → Overdub; tap from Overdub → exit (Disintegration pass if in Disint mode). Tap also selects
  the track for the detail page. **Pad LEDs show state** (dim/red/green/amber/white, blink =
  pending quantized action).
- **Right 16 (punch FX):** momentary + pressure; Shift+hold = latch.

### Step buttons 16–31
Track select (mirrors selected track), or scene/session select with a modifier.

### 8 knobs + jog (`onMidiMessageInternal`, CC, relative via `decodeDelta`)
Selected-track params (Start/End/Sat/WowFlut/Send/Glitch/Tilt/…) with a **Shift layer** for the
second bank (EQ/pitch/pan/vol/decay). Jog = BPM / data. Buttons: `MoveShift`, `MoveMenu`
(track-picker mode), `MoveBack` (clean exit), `MoveCapture`, `MovePlay` (passthrough → clock).

### LaunchControl XL (external MIDI, `onMidiMessageExternal`) — the primary performance surface
Consumes the two existing templates. **In Overtake, this is handled in `ui.js`** (translate
CC/notes → engine params), not in the DSP `on_midi`.

**Template 1 — Voices 1-8 (`BlackBox 1-8.syx`):** Pitch CC1-8, Filter CC9-16, Pan CC17-24,
Vol CC25-32, Rec/OD Note 68-75, Play/Stop Note 36-43.
**Template 2 — Voices 9-16 (`BlackBox 9-16.syx`):** Pitch CC41-48, Filter CC49-56, Pan CC57-64,
Vol CC65-72, Rec/OD Note 76-83, Play/Stop Note 44-51.

> Template 1's bottom-button notes (36-43) overlap Move pad notes — but external vs internal
> arrive on **different JS callbacks** (`onMidiMessageExternal` vs `onMidiMessageInternal`), so
> the ambiguity that existed in the DSP-routed v0.3.0 design disappears.

---

## 8. Memory & CPU (the real risks — with mitigations)

- **Buffers:** 16 × 45 s × 44100 × 2ch × int16 ≈ **127 MB**, allocated once in
  `create_instance`. **Mitigation (Mark's shrinking-capacity fallback ladder):** request the
  largest buffer `calloc` grants and step down (e.g. 45→30→20 s, or 16→12→8 tracks) until it
  succeeds, so the module *degrades* instead of failing to load. Report the achieved capacity
  to the UI. **This must be validated on hardware first (see §12).**
- **CPU:** 16 voices × full FX chain at 128fr/44.1k is heavy on the CM-class core. Likely
  outcomes to design for: a per-voice "FX enable" so idle/empty voices cost ~nothing; a cap on
  simultaneously-playing full-chain voices; lighter default chains. Measure, then trim.
- **Param bandwidth:** 16 tracks × several params exceeds the single-key `get_param` rate →
  **bulk `host_module_get_params`** for UI sync; **base64-windowed state blob** for presets.
- Denormal guards on all biquads/feedback (no FTZ on ARM). No heap alloc / no `printf` in render.

---

## 9. State & sessions

- `get_param("state")` / `set_param("state")` round-trip (JSON; whitespace-tolerant parser).
- Sessions (loop audio + settings) saved under `/data/UserData/schwung/loopbox-sessions`
  (outside the module dir → survive reinstalls), on a `SCHED_OTHER` I/O worker (Mark pattern).

---

## 10. Deploy safety (from SDK §11 — do not skip)

- Overwrite `dsp.so` **only with the tool closed** (overtake mode off) or MoveOriginal crashes
  → power cycle.
- **Full-exit before relaunch** (`suspend_keeps_js` resumes old code otherwise).
- Package with GNU tar inside the Docker container; extract to `loopbox/`.
- Install to `/data/UserData/schwung/modules/overtake/loopbox/`.

---

## 11. Migration map (v0.3.0 `sound_generator` → Overtake)

| v0.3.0 | Overtake |
|---|---|
| `module.json` `component_type:"sound_generator"`, `plugin_api_v1.h` | `"overtake"`, `api_version:2`, add `ui:"ui.js"`, capabilities incl. `suspend_keeps_js`, `button_passthrough:[85]` |
| MIDI handled in DSP `on_midi` (pads + LCXL by source) | **all input in `ui.js`** (`onMidiMessageInternal` + `onMidiMessageExternal`); DSP driven by params |
| No pad LEDs | `ui.js` lights left-16 by loop state (batched 8/tick, resync on resume, clear on exit) |
| `ui_chain.js` (host knob pages) | replaced by full-screen `ui.js` (128×64 immediate mode) |
| Right 16 pads unused | Punch-in FX bank (merged `punchfx.c`) |
| 127 MB hard `calloc` | fallback ladder |
| Author `fillioning` (punchfx) | `Filliformes` |
| — | quantized launch via `get_beat_position()`; bulk param reads; base64 state blob |

Reusable as-is: the DSP voice/record/playback/FX/sends math, preamp models, overdub modes,
the LCXL `.syx` templates.

---

## 12. Build order (de-risk first)

1. **Fix Move connectivity** (USB-C; `move.local` not resolving — find IP/mDNS).
2. **Minimal Overtake shell** (SDK §12): screen + 32 pad LEDs + input + clean exit, tiny DSP.
   Locks the SDK mechanics *and* the crash-safe deploy loop before the engine goes in.
3. **Port `loopbox.c` behind the shell**; validate the **127 MB fallback ladder loads** and
   how many full-chain voices actually run (the two open hardware questions).
4. **Merge Punch-in FX** on the right 16 pads.
5. **Wire the LaunchControl XL** via `onMidiMessageExternal`.
6. Sessions/presets, help, then `/move` release + catalog PR.

---

## 13. Open questions

- [x] **v1 milestone → FULL INSTRUMENT**: 16-track looper + Magnéto per-voice tape + master
      punch-FX + per-track Palette insert + LaunchControl XL. Scatter/Stumble remain post-v1.
- [x] **Track count → DYNAMIC**: target 16, allocated via the shrinking-capacity fallback
      ladder; the Move settles it (16/12/8). UI reports the achieved count.
- [x] **Loop size → 45 s stereo** (127 MB target at 16 tracks; ladder degrades if refused).
- [x] Hardware RAM: **16×45s stereo = 127 MB allocates AND commits on the Move** (real touched
      pages, held live) — full spec fits, no fallback needed. Verified 2026-09-09 via the LBX
      Shell probe. The fallback ladder stays as insurance but isn't triggered.
- [ ] Hardware CPU: how many of the 16 voices sustain the full FX chain at 128fr/44.1k?
      **← the remaining gating unknown; measured during the engine port (§12 step 3).**
- [x] Punch-FX target → **master by default; switchable to per-track (one target at a time).**
- [x] Quantized launch → **default OFF (unquantized tape-style); optional grid selectable 1–16 bars.**
- [x] Magnéto → **bake as the per-voice tape engine** (9 models + gestures). See §6.5.
- [x] Palette → **bake a curated subset as the per-track insert bank** for v1; dlopen-hosting
      is the post-v1 lever for "any Schwung FX per track." See §6.5.
- [x] **Palette curated insert (10):** Drive, Sweeten, Fold, Doubler, Pitch, Tremolo, Filter,
      Squash, Broken, Halo — one insert slot per track picks one of these.
- [x] **Two send buses, each selectable from all 24 Palette effects** (replaces fixed
      delay+reverb sends); per-voice `v_sendA`/`v_sendB`. See §6.
- [x] **Bake vs host** for Palette/Magnéto → **BAKE** (copy the DSP into `loopbox.c`);
      `dlopen`-hosting stays a documented post-v1 lever. Decided 2026-09-09.
- [ ] Smack-style Scatter re-roll → **post-v1 per-track mode** (reserve UI now). See §6.5.
- [ ] Forgetful-style **Stumble** master glitch → **post-v1 master page** (shares punch-FX
      master-insert plumbing; Reach can quote the loop buffers). See §6.5.
- [x] Move connectivity → on the **SuperSpeed USB-C port**; resolve exact IP/mDNS at §12 step 1.
