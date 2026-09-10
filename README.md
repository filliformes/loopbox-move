# LoopBox

**A 16-track live tape looper for the Ableton Move, built as a Schwung *Overtake* module.**

LoopBox turns the Move into an asynchronous, tape-flavoured looping instrument: sixteen
stereo loops on the left pads, a bank of momentary/latchable glitch FX on the right pads,
two send buses drawing on a 25-effect Palette engine plus a Dattorro plate, a MIDI-keyboard
polyphony layer, and deep per-track tone-shaping — all driven from the Move's pads, steps,
knobs and screen.

> Inspired by the 1010music BlackBox looper workflow, and by the sound of Kinotone Ribbons,
> Chase Bliss Blooper / Mood MK2, and Chase Bliss × Hologram Microcosm-style glitch.

- **Module:** `loopbox` · **Name:** LoopBox · **Abbrev:** LBX · **Type:** Overtake (Schwung) · **API v2**
- **Format:** 44100 Hz, 128-frame blocks, stereo · **License:** GPL-3.0

---

## Requirements

- An **Ableton Move** running **Schwung** with Overtake module support.
- A host with **Docker** (cross-compiles the ARM64 `dsp.so`) and **ssh/scp** for deploying to the Move.
- The Move reachable over USB-C at `move.local` (or `172.16.254.1`).

Optional: a MIDI keyboard (external MIDI in) to play loops chromatically.

---

## What it does

### 16 stereo tape loops
Each of the left 16 pads is an independent, free-running stereo loop (up to 45 s). Record,
play, pause and overdub live; every loop has its own speed, tone, FX sends and amp envelope.

- **Overdub modes:** Replace · Multiply (endless layering with decay) · Disintegration (the
  loop's own FX are re-applied each pass, so it slowly falls apart).
- **Playback speed** is tinted onto the pad LED (blue = ½×, green = 1×, yellow = 2×).

### Per-track controls (3 loop pages)
| Page | Knobs |
|------|-------|
| **1 · Main** (Up / Left arrow) | Pitch · Filter · Pan · Volume · Start · End · Reverse · **Send A** |
| **2 · Perform** (Down arrow) | Clock · Tilt · Sat · Comp · Wow-Flutter · **Scatter** · **Seed** · **Send B** |
| **3 · Tone** (Right arrow) | Studer **Bass · MidF · MidGain · Treble** · DJ-filter **Reso** · Filter · **Attack · Decay** |

- **Seed** — a Smack-style *seeded slice re-order* (2/4/8/16 slices, some reversed). The knob
  *is* the seed: every position is a different reproducible mangle; click-free (crossfaded).
- **Scatter** — stochastic slice jumps (non-repeating), also crossfaded.
- **DJ Filter + Reso** — continuous LP↔HP sweep, transparent at centre, with a resonance control.
- **Attack / Decay** — a per-loop amplitude envelope (3 ms → ~2 s). Fades a loop *in* when
  triggered/unmuted and *out* when muted/paused/stopped.
- **Studer 962 EQ**, Tilt EQ, tube-style Saturation, Wow/Flutter — the tape character chain.

### Two send buses — the Palette engine
Send A and Send B each select from **26 effects** (Off + 24 Palette effects + a Dattorro
**Plate** reverb): Drive, Sweeten, Fuzz, Howl, Fold, Swell, Doubler, Vibrato, Phaser,
Tremolo, Pitch, Shift, Cascade, Reels, Collage, Reverse, Space, Bloom, Filter, Squash,
Cassette, Broken, Interference, Halo, **Plate**. Each bus has Amount / Macro / Drift.
Configured in the **Global FX** track-menu.

### Punch-in FX (right 16 pads)
Sixteen momentary glitch effects reading a 2-second capture ring — Loops, Stutter, Oct±,
Haze (granular cloud), Shimmer, Stretch/Freeze, Reverse, Saturate — **up to 4 stacked in
series**. Hold a pad to apply; **knobs 5–8 edit the held effect's four parameters** live.
**Shift + pad = latch** it on hands-free (Shift + pad again to release).

### Perform, MIDI & I/O
- **Perform menu:** Stumble (Forgetful-style probabilistic step glitch), Jump, Scan, Dropout.
- **MIDI keyboard:** play the selected loop (or per-channel loops) chromatically, 8-voice
  polyphony, through that loop's full FX chain.
- **Input FX menu:** 12 preamp/tape models + a 3-band record EQ + input monitor/trim.
- **Settings menu:** Master Out, root note, overdub mode, master Lo/Hi cut, global sat,
  stability, global wow/flutter.

---

## Controls

### Pads & steps
| Gesture | Action |
|---------|--------|
| **Tap** left pad | cycle Empty → Rec → Play ⇄ Pause |
| **Double-tap** (playing) | Overdub |
| **Hold** left pad (~1 s) | Clear the loop *(undoable)* |
| **Shift + tap** | cycle playback speed (½× / 1× / 2×) |
| **Mute + tap** | quick-mute the track (playhead keeps running — comes back in phase) |
| **Right pad** | apply punch-FX (momentary) · **Shift + pad** = latch |
| **Step buttons** | select track (and jump to its loop page 1) |
| **Track buttons 1–4** | open menus: Input FX · Global FX · Perform · Settings |

### Buttons & knobs
| Control | Action |
|---------|--------|
| **8 knobs** | selected-track params (or menu / punch params) |
| **Up / Down** | previous / next loop page · **Left** = Main · **Right** = Tone |
| **Mute** (held) | quick-mute modifier — lights while held |
| **Undo** | restore the last-cleared loop |
| **Shift** | speed / punch-latch modifier |
| **Back** | close a menu, then exit the module |
| **Full exit** | **Shift + Volume + Jog-click** (a plain Back only *suspends*, keeping loops alive) |

---

## Signal chain

```
Per voice:  variable-rate pitch → Seed slice re-order → saturation → wow/flutter
            → DJ filter (+reso) → tilt EQ → Studer 962 EQ → stability → amp env → pan/vol → sends A/B

Master:     Σ voices + MIDI-poly → input monitor → clock SR-decimation → + Palette send returns
            → global saturation → master wow/flutter → compressor → lo/hi cut
            → Stumble → dropout → punch-FX (×4 series) → master out → soft limiter → output
```

Sends run through the block-processed Palette buses with one block of latency; the master
finishes on an analog-style soft limiter that tames the glitch transients.

---

## Build & install

```bash
cd overtake-shell
./scripts/build.sh      # Docker cross-compiles dsp.so (aarch64) + validates ui.js
./scripts/install.sh    # scp + atomic-rename install to move.local
```

Then on the Move: rescan modules (or Schwung Manager) and open **LBX / LoopBox** from the
Overtake list. **If it was already loaded, full-exit first** (Shift + Volume + Jog-click) —
`suspend_keeps_js` otherwise resumes the old code.

`MOVE_HOST` overrides the target (`MOVE_HOST=ableton@172.16.254.1 ./scripts/install.sh`).

> **Deploy safety:** the installer stages files to `/data/UserData` and `mv`s them into place
> (atomic) — never write straight over a mapped `.so`, which crashes the audio process.

---

## Repository layout

```
overtake-shell/            ← the active module (v0.4.0)
  module.json              Overtake manifest (id/name/capabilities)
  src/
    loopbox.c              engine: voices, punch-FX, stumble, sends, master, params
    palette_fx.c/.h        24-effect Palette engine + Dattorro Plate
    fx_clouds.cc           Clouds-based Space/Bloom (C++)
    warps_data.c           Warps wavetables (Fold/Shift)
    ui.js                  QuickJS Overtake UI (pads, knobs, screen, LEDs)
  include/plugin_api_v1.h  host API
  vendor/                  clouds_engine + signalsmith (third-party DSP)
  scripts/                 build.sh · install.sh · Dockerfile
OVERTAKE-SDK.md            reverse-engineered Overtake SDK reference
design-spec.md             full design rationale
CLAUDE.md                  project notes
```

The top-level `src/` and `scripts/` are the earlier standalone build and are superseded by
`overtake-shell/`.

---

## Credits

- **Palette / Clouds** texture engine after Mutable Instruments *Clouds* and *Warps* (open source).
- **signalsmith** stretch/DSP helpers.
- Glitch behaviours inspired by *Forgetful* (Stumble) and *Smack* (Seed slice re-order).
- Tape/character voicing after classic reel-to-reel, cassette and VHS colouration.

All third-party DSP is used under its own open-source license; see `overtake-shell/vendor/`.

## License

GPL-3.0
