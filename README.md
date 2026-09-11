# LoopBox

**A 16-track live tape looper for the Ableton Move, built as a Schwung *Overtake* module.**

LoopBox turns the Move into an asynchronous, tape-flavoured looping instrument: sixteen
stereo loops on the left pads — each with **up to four independent playheads** — a bank of
momentary/latchable glitch FX on the right pads, two send buses drawing on a 25-effect
Palette engine plus a Dattorro plate, a full record-path tape machine, session save/load,
and a MIDI-keyboard polyphony layer.

> Inspired by the 1010music BlackBox looper workflow, and by the sound of Kinotone Ribbons,
> Chase Bliss Blooper / Mood MK2, and Microcosm-style glitch. Interaction ideas borrowed
> from the norns looper world (*wrms*, *cranes*, *oooooo*, *samsara*, *nydl*, *otis*).

- **Module:** `loopbox` · **Name:** LoopBox · **Abbrev:** LBX · **Type:** Overtake (Schwung) · **API v2**
- **Format:** 44100 Hz, 128-frame blocks, stereo · **Version:** 0.5.0 · **License:** GPL-3.0

---

## Requirements

- An **Ableton Move** running **Schwung** with Overtake module support.
- A host with **Docker** (cross-compiles the ARM64 `dsp.so`) and **ssh/scp** for deploying.
- The Move reachable over USB-C at `move.local` (or `172.16.254.1`).

---

## What it does

### 16 stereo tape loops
Each left pad is an independent, free-running stereo loop (up to 45 s). Record, play, pause
and overdub live; every loop has its own speed, tone, FX sends, amp envelope and playheads.

- **Overdub modes:** Replace · Multiply (endless layering with decay) · Disintegration
  (the loop's FX are re-applied each pass, so it slowly falls apart).
- Playback speed is tinted onto the pad LED (blue ½× · green 1× · yellow 2×).

### Up to 4 playheads per loop
Every loop can be read by **four independent heads at once**, each with its own mode and
speed — one recording becomes a canon, a drone, or a ping-ponging texture.

- Modes: **Off · Fwd · Bwd · Ping** (ping-pong).
- Per-head speed (0.25×–4×); heads sum with `1/sqrt(n)` normalisation so stacking stays sane.
- Turning a head on **always restarts it at the loop start**.
- Head 1 is the main head — Scatter, Seed, scrub and Jump all drive it.
- In the Playheads page, **touch a head's knob and the jog wheel moves that head** along the waveform.

### The four loop pages
Navigate with **Down** (next) and **Up** (previous).

| Page | Knobs |
|------|-------|
| **P1 · Main** | Pitch · Filter · Pan · Volume · Start · End · Reverse · **Send A** |
| **P2 · Perform** | Clock · **Reso** · Sat · Comp · Wow/Flutter · Scatter · **Seed** · Send B |
| **P3 · Tone** | Studer **Bass · MidF · MidGain · Treble** · Tilt · **Heads ▸** · Attack · Decay |
| **P4 · Playheads** | H1 mode/speed · H2 · H3 · H4 |

- **Seed** — a Smack-style *seeded slice re-order* (2/4/8/16 slices, some reversed). The knob
  *is* the seed: every position is a different reproducible mangle, click-free.
- **Scatter** — stochastic slice jumps, crossfaded.
- **DJ Filter + Reso** — continuous LP/HP sweep with resonance, smoothed over ~10 ms.
- **Attack / Decay** — per-loop amplitude envelope (3 ms → 3 s / 5 s), used on trigger,
  mute, pause and stop.

### Two send buses — the Palette engine
Send A and B each select from **26 effects** (Off + 24 Palette effects + a Dattorro **Plate**):
Drive, Sweeten, Fuzz, Howl, Fold, Swell, Doubler, Vibrato, Phaser, Tremolo, Pitch, Shift,
Cascade, Reels, Collage, Reverse, Space, Bloom, Filter, Squash, Cassette, Broken,
Interference, Halo, Plate — each with Amount / Macro / Drift.

### Punch-in FX (right 16 pads)
Sixteen momentary glitch effects over a 2-second capture ring — Loops, Stutter, Oct±,
Haze, Shimmer, Stretch/Freeze, Reverse, Saturate — **up to 4 stacked in series**.
Hold a pad to apply; **knobs 5–8 edit the held effect's four parameters** live.
**Shift + pad latches** it on hands-free.

### The Tape machine (record path)
A full input tape stage on the **Capture** button: **Tape model** (13, including a true
**Tapeless** bypass) · Drive · Wow · Flutter · HF rolloff · Low cut · Hiss · **Generations**
(approximates repeated dubs). Default is `Clean`.

### Sessions
**8 numbered slots**, saved and loaded from the **Sample/Record** button. Settings *and*
recorded audio are stored; all disk work runs on a `SCHED_OTHER` worker thread pinned to
cores 0–2, never on the audio callback. Sessions live in
`/data/UserData/schwung/loopbox-sessions/` so reinstalls keep them.

### Perform, MIDI and I/O
- **Perform menu:** Stumble (probabilistic step glitch), Jump, Scan, Dropout.
- **MIDI keyboard:** 8-voice polyphony playing a loop chromatically through its full FX
  chain. **Off by default** (Settings → MIDI) so Move tracks' MIDI-out cannot trigger loops.
- **Settings:** Master Out, root note, overdub mode, master Lo/Hi cut, global sat, MIDI, arm threshold.

---

## Controls

### Pads and steps
| Gesture | Action |
|---------|--------|
| **Tap** left pad | cycle Empty → Rec → Play ⇄ Pause |
| **Double-tap** (playing) | Overdub |
| **Hold** left pad (~1 s) | Clear the loop *(Undo restores it)* |
| **Shift + tap** | cycle playback speed (½× / 1× / 2×) |
| **Mute + tap** | quick-mute (playhead keeps running — returns in phase) |
| **Copy + pad, then pad** | clone a loop (source blinks, second pad receives it) |
| **Loop + pad** | cycle loop length 1× → ½× → ¼× → ⅛× |
| **Right pad** | punch-FX (momentary) · **Shift + pad** = latch |
| **Step** | select track (shows its waveform) · same step again = next loop page |
| **Track buttons 1–4** | menus: Input FX · Global FX · Perform · Settings |

### Buttons and knobs
| Control | Action |
|---------|--------|
| **8 knobs** | selected-track params (or menu / punch params) |
| **Touch a knob** | full 8-knob page on screen (~5 s) |
| **Up / Down** | previous / next loop page (P1–P4) |
| **Jog wheel** | scrub the selected loop (audible, tape-style) · in P4 moves the touched head |
| **Capture** | Tape menu |
| **Sample/Record** | Sessions menu · **Shift + Sample** = threshold-arm (pad blinks red) · **+ jog** sets the threshold |
| **Mute / Copy / Loop** (held) | modifiers — lit while held |
| **Undo** | restore the last-cleared loop |
| **Back** | close a menu, then exit |
| **Full exit** | **Shift + Volume + Jog-click** (a plain Back only *suspends*) |

### Screens
The main screen shows the 16-track strip, CPU and input level. Touching a knob or opening a
menu shows the **full 8-knob page**; pressing a step (or scrubbing) shows the loop's
**waveform with the active playheads riding over it**. Both fall back after ~5 s.

---

## Signal chain

```
Record path: input -> preamp/tape model -> tape drive -> input EQ -> HF rolloff / low cut
             -> wow + flutter -> generations -> [loop buffers]

Per voice:   4 playheads -> Seed slice re-order -> saturation -> wow/flutter
             -> DJ filter (+reso) -> tilt EQ -> Studer 962 EQ -> stability
             -> amp env -> pan/vol -> sends A/B

Master:      sum of voices + MIDI-poly -> input monitor -> clock SR-degradation
             -> + Palette send returns -> global saturation -> master wow/flutter
             -> compressor -> lo/hi cut -> Stumble -> dropout
             -> punch-FX (4 in series) -> master out -> soft limiter -> output
```

---

## Build and install

```bash
cd overtake-shell
./scripts/build.sh      # Docker cross-compiles dsp.so (aarch64) + validates ui.js
./scripts/install.sh    # scp + atomic-rename install to move.local
```

Then on the Move: rescan modules and open **LBX / LoopBox** from the Overtake list.
**If it was already loaded, full-exit first** (Shift + Volume + Jog-click) — `suspend_keeps_js`
otherwise resumes the old code.

`MOVE_HOST` overrides the target (`MOVE_HOST=ableton@172.16.254.1 ./scripts/install.sh`).

> **Deploy safety:** the installer stages to `/data/UserData` and moves files into place
> atomically — never write straight over a mapped `.so`, which crashes the audio process.

---

## Repository layout

```
overtake-shell/            <- the active module
  module.json              Overtake manifest (id/name/capabilities)
  src/
    loopbox.c              engine: voices, playheads, punch-FX, stumble, sessions, master
    palette_fx.c/.h        24-effect Palette engine + Dattorro Plate
    fx_clouds.cc           Clouds-based Space/Bloom (C++)
    warps_data.c           Warps wavetables (Fold/Shift)
    ui.js                  QuickJS Overtake UI (pads, knobs, screens, LEDs)
  include/plugin_api_v1.h  host API
  vendor/                  clouds_engine + signalsmith (third-party DSP)
  scripts/                 build.sh, install.sh, Dockerfile
OVERTAKE-SDK.md            reverse-engineered Overtake SDK reference
design-spec.md             full design rationale
```

The top-level `src/` and `scripts/` are the earlier standalone build, superseded by `overtake-shell/`.

---

## Credits

- **Palette / Clouds** texture engine after Mutable Instruments *Clouds* and *Warps*.
- **signalsmith** stretch/DSP helpers.
- Glitch behaviours inspired by *Forgetful* (Stumble) and *Smack* (Seed slice re-order);
  tape and session patterns after *Magneto*; playhead ideas from *wrms* and *concrete*.

All third-party DSP is used under its own open-source license; see `overtake-shell/vendor/`.

## License

GPL-3.0
