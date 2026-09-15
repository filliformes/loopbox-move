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
- **Format:** 44100 Hz, 128-frame blocks, stereo · **Version:** 0.8.0 · **Manual:** [online](https://filliformes.github.io/loopbox-move/) · [markdown](docs/MANUAL.md) · **License:** GPL-3.0

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
| **P1 · Loop** | **Speed** · Filter · Pan · Volume · Start · End · Reverse · **Send A** |
| **P2 · Texture** | **Pitch** · **Reso** · Sat · Comp · Wow/Flutter · Scatter · **Seed** · Send B |
| **P3 · Tone** | Studer **Bass · MidF · MidGain · Treble** · Tilt · Attack · Decay · **Heads ▸** |
| **P4 · Playheads** | H1 mode/speed · H2 · H3 · H4 |

- **Speed** — playback rate, ±2 octaves in 0.1-semitone steps (pitch and tempo together, like tape).
- **Pitch** — an independent shift, −24 to +24 semitones, tempo untouched: a Signalsmith Stretch
  phase-vocoder shifter whose latency is cancelled by nudging the playheads, so the loop stays in time.
- **Seed** — a Smack-style *seeded slice re-order* (2/4/8/16 slices, some reversed). The knob
  *is* the seed: every position is a different reproducible mangle, click-free.
- **Scatter** — stochastic slice jumps, crossfaded.
- **DJ Filter + Reso** — continuous LP/HP sweep with resonance, smoothed over ~10 ms.
- **Attack / Decay** — per-loop amplitude envelope (3 ms → 3 s / 5 s), used on trigger,
  mute, pause and stop.

### Two send buses — the Palette engine
Send A and B each select from **29 effects** (Off + 24 Palette effects + four reverbs):
Drive, Sweeten, Fuzz, Howl, Fold, Swell, Doubler, Vibrato, Phaser, Tremolo, Pitch, Shift,
Cascade, Reels, Collage, Reverse, Space, Bloom, Filter, Squash, Cassette, Broken,
Interference, Halo, **Plate**, **Quartz**, **Prism**, **Veil** — each with Amount / Macro / Drift.

The last four are proper reverbs, and the last three come from the standalone instruments:

| | Tank | Amount | Macro | Drift |
|---|---|---|---|---|
| **Plate** | Dattorro plate | decay | pre-delay | damping |
| **Quartz** | 8-line Hadamard FDN, dual-band damping | room → hall | dark → bright | still → swimming |
| **Prism** | same tank, frequency-dependent decay | decay | lows ring ⇄ highs shimmer | crossover + movement |
| **Veil** | Householder FDN, modulated diffusers | size + tail | dark → bright | movement + colour |

### Punch-in FX (right 16 pads)
Sixteen momentary effects over a 2-second capture ring, in four families —
**Loops** (1/12 · 1/16 · short · **Chop**, with eight rhythmic patterns from Signal),
**Grains** (Haze · Mosaic · Smear · Strum), **Pitch** (Oct+ · Oct− · Glide · Shimmer) and
**Time** (Stretch · Freeze · Reverse · **PalFX**, one Palette effect as a punch: FX / Amount /
Macro / Drift, default Space) — **up to 5 stacked in series**. Slice effects auto-pan in
sync with their rate, and every slot loudness-matches its wet to the dry it replaces.
Hold a pad to apply; **knobs 5–8 edit the held effect's four parameters** live, and
**pad pressure** drives a per-effect expression (subdivide, density, glide, freeze, rate…)
shown in the footer. **Shift + pad latches** it on hands-free; **Shift while holding** latches
it exactly as it is, pressure included. **Undo + pad** resets a pad to its defaults.

### FX sequencer (✕ button)
One shared 16-step pattern of punch pads, modelled on the Polyend MESS. **Tap ✕** to run or
stop; **hold ✕** to see the pattern on the step buttons (orange = step, dim = extension,
white = playhead) and its page: Run · Speed (1/32 … 1 beat) · Length · Chance · Gate ·
Swing · Direction · Clear. **✕ + pad(s) + step** writes up to five pads into a step with
their knobs and pressure locked; **✕ + step** clears it; **✕ + step + later step** extends
it; **✕ + held step + knob 4** sets that step's play chance (Always, 10–90 %, Like Last,
Play X Skip Y) and **knobs 5–8** edit its locks. Play restarts the pattern.

### Tape transport (◀ ▶)
Hold **Left** and the whole master brakes to a stop in about three seconds; hold **Right**
and it winds up to a tone. Release and it eases back to 1×. The glide is linear in
semitones and the last octaves of a stop fade to silence.

### The Tape machine (record path)
A full input tape stage on the **Capture** button: **Tape model** (13, including a true
**Tapeless** bypass) · Drive · Wow · Flutter · HF rolloff · Low cut · Hiss · **Generations**
(approximates repeated dubs). Default is `Clean`.

### Sessions
**32 numbered slots**, saved and loaded from the **Sample/Record** button, each named
`slot_YYYYMMDD_HHMM`; saving over a used slot asks for confirmation. Settings *and*
recorded audio are stored; all disk work runs on a `SCHED_OTHER` worker thread pinned to
cores 0–2, never on the audio callback. Sessions live in
`/data/UserData/schwung/loopbox-sessions/` so reinstalls keep them.

### Perform, MIDI and I/O
- **Perform menu:** Stumble (probabilistic step glitch), Jump, Scan, Dropout.
- **MIDI keyboard:** 8-voice polyphony playing a loop chromatically through its full FX
  chain. **Off by default** (Settings → MIDI) so Move tracks' MIDI-out cannot trigger loops.
- **Settings:** Master Out, global sat (to 2.0), master Lo/Hi cut, arm threshold, overdub mode, root note, MIDI.
- **Undo** reverts the last overdub exactly (each overwritten sample is saved as it goes), else restores the last cleared loop.

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
| **Right pad** | punch-FX (momentary) · **Shift + pad** = latch · **Undo + pad** = reset params · **✕ + pad + step** = sequence |
| **Step** | select track (shows its waveform) · same step again = next loop page |
| **Track buttons 1–4** | menus: Input FX · Perform · Send FX · Settings |

### Buttons and knobs
| Control | Action |
|---------|--------|
| **8 knobs** | selected-track params (or menu / punch params) |
| **Touch a knob** | full 8-knob page on screen (~5 s) |
| **Up / Down** | previous / next loop page (P1–P4) |
| **Jog wheel** | scrub the selected loop (audible, tape-style) · in P4 moves the touched head |
| **Capture** | Input Tape menu |
| **✕ (Delete)** | tap = run/stop the FX sequencer · hold = pattern view + FX Seq page |
| **Left / Right** | tape stop / tape wind (held) |
| **Sample/Record** | Sessions menu · **Shift + Sample** = threshold-arm (pad blinks red) · **+ jog** sets the threshold |
| **Mute / Copy / Loop** (held) | modifiers — lit while held |
| **Undo** | revert the last overdub, else restore the last-cleared loop |
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

Per voice:   4 playheads -> Seed slice re-order -> Scatter -> Pitch (Stretch) -> saturation
             -> wow/flutter -> DJ filter (+reso) -> tilt EQ -> Studer 962 EQ -> stability
             -> compressor -> amp env -> tape transport -> pan/vol -> sends A/B

Master:      sum of voices + MIDI-poly -> input monitor -> + Palette send returns
             -> global saturation -> master wow/flutter -> compressor -> lo/hi cut
             -> Stumble -> dropout -> punch-FX (5 in series) -> master out
             -> soft limiter -> output
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

---

## Credits

LoopBox stands on a lot of open work. Code that is vendored or ported is used under its own
license (see `overtake-shell/vendor/` and the file headers); the rest is inspiration.

### Code used
- **Mutable Instruments Clouds and Warps** — Émilie Gillet, MIT. The Palette texture and
  Space engines. https://github.com/pichenettes/eurorack
- **Signalsmith DSP library** — Geraint Luff, MIT. Filters, delays, STFT helpers.
  https://github.com/Signalsmith-Audio/dsp
- **Signalsmith Stretch 1.1.0** — Geraint Luff, MIT. The per-loop Pitch shifter.
  https://github.com/Signalsmith-Audio/signalsmith-stretch
- **Airwindows** — Chris Johnson, MIT. Ported or adapted: Spiral, Density, Mojo, Swell,
  Tremolo, Pressure4, ToTape6 (flutter), DeRez2 (Interference), IronOxide (global saturation).
  https://github.com/airwindows/airwindows · https://www.airwindows.com
- **Dattorro plate** — Jon Dattorro, *Effect Design Part 1: Reverberator and Other Filters*,
  JAES 45(9), 1997. The Plate send effect at the paper's delay lengths.
  https://ccrma.stanford.edu/~dattorro/EffectDesignPart1.pdf
- **Schwung** — Charles Vestal and contributors. The Overtake framework, and the knob-page
  widgets (arc knobs, enum squares, buttons, header, bank bar, footer pills) are ported
  verbatim from its `render_page_movy.mjs`. https://github.com/charlesvestal/schwung

### Sibling Move modules (same author)
- **Palette** — the 24-effect send engine LoopBox embeds. https://github.com/filliformes/palette-move
- **Magnéto** — the tape-input stage, sessions, scrub and Tape page patterns. https://github.com/filliformes/magneto-move
- **Signal** — the eight Chop rhythm patterns. https://github.com/filliformes/signal-move
- **Structor** — several punch-effect ideas. https://github.com/filliformes/structor-move
- **Res** and **Essaim** — the Quartz and Prism reverbs: one dual-mode 8-line Hadamard FDN whose
  two feedback colourings fill the roles of Ableton's `abl.dsp.quartz~` and `abl.dsp.prism~` Max
  objects. https://github.com/filliformes/res · https://github.com/filliformes/essaim
- **Phasma** — the Veil reverb: a modulated Householder FDN (Signalsmith-style tank with
  CloudSeed-style modulated diffusers) carrying Prism's band-split colouring.
  https://github.com/filliformes/Phasma

### Other Schwung modules
- **Smack** — Tim Cox. The Seed slice re-order is modelled on its Seed parameter.
  https://github.com/timncox/schwung-smack

### Design inspiration
- **Polyend MESS** — the FX sequencer: per-step locks, play chance, extensions, gate and swing.
  https://polyend.com/mess/
- **1010music Blackbox** — sixteen free-running pads. https://1010music.com/product/blackbox
- **Kinotone Ribbons** — tape character as an instrument. https://kinotone.com/ribbons
- **Puremagnetik LAPS** — layered asynchronous loops. https://puremagnetik.com
- **Chase Bliss Blooper, Mood MK2, Generation Loss MK2** — Stability, the old Clock's
  degradation, Disintegration overdub, Generations. https://www.chasebliss.com
- **Hologram Microcosm** — the grain and glide punch families. https://hologramelectronics.com/microcosm
- **norns loopers** — wrms, concrète, cranes, oooooo, otis, reels, ndls, samsara, mlre, nydl,
  giro: multiple playheads, threshold arm, loop multiples, jog scrub. https://norns.community
- **Forgetful** — the Stumble perform gesture. https://github.com/charlesvestal/schwung (module catalog)

## License

GPL-3.0
