# LoopBox Manual

*16-track stereo tape looper for Ableton Move (Schwung Overtake module) — v0.6*

This is the full reference. The [README](../README.md) is the short tour.

---

## 1. Layout

```
 ┌────────────────────────── screen ──────────────────────────┐   [K1] [K2] [K3] [K4] [K5] [K6] [K7] [K8]
 │ header · knob page / waveform / overview · footer hints     │
 └─────────────────────────────────────────────────────────────┘
 [Step 1 … Step 16]                       ← select a loop / FX-sequencer pattern
 [Track 1] [Track 2] [Track 3] [Track 4]  ← menus: Input FX · Perform · Send FX · Settings
 ┌ left 4×4 pads ┐  ┌ right 4×4 pads ┐
 │  16 loops     │  │  16 punch FX   │
 └───────────────┘  └────────────────┘
```

- **Left pads** are the sixteen loops. Their colour shows the state (empty · recording · playing · paused · overdubbing) tinted by speed (blue ½×, green 1×, yellow 2×).
- **Right pads** are the sixteen punch-in effects, coloured by family (blue Loops, pink Grains, purple Pitch, yellow Time, red PalFX).
- **Steps** select the loop whose knobs you are editing, and double as the FX-sequencer pattern while ✕ is held.
- **Knobs 1–8** edit the page on screen. Touching a knob shows the full page and inverts the header with the parameter's full name and value while you hold it.

The screen has three views: the **overview** (track strip, CPU, input level, footer hints), the **knob page** (Schwung's arc knobs, enum squares, buttons and big numbers), and the **waveform** of the selected loop with its playheads. Knob pages and the waveform fall back to the overview after ten seconds without input.

---

## 2. Loops

### 2.1 Recording and playback

| Gesture | Action |
|---|---|
| Tap an empty pad | start recording (up to 45 s) |
| Tap while recording | close the loop and play |
| Tap while playing | pause · tap again to resume (click-free) |
| Double-tap while playing or paused | overdub · tap again to stop overdubbing |
| Hold a pad (~1 s) | clear the loop (Undo restores it) |
| Shift + tap | cycle playback speed ½× · 1× · 2× |
| Mute + tap | quick mute; the playhead keeps running so the loop returns in phase |
| Copy + pad, then a second pad | clone the first loop into the second (audio and settings, done on the worker) |
| Loop + pad | cycle the loop length 1× · ½× · ¼× · ⅛× |
| Shift + Sample, then a pad | threshold-armed record: the pad blinks red and recording starts when the input crosses the threshold (Shift + Sample + jog sets it) |

Loops are free-running: they do not need to share a length or a downbeat.

### 2.2 Overdub modes (Settings → ODub)

- **Replace** — new audio replaces the old.
- **Multiply** — endless layering; older passes decay each cycle. **This is the default.**
- **Disintegration** — the loop's own effects are baked in on every pass, so it slowly falls apart.

### 2.3 Undo

**Undo** reverts the last overdub exactly. While an overdub runs, every sample it overwrites is saved the moment it is replaced, so the restore is sample-accurate and costs nothing on the audio thread; the restore itself runs on the worker. If no overdub is pending, Undo restores the last cleared loop instead.

### 2.4 Selecting and pages

Press a **step** to select that loop: the screen shows its waveform with the active playheads riding over it. Press the same step again to cycle through its four knob pages. **Up** and **Down** move between pages from anywhere, including the waveform view.

| Page | K1 | K2 | K3 | K4 | K5 | K6 | K7 | K8 |
|---|---|---|---|---|---|---|---|---|
| **P1 Loop** | Speed | Filter | Pan | Volume | Start | End | Reverse | Send A |
| **P2 Texture** | Pitch | Reso | Sat | Comp | Wow/Flutter | Scatter | Seed | Send B |
| **P3 Tone** | Bass | Mid Freq | Mid Gain | Treble | Tilt | Attack | Decay | Heads ▸ |
| **P4 Playheads** | H1 mode | H1 speed | H2 mode | H2 speed | H3 mode | H3 speed | H4 mode | H4 speed |

- **Speed** — playback rate, ±2 octaves in 0.1-semitone steps. Pitch and tempo move together, like tape.
- **Pitch** — an independent pitch shift, −24 to +24 semitones, that leaves the tempo alone. It is a Signalsmith Stretch phase-vocoder shifter; its latency is cancelled by nudging the loop's playheads, so a shifted loop stays in time. At exactly 0 it is fully bypassed.
- **Filter + Reso** — a DJ-style filter: left of centre low-pass, right of centre high-pass, resonance on Reso. Smoothed over 10 ms.
- **Start / End** — loop window. End is a length from Start.
- **Sat** — per-loop tape saturation with unity makeup.
- **Comp** — per-loop compressor; makeup gain fades in with the knob.
- **Wow/Flutter** — per-loop tape instability.
- **Scatter** — random slice jumps, crossfaded.
- **Seed** — a seeded slice re-order (2/4/8/16 slices, some reversed). The knob *is* the seed: every position is a different, repeatable mangle.
- **Bass / Mid Freq / Mid Gain / Treble** — a Studer-style channel EQ. **Tilt** tips the whole spectrum.
- **Attack / Decay** — a per-loop amplitude envelope (3 ms – 3 s / 5 s) used on trigger, mute, pause and stop.
- **Heads ▸** — jumps to the Playheads page.

### 2.5 Playheads

Every loop can be read by four heads at once. Modes are **Off · Fwd · Bwd · Ping · Jump**; speeds run 0.25× to 4× in 0.1-semitone steps. **Jump** plays forward but leaps to a random position at random intervals, each leap crossfaded. Head 1 is the main head (Scatter, Seed and scrub drive it). Turning a head on restarts it at the loop start. Heads sum with 1/√n normalisation. Any head that moves position (jog, pitch-shift nudge, scatter, a Jump leap) does so through a short crossfade.

In the Playheads page, **touch a head's knob and the jog wheel moves that head** along the waveform. Everywhere else the jog **scrubs** the selected loop, tape-style: the head travels for a moment and you hear it, even on a paused loop.

---

## 3. Punch-in FX (right pads)

Sixteen momentary effects on the master, reading a 2-second capture ring. Hold a pad to apply; up to **five** can run in series. Each pad has four parameters on knobs 5–8 while it is held, and **pad pressure** drives a per-effect expression shown in the footer.

| Row | Pads | Knobs 5–8 | Pressure |
|---|---|---|---|
| **Loops** | Loop12 · Loop16 · LoopSh · **Chop** | Rate · Pitch · Tone · Mix (Chop: Rate · **Pattern** · Tone · Mix) | subdivides the loop continuously (Chop: rate ×2) |
| **Grains** | Haze · Mosaic · Smear · Strum | Size/Grid/Rate · Pitch/Dir · Density/Var/Tone · Mix | density · grid ×2 · density · faster + wider |
| **Pitch** | Oct+ · Oct− · Glide · Shimmer | Fine/Len/Regen · Pitch/Glide · Tone · Mix | mix · mix · glide · regen |
| **Time** | Stretch · Freeze · Reverse · **PalFX** | Stretch/Frz/Len · Pitch · Grain/Tone · Mix (PalFX: **FX · Amount · Macro · Drift**) | freeze · freeze · shorter · amount |

- **Chop** repeats the last transient on one of eight rhythmic patterns (Morse bursts, Ikeda burst/silence, pairs, straight eighths, syncopated) chosen with knob 6.
- **PalFX** runs one Palette effect (all 26, default Space) as a punch. It is block-processed, so its wet is one block late, like the send buses.
- Slice effects auto-pan in time with their rate. Every slot loudness-matches its wet to the dry it replaces. Loops, Reverse, Glide, Chop and a frozen Stretch freeze their capture ring while held, so a loop held longer than two seconds is never overwritten under the head.

| Gesture | Action |
|---|---|
| Hold pad | apply |
| Shift + pad | latch (hands-free); Shift + pad again unlatches |
| Shift while holding a pad | latch it exactly as it is, pressure included |
| Undo + pad | reset the pad's four knobs to their defaults |
| ✕ + pad(s) + step | write the pads into the FX sequencer (section 5) |

Knobs 5–8 only follow a pad while it is physically held; latched pads keep running but give the knobs back to the page.

---

## 4. Menus (Track buttons, Capture, Sample, Menu)

| Button | Menu | K1 | K2 | K3 | K4 | K5 | K6 | K7 | K8 |
|---|---|---|---|---|---|---|---|---|---|
| Track 1 | **Input FX** | Monitor | Tape Style | Input Gain | Low | Mid | Mid Freq | High | High Freq |
| Track 2 | **Perform** | Stumble Mix | Stumble Step | Stumble Odds | Stumble Size | Stumble Reach | Stumble Kind | Jump | Scan |
| Track 3 | **Send FX** | A FX | A Amount | A Macro | A Drift | B FX | B Amount | B Macro | B Drift |
| Track 4 | **Settings** | Master Vol | gSat (to 2.0) | Lo Cut (20–1000 Hz) | Hi Cut | Arm Threshold | ODub mode | Root | MIDI |
| Capture | **Input Tape** | Tape Style | Drive | Wow | Flutter | HF | Lo Cut | Hiss | Generations |
| Sample | **Drift** | Drift | Rate | Size | FBk | Supr | Blur | Damp | Mix |
| ≡ (Menu) | **Sessions** | Slot | Save | Load | | | | | |
| ✕ (held) | **FX Seq** | Run | Speed | Length | Chance | Gate | Swing | Direction | Clear |

Press the same button again, or **Back**, to close a menu. Enums step once per four detents so a fast turn does not race through the list.

- **Input Tape** — 13 tape styles including a true **Tapeless** bypass (default Clean), drive, wow, flutter, HF rolloff, low cut, hiss and **Generations** (repeated-dub loss). It shapes what gets recorded.
- **Send FX** — two Palette buses (29 effects: Drive, Sweeten, Fuzz, Howl, Fold, Swell, Doubler, Vibrato, Phaser, Tremolo, Pitch, Shift, Cascade, Reels, Collage, Reverse, Space, Bloom, Filter, Squash, Cassette, Broken, Interference, Halo, Plate, Quartz, Prism, Veil), each with Amount, Macro and Drift. Effect switches happen on the worker; the bus mutes for a few milliseconds while it swaps.

  The last four are full reverbs, all 100% wet (they sit on a send):

  | Effect | Tank | Amount | Macro | Drift |
  |---|---|---|---|---|
  | **Plate** | Dattorro plate at the paper's delay lengths | decay | pre-delay + darkening | damping |
  | **Quartz** | 8-line Hadamard FDN, 4 input diffusers, HF one-pole + LF shelf damping | room → hall | dark → bright | modulation + pre-delay |
  | **Prism** | the same tank with per-band RT60: lows, mids and highs decay separately | decay length | tilt: lows ring ⇄ highs shimmer | crossover + modulation |
  | **Veil** | Householder FDN with LFO-modulated diffusers, long in-loop-damped lines | size + tail | dark → bright | movement + band colour |

  Quartz and Prism are ported from Res and Essaim, where they stand in for Ableton's
  `abl.dsp.quartz~` and `abl.dsp.prism~`; Veil is Phasma's tank, which is what makes it
  lush rather than metallic. Size changes glide, so turning Amount morphs the room
  tape-style instead of clicking.
- **Perform** — Stumble (a probabilistic step glitcher), plus **Jump** (crossfaded random jump on every playing loop) and **Scan** (a fast sweep) as buttons.
- **Drift** (Sample button) — a global drifting-delay memory in the spirit of Soma COSMOS. Four coprime-length delay lines, each read at a slowly drifting tap, feed back through a matrix that morphs from self-feedback to a normalised Hadamard cross-mix. The loop mix feeds it, the memory recirculates, and because the line lengths are coprime and each has its own asynchronous LFO, the recombination never lands on an exact repeat. It sits in the master chain just before the pump, so the ambient layer picks up the character EQ, glue and limiter. Feedback is capped below unity, so the tail always fades (up to a few minutes at maximum), and a **silence bleed** clears an abandoned tail after about eight seconds with no input. Knobs: **Drift** (how much loop mix is fed in) · **Rate** (tap-drift speed) · **Size** (tap length, shimmer to long hall) · **FBk** (memory sustain, below unity fades, near unity holds) · **Supr** (loud new input erases old memory: play over to replace) · **Blur** (self-feedback → full cross-mix) · **Damp** (high-frequency damping of the tail) · **Mix** (wet level into the master). Drift and Mix start at zero, so it is silent until dialled in; it saves with the session.
- **MIDI** — off by default so Move's track MIDI cannot trigger loops. On, an external keyboard plays the selected loop chromatically with 8-voice polyphony.

---

## 5. FX sequencer (✕ / Delete)

One shared 16-step pattern of punch pads, in the spirit of the Polyend MESS.

| Gesture | Action |
|---|---|
| Tap ✕ | run / stop (the LED is bright while running) |
| Hold ✕ | pattern view on the steps: orange = step with FX, dim orange = extension, white = playhead; the FX Seq page opens. Turn a knob and the page stays open after release |
| ✕ + pad(s) + step | write up to five pads into the step. Each pad's four knobs and current pressure are locked into the step, so editing the pad later leaves the step alone. Pads pressed while ✕ is held are silent selections |
| ✕ + step (no pads) | clear the step |
| ✕ + hold a step + tap a later step | the steps between become extensions that hold the first step's effects |
| ✕ + hold a step + K4 | that step's Play Chance |
| ✕ + hold a step + K5–K8 | the locks of the step's first effect |
| Play | restarts the pattern from step 1 |

Page: **Run** · **Speed** (1/32 · 1/16 · 1/8T · 1/8 · 1/4 · 1/2 · 1 beat) · **Length** (1–16) · **Chance** (Always · 10–90% · Like Last · Play 1 Skip 1 · Play 2 Skip 1 · Skip 1 Play 1; the default for new steps when no step is held) · **Gate** (10% – Hold) · **Swing** (50–75%) · **Direction** (Fwd · Bwd · Ping · Rand) · **Clear** (asks first).

The clock runs from the Move tempo. A step retriggers its effects the way a finger would, through a short fade. A pad you are holding by hand is never cut by the sequencer. Gate below Hold releases the effects part-way through the step unless the next step is an extension. The pattern and its locks save with sessions.

---

## 6. Tape transport (◀ ▶)

Hold **Left** and the whole master brakes to a stop in about three seconds; hold **Right** and it winds up to a tone. Release and it eases back to 1× in a third of a second. The glide is linear in semitones and the last octaves of a stop fade to silence, so it never sits on a hum.

---

## 7. Sessions (≡ Menu button)

Thirty-two slots. **Slot** browses (one slot per four detents), **Save** writes, **Load** reads. Saving over a used slot asks first (K8 = yes, K5 = no, Back cancels). Each slot is named by date and time (`Sep 14 21:30` in the footer); the header shows which session is loaded, or **New**. A successful save shows a burst.

A session holds every setting, the punch pad values, the FX-sequencer pattern and all recorded audio. Disk work runs on a worker thread pinned to cores 0–2, never on the audio callback. Files live in `/data/UserData/schwung/loopbox-sessions/` and survive reinstalls.

---

## 8. Signal chain

```
Record path: input -> tape style -> drive -> input EQ -> HF rolloff / low cut
             -> wow + flutter -> generations -> [loop buffer]

Per loop:    4 playheads -> Seed re-order -> Scatter -> Pitch (Stretch) -> saturation
             -> wow/flutter -> DJ filter (+reso) -> tilt EQ -> Studer EQ -> stability
             -> compressor -> amp envelope -> tape transport gain -> pan/vol -> sends A/B

Master:      sum of loops + MIDI poly -> input monitor -> + Palette send returns
             -> global saturation -> master wow/flutter -> master compressor
             -> lo/hi cut -> Stumble -> dropout -> punch FX (5 in series)
             -> Drift -> master volume -> soft limiter -> output
```

---

## 9. Shortcut sheet

| Keys | Action |
|---|---|
| Pad tap / double-tap / hold | rec-play-pause · overdub · clear |
| Shift + pad | speed ½× 1× 2× |
| Mute + pad · Copy + pad, pad · Loop + pad | quick mute · clone · loop length |
| Shift + Sample (+ jog) | threshold-arm (+ threshold) |
| Right pad · Shift + right pad · Shift while held · Undo + right pad | punch · latch · latch as-is · reset |
| Step · same step again · Up / Down | select loop (waveform) · next page · prev/next page |
| Jog | scrub · in P4: move the touched head |
| Track 1–4 · Capture · Sample · ≡ | Input FX · Perform · Send FX · Settings · Input Tape · Drift · Sessions |
| ✕ tap · ✕ hold · ✕ + pads + step · ✕ + step · ✕ + step + step | seq run/stop · pattern view · write · clear · extend |
| ◀ / ▶ (held) | tape stop · tape wind |
| Undo | revert last overdub, else restore last clear |
| Back | close menu / popup, then leave |
| Shift + Back (or Shift + Volume + Jog-click) | full exit (a plain Back only suspends) |
