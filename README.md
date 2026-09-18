# Loopex

**A 16-track live tape looper for the Ableton Move, built as a Schwung *Overtake* module.**

Loopex turns the Move into an asynchronous, tape-flavoured looping instrument: sixteen
stereo loops on the left pads — each with **up to four independent playheads** — a bank of
momentary/latchable glitch FX on the right pads, two send buses drawing on a 24-effect
Palette engine plus four reverbs, a full record-path tape machine, session save/load,
and a MIDI-keyboard polyphony layer.

> Inspired by the 1010music BlackBox looper workflow, and by the sound of Kinotone Ribbons,
> Chase Bliss Blooper / Mood MK2, and Microcosm-style glitch. Interaction ideas borrowed
> from the norns looper world (*wrms*, *cranes*, *oooooo*, *samsara*, *nydl*, *otis*).

- **Module:** `loopex` · **Name:** Loopex · **Type:** Overtake (Schwung) · **API v2**
- **Format:** 44100 Hz, 128-frame blocks, stereo · **Version:** 0.8.6 · **Manual:** [online](https://filliformes.github.io/loopex-move/) · [markdown](docs/MANUAL.md) · **License:** GPL-3.0

---

## What it does

### 16 stereo tape loops
Each left pad is an independent, free-running stereo loop (up to 45 s). Record, play, pause
and overdub live; every loop has its own speed, tone, FX sends, amp envelope and playheads.

- **Overdub modes:** Replace · **Multiply** (default; endless layering with decay) · Disintegration
  (the loop's FX are re-applied each pass, so it slowly falls apart).
- Playback speed is tinted onto the pad LED (blue ½× · green 1× · yellow 2×).

### Up to 4 playheads per loop
Every loop can be read by **four independent heads at once**, each with its own mode and
speed — one recording becomes a canon, a drone, or a ping-ponging texture.

- Modes: **Off · Fwd · Bwd · Ping** (ping-pong) **· Jump** (leaps to a random spot at random intervals, crossfaded).
- Per-head speed (0.25×–4×); heads sum with `1/sqrt(n)` normalisation so stacking stays sane.
- Turning a head on **always restarts it at the loop start**.
- Head 1 is the main head — Scatter, Seed, scrub and Jump all drive it.
- In the Playheads page, **touch a head's knob and the jog wheel moves that head** along the waveform.

### The five loop pages
Navigate with **Down** (next) and **Up** (previous).

| Page | Knobs |
|------|-------|
| **P1 · Loop** | **Speed** · Filter · Pan · Volume · Start · End · Reverse · **Send A** |
| **P2 · Texture** | **Pitch** · **Reso** · Sat · Comp · Wow/Flutter · Scatter · **Seed** · Send B |
| **P3 · Tone** | Studer **Bass · MidF · MidGain · Treble** · Tilt · Attack · Decay · **Heads ▸** |
| **P4 · Playheads** | H1 mode/speed · H2 · H3 · H4 |
| **P5 · HeadMix** | per-head **Vol/Pan** — H1 (mirrors P1 Vol/Pan) · H2 · H3 · H4 |

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
**Grains** (Haze · Mosaic · Smear · Strum), **Pitch** (Oct− · Oct+ · Glide · Shimmer) and
**Time** (Stretch · Freeze · Reverse · **PalFX**, one Palette effect as a punch: FX / Amount /
Macro / Drift, default Space) — **up to 5 stacked in series**. Slice effects auto-pan in
sync with their rate, and every slot loudness-matches its wet to the dry it replaces.
**Oct+, Oct−, Shimmer and Chop are spread to stereo by a microshifter**, the same widener
Stretch and Freeze use.
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

### Drift — a global evolving memory (Sample button)
A drifting-delay memory station inspired by **Soma COSMOS**, on the **Sample** button. Four
coprime-length delay lines, each read at a slowly drifting tap, feed back through a matrix
that morphs from self-feedback to a normalised **Hadamard** cross-mix. The loop mix feeds it,
the memory recirculates, and the coprime lengths plus per-line async LFOs keep it from ever
landing on an exact repeat. It runs in the master chain just before the pump, tanh-limited so
high feedback sustains without runaway.

- **Drift** feed level · **Rate** tap-drift speed · **Size** tap length (shimmer → long hall)
  · **FBk** memory sustain (below unity fades, near unity holds) · **Supr** loud input erases
  old memory (play over to replace) · **Blur** self-feedback → full cross-mix · **Damp** tail
  high-frequency damping · **Mix** wet into the master.
- **FBk** is capped below unity, so the tail always fades: up to a few minutes at maximum. Leave everything
  paused and a **silence bleed** clears an abandoned tail after about eight seconds with no input.
- Drift and Mix start at zero (silent until dialled in); ~1.7 MB of memory; saves with the session.

### Sessions
**64 numbered slots**, saved and loaded from the **≡ (Menu)** button, each named
`slot_YYYYMMDD_HHMM`; saving over a used slot asks for confirmation. Settings *and*
recorded audio are stored; all disk work runs on a `SCHED_OTHER` worker thread pinned to
cores 0–2, never on the audio callback. Sessions live in
`/data/UserData/schwung/loopex-sessions/` so reinstalls keep them.

### Perform, MIDI and I/O
- **Perform menu:** Stumble (probabilistic step glitch), Jump, Scan, Dropout.
- **MIDI keyboard:** 8-voice polyphony playing a loop chromatically through its full FX
  chain. **Off by default** (Settings → MIDI) so Move tracks' MIDI-out cannot trigger loops.
- **Settings (two pages):** page 1 is behaviour and I/O — arm threshold, overdub mode, **loop filter**
  (the 12 Master-filter voicings applied to every loop's low-pass), root note, input monitoring,
  **input source** (see below), MIDI In and MIDI Out. Page 2 is the master — Master Out, master Lo/Hi
  cut, punch width, character EQ, global sat (to 2.0), glue and tape limiter.
- **Record source (Settings p1 → InSrc):** `Line · Master · S1 · S2 · S3 · S4 · M1 · M2 · M3 · M4`.
  **Line** is the line/mic input (default). **Master** is the Move's whole master mix, minus Loopex's
  own output so it can never feed back. **S1–S4** are Schwung's own four mixer slots and **M1–M4** the
  four OG Move hardware tracks, each captured as an isolated stereo stem over **Ableton Link Audio**:
  the host publishes its slots on `/schwung-pub-audio` and reconstructs the Move tracks on
  `/schwung-link-in`, and Loopex reads the selected channel (needs the host's `link_audio_publish`, on
  by default). So a loop can sample any single Schwung or Move track on its own, not just the summed mix.
- **Undo** reverts the last overdub exactly (each overwritten sample is saved as it goes), else restores the last cleared loop.

### LaunchControl XL (external MIDI)
A Novation LaunchControl XL can drive all sixteen loops. **MIDI In** (Settings p1 → `MIDI`) enables
external control; **MIDI Out** (Settings p1 → `MidiO`) mirrors each loop's transport back to the LCXL's
LEDs, sent on **MIDI channel 2** so the notes never collide with the Move's own channel-1 pads.

- **Track Focus buttons (row 1)** — record / play / pause per loop; **Track Control buttons (row 2)** —
  mute / unmute.
- Per column the **top knob is Loop Speed**, then Filter, then Pan, and the **fader is Volume**, for
  that loop.
- Two SysEx templates ship at the repo root — `BlackBox 1-8.syx` (loops 1–8) and `BlackBox 9-16.syx`
  (loops 9–16); set both to **MIDI channel 2** so the LED feedback lands.

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
| **Up / Down** | previous / next loop page (P1–P5) |
| **Jog wheel** | scrub the selected loop (audible, tape-style) · in P4 moves the touched head |
| **Capture** | Input Tape menu |
| **✕ (Delete)** | tap = run/stop the FX sequencer · hold = pattern view + FX Seq page |
| **Left / Right** | tape stop / tape wind (held) |
| **Sample/Record** | Drift menu · **Shift + Sample** = threshold-arm (pad blinks red) · **+ jog** sets the threshold |
| **≡ (Menu)** | Sessions menu |
| **Mute / Copy / Loop** (held) | modifiers — lit while held |
| **Undo** | revert the last overdub, else restore the last-cleared loop |
| **Back** | close a menu, then exit |
| **Full exit** | **Shift + Volume + Jog-click** (a plain Back only *suspends*) |

### Screens
The main screen shows the 16-track strip, CPU and input level. Touching a knob or opening a
menu shows the **full 8-knob page**; pressing a step (or scrubbing) shows the loop's
**waveform with the active playheads riding over it**. Turning a loop's **Start** or **End** shows that
waveform with labelled **S** and **E** markers at the trim points. Both fall back after ~5 s.

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
             -> Stumble -> dropout -> punch-FX (5 in series) -> Drift -> master out
             -> soft limiter -> output
```

---

## Build and install

Loopex is on the **Schwung Module Store** — most people can install it from the Schwung
Manager (search *Loopex*), no build needed. To build from source you need a host with
**Docker** (cross-compiles the ARM64 `dsp.so`) and **ssh/scp**, and an **Ableton Move**
running **Schwung** reachable over USB-C at `move.local` (or `172.16.254.1`):

```bash
cd overtake-shell
./scripts/build.sh      # Docker cross-compiles dsp.so (aarch64) + validates ui.js
./scripts/install.sh    # scp + atomic-rename install to move.local
```

Then on the Move: rescan modules and open **Loopex** from the Overtake list.
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
    loopex.c              engine: voices, playheads, punch-FX, stumble, sessions, master
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

Loopex stands on a lot of open work. Code that is vendored or ported is used under its own
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
- **Palette** — the 24-effect send engine Loopex embeds. https://github.com/filliformes/palette-move
- **Magnéto** — the tape-input stage, sessions, scrub and Tape page patterns. https://github.com/filliformes/magneto-move
- **Signal** — the eight Chop rhythm patterns. https://github.com/filliformes/signal-move
- **Punchfx** — the right-pad **Punch-in FX** bank: sixteen PO-33-style, pressure-sensitive
  momentary effects. Loopex's punch engine descends directly from it. https://github.com/filliformes/punchfx-move
- **Fizzik** — the **12 loop/master filter voicings** (Clean · SEM · MS-20 · Steiner · three
  ladders · Prophet · Oberheim · Diode · K35 · Vintage): original analog-voiced designs built on
  a trapezoidal SVF + zero-delay-feedback transistor ladder (Zavalishin/Cytomic VA-filter math,
  RBJ cookbook); the MS-20 voicing was modelled from original Korg MS-20 schematics while working
  on Aphex. https://github.com/filliformes/fizzik-move
- **Structor** — several punch-effect ideas. https://github.com/filliformes/structor-move

### Other software instruments (same author)
- **Res** and **Essaim** — the Quartz and Prism reverbs: one dual-mode 8-line Hadamard FDN whose
  two feedback colourings fill the roles of Ableton's `abl.dsp.quartz~` and `abl.dsp.prism~` Max
  objects.
- **Phasma** — the Veil reverb: a modulated Householder FDN (Signalsmith-style tank with
  CloudSeed-style modulated diffusers) carrying Prism's band-split colouring.

### Other Schwung modules
- **Performance FX** — Charles Vestal. The framework author's pressure punch-in FX module,
  the pattern the Punch-in bank grows from (Loopex carries 16 on the right pads).
  https://github.com/charlesvestal/schwung-performance-fx
- **Smack** — Tim Cox. The Seed slice re-order is modelled on its Seed parameter.
  https://github.com/timncox/schwung-smack
- **Forgetful** — Idlir Fida. The Stumble perform gesture. https://github.com/kliegsablaze/forgetful

### Design inspiration
- **Polyend MESS** — the FX sequencer: per-step locks, play chance, extensions, gate and swing.
  https://polyend.com/mess/
- **1010music Blackbox** — sixteen free-running pads. https://1010music.com/product/blackbox
- **Kinotone Ribbons** — tape character as an instrument. https://kinotone.com/ribbons
- **Puremagnetik LAPS** — layered asynchronous loops. https://puremagnetik.com
- **Chase Bliss Blooper, Mood MK2, Generation Loss MK2** — Stability, the old Clock's
  degradation, Disintegration overdub, Generations. https://www.chasebliss.com
- **Hologram Microcosm** — the grain and glide punch families. https://hologramelectronics.com/microcosm
- **Studer 962** — the per-loop channel EQ, voiced from the broadcast console's schematic band specs.
- **Soma Laboratory COSMOS** — the Drift memory: prime-length shifting delay lines that recombine
  endlessly into an ever-evolving ambient layer. https://somasynths.com/cosmos/
- **norns loopers** — wrms, concrète, cranes, oooooo, otis, reels, ndls, samsara, mlre, nydl,
  giro: multiple playheads, threshold arm, loop multiples, jog scrub. https://norns.community

## License

GPL-3.0
