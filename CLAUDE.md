# LoopBox — Claude Code context

## What this is
16-track asynchronous stereo tape looper for Ableton Move, inspired by 1010music BlackBox.

Plugin type: `sound_generator`
Module ID: `loopbox`
API: `plugin_api_v2_t`
Language: C

Read `design-spec.md` for full design intent. This file is the compressed version.

---

## Sonic intent
Creative tape looper where recordings are living, degradable material. Every stage adds
character: preamp models color input, per-voice chain sculpts playback, Stability compounds
degradation, Disintegration dissolves loops into abstraction. References: BlackBox, Ribbons,
Blooper, Mood mk2, Generation Loss mk2, Studer 962, Airwindows, Chowdhury DSP.
Not a clean digital looper. Not a sampler. Not granular.

---

## DSP architecture
Input (mic/line-in) → Preamp (12 models) → Stereo Record Buffer (16 × 60s × 44.1k × 2ch × int16 ≈ 161MB).
Per-voice (×16): Variable-rate pitch playback → Isolator3 DJ filter (3-stage cascaded biquad) →
Tube saturation → Flutter2 wow/flutter → Glitch/beat-repeat (+ random octaves + bit-crush) →
Tonelux tilt EQ (800Hz, ±6dB) → Studer 962 EQ (20Hz shelf ±15dB, parametric mid 150-7kHz ±11dB Q=0.6,
20kHz shelf ±15dB) → Equal-power pan → Volume → Post-fader send.
Send bus → Tape delay (100% wet, TapeDelay2 bandpass feedback + wow) + Plate reverb (100% wet, Dattorro).
Sum → Global saturation (IronOxide) → Clock SR decimation (S&H + aliasing) → Master comp →
Lo/Hi cut → lb_tanh limiter → int16 stereo out.

3 overdub modes: Replace (overwrite), Multiply (additive layering with Decay), Disintegration
(FX chain re-applied to buffer on overdub stop — loop dissolves progressively).

---

## Parameters

### Main (knobs 1-8)
| # | Key | Name | Type | Range | Default |
|---|-----|------|------|-------|---------|
| 1 | globalSat | Sat | float | 0–1 | 0 |
| 2 | masterComp | Comp | float | 0–1 | 0 |
| 3 | masterLoCut | LoCut | float | 20–500 | 20 |
| 4 | masterHiCut | HiCut | float | 1k–20k | 20000 |
| 5 | clock | Clock | float | 0–1 | 0.5 |
| 6 | delayRate | DlyRt | float | 0.01–1 | 0.3 |
| 7 | delayFeedback | DlyFb | float | 0–0.95 | 0.35 |
| 8 | reverb | Reverb | float | 0–1 | 0.4 |

### Control (knobs 1-8 + jog)
| # | Key | Name | Type | Options/Range | Default |
|---|-----|------|------|---------------|---------|
| 1 | preamp | Preamp | enum | Clean,Cass1,Cass2,VHS1,VHS2,Reel15,Reel7,Reel3,4trk,Porta,Dub,Warp | 0 |
| 2 | overdubMode | OdMode | enum | Replace,Multiply,Disint | 0 |
| 3 | stability | Stabil | float | 0–1 | 0 |
| 4 | globalWowFlut | W/Flut | float | 0–1 | 0 |
| 5 | inputMonitor | InMon | float | 0–1 | 0 |
| 6 | clearSel | ClrSel | float | 0–1 (trigger >0.5) | 0 |
| 7 | clearAll | ClrAll | float | 0–1 (trigger >0.5) | 0 |
| 8 | selTrack | Track | int | 1–16 | 1 |

### Loop (per-voice, knobs 1-8)
| # | Key | Name | Type | Range | Default |
|---|-----|------|------|-------|---------|
| 1 | v_start | Start | float | 0–1 | 0 |
| 2 | v_end | End | float | 0–1 | 1 |
| 3 | v_reverse | Rev | enum | Normal,Reverse | 0 |
| 4 | v_sat | Sat | float | 0–1 | 0 |
| 5 | v_wowflut | W/Flut | float | 0–1 | 0 |
| 6 | v_send | Send | float | 0–1 | 0 |
| 7 | v_glitch | Glitch | float | 0–1 | 0 |
| 8 | v_tilt | Tilt | float | -1–1 | 0 |

### Loop (per-voice, menu entries)
| Key | Name | Type | Range | Default |
|-----|------|------|-------|---------|
| v_eqBass | Bass | float | -1–1 (±15dB) | 0 |
| v_eqPresFrq | MidF | float | 0–1 (150–7kHz) | 0.5 |
| v_eqPresAmt | MidG | float | -1–1 (±11dB) | 0 |
| v_eqTreble | Treble | float | -1–1 (±15dB) | 0 |
| v_pitch | Pitch | float | -2–2 oct | 0 |
| v_filter | Filter | float | 0–1 | 0.5 |
| v_pan | Pan | float | -1–1 | 0 |
| v_volume | Vol | float | 0–1 | 0.8 |
| v_decay | Decay | float | 0–1 | 1 |

### Read-only
| Key | Returns |
|-----|---------|
| v_state | Empty/Rec/Play/Pause/Odub |
| v_loopLen | seconds (float) |

---

## MIDI mapping (hard-coded)

### Move pads (MIDI_SOURCE_INTERNAL, Note 36-51)
Single tap cycles: Empty→Recording→Playing⇄Paused. Also selects track.
Quick double-tap (400ms window) from Playing/Paused enters Overdubbing.
Single tap from Overdubbing exits to Playing (Disintegration pass if Disint mode).

### LaunchControl XL (MIDI_SOURCE_EXTERNAL)

**Template 1 (voices 1-8):** Pitch CC1-8, Filter CC9-16, Pan CC17-24, Vol CC25-32,
Rec/OD Note 68-75, Play/Stop Note 36-43.

**Template 2 (voices 9-16):** Pitch CC41-48, Filter CC49-56, Pan CC57-64, Vol CC65-72,
Rec/OD Note 76-83, Play/Stop Note 44-51.

Note: Template 1 bottom buttons (Note 36-43) overlap Move pad notes. Differentiate
by MIDI source (internal vs external).

---

## Critical implementation notes

- **Stereo recording**: buffers must store L+R (not mono sum)
- **Clock SR degradation**: sample-and-hold decimation + aliasing noise injection at lower speeds
- **get_param MUST return -1 for unknown keys** (not 0 — breaks Master FX menu editing)
- **knob_N_adjust/name/value pattern required** in DSP for Schwung knob overlay
- **Denormal guards**: flush-to-zero not available on ARM; guard all biquad states and feedback paths
- **No heap allocation in render_block**
- **No printf/logging in render_block**

---

## Move hardware constraints (never violate)
- Block size: 128 frames at 44100 Hz (~2.9ms)
- Audio: int16 stereo interleaved
- No heap allocation in render path
- No `printf` / logging in render path
- No FTZ on ARM — denormal guard required
- Files on device must be owned by `ableton:users`
- Memory budget: ~127 MB for 16 stereo buffers at 45s (allocated once in create_instance)

---

## API constraints (sound generator)
- API: `plugin_api_v2_t`, entry: `move_plugin_init_v2`
- `render_block`: output-only int16 stereo, 128 frames
- Full MIDI: note on/off, CC
- Capabilities: `chainable: true, audio_in: true, component_type: "sound_generators"`
- Install path: `modules/sound_generators/loopbox/`

---

## Repo map
- `src/dsp/loopbox.c` — all DSP
- `src/dsp/plugin_api_v1.h` — Schwung plugin API header (v1 + v2)
- `src/module.json` — parameter schema + ui_hierarchy
- `src/ui_chain.js` — chain UI (3 pages: Main, Control, Loop)
- `src/help.json` — in-app manual
- `scripts/build.sh` — Docker ARM64 cross-compile
- `scripts/install.sh` — deploy + fix ownership
- `scripts/Dockerfile` — build environment
- `BlackBox 1-8.syx` — LCXL template for voices 1-8
- `BlackBox 9-16.syx` — LCXL template for voices 9-16
- `design-spec.md` — full design intent and rationale

## Build & deploy
```bash
./scripts/build.sh && ./scripts/install.sh
```

## Release
Use `/schwung-release` when ready.
