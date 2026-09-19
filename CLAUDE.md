# Loopex — Claude Code context

16-track asynchronous **stereo tape looper** for Ableton Move, built as a Schwung
**Overtake** module. An instrument for ambient & experimental music; every gesture click-free.
The name is a contraction of *loop* + *experimental*.

| | |
|---|---|
| Module id | `loopex` (name `Loopex`) — **never call it "LPX"** in prose/docs |
| Type | `component_type: "overtake"`, `api_version: 2` |
| Capabilities | `audio_in`, `midi_out`, `suspend_keeps_js`, `button_passthrough: [85]` |
| Language | C (`src/loopex.c`) + QuickJS UI (`src/ui.js`) |
| Format | 44100 Hz, 128-frame blocks, stereo int16 |
| Author | **Filliformes** |
| Repo | `filliformes/loopex-move` (branch `main`); local folder `loopbox-move/` is **legacy naming** |
| Install path | `/data/UserData/schwung/modules/overtake/loopex/` |

> There is a dedicated **`/loopex` skill** with deep references (architecture, surface,
> workflow). This file is the in-repo compressed context. `design-spec.md` holds the original
> design rationale (parts of it predate the Overtake conversion); `OVERTAKE-SDK.md` is the
> reverse-engineered SDK reference.

---

## Sonic intent
A creative tape looper where recordings are living, degradable material. Every stage adds
character: tape models colour the input, the per-voice chain sculpts playback, Stability
compounds degradation, Disintegration dissolves loops into abstraction, Drift remembers and
recombines. References: 1010music BlackBox, Kinotone Ribbons, Puremagnetik LAPS, Chase Bliss
Blooper / Mood mk2 / Generation Loss mk2, Hologram Microcosm, Soma COSMOS, Studer 962,
Airwindows, norns loopers. **Not** a clean digital looper, not a sampler, not (only) granular.

---

## DSP architecture
```
Record path: input -> preamp/tape model (13) -> tape drive -> input EQ -> HF rolloff / low cut
             -> wow + flutter -> generations -> [loop buffers]

Per voice:   4 playheads -> Seed slice re-order -> Scatter -> Pitch (Signalsmith Stretch)
             -> saturation -> wow/flutter -> DJ filter (+reso) -> tilt EQ -> Studer 962 EQ
             -> stability -> compressor -> amp env -> tape transport -> pan/vol -> sends A/B

Master:      sum of voices + MIDI-poly -> input monitor -> + Palette send returns
             -> global saturation -> master wow/flutter -> compressor -> lo/hi cut
             -> Stumble -> dropout -> punch-FX (5 in series) -> Drift -> master out
             -> soft limiter -> output
```
Overdub modes: **Replace** / **Multiply** (default, additive with Decay) / **Disintegration**
(the loop's FX are re-applied each pass, so it dissolves).

### Key constants (`src/loopex.c`)
```
SR 44100   NUM_VOICES 16   LOOP_SECONDS 45   POLY_VOICES 8   NUM_PREAMP 13
PUNCH_BUF 88200 (2 s)      NUM_PUNCH 16      NUM_PSLOTS 5    NUM_CHOP_PAT 32
FXSEQ_STEPS 16             NUM_SLOTS 64      MF_NVOICE 12    DRIFT_N 4
```

### Subsystems
- **Playheads** — 4 per voice (Off/Fwd/Bwd/Ping/**Jump**), `1/sqrt(n)` sum, 64-sample declick
  crossfade on any jump. Head 0 is the main head (Scatter/Seed/scrub drive it).
- **DJ filter** — LP left (analog voicing via `mf_run`, 12 models) / HP right (biquad). Both
  crossfade to dry across ±0.08 of centre and **the engine swap is deferred until the wet blend
  reaches ~0** (`djWet`/`djWetTgt`) → pop-free at any sweep speed.
- **Palette sends** — two buses, `Off` + 24 effects + 4 reverbs (Plate/Quartz/Prism/Veil) =
  `PFX_COUNT` 29; block-processed, 1-block latency; effect swaps go through the worker.
- **Punch-in FX** — 16 pads, **per-sample, zero-latency, up to 5 in series** over a 2 s ring.
  Stretch/Freeze is a 4-grain wander-wash (randomised lengths + backward wander + per-grain
  stereo, weighted-average sum).
- **Drift** — 4 coprime delay lines with drifting taps and a Hadamard cross-mix; feedback capped
  below unity, silence bleed after ~8 s.
- **Sessions** — 64 slots in `/data/UserData/schwung/loopex-sessions/`; **all disk I/O on a
  `SCHED_OTHER` worker pinned to cores 0-2**, joined in `destroy_instance`.
- **InSrc (Link Audio)** — `Line · Master · S1-4 · M1-4`. `S1-4` read Schwung's published stems
  from `/schwung-pub-audio` ("BPAL"); `M1-4` read the reconstructed Move tracks from
  `/schwung-link-in` ("LAIN"). Both read-only, with a **private cursor** (never touch `read_pos`).

---

## Surface
- **Pads are notes 68–99** (`padNoteFor`: `68 + row*8 + col`, 4×8). Left 16 = loops,
  right 16 = punch FX. *(Older notes in this file claimed 36–51 — that was wrong.)*
- **5 loop pages** (Up/Down): `Loop · Texture · Tone · Heads · HeadMix`.
  HeadMix = per-head Vol/Pan; **H1V/H1P are the same params as page 1's Vol/Pan** by design.
- **8 menus** — track buttons 1-4 → Input FX / Perform / Send FX / Settings; Capture → Input
  Tape; ≡ → Sessions; ● → Drift; ✕ → FX Seq. **Perform and Settings are two-page.**
- **Settings p1** ArmTh · ODub · LpFlt · Root · InMon · InSrc · MIDI · MidiO —
  **p2** Out · LoCut · HiCut · PWide · Char · gSat · Glue · Limit.
- Gestures: tap = Empty→Rec→Play⇄Pause; double-tap = Overdub; hold = Clear (Undo restores);
  Shift+tap = speed ½/1/2×; Mute+tap = quick-mute; Copy+pad+pad = clone; Loop+pad = loop length;
  Shift+punch pad = latch; Undo+punch pad = reset that effect.

### ⚠ `ui.js` traps
- **`MENU_DEFS[1]` and `[2]` are swapped at runtime** right after the literal
  (`{ const t = MENU_DEFS[1]; MENU_DEFS[1] = MENU_DEFS[2]; MENU_DEFS[2] = t; }`). Literal order
  is Input FX / Sends / Perform / Settings; **runtime** order (which `MENU_NAMES` and
  `MENU_PAGED = {1:true, 3:true}` assume) is Input FX / **Perform** / **Sends** / Settings.
- **Punch defaults live in `ui.js` `punchVals`**, not the DSP — the UI pushes them on every pad
  press, so the DSP's `punchParams` init is only a fallback. `PUNCH_DEFAULTS` (Undo+pad) is a
  snapshot of `punchVals`. Stretch/Freeze default **Grain = 0**.

---

## Parameters (string bridge)
JS drives the DSP entirely through `set_param(key,val)` / `get_param(key)`.
- **Per-voice (selected track):** `v_start v_end v_reverse v_pitch v_filter v_pan v_volume
  v_decay v_sat v_wowflut v_sendA v_sendB v_scatter v_glitch v_tilt v_eqBass v_eqPresFrq
  v_eqPresAmt v_eqTreble v_djReso v_atk v_rel v_comp v_clock`, `v_ph<1-4>mode` / `v_ph<1-4>spd`,
  `v_hvol2..4` / `v_hpan2..4`. Read-only: `v_state`, `v_loopLen`.
  Indexed form for state restore: `v<0-15>.<sub>`.
- **Globals:** master/output (`masterVol masterLoCut masterHiCut masterEQ masterGlue tapeLimit
  globalSat masterComp`), behaviour (`overdubMode armThresh rootNote stability globalWowFlut
  loopFiltMode selTrack`), I/O (`inSource inputMonitor inputGain inLow inMid inMidFreq inHigh
  inHighFreq`), tape (`tape*`), perform (`st* mf* mClock* perfTrem* dropAmt`), drift (`drift*`),
  punch (`punchWidth pfx pflfo punch punchPress`), sequencer (`fxseq*`), MIDI (`midiIn midiOut`).
- **Commands:** `cmd` (`tap/odub/clear/unclr/undo/mute/sel/arm/clone`), `session`, `scrub`,
  `headpos`, `jump`, `scan`, `tapeHold`, `clearSel`, `clearAll`.
- **Read-only:** `states mutes wave heads cpu inputPeak undoAvail sessNames sessStatus sessName
  state chain_params ui_hierarchy`.
- **State blob:** `get_param("state")` emits newline `key=value` lines (globals + compact
  `v<N>.*`); `set_param("state")` replays each line through the same dispatcher. It returns
  **−1 rather than truncating** on overflow. Enums `match_enum` the name **before** `atof`.

---

## MIDI
**`Settings → MIDI` is a 3-way mode: `Off` (default) / `Keys` / `Ctrl`.**
- **Keys** — keyboard poly. `on_midi` maps **channel n → loop n** (`msg[0] & 0x0F`), 8 voices.
- **Ctrl** — LaunchControl XL, handled in `ui.js onMidiMessageExternal` (gated on the cached
  `midiMode`). Track Focus notes **68-75 / 76-83 = tap**; Track Control notes **36-43 / 44-51 =
  mute**; CC **1-32** (loops 1-8) and **41-72** (loops 9-16) in rows of 8 → Speed / Filter / Pan /
  Volume. **`MidiO`** mirrors transport to the LCXL LEDs on **MIDI channel 2** (channel 1 would
  collide with Move's own pad notes). Templates: `BlackBox 1-8.syx`, `BlackBox 9-16.syx` — set
  both to channel 2.
- **Known limitation — 4 loops, not 16.** A Schwung Overtake module only receives external MIDI
  through the host's **four forwarding slots' "out"**, so only 4 channels arrive. Confirmed by
  Charles Vestal: *"modules only listen to the out of schwung modules, that's the only mailbox we
  have."* `/schwung-ext-midi-remap` exists (16-entry passthrough) but is **disabled and unwired**,
  and `MOVE_MIDI_SOURCE_FX_BROADCAST` is audio-FX-only. **The mapping already handles all 16** if
  a host ever delivers them — do not "fix" this in the module.

---

## Critical implementation notes
- **Every entry point runs on the SPI audio callback** (`create_instance`, `destroy_instance`,
  `set_param`, `get_param`, `on_midi`, `render_block`). No allocation, file I/O, logging or locks
  on the recurring paths. One-time `calloc`/`mmap` in create; `free`/`munmap`/`pthread_join` in
  destroy. No mutexes anywhere — lock-free atomics only.
- **`suspend_keeps_js`: a plain Back only SUSPENDS.** Old JS *and* DSP stay live, so a new build
  silently does nothing. **Full exit = `Shift + Volume + Jog-click`.**
- **Denormals:** FPCR FZ is **per-thread** on aarch64 and `-ffast-math`'s `crtfastmath` does not
  set it on the callback thread → `lb_enable_ftz()` runs every block. The build also uses
  **`-fno-finite-math-only`** so the `x!=x` NaN guards in `lb_clampf/lb_clampd` survive — do not
  remove it.
- `frames` is clamped to ≤128 at the top of `render_block` **before** any `frames*2` use.
- **`get_param` MUST return −1 for unknown keys** (not 0).
- Publish `loopLen` **last** with `__ATOMIC_RELEASE` when filling a buffer, so render never sees
  a half-written loop.
- Feedback paths capped below unity + saturated in-loop; filter cutoffs clamped inside
  (0, Nyquist).

## Move hardware constraints (never violate)
- 128 frames @ 44100 Hz (~2.9 ms); the module's slice of the callback is ~2370 µs.
- Audio is int16 stereo interleaved.
- No heap allocation / logging in the render path. No FTZ by default on ARM.
- Files on the device must be owned by `ableton:users`.
- **Memory:** ≈127 MB for the 16 stereo loop buffers (16 × 45 s × 2 ch × int16), plus ≈12 MB of
  shared overdub-undo buffers, ≈3.5 MB of punch rings, ≈1.7 MB of Drift lines — all allocated
  once in `create_instance`.

---

## Repo map
```
overtake-shell/
  module.json              Overtake manifest (id / capabilities / version)
  src/
    loopex.c               the engine: voices, playheads, punch FX, Stumble, Drift, sessions, master
    palette_fx.c/.h        Palette send engine (Off + 24 effects + 4 reverbs)
    fx_clouds.cc           Clouds-based Space / Bloom (C++)
    pitch_shift.cc         Signalsmith Stretch wrapper (per-loop Pitch)
    warps_data.c           Warps wavetables (Fold / Shift)
    shell_dsp.c            minimal smoke-test shell (not in the shipped link line)
    ui.js                  QuickJS Overtake UI (pads, knobs, screens, LEDs, gestures)
  include/plugin_api_v1.h  host API (v1 + v2)
  vendor/                  clouds_engine, signalsmith, signalsmith-stretch
  scripts/                 build.sh, install.sh, Dockerfile
README.md                  maintained feature reference + Known limitations + Credits
docs/MANUAL.md             long-form manual
docs/index.html            GitHub Pages site (filliformes.github.io/loopex-move)
design-spec.md             original design rationale (partly pre-Overtake)
OVERTAKE-SDK.md            reverse-engineered Overtake SDK reference
BlackBox 1-8.syx / 9-16    LaunchControl XL templates (set to MIDI channel 2)
release.json               version + release asset URL for the catalog
```

## Build & deploy
```bash
cd overtake-shell
bash scripts/build.sh      # Docker cross-compiles dsp.so (aarch64) + validates ui.js
bash scripts/install.sh    # scp + atomic-rename to move.local
```
Then on the Move: **full-exit (`Shift + Volume + Jog-click`) and reopen** — a plain Back only
suspends. `MOVE_HOST=ableton@172.16.254.1` overrides the target. The installer stages to
`/data/UserData` and `mv -f`s into place; **never write straight over a mapped `.so`**.

## Release
1. Bump the version in **both** `overtake-shell/module.json` **and** `release.json`.
2. Commit, push `main`, then `git tag vX.Y.Z && git push origin vX.Y.Z`.
3. `.github/workflows/release.yml` verifies tag == `module.json` version, builds, and publishes
   `loopex-module.tar.gz`.
4. Catalog: PR to `charlesvestal/schwung` → `module-catalog.json`. The fork is
   **`filliformes/move-everything`** and its `main` has diverged, so **branch off `upstream/main`**.
   `tags` must come from `taxonomy.json` **and be sorted**; `min_host_version: "1.3.0"`.
5. Run **`/dsp-review`** over `src/loopex.c` before tagging.

## Definition of done — keep all four in sync
Any user-visible change must land in **all** of these before it's finished:
1. **`README.md`** — feature sections, the loop-page table, Controls tables, Signal chain,
   Known limitations, Credits.
2. **`docs/MANUAL.md`** — menu tables and the matching prose section.
3. **`docs/index.html`** — the site (nav list, hero pills, `#pages` table, `#credits`); reuse the
   existing CSS classes, no new dependencies.
4. **`CLAUDE.md`** (this file) — if the architecture, params, surface or constraints moved.

Counts must agree across all four (e.g. **24 Palette effects + 4 reverbs**, **32** Chop patterns,
**64** session slots, **5** loop pages, **13** tape models). Push the docs with the code.
