/*
 * Loopex - Overtake UI (QuickJS). Drives the loopex.c engine via string params.
 *
 * Left 16 pads   = loop tracks: tap cycles Empty->Rec->Play<->Pause; double-tap
 *                  = Overdub; hold = clear (Undo restores). Mute/Copy/Loop/Shift
 *                  are held modifiers (mute, clone, length multiple, speed).
 * Right 16 pads  = punch-in FX (hold = momentary, Shift+pad = latch); knobs 5-8
 *                  edit the held effect.
 * Step row       = select track (shows its waveform); same step again = next page.
 * 8 knobs        = selected track over 4 pages (Up/Down); Track buttons 1-4,
 *                  Capture and Sample open menus. Jog scrubs / moves a playhead.
 * Screen         = main overview | knob grid (touch a knob / open a menu) |
 *                  waveform (step / jog), falling back after 10s.
 * Widgets are Schwung's own (fonts, frame ctx, geometry, animations).
 */

import {
    Black, White, LightGrey, DarkGrey,
    BrightRed, NeonGreen, NeonPink, Purple, AzureBlue, VividYellow, BrightOrange, DarkOrange,
    MoveKnob1, MoveShift, MoveBack, MoveMenu, MoveUp, MoveDown, MoveLeft, MoveRight, MoveUndo, MoveMute, MoveDelete, MovePlay,
    MoveSample, MoveCapture, MoveCopy, MoveLoop, MoveMainKnob,
    MoveSteps, MoveRow1, MoveRow2, MoveRow3, MoveRow4,
    WhiteLedOff, WhiteLedDim, WhiteLedBright,
} from '/data/UserData/schwung/shared/constants.mjs';

import { setLED, setButtonLED, decodeDelta }
    from '/data/UserData/schwung/shared/input_filter.mjs';
/* Schwung's own render primitives, imported so Loopex draws the SAME widgets
 * as every other module. These fonts and frame_ctx have no imports of their
 * own; render_page_movy.mjs is not imported because it drags in viz*.mjs. */
import { frameCtx } from '/data/UserData/schwung/shared/param_pages/frame_ctx.mjs';
import { fontPrint as tzPrint, fontWidth as tzWidth }
    from '/data/UserData/schwung/shared/param_pages/font_tamzen6x12.mjs';
import { fontPrint as bigPrint, fontWidth as bigWidth }
    from '/data/UserData/schwung/shared/param_pages/font_big_num.mjs';
import { fontPrint4x5, fontWidth4x5, FONT4_MEASURE, FONT4_HEIGHT }
    from '/data/UserData/schwung/shared/param_pages/font4x5.mjs';
import { enumSquareLines }
    from '/data/UserData/schwung/shared/param_pages/font5x3.mjs';
import { createAnimState, observeLanded, easeOut, lerp }
    from '/data/UserData/schwung/shared/param_pages/anim_state.mjs';

const SCREEN_W = 128, SCREEN_H = 64;
const NV = 16;

/* ---- pad geometry (notes 68..99, 4 rows x 8 cols) ---- */
function padNoteFor(sec, i) { const row = Math.floor(i / 4), col = sec * 4 + (i % 4); return 68 + row * 8 + col; }
const LEFT_NOTES = [], RIGHT_NOTES = [], NOTE_TO_LEFT = {}, NOTE_TO_RIGHT = {};
for (let i = 0; i < NV; i++) {
    LEFT_NOTES.push(padNoteFor(0, i)); RIGHT_NOTES.push(padNoteFor(1, i));
    NOTE_TO_LEFT[LEFT_NOTES[i]] = i;   NOTE_TO_RIGHT[RIGHT_NOTES[i]] = i;
}
const STEP_TO_TRACK = {}; for (let i = 0; i < NV; i++) STEP_TO_TRACK[MoveSteps[i]] = i;

/* ---- state ---- */
const STATE_COLORS = [DarkGrey, BrightRed, NeonGreen, White, Purple]; /* empty rec play pause odub */
/* a PLAYING loop is tinted by its speed so you can see which are 0.5x / 2x / 1x */
const PLAY_SPEED_COLORS = [AzureBlue, VividYellow, NeonGreen]; /* idx 0=0.5x(blue) 1=2x(yellow) 2=1x(green) */
const STATE_NAMES  = ['Empty', 'Rec', 'Play', 'Pause', 'Odub'];
let voiceState = new Array(NV).fill(0);
let sel = 0;                 /* selected track, 0-based */
let shiftHeld = false;
let muteHeld = false;       /* MoveMute held = quick-mute modifier */
let undoHeld = false, undoUsed = false;   /* Undo held: + punch pad = reset its params; released unused = undo */
function doUndo() {
    const ua = parseInt(gp('undoAvail') || '0', 10);
    if (ua > 0) { spCmd('undo'); setMsg('T' + ua + ' overdub undone'); return; }
    if (lastCleared >= 0) { spCmd('unclr:' + lastCleared); voiceState[lastCleared] = 2;
        enqLED(LEFT_NOTES[lastCleared], padColor(lastCleared)); setMsg('T' + (lastCleared + 1) + ' restored'); lastCleared = -1; }
    else setMsg('nothing to undo');
}
let loopPage = 0;           /* 0/1/2 = loop pages 1/2/3 (Up/Down/Left/Right arrows switch) */
let dirty = false;          /* screen repaint hint (declared explicitly; strict-mode safe) */
let lastCleared = -1;       /* last track cleared, for Undo (MoveUndo) */
const mutes = new Array(NV).fill(false);
const mutePressed = new Array(NV).fill(false);   /* pad press was a Mute+tap — its release must not clear */
let tickCount = 0;
const lastTapMs = new Array(NV).fill(0);
const pressMs   = new Array(NV).fill(0);
const speedIdx  = new Array(NV).fill(2);   /* 0=0.5x 1=2x 2=1x ; start at 1x */
const DOUBLE_TAP_MS = 350;
const CLEAR_HOLD_MS = 1000;
let statusMsg = '', statusMsgUntil = 0;
function setMsg(m) { statusMsg = m; statusMsgUntil = tickCount + 40; }
function now() { return (typeof Date !== 'undefined' && Date.now) ? Date.now() : tickCount * 23; }

/* ---- Punch-in FX (right 16 pads) ---- */
const PUNCH_NAMES = ['Loop12','Loop16','LoopSh','Chop',   'Haze','Mosaic','Smear','Strum',
                     'Oct-','Oct+','Glide','Shmr',      'Strch','Freez','Revrse','PalFX'];
const PUNCH_PAD_COLORS = [AzureBlue,AzureBlue,AzureBlue,AzureBlue, NeonPink,NeonPink,NeonPink,NeonPink,
                          Purple,Purple,Purple,Purple,             VividYellow,VividYellow,VividYellow,BrightRed];
const PUNCH_PARAMS = [ /* per-effect labels for knobs 5,6,7,8 */
  ['Rate','Pit','Tone','Mix'],['Rate','Pit','Tone','Mix'],['Rate','Pit','Tone','Mix'],['Rate','Patrn','Tone','Mix'],
  ['Size','Pit','Dens','Mix'],['Grid','Pit','Var','Mix'],['Size','Pit','Dens','Mix'],['Rate','Dir','Tone','Mix'],
  ['Fine','Pit','Tone','Mix'],['Fine','Pit','Tone','Mix'],['Len','Glide','Tone','Mix'],['Regn','Pit','Tone','Mix'],
  ['Strch','Pit','Grn','Mix'],['Frz','Pit','Grn','Mix'],['Len','Pit','Tone','Mix'],['FX','Amt','Mac','Drift']
];
/* what pad PRESSURE does on each effect (shown while the pad is held) */
const PUNCH_PRESS = ['subdivide','subdivide','subdivide','rate x2', 'density','grid x2','density','faster+wider',
                     'mix','mix','glide','regen',                     'freeze','freeze','shorter','amount'];
/* PFX_NAMES (declared with the Send FX menu below) also names knob 5 of the PalFX pad */
const NUM_CHOP_PAT = 32;
const LFO_DEST = ['Off', 'P1', 'P2', 'P3', 'P4'];
const LFO_SHAPE = ['Sine', 'Tri', 'Saw', 'Ramp', 'Squ', 'Puls', 'S&H', 'Smth'];
const PUNCH_LFO_LBL = ['Dest', 'Shape', 'Rate', 'Depth'];
/* knob cells that are enums rather than 0..1 floats: [pad, knob] -> {n, disp, toVal, fromVal} */
function punchEnum(i, j) {
    if (i === 15 && j === 0) return { n: PFX_NAMES.length, disp: ix => PFX_NAMES[ix], toVal: ix => ix / (PFX_NAMES.length - 1), fromVal: v => Math.max(0, Math.min(PFX_NAMES.length - 1, Math.round(v * (PFX_NAMES.length - 1)))) };
    if (i === 3 && j === 1)  return { n: NUM_CHOP_PAT, disp: ix => 'Pat ' + (ix + 1), toVal: ix => (ix + 0.5) / NUM_CHOP_PAT, fromVal: v => Math.max(0, Math.min(NUM_CHOP_PAT - 1, Math.floor(v * NUM_CHOP_PAT))) };
    return null;
}
function punchDisp(i, j, v) { const e = punchEnum(i, j); return e ? e.disp(e.fromVal(v)) : Number(v).toFixed(2); }
let punchMode = false, punchActive = -1, punchTookMenu = false;   /* a knob turn pulled us out of a menu during this punch */
const heldPunch = [];  /* currently-held punch pads (up to 4, in press order) */
const punchLatched = new Array(NV).fill(false);  /* Shift+pad = latch on (hands-free) */
const padFlash = new Array(NV).fill(0);          /* right-pad LED flash-until (ms) for undo-reset confirmation */
const padPress = new Array(NV).fill(0), pressMax = new Array(NV).fill(0);   /* live pad pressure, peak during the hold */
const pressFrozen = new Array(NV).fill(false);   /* Shift while holding: latch WITH the pressure of that moment */
const physHeld = [];   /* punch pads currently pressed — only THESE capture knobs 5-8 */
const punchVals = [];  /* [16][4] per-effect stored values */
for (let i = 0; i < 16; i++) punchVals.push([0.5, 0.5, 1.0, 1.0]);
punchVals[4]  = [0.4, 0.5, 0.5, 1.0];   /* Haze:   size, pitch, density */
punchVals[5]  = [0.5, 0.5, 0.4, 1.0];   /* Mosaic: 1/8 grid, some variation */
punchVals[6]  = [0.5, 0.5, 0.5, 1.0];   /* Smear:  long grains, mid density */
punchVals[7]  = [0.6, 0.75, 1.0, 1.0];  /* Strum:  brisk, upward, open tone */
punchVals[10] = [0.5, 0.25, 1.0, 1.0];  /* Glide:  1/4 beat, gliding down */
punchVals[11] = [0.5, 0.5, 0.5, 1.0];   /* Shimmer: regen, +1 octave, mid tone */
punchVals[12] = [0.5, 0.5, 0.3, 1.0];   /* Stretch: mid stretch, small grain */
punchVals[13] = [1.0, 0.5, 0.3, 1.0];   /* Freeze:  full freeze */
punchVals[3]  = [0.5, 0.0625, 1.0, 1.0]; /* Chop:   1/4 grid, pattern 1 */
punchVals[15] = [1.0, 0.5, 0.5, 0.0];   /* PalFX:  Veil reverb, half amount */
punchVals[3][0] = 0.7;                                 /* Chop: brisker default rate */
const PUNCH_DEFAULTS = punchVals.map(a => a.slice());   /* Undo + pad restores these */
const punchLfo = []; for (let i = 0; i < 16; i++) punchLfo.push([0, 0, 0.35, 0.35]);   /* [dest, shape, rate, depth] */
const PUNCH_LFO_DEFAULTS = punchLfo.map(a => a.slice());
function punchLfoEnum(j) {
    if (j === 0) return { n: LFO_DEST.length, disp: ix => LFO_DEST[ix], toVal: ix => ix / (LFO_DEST.length - 1), fromVal: v => Math.max(0, Math.min(LFO_DEST.length - 1, Math.round(v * (LFO_DEST.length - 1)))) };
    if (j === 1) return { n: LFO_SHAPE.length, disp: ix => LFO_SHAPE[ix], toVal: ix => ix / (LFO_SHAPE.length - 1), fromVal: v => Math.max(0, Math.min(LFO_SHAPE.length - 1, Math.round(v * (LFO_SHAPE.length - 1)))) };
    return null;
}
function punchLfoDisp(j, v) { const e = punchLfoEnum(j); return e ? e.disp(e.fromVal(v)) : Number(v).toFixed(2); }

/* ---- Track-button menus (MoveRow1..4) ---- */
const ROW_CCS = [MoveRow1, MoveRow2, MoveRow3, MoveRow4];   /* Track buttons 1..4 */
const MENU_NAMES = ['Input FX', 'Perform', 'Send FX', 'Settings', 'Input Tape', 'Sessions', 'FX Seq', 'Drift'];
const CHANCE_NAMES = ['Always','10%','20%','30%','40%','50%','60%','70%','80%','90%','LikeLast','P1 S1','P2 S1','S1 P1'];
const PFX_NAMES = ['Off','Drive','Sweeten','Fuzz','Howl','Fold','Swell','Doubler','Vibrato','Phaser','Tremolo','Pitch','Shift',
                   'Cascade','Reels','Collage','Reverse','Space','Bloom','Filter','Squash','Cassette','Broken','Interference','Halo','Plate','Quartz','Prism','Veil'];
const PREAMP_NAMES = ['Tapeless','Clean','Cass1','Cass2','VHS1','VHS2','Reel15','Reel7','Reel3','4trk','Porta','Dub','Warp'];
const MEQ_NAMES = ['Off','Studer','Neve','SSL','API','SP12','MPC','Emu','Ampex','Juno','Console'];
const MENU_DEFS = [
    [ /* Track 1 — Input FX */
      { k:'inputMonitor', lo:0, hi:1, lbl:'Mon' },    { k:'preamp', opts:PREAMP_NAMES, lbl:'Style' },
      { k:'inputGain', lo:0, hi:2, lbl:'Trim' },      { k:'inLow', lo:-1, hi:1, lbl:'Low' },
      { k:'inMid', lo:-1, hi:1, lbl:'Mid' },          { k:'inMidFreq', lo:0, hi:1, lbl:'MidF' },
      { k:'inHigh', lo:-1, hi:1, lbl:'High' },        { k:'inHighFreq', lo:0, hi:1, lbl:'HiF' },
    ],
    [ /* Track 2 — Global FX: two Palette send buses (all 24 effects) */
      { k:'sendAType', opts:PFX_NAMES, lbl:'A Fx' }, { k:'sendAM1', lo:0, hi:1, lbl:'A Amt' },
      { k:'sendAM2', lo:0, hi:1, lbl:'A Mac' },  { k:'sendADrift', lo:0, hi:1, lbl:'A Drf' },
      { k:'sendBType', opts:PFX_NAMES, lbl:'B Fx' }, { k:'sendBM1', lo:0, hi:1, lbl:'B Amt' },
      { k:'sendBM2', lo:0, hi:1, lbl:'B Mac' },  { k:'sendBDrift', lo:0, hi:1, lbl:'B Drf' },
    ],
    [ /* Track 3 — Perform: p1 Stumble/Dropout, p2 master filter + Mood clock + tremolo */
      { k:'stMix', lo:0, hi:1, lbl:'Stmb' }, { k:'stStep', lo:0, hi:1, lbl:'Step' },
      { k:'stOdds', lo:0, hi:1, lbl:'Odds' }, { k:'stSize', lo:0, hi:1, lbl:'Size' },
      { k:'stKind', opts:['Tumble','Stutter','Reverse','Tape','Gate','Crush'], lbl:'Kind' }, { k:'stReach', lo:0, hi:1, lbl:'Reach' },
      { k:'jump', trig:true, lbl:'Jump' }, { k:'scan', trig:true, lbl:'Scan' },
      { k:'mfCut', lo:0, hi:1, lbl:'Cut' },  { k:'mfReso', lo:0, hi:1, lbl:'Reso' },
      { k:'mfMode', opts:['Clean','SEM','MS-20','Steiner','Ladder4','Ladder2','Ladder1','Prophet','Oberheim','Diode','K35','Vintage'], lbl:'FChar' }, { k:'mClock', lo:0, hi:1, lbl:'Clock' },
      { k:'mClockMode', opts:['Music','Free'], lbl:'ClkMd' }, { k:'mClockSpot', opts:['Pre','Post'], lbl:'ClkAt' },
      { k:'perfTrem', lo:0, hi:1, lbl:'Pump' }, { k:'perfTremRate', lo:0, hi:1, lbl:'PmpRt' },
    ],
    [ /* Track 4 — Settings: p1 behaviour + I/O, p2 output & character */
      { k:'armThresh', lo:0, hi:1, lbl:'ArmTh' },     { k:'overdubMode', opts:['Replace','Multiply','Disint'], lbl:'ODub' },
      { k:'loopFiltMode', opts:['Clean','SEM','MS-20','Steiner','Ladder4','Ladder2','Ladder1','Prophet','Oberheim','Diode','K35','Vintage'], lbl:'LpFlt' },
      { k:'rootNote', lo:24, hi:96, lbl:'Root', int:true }, { k:'inputMonitor', lo:0, hi:1, lbl:'InMon' },
      { k:'inSource', opts:['Line','Master','Trk1','Trk2','Trk3','Trk4','Sch1','Sch2','Sch3','Sch4'], lbl:'InSrc' },
      { k:'midiIn', opts:['Off','On'], lbl:'MIDI' },  { k:'midiOut', opts:['Off','On'], lbl:'MidiO' },
      { k:'masterVol', lo:0, hi:1.5, lbl:'Out' },     { k:'masterLoCut', lo:20, hi:1000, lbl:'LoCut', int:true, step:5 },
      { k:'masterHiCut', lo:1000, hi:20000, lbl:'HiCut', int:true, step:100 }, { k:'punchWidth', lo:0, hi:1, lbl:'PWide' },
      { k:'masterEQ', opts:MEQ_NAMES, lbl:'Char' },   { k:'globalSat', lo:0, hi:2, lbl:'gSat' },
      { k:'masterGlue', lo:0, hi:1, lbl:'Glue' },     { k:'tapeLimit', lo:0, hi:1, lbl:'Limit' },
    ],
    [ /* 4 — Tape (Capture button): the record-path tape machine, Magneto-style */
      { k:'preamp', opts:PREAMP_NAMES, lbl:'Tape' },  { k:'tapeDrive', lo:0, hi:1, lbl:'Drive' },
      { k:'tapeWow', lo:0, hi:1, lbl:'Wow' },         { k:'tapeFlut', lo:0, hi:1, lbl:'Flut' },
      { k:'tapeHF', lo:0, hi:1, lbl:'HF' },           { k:'tapeLoCut', lo:0, hi:1, lbl:'LoCut' },
      { k:'tapeNoise', lo:0, hi:1, lbl:'Hiss' },      { k:'tapeGen', lo:0, hi:1, lbl:'Gen' },
    ],
    [ /* 5 — Sessions (Rec button): slot select + save/load (worker thread does the disk I/O) */
      { k:'sessSlot', lo:1, hi:64, lbl:'Slot', int:true, local:true },
      { k:'sessSave', trig:true, lbl:'Save' },
      { k:'sessLoad', trig:true, lbl:'Load' },
      { k:'sessDelete', trig:true, lbl:'Del' },
    ],
    [ /* 6 — FX Seq (Delete button): one shared 16-step pattern of punch pads (MESS-style) */
      { k:'fxseqRun', opts:['Off','On'], lbl:'Run' },        { k:'fxseqSpeed', opts:['1/32','1/16','1/8T','1/8','1/4','1/2','1'], lbl:'Speed' },
      { k:'fxseqLen', lo:1, hi:16, lbl:'Len', int:true },    { k:'fxseqChance', opts:CHANCE_NAMES, lbl:'Chnc', chance:true },
      { k:'fxseqGate', lo:0.1, hi:1, lbl:'Gate', hold:true }, { k:'fxseqSwing', lo:0.5, hi:0.75, lbl:'Swing' },
      { k:'fxseqDir', opts:['Fwd','Bwd','Ping','Rand'], lbl:'Dir' }, { k:'fxseqClear', trig:true, lbl:'Clear' },
    ],
    [ /* 7 — Drift (Sample button): global COSMOS-style shifting-delay memory */
      { k:'driftAmt', lo:0, hi:1, lbl:'Drift' }, { k:'driftRate', lo:0, hi:1, lbl:'Rate' },
      { k:'driftSize', lo:0, hi:1, lbl:'Size' }, { k:'driftFb', lo:0, hi:1, lbl:'FBk' },
      { k:'driftSupr', lo:0, hi:1, lbl:'Supr' }, { k:'driftBlur', lo:0, hi:1, lbl:'Blur' },
      { k:'driftDamp', lo:0, hi:1, lbl:'Damp' }, { k:'driftMix', lo:0, hi:1, lbl:'Mix' },
    ],
];
/* ---- FX sequencer UI state ---- */
let delHeld = false, delUsed = false, delDownAt = 0;   /* Delete (X) held: gestures; a quick lone tap toggles Run */
const delPads = [];                                  /* punch pads pressed while X is held: selection only, no sound */
let delStepHeld = -1;                                /* step held with X: locks / chance edit, extension anchor */
let seqRun = false, fxPos = -1, defaultChance = 0, confirmClear = false;
const stepMirror = [];                               /* UI copy of the DSP pattern, for LEDs and lock editing */
for (let i = 0; i < 16; i++) stepMirror.push({ n: 0, ext: 0, chance: 0, pads: [], locks: [], press: [] });
function parseStepMirror(i, r) {
    const m = stepMirror[i]; m.n = 0; m.ext = 0; m.chance = 0; m.pads = []; m.locks = []; m.press = [];
    if (!r) return;
    const parts = String(r).split(';'); const head = parts[0].split(',');
    m.n = parseInt(head[0]) || 0; m.ext = parseInt(head[1]) || 0; m.chance = parseInt(head[2]) || 0;
    for (let k = 1; k < parts.length; k++) { const kv = parts[k].split(':'); const vals = (kv[1] || '').split(',').map(Number);
        m.pads.push(parseInt(kv[0]) || 0); m.locks.push([vals[0] || 0, vals[1] || 0, vals[2] || 0, vals[3] || 0]); m.press.push(vals[4] || 0); }
    m.n = m.pads.length;
}
function reloadPunchMirrors() {   /* pull the loaded punch params/LFO back into the UI mirrors */
    for (let i = 0; i < 16; i++) {
        const a = gp('pfxq' + i); if (a) { const v = a.split(',').map(Number); if (v.length === 4 && v.every(x => !isNaN(x))) punchVals[i] = v; }
        const b = gp('pflq' + i); if (b) { const v = b.split(',').map(Number); if (v.length === 4 && v.every(x => !isNaN(x))) punchLfo[i] = v; }
        punchLatched[i] = false;
    }
}
function pollSeqMirror() { for (let i = 0; i < 16; i++) parseStepMirror(i, gp('fxstep' + i)); seqRun = (gp('fxseqRun') === 'On'); }
function seqPatternView() { return delHeld || menu === 6; }
function paintSteps() {                              /* track view, or the pattern while X is held / FX Seq is open */
    if (!seqPatternView()) { for (let i = 0; i < NV; i++) enqLED(MoveSteps[i], i === sel ? White : DarkGrey); return; }
    for (let i = 0; i < 16; i++) {
        const m = stepMirror[i];
        let c = m.ext ? DarkOrange : (m.n > 0 ? BrightOrange : Black);
        if (seqRun && i === fxPos) c = White;
        if (i === delStepHeld) c = LightGrey;
        enqLED(MoveSteps[i], c);
    }
}
function sendStepPad(i, k) {                         /* push one pad entry of a step to the DSP */
    const m = stepMirror[i], l = m.locks[k];
    sp('fxstep', i + ':' + k + ':' + m.pads[k] + ':' + l[0].toFixed(4) + ':' + l[1].toFixed(4) + ':' + l[2].toFixed(4) + ':' + l[3].toFixed(4) + ':' + m.press[k].toFixed(3));
}
function writeStep(i, pads) {                        /* X + pads + step: snapshot the pads' knobs + pressure into the step */
    const m = stepMirror[i]; m.pads = pads.slice(0, 5); m.locks = []; m.press = []; m.ext = 0; m.chance = defaultChance;
    for (let k = 0; k < m.pads.length; k++) { m.locks.push(punchVals[m.pads[k]].slice()); m.press.push(padPress[m.pads[k]] || 0); sendStepPad(i, k); }
    m.n = m.pads.length; sp('fxext', i + ':0'); sp('fxstepn', i + ':' + m.n); sp('fxchance', i + ':' + m.chance);
    setMsg('Step ' + (i + 1) + ': ' + m.pads.map(x => PUNCH_NAMES[x]).join('+'));
}
function clearStep(i) { const m = stepMirror[i]; m.n = 0; m.ext = 0; m.pads = []; m.locks = []; m.press = []; sp('fxstepn', i + ':0'); setMsg('Step ' + (i + 1) + ' cleared'); }
function setSeqRun(on) { seqRun = on; sp('fxseqRun', on ? '1' : '0'); setMsg(on ? 'FX Seq run' : 'FX Seq stop'); setButtonLED(MoveDelete, on ? WhiteLedBright : WhiteLedDim, true); if (menu === 6) menuReload = true; }
/* button 2 = Perform, button 3 = Send FX (the table above is written in its original order) */
{ const t = MENU_DEFS[1]; MENU_DEFS[1] = MENU_DEFS[2]; MENU_DEFS[2] = t; }
let sessSlot = 1, sessLast = '';
let sessCurrent = 0;                 /* slot the current session came from / was saved to; 0 = New */
let sessPending = '', sessPendSlot = 0;   /* 'save' / 'load' in flight, and for which slot */
let savedBurstAt = 0;                /* wall-clock ms of the last successful save (burst animation) */
const SAVED_BURST_MS = 900;
const NSLOTS = 64;
let sessNames = new Array(NSLOTS + 1).fill('');   /* 1-based; '' = empty slot */
let confirmSave = false, confirmDelete = false;   /* overwrite / delete popup pending */
/* '05_20260914_2130' -> 'Sep 14 21:30' so the slot pill + name fit beside BACK in the footer */
const MONTHS = ['Jan','Feb','Mar','Apr','May','Jun','Jul','Aug','Sep','Oct','Nov','Dec'];
function prettySess(nm) {
    const m = /^(\d+)_(\d{4})(\d{2})(\d{2})_(\d{2})(\d{2})$/.exec(String(nm || ''));
    if (!m) return nm || '';
    return (MONTHS[parseInt(m[3], 10) - 1] || m[3]) + ' ' + parseInt(m[4], 10) + ' ' + m[5] + ':' + m[6];
}
function pollSessNames() {
    const r = gp('sessNames'); if (!r) return;
    const parts = String(r).split(';');
    for (let i = 1; i <= NSLOTS; i++) sessNames[i] = parts[i - 1] || '';
}
function cancelPopups() { confirmSave = false; confirmClear = false; confirmDelete = false; }
function doSessionSave() {
    sp('session', 'save:' + sessSlot); setMsg('Saving slot ' + sessSlot); confirmSave = false; sessPending = 'save'; sessPendSlot = sessSlot; sessLast = '';
}
function doSessionDelete() {
    sp('session', 'delete:' + sessSlot); setMsg('Deleting slot ' + sessSlot); confirmDelete = false; sessPending = 'delete'; sessPendSlot = sessSlot; sessLast = '';
}
let armThreshVal = 0.08;
let armChord = false, armJogged = false;   /* Shift+Sample: defer arm to release unless the jog was used */
let copyHeld = false, loopHeld = false, cloneSrc = -1;
const armedArr = new Array(NV).fill(false);
let blinkOn = false, resumeRepaint = 0;
let view = 'main', viewUntil = 0;          /* 'main' | 'knobs' | 'wave' */
const VIEW_MS = 10000;                     /* 10s of real time before falling back */
let sampleHeld = false, jogHead = -1;      /* Shift+Sample+jog = arm threshold; P4 touch = head to move */
let waveStr = '', headsStr = '';
let waveStart = 0, waveEnd = 1;   /* current loop trim, for the waveform markers */
let driftMixOn = false;           /* Drift Mix > 0.1 -> the Sample LED glows */
let clkMusic = true;              /* Master Clock mode: Music (snap) vs Free */
const MCLK_SEMI_UI = [-24,-19,-17,-12,-7,-5,0,5,7,12,17,19,24];   /* mirrors MCLK_SEMI in the DSP */
function showView(v) { view = v; viewUntil = now() + VIEW_MS; dirty = true; }
const LOOP_MULTS = [1.0, 0.5, 0.25, 0.125];
const loopMultIdx = new Array(NV).fill(0);
let menu = -1, menuReload = false, menuPage = 0;
const MENU_PAGED = { 1: true, 3: true };   /* Perform + Settings have a 2nd knob page */
function curMenuDefs() { const d = MENU_DEFS[menu]; if (!d) return null; return MENU_PAGED[menu] ? d.slice(menuPage * 8, menuPage * 8 + 8) : d; }
function menuPages() { const d = MENU_DEFS[menu]; return (d && MENU_PAGED[menu]) ? Math.ceil(d.length / 8) : 1; }
const menuVals = [0,0,0,0,0,0,0,0];

let knobVals = new Array(8).fill(0);
/* Schwung (movy_knob.mjs): an enum advances one option per ENUM_DELTA_DIV physical
 * detents, so a fast knob turn does not race through every option. */
const ENUM_DELTA_DIV = 4;
const enumAccum = new Array(8).fill(0);
function enumSteps(k, delta) {
    enumAccum[k] += delta;
    const steps = Math.trunc(enumAccum[k] / ENUM_DELTA_DIV);
    if (steps !== 0) enumAccum[k] -= steps * ENUM_DELTA_DIV;
    return steps;
}
let needReload = true;
let lastKnob = -1, lastKnobLbl = '', lastKnobVal = '';
let cpu = '0', loopLen = '0', inPeak = '0';

const PAGE0 = [   /* Loop page 1 (Up arrow) — knob 8 = Send A */
    { k: 'v_pitch', lo: -2, hi: 2, lbl: 'Spd', spd: true, step: 0.1 / 12 }, { k: 'v_filter', lo: 0, hi: 1, lbl: 'Fil', step: 0.002 },
    { k: 'v_pan', lo: -1, hi: 1, lbl: 'Pan' },   { k: 'v_volume', lo: 0, hi: 1, lbl: 'Vol' },
    { k: 'v_start', lo: 0, hi: 1, lbl: 'Srt' },  { k: 'v_end', lo: 0, hi: 1, lbl: 'End' },
    { k: 'v_reverse', lo: 0, hi: 1, lbl: 'Rev', e2: ['Nrm', 'Rev'] }, { k: 'v_sendA', lo: 0, hi: 1, lbl: 'SndA' },
];
const PAGE1 = [   /* Loop page 2 (Down arrow) — knobs 6/7 = Scatter/Seed, knob 8 = Send B */
    { k: 'v_clock', lo: -2, hi: 2, lbl: 'Pit', st: true, step: 0.1 / 12 },  { k: 'v_djReso', lo: 0, hi: 1, lbl: 'Reso' },
    { k: 'v_sat', lo: 0, hi: 1, lbl: 'Sat' },             { k: 'v_comp', lo: 0, hi: 1, lbl: 'Cmp' },
    { k: 'v_wowflut', lo: 0, hi: 1, lbl: 'WF' },          { k: 'v_scatter', lo: 0, hi: 1, lbl: 'Scat' },
    { k: 'v_glitch', lo: 0, hi: 1, lbl: 'Seed' },         { k: 'v_sendB', lo: 0, hi: 1, lbl: 'SndB' },
];
const PAGE2 = [   /* Loop page 3 / Tone (Right arrow) — Studer EQ + DJ reso + amp envelope */
    { k: 'v_eqBass', lo: -1, hi: 1, lbl: 'Bass' },    { k: 'v_eqPresFrq', lo: 0, hi: 1, lbl: 'MidF' },
    { k: 'v_eqPresAmt', lo: -1, hi: 1, lbl: 'MidG' }, { k: 'v_eqTreble', lo: -1, hi: 1, lbl: 'Treb' },
    { k: 'v_tilt', lo: -1, hi: 1, lbl: 'Tilt' },      { k: 'v_atk', lo: 0, hi: 1, lbl: 'Atk' },
    { k: 'v_rel', lo: 0, hi: 1, lbl: 'Dec' },         { k: '_heads', page: 3, lbl: 'Heads' },
];
const HEAD_MODES = ['Off', 'Fwd', 'Bwd', 'Ping', 'Jump'];
const PAGE3 = [   /* Loop page 4 — Playheads: mode + speed per head (touch one, jog moves it) */
    { k: 'v_ph1mode', opts: HEAD_MODES, lbl: 'H1' },  { k: 'v_ph1spd', lo: 0, hi: 1, lbl: 'H1spd', clk: true, step: 0.1 / 48 },
    { k: 'v_ph2mode', opts: HEAD_MODES, lbl: 'H2' },  { k: 'v_ph2spd', lo: 0, hi: 1, lbl: 'H2spd', clk: true, step: 0.1 / 48 },
    { k: 'v_ph3mode', opts: HEAD_MODES, lbl: 'H3' },  { k: 'v_ph3spd', lo: 0, hi: 1, lbl: 'H3spd', clk: true, step: 0.1 / 48 },
    { k: 'v_ph4mode', opts: HEAD_MODES, lbl: 'H4' },  { k: 'v_ph4spd', lo: 0, hi: 1, lbl: 'H4spd', clk: true, step: 0.1 / 48 },
];
const PAGES = [PAGE0, PAGE1, PAGE2, PAGE3];
const NPAGES = 4;
function page() { return loopPage; }
function setPage(p) { p = p < 0 ? 0 : (p > NPAGES - 1 ? NPAGES - 1 : p); if (p !== loopPage) { loopPage = p; needReload = true; dirty = true; paintNav(); } }

/* ---- host bridge ---- */
function sp(key, val) { try { host_module_set_param(key, val); } catch (e) {} }
function spCmd(v) {
    try {
        if (typeof host_module_set_param_blocking === 'function') host_module_set_param_blocking('cmd', v, 50);
        else host_module_set_param('cmd', v);
    } catch (e) {}
}
function gp(key) { try { return host_module_get_param(key); } catch (e) { return null; } }
function clampf(x, lo, hi) { return x < lo ? lo : (x > hi ? hi : x); }

/* ---- LED queue (drain ~6/tick) ---- */
let ledQ = [];
function enqLED(note, color) { ledQ.push([note, color]); }
function drainLEDs() { let n = 6; while (n-- > 0 && ledQ.length) { const e = ledQ.shift(); setLED(e[0], e[1]); } }
function padColor(i) {
    if (armedArr[i]) return blinkOn ? BrightRed : Black;    /* armed: blink red until input crosses */
    if (i === cloneSrc) return blinkOn ? White : DarkGrey;  /* clone source blinks */
    if (mutes[i] && voiceState[i] >= 2) return LightGrey;   /* muted (still running) = grey */
    if (voiceState[i] === 2) return PLAY_SPEED_COLORS[speedIdx[i]] || NeonGreen; /* playing: show speed */
    return STATE_COLORS[voiceState[i]] || DarkGrey;
}
function rightColor(i) { return (heldPunch.indexOf(i) >= 0) ? White : (PUNCH_PAD_COLORS[i] || DarkGrey); }

function paintAll(force) {
    for (let i = 0; i < NV; i++) {
        if (force) { setLED(LEFT_NOTES[i], padColor(i), true); setLED(RIGHT_NOTES[i], rightColor(i), true); }
        else       { enqLED(LEFT_NOTES[i], padColor(i));       enqLED(RIGHT_NOTES[i], rightColor(i)); }
    }
    for (let i = 0; i < NV; i++) { if (force) setLED(MoveSteps[i], i === sel ? White : DarkGrey, true); else enqLED(MoveSteps[i], i === sel ? White : DarkGrey); }
    setButtonLED(MoveBack, WhiteLedDim, !!force);
    setButtonLED(MoveShift, WhiteLedDim, !!force);
    setButtonLED(MoveDelete, seqRun ? WhiteLedBright : WhiteLedDim, !!force);
    paintNav();
    paintTrackLEDs();
}
/* Open (or toggle off) a menu by index; 0-3 are the track buttons, 4=Tape, 5=Sessions */
function openMenu(idx) {
    cancelPopups();
    if (menu === idx) {
        /* already open: step to the next knob page if this menu has one, else quit */
        if (MENU_PAGED[idx] && menuPage < menuPages() - 1) { menuPage++; menuReload = true; showView('knobs'); }
        else { menu = -1; menuPage = 0; }
    } else {
        menu = idx; menuPage = 0; menuReload = true; setMsg(MENU_NAMES[idx]); showView('knobs'); if (idx === 5) pollSessNames();
    }
    paintTrackLEDs(); paintNav(); dirty = true;
}
/* Nav buttons: arrows lit (current page's jump-arrow bright), Undo dim, Mute bright while held */
function paintNav() {
    setButtonLED(MoveDown,  loopPage < NPAGES - 1 ? WhiteLedBright : WhiteLedDim, true);
    setButtonLED(MoveUp,    loopPage > 0 ? WhiteLedBright : WhiteLedDim, true);
    setButtonLED(MoveUndo,  WhiteLedDim, true);
    setButtonLED(MoveMute,  muteHeld ? WhiteLedBright : WhiteLedDim, true);
    setButtonLED(MoveCapture, menu === 4 ? WhiteLedBright : WhiteLedDim, true);
    setButtonLED(MoveSample,  menu === 7 ? WhiteLedBright : (driftMixOn ? WhiteLedDim : WhiteLedOff), true);
    setButtonLED(MoveMenu,    menu === 5 ? WhiteLedBright : WhiteLedDim, true);
    setButtonLED(MoveCopy,    copyHeld ? WhiteLedBright : WhiteLedDim, true);
    setButtonLED(MoveLoop,    loopHeld ? WhiteLedBright : WhiteLedDim, true);
}
function clearAllLEDs() {
    for (let i = 0; i < NV; i++) { setLED(LEFT_NOTES[i], Black, true); setLED(RIGHT_NOTES[i], Black, true); setLED(MoveSteps[i], Black, true); }
    setButtonLED(MoveBack, WhiteLedOff, true);
    setButtonLED(MoveShift, WhiteLedOff, true);
    setButtonLED(MoveUp, WhiteLedOff, true); setButtonLED(MoveDown, WhiteLedOff, true);
    setButtonLED(MoveLeft, WhiteLedOff, true); setButtonLED(MoveRight, WhiteLedOff, true);
    setButtonLED(MoveUndo, WhiteLedOff, true); setButtonLED(MoveMute, WhiteLedOff, true);
    setButtonLED(MoveCapture, WhiteLedOff, true); setButtonLED(MoveSample, WhiteLedOff, true);
    setButtonLED(MoveCopy, WhiteLedOff, true); setButtonLED(MoveLoop, WhiteLedOff, true);
    for (let i = 0; i < 4; i++) setButtonLED(ROW_CCS[i], WhiteLedOff, true);
}

/* ---- knob page load ---- */
function reloadKnobs() {
    const defs = PAGES[page()];
    for (let i = 0; i < 8; i++) {
        const d = defs[i];
        if (!d || d.page !== undefined) { knobVals[i] = 0; continue; }   /* page-jump cell */
        const r = gp(d.k);
        if (d.opts) { let ix = d.opts.indexOf(r); if (ix < 0) ix = parseInt(r) || 0;
            knobVals[i] = Math.max(0, Math.min(d.opts.length - 1, ix)); }
        else if (d.e2) knobVals[i] = (r === 'Reverse') ? 1 : 0;
        else { const f = parseFloat(r); knobVals[i] = isNaN(f) ? (d.lo || 0) : f; }
    }
    needReload = false;
}

/* ---- menu helpers ---- */
function reloadMenu() {
    const defs = curMenuDefs(); if (!defs) return;
    for (let i = 0; i < 8; i++) {
        const d = defs[i]; if (!d) { menuVals[i] = 0; continue; }
        if (d.local) { menuVals[i] = sessSlot; continue; }
        if (d.chance) { menuVals[i] = (delStepHeld >= 0) ? stepMirror[delStepHeld].chance : defaultChance; continue; }
        const r = gp(d.k);
        if (d.opts) { let idx = d.opts.indexOf(r); if (idx < 0) idx = parseInt(r) || 0; menuVals[i] = Math.max(0, Math.min(d.opts.length - 1, idx)); if (d.k === 'mClockMode') clkMusic = (r !== 'Free'); }
        else { const f = parseFloat(r); menuVals[i] = isNaN(f) ? (d.lo || 0) : f; }
    }
    menuReload = false;
}
function menuKnob(k, delta) {
    if (confirmSave || confirmClear || confirmDelete) {   /* popup: knob 8 = YES, knob 5 = NO (those cells have no def, so check first) */
        if (delta === 0) return;
        if (k === 7) { stampButton(k);
            if (confirmClear) { confirmClear = false; sp('fxseqClear', '1'); for (let i = 0; i < 16; i++) clearStep(i); setMsg('Pattern cleared'); paintSteps(); }
            else if (confirmDelete) { doSessionDelete(); }
            else doSessionSave(); }
        else if (k === 4) { stampButton(k); confirmSave = false; confirmClear = false; confirmDelete = false; setMsg('cancelled'); }
        dirty = true; return;
    }
    if (menu === 6 && delStepHeld >= 0 && k >= 4 && stepMirror[delStepHeld].n > 0) {   /* X + step + knobs 5-8: locks of the step's first effect */
        const m = stepMirror[delStepHeld], pad = m.pads[0], j = k - 4, pe = punchEnum(pad, j); let nv;
        if (pe) { const st = enumSteps(k, delta); if (st === 0) return; nv = pe.toVal(Math.max(0, Math.min(pe.n - 1, pe.fromVal(m.locks[0][j]) + st))); }
        else { nv = clampf(0.5 + Math.round((m.locks[0][j] + delta * 0.02 - 0.5) / 0.02) * 0.02, 0, 1); }
        m.locks[0][j] = nv; sendStepPad(delStepHeld, 0); delUsed = true;
        lastKnob = k; lastKnobLbl = PUNCH_PARAMS[pad][j]; lastKnobVal = punchDisp(pad, j, nv); return;
    }
    const d = curMenuDefs()[k]; if (!d) return;
    if (d.chance) {                   /* Chance: of the held step, else the default for new steps */
        const st = enumSteps(k, delta); if (st === 0) return;
        const idx = Math.max(0, Math.min(d.opts.length - 1, Math.round(menuVals[k]) + st)); menuVals[k] = idx;
        if (delStepHeld >= 0) { stepMirror[delStepHeld].chance = idx; sp('fxchance', delStepHeld + ':' + idx); } else defaultChance = idx;
        delUsed = true; lastKnob = k; lastKnobLbl = d.lbl; lastKnobVal = d.opts[idx]; return;
    }
    if (d.local) {                    /* UI-local (session slot): one slot per 4 detents */
        const st = enumSteps(k, delta); if (st === 0) return;
        const nv = Math.max(d.lo, Math.min(d.hi, Math.round(menuVals[k]) + st));
        menuVals[k] = nv; sessSlot = nv; lastKnob = k; lastKnobLbl = d.lbl; lastKnobVal = String(nv); return;
    }
    if (d.trig) {
        if (delta !== 0) {
            stampButton(k);
            if (d.k === 'sessSave') {
                if (sessNames[sessSlot]) { confirmSave = true; setMsg('slot ' + sessSlot + ' exists'); }
                else doSessionSave();
            }
            else if (d.k === 'sessLoad') { sp('session', 'load:' + sessSlot); setMsg('Loading slot ' + sessSlot); sessPending = 'load'; sessPendSlot = sessSlot; sessLast = ''; }
            else if (d.k === 'sessDelete') { if (sessNames[sessSlot]) { confirmDelete = true; setMsg('delete slot ' + sessSlot + '?'); } else setMsg('slot ' + sessSlot + ' empty'); }
            else if (d.k === 'fxseqClear') { confirmClear = true; setMsg('clear pattern?'); }
            else sp(d.k, '1');
            lastKnob = k; lastKnobLbl = d.lbl; lastKnobVal = 'fire';
        }
        return;
    }
    if (d.opts) {
        const st = enumSteps(k, delta); if (st === 0) return;
        let idx = Math.max(0, Math.min(d.opts.length - 1, Math.round(menuVals[k]) + st));
        menuVals[k] = idx; sp(d.k, d.opts[idx]); lastKnobVal = d.opts[idx];
        if (d.k === 'mClockMode') clkMusic = (d.opts[idx] !== 'Free');
        if (d.k === 'fxseqRun') { seqRun = idx === 1; setButtonLED(MoveDelete, seqRun ? WhiteLedBright : WhiteLedDim, true); }
        if (menu === 6) delUsed = true;
    } else if (d.int) {
        if (menu === 6) delUsed = true;
        const step = d.step || Math.max(1, Math.round((d.hi - d.lo) * 0.02));
        const nv = Math.max(d.lo, Math.min(d.hi, Math.round(menuVals[k] + delta * step)));
        menuVals[k] = nv; sp(d.k, String(nv)); lastKnobVal = String(nv);
    } else {
        if (menu === 6) delUsed = true;
        const step = d.step || (d.hi - d.lo) * 0.006;   /* fine + continuous: no stepping on sound controls */
        let nv = clampf(menuVals[k] + delta * step, d.lo, d.hi);
        if (d.k === 'mClock' && (menuVals[k] - 0.5) * (nv - 0.5) < 0) nv = 0.5;   /* catch exact unity (1.00x) when crossing centre */
        menuVals[k] = nv; sp(d.k, nv.toFixed(4)); lastKnobVal = (d.k === 'mClock') ? knobInfo(d, k)[1] : ((d.hi - d.lo > 4) ? String(Math.round(nv)) : nv.toFixed(2));
    }
    lastKnob = k; lastKnobLbl = d.lbl;
}
function paintTrackLEDs() { for (let i = 0; i < 4; i++) setButtonLED(ROW_CCS[i], (menu === i) ? WhiteLedBright : WhiteLedDim); }

/* capacitive knob touch (notes 0-7 = E1-E8): show the param it affects, without changing it */
function handleKnobTouch(d1) {
    const k = d1; if (k < 0 || k > 7) return;
    if (punchMode && punchActive >= 0) {   /* a held punch pad wins: touching a knob opens its params, taking over any menu */
        if (menu >= 0) { menu = -1; menuPage = 0; punchTookMenu = true; paintTrackLEDs(); paintNav(); }
        if (k < 4) { lastKnobLbl = PUNCH_LFO_LBL[k]; lastKnobVal = punchLfoDisp(k, punchLfo[punchActive][k]); }
        else { const j = k - 4; lastKnobLbl = PUNCH_PARAMS[punchActive][j]; lastKnobVal = punchDisp(punchActive, j, punchVals[punchActive][j]); }
    } else if (menu >= 0 && curMenuDefs()) {
        const d = curMenuDefs()[k]; if (!d) return; lastKnobLbl = d.lbl;
        lastKnobVal = d.local ? String(sessSlot) : d.trig ? '(fire)' : (d.opts ? d.opts[Math.round(menuVals[k])] : (d.int ? String(Math.round(menuVals[k])) : Number(menuVals[k]).toFixed(2)));
    } else if (menu < 0) {
        const d = PAGES[page()][k]; if (!d) return; lastKnobLbl = d.lbl;
        lastKnobVal = knobInfo(d, k)[1];
    } else return;
    lastKnob = k; dirty = true;
    if (menu < 0 && page() === 3) jogHead = Math.floor(k / 2);   /* P4: touching Hn binds the jog to it */
    showView('knobs');
}

/* ---- state poll -> recolor pads ---- */
function pollStates() {
    const s = gp('states');
    if (!s || s.length < NV) return;
    for (let i = 0; i < NV; i++) {
        const st = s.charCodeAt(i) - 48;
        if (st !== voiceState[i]) { voiceState[i] = st; enqLED(LEFT_NOTES[i], padColor(i)); }
    }
    const mu = gp('mutes');   /* mute lives in the DSP; keep the UI mirror in sync (pad, LCXL, load) */
    if (mu && mu.length >= NV) for (let i = 0; i < NV; i++) {
        const m = mu.charCodeAt(i) === 49;
        if (m !== mutes[i]) { mutes[i] = m; enqLED(LEFT_NOTES[i], padColor(i)); }
    }
}

/* ---- screen ---- */
function px(x, y) { if (x >= 0 && x < SCREEN_W && y >= 0 && y < SCREEN_H) fill_rect(x, y, 1, 1, 1); }

/* ---- Schwung param grid, reproduced exactly (render_page_movy.mjs geometry) ---- */
const CELL_W = 32, KW = 17, KNOB_R = 8;
const ROW0_Y = 9, LBL0_Y = 24, ROW1_Y = 33, LBL1_Y = 48;
const KNOB_START_DEG = 225, KNOB_SWEEP_DEG = 270;
const ARC_START_DEG = 230, ARC_SWEEP_DEG = 260;
const POINTER_INNER = 0.0, POINTER_OUTER = 0.68;
const BTN_RX = 7, BTN_RY = 3, BTN_DEPTH = 6, BTN_TRAVEL = 2;
const ENUM_W = 28, BOX_H = 15;

/* Parent context over the Overtake primitives; frameCtx clips and translates. */
const rootCtx = {
    fillRect(x, y, w, h, c) { fill_rect(x, y, w, h, c ? 1 : 0); },
    print(x, y, t, c) { print(x, y, String(t), c ? 1 : 0); },
    textWidth(t) { return tzWidth(String(t)); },
};
function screenCtx() { return frameCtx(rootCtx, { x: 0, y: 0, w: SCREEN_W, h: SCREEN_H }); }

/* render_page_movy.mjs drawArcKnob, verbatim. */
function drawArcKnob(ctx, kx, ky, normVal) {
    const cx = kx + KNOB_R, cy = ky + KNOB_R, r = KNOB_R;
    ctx.drawArc(cx, cy, r, ARC_START_DEG, ARC_SWEEP_DEG, 1);
    const rad = (KNOB_START_DEG + normVal * KNOB_SWEEP_DEG) * Math.PI / 180;
    const sin = Math.sin(rad), cos = Math.cos(rad);
    ctx.line(Math.round(cx + r * POINTER_INNER * sin), Math.round(cy - r * POINTER_INNER * cos),
             Math.round(cx + r * POINTER_OUTER * sin), Math.round(cy - r * POINTER_OUTER * cos), 1);
}

/* render_page_movy.mjs ellipse helpers + drawButton, verbatim. */
function ellipseOutline(ctx, cx, cy, rx, ry, color, bottomOnly) {
    const put = (x, y) => { if (bottomOnly && y < cy) return; ctx.fillRect(x, y, 1, 1, color); };
    for (let dx = -rx; dx <= rx; dx++) {
        const dy = Math.round(ry * Math.sqrt(Math.max(0, 1 - Math.pow(dx / rx, 2))));
        put(cx + dx, cy + dy); put(cx + dx, cy - dy);
    }
    for (let dy = -ry; dy <= ry; dy++) {
        const dx = Math.round(rx * Math.sqrt(Math.max(0, 1 - Math.pow(dy / ry, 2))));
        put(cx + dx, cy + dy); put(cx - dx, cy + dy);
    }
}
function ellipseFill(ctx, cx, cy, rx, ry, color) {
    for (let dy = -ry; dy <= ry; dy++) {
        const w = Math.round(rx * Math.sqrt(Math.max(0, 1 - Math.pow(dy / ry, 2))));
        if (w > 0) ctx.fillRect(cx - w, cy + dy, w * 2 + 1, 1, color);
    }
}
/* render_page_movy.mjs button animation, verbatim. */
const BTN_PRESS_MS = 120;
const BTN_FLASH_MS = 300;
const BTN_RAYS = 8;
const BTN_RAY_GAP = 2;
const BTN_RAY_LEN = 2;
const BTN_RAY_TRAVEL = 4;   /* how far the burst moves out over its life */

/* Impact stubs, following the cap's ellipse so they sit an even gap off the
 * rim rather than bunching at the flat top and bottom. */
function buttonRays(ctx, cx, cy, progress) {
    const out = BTN_RAY_GAP + Math.round(progress * BTN_RAY_TRAVEL);
    for (let i = 0; i < BTN_RAYS; i++) {
        const a = (Math.PI * 2 * i) / BTN_RAYS;
        const ux = Math.cos(a), uy = Math.sin(a);
        const x0 = cx + ux * (BTN_RX + out);
        const y0 = cy + uy * (BTN_RY + out);
        const x1 = cx + ux * (BTN_RX + out + BTN_RAY_LEN);
        const y1 = cy + uy * (BTN_RY + out + BTN_RAY_LEN);
        ctx.line(Math.round(x0), Math.round(y0), Math.round(x1), Math.round(y1), 1);
    }
}

/* Idle / highlighted / pressed, resolved from the press timestamps alone.
 * Every press still inside BTN_FLASH_MS keeps its own burst, so a fast
 * double-tap throws two rings rather than cancelling the first. */
function buttonPhase(fired, now, held) {
    const stamps = Array.isArray(fired) ? fired : (fired > 0 ? [fired] : []);
    const bursts = [];
    let pressed = false;
    if (typeof now === "number") {
        for (const t of stamps) {
            const age = now - t;
            if (age < 0 || age >= BTN_FLASH_MS) continue;
            bursts.push(age / BTN_FLASH_MS);
            if (age < BTN_PRESS_MS) pressed = true;
        }
    }
    return { pressed, filled: held || bursts.length > 0, bursts };
}

/* Three states: idle = raised outline; selected = cap filled; fired = cap
 * pressed down BTN_TRAVEL, sides shortened, stubs radiating. */
function drawButton(ctx, cx, rowY, phase) {
    const pressed = phase.pressed;
    const travel = pressed ? BTN_TRAVEL : 0;
    const capY = rowY + 1 + BTN_RY + travel;
    const baseY = capY + BTN_DEPTH - travel;
    ellipseOutline(ctx, cx, baseY, BTN_RX, BTN_RY, 1, true);   /* base arc */
    ctx.line(cx - BTN_RX, capY, cx - BTN_RX, baseY, 1);        /* sides */
    ctx.line(cx + BTN_RX, capY, cx + BTN_RX, baseY, 1);
    if (phase.filled) ellipseFill(ctx, cx, capY, BTN_RX, BTN_RY, 1);
    ellipseOutline(ctx, cx, capY, BTN_RX, BTN_RY, 1, false);
    for (const b of phase.bursts) buttonRays(ctx, cx, capY, b);
}

/* Press timestamps per cell (index 0-7) for the current page/menu. */
const btnFired = [[], [], [], [], [], [], [], []];
function stampButton(k) {
    const t = now();
    btnFired[k].push(t);
    /* keep only presses still inside the flash window */
    btnFired[k] = btnFired[k].filter(x => t - x < BTN_FLASH_MS);
    showView('knobs');
}

/* ================================================================
 * Labels + enum squares, ported verbatim from Schwung's
 * render_page_movy.mjs / render_page.mjs so Loopex cells match the
 * host's cells pixel for pixel.
 * ================================================================ */
const LABEL_CHARS = 5;
const LBL_H = 7, LBL_FONT_H = 5;
const ENUM_TEXT_W = ENUM_W - 4;          /* 24 */
const ENUM_MIN_W = 15;
const ENUM_PAD_1LINE = 8;                /* 1px frame + 3px margin, both sides */
const ENUM_PAD_2LINE = 4;                /* 1px frame + 1px margin, both sides */

/* Left edge for a run of `w` pixels centred in the span [x0, x0+span-1];
 * the extra pixel of an odd leftover always goes to the RIGHT. */
function centreX(x0, span, w) { return x0 + Math.floor((span - w) / 2); }

function notchCorners(ctx, x, y, w, h) {
    ctx.fillRect(x, y, 1, 1, 0);
    ctx.fillRect(x + w - 1, y, 1, 1, 0);
    ctx.fillRect(x, y + h - 1, 1, 1, 0);
    ctx.fillRect(x + w - 1, y + h - 1, 1, 1, 0);
}

/* ---------------- text helpers (render_page.mjs) ---------------- */
const ASCII_FOLD = {
    "→": ">", "←": "<", "↔": "<>", "↑": "^", "↓": "v",
    "—": "-", "–": "-", "−": "-", " ": " ",
    "°": "deg", "¢": "c", "µ": "u", "μ": "u",
    "×": "x", "÷": "/", "±": "+/-",
    "‘": "'", "’": "'", "“": "\"", "”": "\"",
    "…": "...", "≤": "<=", "≥": ">=", "≠": "!=",
    "½": "1/2", "¼": "1/4", "¾": "3/4",
    "²": "2", "³": "3", "∞": "inf", "Ω": "ohm",
};
function asciiFold(text) {
    const s = String(text == null ? "" : text);
    if (!/[^\x20-\x7e]/.test(s)) return s;
    let out = "";
    for (const ch of s) {
        if (ch >= " " && ch <= "~") { out += ch; continue; }
        out += ASCII_FOLD[ch] !== undefined ? ASCII_FOLD[ch] : "?";
    }
    return out;
}
function fitText(ctx, text, maxWidth) {
    let s = asciiFold(text);
    if (ctx.textWidth(s) <= maxWidth) return s;
    while (s.length > 1 && ctx.textWidth(s) > maxWidth) s = s.slice(0, -1);
    return s;
}
function devowel(word, ctx, maxWidth) {
    const chars = word.split("");
    for (let i = chars.length - 1; i > 0 && ctx.textWidth(chars.join("")) > maxWidth; i--) {
        if (/[aeiou]/i.test(chars[i])) chars.splice(i, 1);
    }
    return chars.join("");
}
function shortenLabel(ctx, label, maxWidth, joiner = "") {
    const s = asciiFold(label).trim();
    if (!s) return "";
    if (ctx.textWidth(s) <= maxWidth) return s;
    const words = s.split(/[\s_]+/).filter(Boolean);
    if (words.length > 1) {
        const head = words.slice(0, -1);
        const tail = words[words.length - 1];
        const abbrev = (w, n) => (/^\d+$/.test(w) ? w : w.slice(0, Math.max(1, n)));
        for (let n = Math.max(...head.map((w) => w.length)); n >= 1; n--) {
            const cand = head.map((w) => abbrev(w, n)).join(joiner) + joiner + tail;
            if (ctx.textWidth(cand) <= maxWidth) return cand;
        }
        const stem = head.map((w) => abbrev(w, 1)).join(joiner) + joiner;
        const room = maxWidth - ctx.textWidth(stem);
        const shortTail = tail.length <= 7 ? tail : devowel(tail, ctx, room);
        return fitText(ctx, stem + shortTail, maxWidth);
    }
    const word = words[0] || s;
    if (word.length <= 7) return fitText(ctx, word, maxWidth);
    return fitText(ctx, devowel(word, ctx, maxWidth), maxWidth);
}

/* ---------------- abbreviation tables (render_page_movy.mjs) ---------------- */
const WORD_ABBREV = {
    bandpass: "BPF", highpass: "HPF", lowpass: "LPF", bandwidth: "BW",
    bitcrusher: "CRU", crusher: "CRU", character: "CHR", channels: "CHN",
    complexity: "CPX", destination: "DES", multiplier: "MUL",
    modulations: "MOD", generations: "GNS", progression: "PRG",
    recordings: "REC", resonators: "RSN", smoothing: "SMO",
    passthru: "PTH", passthrough: "PTH", fallthrough: "FAL",
    configuration: "CFG", soundfont: "SF", category: "CAT",
    attack: "ATK", decay: "DEC", sustain: "SUS", release: "REL", hold: "HLD",
    envelope: "ENV", env: "ENV", amount: "AMT", amt: "AMT", depth: "DPT",
    cutoff: "CUT", frequency: "FRQ", freq: "FRQ", resonance: "RES", reso: "RES",
    filter: "FLT", resonant: "RES", slope: "SLP",
    oscillator: "OSC", waveform: "WAV", wave: "WAV", shape: "SHP",
    pitch: "PIT", tune: "TUN", detune: "DET", fine: "FIN", coarse: "CRS",
    octave: "OCT", transpose: "TRN", semitone: "SEM", glide: "GLD",
    portamento: "GLD", noise: "NSE", spread: "SPR", offset: "OFS",
    position: "POS", threshold: "THR", ratio: "RAT", knee: "KNE",
    modulation: "MOD", velocity: "VEL", pressure: "PRS", aftertouch: "AFT",
    sensitivity: "SNS", amplitude: "AMP", volume: "VOL", level: "LEV",
    balance: "BAL", panning: "PAN", width: "WID", phase: "PHS",
    feedback: "FBK", delay: "DLY", reverb: "REV", chorus: "CHO",
    flanger: "FLG", phaser: "PHR", tremolo: "TRM", vibrato: "VIB",
    overdrive: "OVR", distortion: "DST", saturation: "SAT", drive: "DRV",
    compressor: "CMP", limiter: "LIM", damping: "DMP", diffusion: "DIF",
    sample: "SMP", start: "STR", length: "LEN", reverse: "RVS",
    speed: "SPD", random: "RND", quantize: "QNT", divide: "DIV",
    portion: "PRT", channel: "CHN", output: "OUT", input: "IN",
    voice: "VCE", voices: "VCES",
    trigger: "TRG", retrigger: "RTG", retrig: "RTG",
    scaling: "SCLG", rotation: "ROT",
    rotate: "ROT", rhythm: "RHY",
    density: "DNS", unison: "UNI", macro: "MCR", operator: "OP",
    right: "RGT", left: "LFT", deform: "DFM", unipolar: "UNP", bipolar: "BIP",
    division: "DIV", matrix: "MTX", color: "CLR", colour: "CLR",
    porta: "GLD", branch: "BRN", enabled: "EN", enable: "EN",
    settings: "SET", stereo: "STO", distort: "DST", keytrack: "KTK",
    cycle: "CYC", general: "GEN", polyphony: "POLY",
    sends: "SND", send: "SND", expression: "EXP", switch: "SW", group: "GRP",
    number: "NUM", reset: "RST", advanced: "ADV",
    predelay: "PDLY", humanize: "HUM", sweep: "SWP", control: "CTL",
    break: "BRK", point: "PNT", assign: "ASN", performance: "PERF",
    perform: "PERF", route: "RTE", source: "SRC", grain: "GRN",
    spectra: "SPC", motion: "MTN", capture: "CAP", oscillators: "OSCS",
    harmonics: "HRM", brightness: "BRT",
    transport: "TRS", compress: "CMP", flutter: "FLU", stutter: "STU",
    octaves: "OCTS", reson: "RES",
};
const ABBREV_SYNONYMS = [
    ["envelope", "env"], ["modulations", "modulation"], ["channels", "channel"],
    ["bitcrusher", "crusher"], ["passthru", "passthrough"], ["amount", "amt"],
    ["frequency", "freq"], ["resonance", "reso", "resonant", "reson"],
    ["glide", "portamento", "porta"], ["color", "colour"], ["divide", "division"],
    ["distortion", "distort"], ["compressor", "compress"], ["retrigger", "retrig"],
    ["enable", "enabled"], ["send", "sends"], ["perform", "performance"],
    ["rotation", "rotate"], ["waveform", "wave"],
];
const ABBREV_CANONICAL = (() => {
    const m = new Map();
    for (const g of ABBREV_SYNONYMS) for (const w of g) m.set(w, g[0]);
    return m;
})();
function caps(s) { return asciiFold(String(s == null ? "" : s)).toUpperCase(); }
function preAbbreviate(label, budget) {
    const s = asciiFold(String(label == null ? "" : label)).trim();
    if (!s) return "";
    const parts = s.split(/[\s_]+/).filter(Boolean);
    const expandable = parts.length === 1 && budget > 0;
    return parts.map((w) => {
        const lw = w.toLowerCase();
        const abbrev = WORD_ABBREV[lw];
        if (!abbrev) return w;
        if (!expandable) return abbrev;
        const canonical = (ABBREV_CANONICAL.get(lw) || lw).toUpperCase();
        return fontWidth4x5(canonical) <= budget ? canonical : abbrev;
    }).join(" ");
}
/* The label a cell actually shows: abbreviate per word, then squeeze to fit. */
function labelForCell(text, cellW = CELL_W) {
    const labelWidth = Math.min(cellW, fontWidth4x5("M".repeat(LABEL_CHARS)));
    return shortenLabel(FONT4_MEASURE, caps(preAbbreviate(text, labelWidth)), labelWidth);
}

/* ---------------- enum square (render_page_movy.mjs) ---------------- */
function fitLine(text, maxWidth) {
    let t = String(text || "");
    while (t.length > 1 && fontWidth4x5(t) > maxWidth) t = t.slice(0, -1);
    return t;
}
function enumSquareNaturalLines(text) {
    const two = enumSquareLines(text, (s) => fontWidth4x5(s) <= ENUM_TEXT_W);
    return [fitLine(two[0], ENUM_TEXT_W), fitLine(two[1], ENUM_TEXT_W)];
}
function enumSquareWidth(text) {
    const lines = enumSquareNaturalLines(text);
    const tw = Math.max(fontWidth4x5(lines[0]), fontWidth4x5(lines[1]));
    const w = tw + (lines[1] ? ENUM_PAD_2LINE : ENUM_PAD_1LINE);
    return w < ENUM_MIN_W ? ENUM_MIN_W : (w > ENUM_W ? ENUM_W : w);
}
/* Big number for a small counted range (font_big_num), centred on the cell. */
function drawBigNumber(ctx, cx, ky, text) {
    const t = String(text);
    bigPrint(ctx, cx - Math.floor(bigWidth(t) / 2), ky + 2, t, 1);
}

/* ---------------- label cell (render_page_movy.mjs drawLabelCell) ---------------- */
function drawLabelCell(ctx, cellX, cellW, lblY, label, displayValue, showValue, inverted) {
    let text = String((showValue ? displayValue : label) || "");
    const budget = cellW - 2;
    while (text.length > 1 && fontWidth4x5(text) > budget) text = text.slice(0, -1);
    if (fontWidth4x5(text) > budget) text = "";
    const tw = fontWidth4x5(text);
    const tx = centreX(cellX, cellW, tw);
    const ty = lblY + Math.floor((LBL_H - LBL_FONT_H) / 2);
    const strip = inverted && tw > 0;
    if (strip) {
        ctx.fillRect(tx - 1, lblY, tw + 2, LBL_H, 1);
        notchCorners(ctx, tx - 1, lblY, tw + 2, LBL_H);
        fontPrint4x5(ctx, tx, ty, text, 0);
    } else {
        fontPrint4x5(ctx, tx, ty, text, 1);
    }
}

/* Which widget a cell draws, from its def alone - mirrors widgetKindFor(meta). */
function widgetKindFor(d) {
    if (!d) return 'knob';
    if (d.trig || d.page !== undefined) return 'button';   /* write-only trigger */
    if (d.opts || d.e2) return 'enum';
    if (d.int && (d.hi - d.lo) <= 24) return 'bignum';
    return 'knob';
}

/* normalised 0..1 plus display text for cell i */
function knobInfo(d, i) {
    const inMenu = menu >= 0;
    const raw = inMenu ? menuVals[i] : knobVals[i];
    if (d.page !== undefined) return [1, ''];
    if (d.trig)  return [0, ''];
    if (d.local) return [(sessSlot - d.lo) / (d.hi - d.lo), String(sessSlot)];
    if (d.opts) {
        const ix = Math.max(0, Math.min(d.opts.length - 1, Math.round(raw) || 0));
        return [d.opts.length > 1 ? ix / (d.opts.length - 1) : 0, String(d.opts[ix])];
    }
    if (d.e2) return [raw > 0.5 ? 1 : 0, d.e2[raw > 0.5 ? 1 : 0]];
    const f = (raw - d.lo) / ((d.hi - d.lo) || 1);
    if (d.k === 'mClock') {
        const semis = (raw - 0.5) * 48;   /* +-24 st */
        if (clkMusic) { let sn = MCLK_SEMI_UI[0]; for (const v of MCLK_SEMI_UI) if (Math.abs(v - semis) < Math.abs(sn - semis)) sn = v;
            return [isFinite(f) ? f : 0, (sn > 0 ? '+' : '') + sn + 'st']; }
        return [isFinite(f) ? f : 0, (Math.abs(semis) < 0.5 ? '1.00' : Math.pow(2, semis / 12).toFixed(2)) + 'x'];
    }
    let t;
    if (d.st)       t = (raw * 12 >= 0 ? '+' : '') + (raw * 12).toFixed(1) + 'st';
    else if (d.spd) t = Math.pow(2, raw).toFixed(2) + 'x';
    else if (d.clk) t = (0.25 * Math.pow(16, raw)).toFixed(2) + 'x';
    else if (d.int) t = String(Math.round(raw));
    else if (d.hold && raw >= 0.99) t = 'Hold';
    else            t = Number(raw).toFixed(2);
    return [isFinite(f) ? f : 0, t];
}

/* Animated enum square — render_page_movy.mjs drawEnumSquare with its width
 * morph: only the FRAME travels (120ms, easeOut); the glyphs swap outright.
 * `raw` is the value behind the text so an unread cell never animates in. */
const ENUM_ANIM_MS = 120;
const animState = createAnimState();
function drawEnumSquare(ctx, kx, ky, text, animKey, raw) {
    const h = BOX_H;
    const target = enumSquareWidth(text);
    let w = target;
    if (animKey !== undefined) {
        const a = observeLanded(animState, "enumw:" + animKey, raw, target, now(), ENUM_ANIM_MS);
        if (a.moving && typeof a.from === "number") {
            w = Math.round(lerp(a.from, target, easeOut(a.t)));
            if (w < ENUM_MIN_W) w = ENUM_MIN_W;
            if (w > ENUM_W) w = ENUM_W;
        }
    }
    const bx = kx + Math.round((KW - w) / 2);   /* centre on the knob axis (kx+KW/2), same column as the label */
    ctx.fillRect(bx, ky, w, 1, 1);
    ctx.fillRect(bx, ky + h - 1, w, 1, 1);
    ctx.fillRect(bx, ky, 1, h, 1);
    ctx.fillRect(bx + w - 1, ky, 1, h, 1);
    notchCorners(ctx, bx, ky, w, h);
    const budget = w - 4;
    const nat = enumSquareNaturalLines(text);
    const line1 = fitLine(nat[0], budget);
    const line2 = fitLine(nat[1], budget);
    const totalH = line2.length > 0 ? 11 : 5;
    const startY = ky + 1 + Math.floor((h - 2 - totalH) / 2);
    const tx = (lw) => centreX(bx + 1, w - 2, lw);
    fontPrint4x5(ctx, tx(fontWidth4x5(line1)), startY, line1, 1);
    if (line2.length > 0) fontPrint4x5(ctx, tx(fontWidth4x5(line2)), startY + 6, line2, 1);
}

/* ---- Schwung page chrome (render_page_movy.mjs / list_geometry.mjs, ported) ----
 * HEADER: a 5-row font4x5 line at y=1 in a 7-row band; a held knob takes the
 * band over, inverted, showing that param's full name and value. The split
 * between the two sides is measured, right first, left gets the remainder.
 * BANK BAR: one segment per page on row 7, the current one two rows tall.
 * FOOTER: [key, action] hint pairs at y=57, key inverted into a notched pill,
 * the BACK pair pinned to the right edge. */
const HEADER_H = 7, BAR_Y = 7, FOOTER_Y = 57, FOOTER_H = 7;
const HEADER_GAP = 4, HEADER_MIN_LEFT = Math.floor(SCREEN_W * 0.55);
const HINT_PAD = 2, HINT_GAP = 4;
function fit5(t, maxW) { return caps(fitText(FONT4_MEASURE, caps(t), maxW)); }
function drawHeader(ctx, left, right, inverted) {
    const W = SCREEN_W;
    if (inverted) {
        ctx.fillRect(0, 0, W, HEADER_H, 1);
        ctx.fillRect(0, 0, 1, 1, 0);            /* top two corners only: the band is the screen's edge */
        ctx.fillRect(W - 1, 0, 1, 1, 0);
    }
    const color = inverted ? 0 : 1;
    let r = right ? fit5(right, Math.floor(W * 0.6)) : '';
    let rw = r ? fontWidth4x5(r) : 0;
    if (rw && W - 4 - rw - HEADER_GAP < HEADER_MIN_LEFT) {
        r = fit5(right, Math.max(0, W - 4 - HEADER_MIN_LEFT - HEADER_GAP));
        rw = r ? fontWidth4x5(r) : 0;
    }
    const l = fit5(left || '', W - 4 - (rw ? rw + HEADER_GAP : 0));
    fontPrint4x5(ctx, 2, 1, l, color);
    if (r) fontPrint4x5(ctx, W - rw - 2, 1, r, color);
}
function drawBankBar(ctx, pageIndex, pageCount) {
    if (pageCount <= 1) return;
    const W = SCREEN_W, gap = new Array(pageCount).fill(0);
    const keep = Math.min(pageCount - 1, Math.max(0, W - pageCount));
    for (let i = 0; i < keep; i++) gap[1 + Math.floor(i * (pageCount - 1) / keep)] = 1;
    const area = W - keep, edge = (b) => Math.floor(b * area / pageCount);
    let x = 0;
    for (let b = 0; b < pageCount; b++) {
        x += gap[b];
        const segW = edge(b + 1) - edge(b), h = (b === pageIndex) ? 2 : 1;
        if (segW > 0) ctx.fillRect(x, BAR_Y, segW, h, 1);
        x += segW;
    }
}
function hintPairWidth(key, action) { return fontWidth4x5(caps(key)) + HINT_PAD + HINT_GAP + fontWidth4x5(caps(action)) + HINT_GAP; }
function isBackHint(h) { return !!h && /^back$/i.test(String(h[0]).trim()); }
function drawFooter(ctx, hints) {
    if (!hints || !hints.length) return 0;
    const W = SCREEN_W, ty = FOOTER_Y + Math.floor((FOOTER_H - FONT4_HEIGHT) / 2);
    const list = hints.filter(Boolean);
    const backIdx = list.findIndex(isBackHint);
    const back = backIdx >= 0 ? list[backIdx] : null;
    const flow = backIdx >= 0 ? list.filter((_, i) => i !== backIdx) : list;
    const drawPair = (x, h) => {
        const key = caps(h[0]), action = caps(h[1]);
        const kw = fontWidth4x5(key), pw = kw + HINT_PAD * 2, ph = FONT4_HEIGHT + 2;
        ctx.fillRect(x, ty - 1, pw, ph, 1);
        if (pw >= 3) notchCorners(ctx, x, ty - 1, pw, ph);
        fontPrint4x5(ctx, x + HINT_PAD, ty, key, 0);
        fontPrint4x5(ctx, x + kw + HINT_PAD + HINT_GAP, ty, action, 1);
    };
    let drawn = 0;
    const backW = back ? hintPairWidth(back[0], back[1]) : 0;
    const backX = back ? W - backW : W, limit = back ? backX : W;
    let x = 1;
    for (const h of flow) {
        if (x + hintPairWidth(h[0], h[1]) > limit) break;
        drawPair(x, h); x += hintPairWidth(h[0], h[1]); drawn++;
    }
    if (back) { drawPair(Math.max(x, backX), back); drawn++; }
    return drawn;
}
/* Full parameter names for the touched header (cells keep the abbreviated label). */
const PAGE_NAMES = ['Loop', 'Texture', 'Tone', 'Heads'];
const FULL_NAMES = {
    v_pitch: 'Speed', v_filter: 'Filter', v_pan: 'Pan', v_volume: 'Volume', v_start: 'Start', v_end: 'End',
    v_reverse: 'Reverse', v_sendA: 'Send A', v_clock: 'Pitch', v_djReso: 'Resonance', v_sat: 'Saturation',
    v_comp: 'Compressor', v_wowflut: 'Wow/Flutter', v_scatter: 'Scatter', v_glitch: 'Seed', v_sendB: 'Send B',
    v_eqBass: 'Bass', v_eqPresFrq: 'Mid Freq', v_eqPresAmt: 'Mid Gain', v_eqTreble: 'Treble', v_tilt: 'Tilt',
    v_atk: 'Attack', v_rel: 'Release', _heads: 'Playheads',
    v_ph1mode: 'Head 1 Mode', v_ph1spd: 'Head 1 Speed', v_ph2mode: 'Head 2 Mode', v_ph2spd: 'Head 2 Speed',
    v_ph3mode: 'Head 3 Mode', v_ph3spd: 'Head 3 Speed', v_ph4mode: 'Head 4 Mode', v_ph4spd: 'Head 4 Speed',
    inputMonitor: 'Monitor', preamp: 'Tape Style', inputGain: 'Input Gain', inLow: 'Input Low', inMid: 'Input Mid',
    inMidFreq: 'Input Mid Freq', inHigh: 'Input High', inHighFreq: 'Input High Freq',
    sendAType: 'Send A FX', sendAM1: 'Send A Amount', sendAM2: 'Send A Macro', sendADrift: 'Send A Drift',
    sendBType: 'Send B FX', sendBM1: 'Send B Amount', sendBM2: 'Send B Macro', sendBDrift: 'Send B Drift',
    stMix: 'Stumble Mix', stStep: 'Stumble Step', stOdds: 'Stumble Odds', stSize: 'Stumble Size',
    stReach: 'Stumble Reach', stKind: 'Stumble Kind', jump: 'Jump', scan: 'Scan',
    masterVol: 'Master Volume', rootNote: 'Root Note', overdubMode: 'Overdub Mode', masterLoCut: 'Master Lo Cut',
    masterHiCut: 'Master Hi Cut', globalSat: 'Global Sat', midiIn: 'MIDI In', armThresh: 'Arm Threshold',
    tapeDrive: 'Tape Drive', tapeWow: 'Tape Wow', tapeFlut: 'Tape Flutter', tapeHF: 'Tape HF Loss',
    tapeLoCut: 'Tape Lo Cut', tapeNoise: 'Tape Noise', tapeGen: 'Generations',
    sessSlot: 'Session Slot', sessSave: 'Save Session', sessLoad: 'Load Session',
    fxseqRun: 'FX Seq Run', fxseqSpeed: 'Step Speed', fxseqLen: 'Pattern Length', fxseqChance: 'Play Chance',
    fxseqGate: 'Gate', fxseqSwing: 'Swing', fxseqDir: 'Direction', fxseqClear: 'Clear Pattern',
    mfCut: 'Master Cut', mfReso: 'Master Reso', mfMode: 'Filter Mode', mClock: 'Master Clock',
    mClockMode: 'Clock Mode', mClockSpot: 'Clock Spot', perfTrem: 'Pump Depth', perfTremRate: 'Pump Rate',
    masterEQ: 'Character', masterGlue: 'Glue Comp', tapeLimit: 'Tape Limiter', punchWidth: 'Punch Width', loopFiltMode: 'Loop Filter', midiOut: 'MIDI Out (LCXL LEDs)',
};
function fullName(d) { return (d && (FULL_NAMES[d.k] || d.lbl)) || ''; }
/* The knob grid: loop page, menu, or (with a punch pad held) the held effect's
 * four params on knobs 5-8. One renderer, three sources of cells. */
function drawKnobView() {
    clear_screen();
    const ctx = screenCtx();
    const inPunch = (menu < 0 && punchMode && punchActive >= 0);
    let defs, title, scope;
    if (confirmSave || confirmClear || confirmDelete) {      /* confirm popup, drawn as two buttons */
        drawHeader(ctx, confirmClear ? 'CLEAR FX PATTERN?' : confirmDelete ? 'DELETE SLOT ' + sessSlot + '?' : 'OVERWRITE SLOT ' + sessSlot + '?', null, true);
        if (confirmSave || confirmDelete) fontPrint4x5(ctx, 2, 11, caps(prettySess(sessNames[sessSlot] || '')), 1);
        drawFooter(ctx, [['K5', 'NO'], ['K8', 'YES']]);
        for (const [i, lbl] of [[4, 'NO'], [7, 'YES']]) {
            const col = i % 4, cellX = col * CELL_W;
            drawButton(ctx, cellX + Math.floor(CELL_W / 2), ROW1_Y, buttonPhase(btnFired[i], now(), i === lastKnob));
            drawLabelCell(ctx, cellX, CELL_W, LBL1_Y, lbl, lbl, false, i === lastKnob);
        }
        host_flush_display(); return;
    }
    let pageName, footer;
    if (menu === 6)     { defs = MENU_DEFS[6]; title = 'FX Seq'; pageName = seqRun ? 'Running' : 'Stopped'; scope = 'm6';
                          footer = (delStepHeld >= 0) ? [[String(delStepHeld + 1), stepMirror[delStepHeld].n ? stepMirror[delStepHeld].pads.map(x => PUNCH_NAMES[x]).join('+') : 'empty']]
                                                      : [['X+Pad+Step', 'Write'], ['X', 'Run']]; }
    else if (menu >= 0) { defs = curMenuDefs(); title = (menu === 5) ? 'Sessions' : 'Loopex';
                          pageName = (menu === 5) ? (sessCurrent > 0 ? 'Session ' + sessCurrent : 'New') : MENU_NAMES[menu]; scope = 'm' + menu;
                          footer = (menu === 5) ? [[String(sessSlot) + (sessSlot === sessCurrent ? '*' : ''), sessNames[sessSlot] ? prettySess(sessNames[sessSlot]) : 'empty'], ['Back', 'Exit']] : [['Back', 'Exit']]; }
    else if (inPunch)   { defs = null; title = 'Punch'; pageName = PUNCH_NAMES[punchActive]; scope = 'p' + punchActive;
                          footer = [['Press', PUNCH_PRESS[punchActive]]]; }
    else                { defs = PAGES[page()]; title = 'Track ' + (sel + 1) + ' ' + STATE_NAMES[voiceState[sel]]; pageName = PAGE_NAMES[page()]; scope = 't' + sel + 'p' + page();
                          footer = (page() === 3) ? [['Jog', jogHead >= 0 ? 'Head ' + (jogHead + 1) : 'Touch a head'], ['Up/Dn', 'Page']] : [['Jog', 'Scrub'], ['Up/Dn', 'Page']]; }
    /* a held knob takes the header over: full name + value, inverted (movyHeaderFor) */
    let hdrL = title, hdrR = pageName, hdrInv = false;
    if (lastKnob >= 0) {
        if (inPunch && lastKnob >= 4) { hdrL = PUNCH_PARAMS[punchActive][lastKnob - 4]; hdrR = punchDisp(punchActive, lastKnob - 4, punchVals[punchActive][lastKnob - 4]); hdrInv = true; }
        else if (inPunch && lastKnob >= 0) { hdrL = PUNCH_LFO_LBL[lastKnob]; hdrR = punchLfoDisp(lastKnob, punchLfo[punchActive][lastKnob]); hdrInv = true; }
        else if (defs && defs[lastKnob]) { hdrL = fullName(defs[lastKnob]); hdrR = knobInfo(defs[lastKnob], lastKnob)[1]; hdrInv = true; }
    }
    drawHeader(ctx, hdrL, hdrR, hdrInv);
    if (menu < 0 && !inPunch) drawBankBar(ctx, page(), PAGES.length);
    drawFooter(ctx, footer);
    for (let i = 0; i < 8; i++) {
        const col = i % 4, row = (i < 4) ? 0 : 1;
        const cellX = col * CELL_W;
        const kx = cellX + Math.floor((CELL_W - KW) / 2);
        const ky = row ? ROW1_Y : ROW0_Y;
        const lblY = row ? LBL1_Y : LBL0_Y;
        const touched = (i === lastKnob);
        if (inPunch) {
            if (i < 4) {                               /* knobs 1-4 = per-effect LFO */
                const lv = punchLfo[punchActive][i], le = punchLfoEnum(i);
                if (le) drawEnumSquare(ctx, kx, ky, le.disp(le.fromVal(lv)), 'pl' + punchActive + ':' + i, le.fromVal(lv));
                else drawArcKnob(ctx, kx, ky, isFinite(lv) ? lv : 0);
                drawLabelCell(ctx, cellX, CELL_W, lblY, labelForCell(PUNCH_LFO_LBL[i]), caps(punchLfoDisp(i, lv)), touched, touched);
                continue;
            }
            const j = i - 4, v = punchVals[punchActive][j], pe = punchEnum(punchActive, j);
            if (pe) drawEnumSquare(ctx, kx, ky, pe.disp(pe.fromVal(v)), 'p' + punchActive + ':' + i, pe.fromVal(v));
            else drawArcKnob(ctx, kx, ky, isFinite(v) ? v : 0);
            drawLabelCell(ctx, cellX, CELL_W, lblY, labelForCell(PUNCH_PARAMS[punchActive][j]),
                          caps(punchDisp(punchActive, j, v)), touched, touched);
            continue;
        }
        const d = defs[i]; if (!d) continue;
        const inf = knobInfo(d, i);
        const kind = widgetKindFor(d);
        if (kind === 'button')      drawButton(ctx, cellX + Math.floor(CELL_W / 2), ky, buttonPhase(btnFired[i], now(), touched));
        else if (kind === 'enum') {
            const raw = (menu >= 0) ? menuVals[i] : knobVals[i];
            drawEnumSquare(ctx, kx, ky, inf[1], scope + ':' + i, raw);
        }
        else if (kind === 'bignum') drawBigNumber(ctx, cellX + Math.floor(CELL_W / 2), ky, inf[1]);
        else                        drawArcKnob(ctx, kx, ky, inf[0]);
        drawLabelCell(ctx, cellX, CELL_W, lblY, labelForCell(d.lbl), caps(inf[1]), touched, touched);
    }
    if (menu === 5 && savedBurstAt > 0 && now() - savedBurstAt < SAVED_BURST_MS) drawSavedBurst(ctx, (now() - savedBurstAt) / SAVED_BURST_MS);
    host_flush_display();
}
/* 'SAVED !' in a plate over the grid, with the button-flash rays bursting outward
 * (same BTN_RAYS idiom as a fired trigger, scaled to the plate). */
function drawSavedBurst(ctx, progress) {
    const txt = 'SAVED !', tw = tzWidth(txt), pw = tw + 12, ph = 17;
    const px0 = Math.floor((SCREEN_W - pw) / 2), py0 = Math.floor((SCREEN_H - ph) / 2);
    ctx.fillRect(px0 - 2, py0 - 2, pw + 4, ph + 4, 0);
    ctx.fillRect(px0, py0, pw, ph, 1);
    notchCorners(ctx, px0, py0, pw, ph);
    tzPrint(ctx, px0 + 6, py0 + 5, txt, 0);
    const cx = px0 + pw / 2, cy = py0 + ph / 2, out = 3 + Math.round(progress * 14), len = 3;
    const rays = 12, fade = progress < 0.75 ? 1 : 0;
    if (!fade) return;
    for (let i = 0; i < rays; i++) {
        const a = (Math.PI * 2 * i) / rays + progress * 0.6, ux = Math.cos(a), uy = Math.sin(a);
        const rx = pw / 2 + out, ry = ph / 2 + out;
        ctx.line(Math.round(cx + ux * rx), Math.round(cy + uy * ry), Math.round(cx + ux * (rx + len)), Math.round(cy + uy * (ry + len)), 1);
    }
}

/* ---- Loop waveform (min/max envelope) with the active playheads over it ---- */
function drawWaveView() {
    clear_screen();
    const ctx = screenCtx();
    /* head modes at a glance: - off, F fwd, B bwd, P ping (e.g. 'F-FP') */
    let hm = '';
    if (headsStr) {
        const MODE_CH = ['-', 'F', 'B', 'P'];
        const parts = headsStr.split(';');
        for (let k = 0; k < 4; k++) { const kv = (parts[k] || '0,0').split(','); hm += MODE_CH[(parseInt(kv[0]) || 0) & 3]; }
    }
    drawHeader(ctx, 'Track ' + (sel + 1) + ' ' + STATE_NAMES[voiceState[sel]] + ' ' + loopLen + 's', hm ? 'Heads ' + hm : null, false);
    drawBankBar(ctx, page(), PAGES.length);
    const midY = 34, halfH = 22;
    if (waveStr && waveStr.length >= 256) {
        for (let x = 0; x < 128; x++) {
            const hi = waveStr.charCodeAt(x * 2)     - 48 - 31;   /* -31..31 */
            const lo = waveStr.charCodeAt(x * 2 + 1) - 48 - 31;
            let yTop = midY - Math.round((hi / 31) * halfH);
            let yBot = midY - Math.round((lo / 31) * halfH);
            if (yBot < yTop) { const t = yTop; yTop = yBot; yBot = t; }
            fill_rect(x, yTop, 1, Math.max(1, yBot - yTop + 1), 1);
        }
    } else {
        tzPrint(ctx, 2, 30, 'EMPTY LOOP', 1);
    }
    if (headsStr) {                                   /* playheads: dashed verticals + number */
        const parts = headsStr.split(';');
        for (let k = 0; k < parts.length && k < 4; k++) {
            const kv = parts[k].split(',');
            if ((parseInt(kv[0]) || 0) === 0) continue;
            const x = Math.min(127, Math.round(((parseInt(kv[1]) || 0) / 999) * 127));
            for (let y = 11; y <= 56; y += 2) px(x, y);
            fontPrint4x5(ctx, Math.min(124, Math.max(0, x - 1)), 58, String(k + 1), 1);
        }
    }
    if (waveStr && waveStr.length >= 256) {           /* Start / End trim points: solid verticals + S / E labels */
        const sx = Math.max(0, Math.min(127, Math.round(waveStart * 127)));
        const ef = waveStart + waveEnd * (1 - waveStart);           /* End is a fraction of what's left after Start */
        const ex = Math.max(0, Math.min(127, Math.round(ef * 127)));
        for (let y = 11; y <= 56; y++) { px(sx, y); px(ex, y); }    /* solid, unlike the dashed playheads */
        const label = (x, ch, right) => {
            const lx = right ? Math.max(0, x - 5) : Math.min(123, x + 2);
            fill_rect(lx - 1, 10, 6, 7, 0);                         /* clear a slot so the letter reads over the wave */
            fontPrint4x5(ctx, lx, 11, ch, 1);
        };
        label(sx, 'S', false);
        label(ex, 'E', true);
    }
    host_flush_display();
}

/* Main overview: header, 16-track strip, meters, and a contextual hint. */
function drawUI() {
    if (view === 'knobs') { drawKnobView(); return; }
    if (view === 'wave')  { drawWaveView(); return; }
    clear_screen();
    const ctx = screenCtx();
    drawHeader(ctx, 'Track ' + (sel + 1) + ' ' + STATE_NAMES[voiceState[sel]], 'CPU ' + cpu + '%', false);
    for (let i = 0; i < NV; i++) {                 /* 16-track strip: height = state */
        const st = voiceState[i], x = i * 8;
        const h = (st === 0) ? 1 : (st === 3 ? 3 : 6);
        fill_rect(x, 11, 6, h, 1);
        if (i === sel) fill_rect(x, 19, 6, 1, 1);   /* selected underline */
    }
    draw_line(0, 23, SCREEN_W, 23, 1);
    tzPrint(ctx, 0, 27, ('LOOP ' + loopLen + 'S').toUpperCase(), 1);
    const inTxt = 'IN ' + inPeak;
    tzPrint(ctx, SCREEN_W - tzWidth(inTxt) - 1, 27, inTxt, 1);
    /* one line of context: what the modifiers do right now, or a status message */
    if (tickCount < statusMsgUntil) fontPrint4x5(ctx, 2, FOOTER_Y + 1, caps(statusMsg), 1);
    else {
        let hints;
        if (muteHeld)       hints = [['Mute+Pad', 'Mute track']];
        else if (copyHeld)  hints = (cloneSrc < 0) ? [['Copy+Pad', 'Source']] : [['Copy', 'Tap target pad']];
        else if (loopHeld)  hints = [['Loop+Pad', 'Length']];
        else if (shiftHeld) hints = [['Shift', 'Speed / Latch / Arm']];
        else if (punchMode && punchActive >= 0) hints = [['Punch', PUNCH_NAMES[punchActive]]];
        else hints = [['Hold pad', 'Clear'], ['Undo', 'Restore']];
        drawFooter(ctx, hints);
    }
    host_flush_display();
}

/* ---- lifecycle ---- */
globalThis.init = function () {
    for (let i = 0; i < NV; i++) voiceState[i] = 0;
    pollStates();
    reloadKnobs();
    pollSeqMirror();
    paintAll(true);
    setButtonLED(MoveDelete, seqRun ? WhiteLedBright : WhiteLedDim, true);
};
globalThis.onUnload = function () { clearAllLEDs(); };
globalThis.onResume = function () {
    pollStates();                 /* DSP is the truth after a suspend */
    needReload = true; dirty = true;
    resumeRepaint = 6;            /* force a full LED repaint over the next ticks */
    paintAll(true);
};

globalThis.tick = function () {
    if (globalThis.overtakeParked) return;
    tickCount++;
    if (needReload) reloadKnobs();
    if (menuReload) reloadMenu();
    if ((menu === 5 || sessPending) && tickCount % 4 === 0) {
        const st = gp('sessStatus');
        if (st && st !== sessLast) { sessLast = st;
            if (st === 'OK') {
                needReload = true; menuReload = true; pollSessNames();
                if (sessPending === 'save') { sessCurrent = sessPendSlot; savedBurstAt = now(); setMsg('Saved !'); }
                else if (sessPending === 'load') { sessCurrent = sessPendSlot; setMsg('Loaded session ' + sessPendSlot); pollSeqMirror(); reloadPunchMirrors(); pollStates(); setButtonLED(MoveDelete, seqRun ? WhiteLedBright : WhiteLedDim, true); }
                else if (sessPending === 'delete') { if (sessCurrent === sessPendSlot) sessCurrent = 0; setMsg('Deleted slot ' + sessPendSlot); }
                sessPending = '';
            }
            else if (st === 'Empty') { setMsg('Slot ' + sessPendSlot + ' is empty'); sessPending = ''; }
            else setMsg(st);
            dirty = true; }
    }
    if (resumeRepaint > 0) { resumeRepaint--; paintAll(true); }   /* force LEDs back after resume */
    if (tickCount % 5 === 0) {                                   /* blink driver for armed / clone-src */
        blinkOn = !blinkOn;
        for (let i = 0; i < NV; i++) if (armedArr[i] || i === cloneSrc) setLED(LEFT_NOTES[i], padColor(i), true);
        for (let i = 0; i < NV; i++) if (padFlash[i] && now() >= padFlash[i]) { padFlash[i] = 0; setLED(RIGHT_NOTES[i], rightColor(i), true); }
    }
    if (tickCount % 10 === 4) {                                  /* armed state from the DSP */
        const a = gp('armed');
        if (a && a.length >= NV) for (let i = 0; i < NV; i++) {
            const on = a.charAt(i) === '1';
            if (on !== armedArr[i]) { armedArr[i] = on; setLED(LEFT_NOTES[i], padColor(i), true); }
        }
    }
    if (delHeld) viewUntil = now() + VIEW_MS;
    if ((seqRun || seqPatternView()) && tickCount % 2 === 0) {
        const r = gp('fxpat'); if (r) { const np = parseInt(r) ; if (np !== fxPos) { fxPos = isNaN(np) ? -1 : np; if (seqPatternView()) paintSteps(); } }
    }
    if (view !== 'main' && now() >= viewUntil) { view = 'main'; dirty = true; }
    if (view === 'wave') {
        if (tickCount % 12 === 0) { const w = gp('wave'); if (w) waveStr = w; }
        const h = gp('heads'); if (h) headsStr = h;
        if (tickCount % 12 === 5) { const a = parseFloat(gp('v_start')); if (!isNaN(a)) waveStart = a;
                                    const b = parseFloat(gp('v_end'));   if (!isNaN(b)) waveEnd = b; }
    }
    if (tickCount % 15 === 9) { const m = parseFloat(gp('driftMix')); const on = !isNaN(m) && m > 0.1;
        if (on !== driftMixOn) { driftMixOn = on; paintNav(); } }
    if (tickCount % 6 === 0) pollStates();
    if (tickCount % 15 === 3) { const c = gp('cpu'); if (c) cpu = c; }
    if (tickCount % 12 === 7) { const l = gp('v_loopLen'); if (l) loopLen = l; const p = gp('inputPeak'); if (p) inPeak = p; }
    drainLEDs();
    drawUI();
};

function selectTrack(i) {
    if (i === sel) return;
    setLED(MoveSteps[sel], DarkGrey, true);
    sel = i; spCmd('sel:' + i);
    setLED(MoveSteps[sel], White, true);
    needReload = true;
}

globalThis.onMidiMessageInternal = function (data) {
  try {
    const status = data[0] & 0xf0, d1 = data[1], d2 = data[2];

    if (status === 0xb0) {                          /* CC: knobs + buttons */
        if (d1 === MoveBack && d2 > 0) { if (confirmSave || confirmClear || confirmDelete) { confirmSave = false; confirmClear = false; confirmDelete = false; setMsg('cancelled'); dirty = true; return; } if (menu >= 0) { menu = -1; paintTrackLEDs(); paintNav(); dirty = true; return; } clearAllLEDs(); host_exit_module(); return; }
        if (d1 === MoveShift) { shiftHeld = d2 > 0;
            if (shiftHeld) for (const i of physHeld) {          /* Shift while a punch pad is held: latch it as it is, pressure included */
                if (punchLatched[i]) continue;
                punchLatched[i] = true; pressFrozen[i] = true;
                sp('punchPress', i + ':' + padPress[i].toFixed(3));
                setMsg('Latch ' + PUNCH_NAMES[i] + ' @' + Math.round(padPress[i] * 100) + '%'); enqLED(RIGHT_NOTES[i], rightColor(i));
            }
            return; }
        if (d1 === MoveMute)  { muteHeld = d2 > 0; paintNav(); return; }     /* Mute modifier (lights the button) */
        if (d1 === MoveCapture && d2 > 0) { openMenu(4); return; }           /* Capture = Tape menu */
        if (d1 === MoveMenu && d2 > 0) { openMenu(5); return; }              /* three-lines = Sessions menu */
        if (d1 === MoveSample) { sampleHeld = d2 > 0; paintNav();
            if (d2 === 0) { if (armChord) { if (!armJogged) { spCmd('arm:' + sel); armedArr[sel] = !armedArr[sel];
                    setMsg('T' + (sel + 1) + (armedArr[sel] ? ' ARMED' : ' disarmed')); enqLED(LEFT_NOTES[sel], padColor(sel)); } armChord = false; } return; } }
        if (d1 === MoveSample  && d2 > 0) {                                  /* Sample/Record button */
            if (shiftHeld) { armChord = true; armJogged = false; return; }   /* arm decided on release */
            openMenu(7); return;                                             /* Sample button = Drift menu */
        }
        if (d1 === MoveCopy) { copyHeld = d2 > 0; if (!copyHeld) cloneSrc = -1; paintNav(); return; }
        if (d1 === MoveLoop) { loopHeld = d2 > 0; paintNav(); return; }
        if (d1 === MoveMainKnob) {
            const dv = decodeDelta(d2); if (dv === 0) return;
            if (shiftHeld && sampleHeld) {                                   /* arm threshold */
                armJogged = true;
                armThreshVal = clampf(armThreshVal + dv * 0.01, 0, 1);
                sp('armThresh', armThreshVal.toFixed(4));
                setMsg('ArmTh ' + armThreshVal.toFixed(2)); return;
            }
            if (page() === 3 && jogHead >= 0) {                              /* move a playhead */
                sp('headpos', jogHead + ':' + dv);
                setMsg('H' + (jogHead + 1) + (dv > 0 ? ' >>' : ' <<')); showView('wave'); return;
            }
            sp('scrub', String(dv));                                         /* scrub the tape */
            setMsg('scrub ' + (dv > 0 ? '>>' : '<<')); showView('wave'); return;
        }
        if (d1 === MoveDown  && d2 > 0) { if (menu >= 0 && MENU_PAGED[menu]) { menuPage = Math.min(menuPages() - 1, menuPage + 1); menuReload = true; dirty = true; return; } setPage(loopPage + 1); showView('knobs'); return; }
        if (d1 === MoveUp    && d2 > 0) { if (menu >= 0 && MENU_PAGED[menu]) { menuPage = Math.max(0, menuPage - 1); menuReload = true; dirty = true; return; } setPage(loopPage - 1); showView('knobs'); return; }
        if (d1 === MoveUndo) {                                               /* Undo: last overdub, else last clear */
            if (d2 > 0) { undoHeld = true; undoUsed = false; setButtonLED(MoveUndo, WhiteLedBright, true); }
            else { undoHeld = false; setButtonLED(MoveUndo, WhiteLedDim, true); if (!undoUsed) doUndo(); }
            return;
        }
        if (d1 === MoveDelete) {                                              /* X: FX sequencer */
            if (d2 > 0) { delHeld = true; delUsed = false; delDownAt = now(); delStepHeld = -1;
                if (menu !== 6) { menu = 6; menuReload = true; paintTrackLEDs(); paintNav(); }
                showView('knobs'); paintSteps(); dirty = true; }
            else { delHeld = false; delPads.length = 0; delStepHeld = -1;
                if (!delUsed) { if (now() - delDownAt < 400) setSeqRun(!seqRun); menu = -1; paintTrackLEDs(); paintNav(); }
                paintSteps(); dirty = true; }
            return;
        }
        if (d1 === MovePlay && d2 > 0) { sp('fxseq', 'restart'); }             /* transport start: pattern from step 1 (Play also reaches Move) */
        if (d1 === MoveLeft || d1 === MoveRight) {                            /* tape transport: brake to a stop / wind up to a tone */
            const dir = (d1 === MoveLeft) ? -1 : 1;
            sp('tapeHold', d2 > 0 ? String(dir) : '0');
            setButtonLED(d1, d2 > 0 ? WhiteLedBright : WhiteLedOff, true);
            if (d2 > 0) setMsg(dir < 0 ? 'tape stop' : 'tape wind'); else setMsg('tape back');
            return;
        }
        const rowIdx = ROW_CCS.indexOf(d1);
        if (rowIdx >= 0) {                          /* track buttons = menus */
            if (d2 > 0) openMenu(rowIdx);
            return;
        }
        const k = d1 - MoveKnob1;
        if (k >= 0 && k < 8) {
            if (punchMode && punchActive >= 0) {
                if (menu >= 0) { menu = -1; menuPage = 0; punchTookMenu = true; paintTrackLEDs(); paintNav(); }   /* pad + knob takes over whatever menu was open */
                if (k < 4) {                                           /* knobs 1-4 = LFO dest/shape/rate/depth */
                    const le = punchLfoEnum(k); let nv;
                    if (le) { const st = enumSteps(k, decodeDelta(d2)); if (st === 0) return;
                        nv = le.toVal(Math.max(0, Math.min(le.n - 1, le.fromVal(punchLfo[punchActive][k]) + st))); }
                    else { nv = clampf(punchLfo[punchActive][k] + decodeDelta(d2) * 0.01, 0, 1); }
                    punchLfo[punchActive][k] = nv; sp('pflfo', punchActive + ':' + k + ':' + nv.toFixed(4));
                    lastKnob = k; lastKnobLbl = PUNCH_LFO_LBL[k]; lastKnobVal = punchLfoDisp(k, nv); showView('knobs');
                } else {                                               /* knobs 5-8 = fx params (finer 0.01) */
                    const j = k - 4, pe = punchEnum(punchActive, j); let nv;
                    if (pe) { const st = enumSteps(k, decodeDelta(d2)); if (st === 0) return;
                        nv = pe.toVal(Math.max(0, Math.min(pe.n - 1, pe.fromVal(punchVals[punchActive][j]) + st))); }
                    else { nv = clampf(0.5 + Math.round((punchVals[punchActive][j] + decodeDelta(d2) * 0.01 - 0.5) / 0.01) * 0.01, 0, 1); }
                    punchVals[punchActive][j] = nv; sp('pfx', punchActive + ':' + j + ':' + nv.toFixed(4));
                    lastKnob = k; lastKnobLbl = PUNCH_PARAMS[punchActive][j]; lastKnobVal = punchDisp(punchActive, j, nv); showView('knobs');
                }
                return;
            }
            if (menu >= 0 && curMenuDefs()) { menuKnob(k, decodeDelta(d2)); showView('knobs'); return; }
            const def = PAGES[page()][k];
            if (def.page !== undefined) { if (decodeDelta(d2) !== 0) { stampButton(k); setPage(def.page); } return; }
            if (def.opts) {                                  /* enum page knob (playhead modes) */
                const st = enumSteps(k, decodeDelta(d2)); if (st === 0) return;
                const ix = Math.max(0, Math.min(def.opts.length - 1, Math.round(knobVals[k]) + st));
                knobVals[k] = ix; sp(def.k, def.opts[ix]);
                lastKnob = k; lastKnobLbl = def.lbl; lastKnobVal = def.opts[ix]; showView('knobs'); return;
            }
            const step = (def.step !== undefined) ? def.step : (def.hi - def.lo) * 0.006;
            let nv = knobVals[k] + decodeDelta(d2) * step;
            if (def.clk || def.spd) { const center = (def.lo + def.hi) / 2; nv = center + Math.round((nv - center) / step) * step; }  /* clk/spd land on exact 0.5/1x */
            nv = clampf(nv, def.lo, def.hi);
            knobVals[k] = nv;
            if (def.e2) { sp(def.k, nv > 0.5 ? '1' : '0'); lastKnobVal = def.e2[nv > 0.5 ? 1 : 0]; }
            else { sp(def.k, nv.toFixed(4)); lastKnobVal = def.st ? ((nv * 12 >= 0 ? '+' : '') + (nv * 12).toFixed(1) + 'st') : def.spd ? Math.pow(2, nv).toFixed(2) + 'x' : (def.clk ? (0.25 * Math.pow(16, nv)).toFixed(2) + 'x' : nv.toFixed(2)); }
            lastKnob = k; lastKnobLbl = def.lbl;
            if (def.k === 'v_start') { waveStart = nv; showView('wave'); }
            else if (def.k === 'v_end') { waveEnd = nv; showView('wave'); }
            else showView('knobs');
        }
        return;
    }

    if (status === 0x90 && d2 > 0) {                /* note-on */
        if (d1 < 10) { handleKnobTouch(d1); return; }   /* capacitive knob touch */
        if (d1 in NOTE_TO_LEFT) {
            const i = NOTE_TO_LEFT[d1];
            if (copyHeld) {                             /* Copy+pad: first pad = source (blinks), second = clone target */
                mutePressed[i] = true;                  /* its release must not clear */
                if (cloneSrc < 0) { cloneSrc = i; setMsg('Clone T' + (i + 1) + ' -> ?'); }
                else if (i !== cloneSrc) { spCmd('clone:' + cloneSrc + ':' + i);
                    setMsg('T' + (cloneSrc + 1) + ' -> T' + (i + 1)); cloneSrc = -1; }
                else { cloneSrc = -1; setMsg('clone cancelled'); }
                return;
            }
            if (loopHeld) {                             /* Loop+pad: cycle loop-length multiple */
                mutePressed[i] = true;
                loopMultIdx[i] = (loopMultIdx[i] + 1) % LOOP_MULTS.length;
                const m = LOOP_MULTS[loopMultIdx[i]];
                sp('v' + i + '.end', m.toFixed(4));
                setMsg('T' + (i + 1) + ' len ' + m + 'x'); return;
            }
            if (muteHeld) {                             /* Mute+tap = toggle quick mute (playhead keeps running) */
                mutePressed[i] = true;                  /* flag: this pad's release is a mute, not a clear */
                mutes[i] = !mutes[i]; spCmd('mute:' + i);
                enqLED(LEFT_NOTES[i], padColor(i)); setMsg('T' + (i + 1) + (mutes[i] ? ' muted' : ' unmuted')); return;
            }
            selectTrack(i);
            pressMs[i] = now();
            if (shiftHeld) { mutePressed[i] = true; cycleSpeed(i); return; }   /* Shift+tap = cycle speed (not a clear-hold) */
            const t = now(), dbl = (t - lastTapMs[i]) < DOUBLE_TAP_MS;
            lastTapMs[i] = t;
            if (dbl && voiceState[i] >= 2) {            /* double-tap a loop with content = overdub */
                spCmd('odub:' + i);
                voiceState[i] = (voiceState[i] === 4) ? 2 : 4;
                setMsg('T' + (i + 1) + (voiceState[i] === 4 ? ' overdub' : ' play'));
            } else {
                spCmd('tap:' + i);
                voiceState[i] = nextTap(voiceState[i]);
            }
            enqLED(LEFT_NOTES[i], padColor(i));
            return;
        }
        if (d1 in STEP_TO_TRACK && (delHeld || menu === 6)) {   /* FX Seq: write / clear / extend / hold-to-edit */
            const st = STEP_TO_TRACK[d1]; delUsed = true;
            if (delStepHeld >= 0 && st > delStepHeld) {          /* held step + later step = extension */
                for (let i = delStepHeld + 1; i <= st; i++) { const m = stepMirror[i]; m.ext = 1; m.n = 0; m.pads = []; sp('fxext', i + ':1'); }
                setMsg('Step ' + (delStepHeld + 1) + ' held to ' + (st + 1));
            } else {
                const pads = physHeld.concat(delPads.filter(x => physHeld.indexOf(x) < 0));
                if (pads.length) writeStep(st, pads); else clearStep(st);
                delStepHeld = st; menuReload = true;
            }
            paintSteps(); dirty = true; return;
        }
        if (d1 in STEP_TO_TRACK) {
            const t = STEP_TO_TRACK[d1];
            if (menu >= 0) { menu = -1; paintTrackLEDs(); paintNav(); }
            if (t === sel) { setPage((loopPage + 1) % NPAGES); showView('knobs'); }   /* same step again = next loop page */
            else { selectTrack(t); showView('wave'); }
            return;
        }
        if (d1 in NOTE_TO_RIGHT) {                  /* punch-in FX: hold to apply, knobs edit it */
            const i = NOTE_TO_RIGHT[d1];
            /* a punch pad no longer opens its param screen or leaves the menu on press;
             * the menu is taken over only when a knob is actually touched (see knob handler) */
            if (shiftHeld && punchLatched[i]) {     /* Shift+pad on a latched effect = unlatch (stop) */
                punchLatched[i] = false; pressFrozen[i] = false; padPress[i] = 0; pressMax[i] = 0;
                const hi0 = heldPunch.indexOf(i); if (hi0 >= 0) heldPunch.splice(hi0, 1);
                const pi0 = physHeld.indexOf(i);  if (pi0 >= 0) physHeld.splice(pi0, 1);
                sp('punch', 'off:' + i); sp('punchPress', i + ':0');
                if (physHeld.length) punchActive = physHeld[physHeld.length - 1];
                else { punchActive = -1; punchMode = false; needReload = true; }
                enqLED(RIGHT_NOTES[i], rightColor(i)); setMsg('Unlatch ' + PUNCH_NAMES[i]); return;
            }
            if (delHeld) {                           /* X + pad: select it for the next step press (no sound) */
                if (delPads.indexOf(i) < 0 && delPads.length < 5) delPads.push(i); delUsed = true;
                enqLED(RIGHT_NOTES[i], White); setMsg('Step: ' + delPads.concat(physHeld).map(x => PUNCH_NAMES[x]).join('+')); return;
            }
            if (undoHeld) {                          /* Undo + pad = this effect + its LFO back to defaults */
                punchVals[i] = PUNCH_DEFAULTS[i].slice(); punchLfo[i] = PUNCH_LFO_DEFAULTS[i].slice(); undoUsed = true;
                for (let j = 0; j < 4; j++) { sp('pfx', i + ':' + j + ':' + punchVals[i][j].toFixed(4)); sp('pflfo', i + ':' + j + ':' + punchLfo[i][j].toFixed(4)); }
                padFlash[i] = now() + 130; enqLED(RIGHT_NOTES[i], White);   /* quick LED flash to confirm the reset */
                setMsg('Reset ' + PUNCH_NAMES[i]); dirty = true; return;
            }
            if (heldPunch.indexOf(i) < 0 && heldPunch.length >= 5) { setMsg('5 FX max'); return; }   /* series is full */
            pressFrozen[i] = false; padPress[i] = 0; pressMax[i] = 0;   /* a new hold starts with live pressure */
            if (shiftHeld) punchLatched[i] = true;  /* Shift+pad = latch on (stays after release) */
            if (heldPunch.indexOf(i) < 0 && heldPunch.length < 5) heldPunch.push(i);   /* up to 5 in series (NUM_PSLOTS) */
            if (physHeld.indexOf(i) < 0) physHeld.push(i);
            punchActive = i; punchMode = true;
            for (let j = 0; j < 4; j++) { sp('pfx', i + ':' + j + ':' + punchVals[i][j].toFixed(4)); sp('pflfo', i + ':' + j + ':' + punchLfo[i][j].toFixed(4)); }
            sp('punch', 'on:' + i);
            enqLED(RIGHT_NOTES[i], rightColor(i));
            setMsg((punchLatched[i] ? 'Latch ' : 'Punch ') + PUNCH_NAMES[i]);
            return;
        }
        return;
    }

    if (status === 0xa0) {                          /* pad pressure -> punch intensity (per held effect) */
        if (d1 in NOTE_TO_RIGHT) { const i = NOTE_TO_RIGHT[d1];
            if (delPads.indexOf(i) >= 0) padPress[i] = d2 / 127;   /* pressure of a selected pad becomes the step's lock */
            if (heldPunch.indexOf(i) >= 0 && !pressFrozen[i]) { padPress[i] = d2 / 127; if (padPress[i] > pressMax[i]) pressMax[i] = padPress[i]; sp('punchPress', i + ':' + padPress[i].toFixed(3)); } }
        return;
    }

    if (status === 0x80 || (status === 0x90 && d2 === 0)) {   /* note-off */
        if (d1 < 10) { if (lastKnob === d1) { lastKnob = -1; dirty = true; } return; }   /* knob released */
        if (d1 in STEP_TO_TRACK) { if (delStepHeld === STEP_TO_TRACK[d1]) { delStepHeld = -1; menuReload = true; paintSteps(); dirty = true; } return; }
        if (d1 in NOTE_TO_LEFT) {
            const i = NOTE_TO_LEFT[d1];
            if (mutePressed[i]) { mutePressed[i] = false; return; }   /* release of a Mute+tap — never clears */
            if (pressMs[i] > 0 && now() - pressMs[i] >= CLEAR_HOLD_MS) {   /* long-press = clear loop */
                spCmd('clear:' + i);
                voiceState[i] = 0; lastTapMs[i] = 0; mutes[i] = false; lastCleared = i;
                enqLED(LEFT_NOTES[i], padColor(i));
                setMsg('T' + (i + 1) + ' cleared (Undo)');
            }
            return;
        }
        if (d1 in NOTE_TO_RIGHT) {                  /* release punch effect */
            const i = NOTE_TO_RIGHT[d1];
            { const di = delPads.indexOf(i); if (di >= 0) { delPads.splice(di, 1); enqLED(RIGHT_NOTES[i], rightColor(i)); return; } }   /* was a selection, never engaged */
            /* A latched effect keeps RUNNING, but the knobs go back to the page as
             * soon as no punch pad is physically held — otherwise knobs 5-8 (H3/H4
             * on P4) stay captured by the punch effect indefinitely. */
            const pi = physHeld.indexOf(i); if (pi >= 0) physHeld.splice(pi, 1);
            if (physHeld.length) punchActive = physHeld[physHeld.length - 1];
            else { punchActive = -1; punchMode = false; needReload = true;
                   if (punchTookMenu) { showView('main'); }         /* pad+knob had taken over a menu: land on main, not back in it */
                   punchTookMenu = false; }
            if (punchLatched[i]) {                                   /* latched: keep running; a Shift+pad latch keeps the peak pressure of the hold */
                if (!pressFrozen[i]) { padPress[i] = pressMax[i]; pressFrozen[i] = true; sp('punchPress', i + ':' + padPress[i].toFixed(3)); }
                enqLED(RIGHT_NOTES[i], rightColor(i)); return; }
            const hi = heldPunch.indexOf(i); if (hi >= 0) heldPunch.splice(hi, 1);
            sp('punch', 'off:' + i); sp('punchPress', i + ':0');
            enqLED(RIGHT_NOTES[i], rightColor(i));
        }
        return;
    }
  } catch (e) {}
};

function cycleSpeed(i) {
    speedIdx[i] = (speedIdx[i] + 1) % 3;
    const pit = [-1, 1, 0][speedIdx[i]];               /* 0.5x, 2x, 1x (octaves) */
    sp('v_pitch', pit.toFixed(4));
    needReload = true;
    enqLED(LEFT_NOTES[i], padColor(i));   /* reflect the new speed on the pad */
    setMsg('T' + (i + 1) + ' ' + ['0.5x', '2x', '1x'][speedIdx[i]]);
}

/* optimistic next-state mirror of the DSP tap logic (poll reconciles) */
function nextTap(st) {
    switch (st) { case 0: return 1; case 1: return 2; case 2: return 3; case 3: return 2; case 4: return 2; default: return st; }
}

/* LaunchControl XL: the two "BlackBox" templates drive all 16 loops.
 * Continuous CCs (absolute, ch 1): template 1-8 = CC 1..32, template 9-16 = CC 41..72.
 * Per column (track): top knob = Speed, middle = Filter, bottom = Pan, fader = Volume.
 * Buttons (note-on): bottom row = tap (rec/play/pause), top row = mute.
 *   Track Focus (row 1) = tap: 68..75 / 76..83 ; Track Control (row 2) = mute: 36..43 / 44..51.
 * Both ranges are handled at once, so it works whichever template is loaded. */
globalThis.onMidiMessageExternal = function (data) {
    if (!data || data.length < 3) return;
    let st, d1, d2;
    if ((data[0] & 0x80) === 0 && data.length >= 4) { st = data[1] & 0xF0; d1 = data[2]; d2 = data[3]; }  /* 4-byte USB-framed */
    else { st = data[0] & 0xF0; d1 = data[1]; d2 = data[2]; }

    if (st === 0xB0) {                                   /* knobs + faders */
        let base;
        if (d1 >= 1 && d1 <= 32) base = 0;               /* loops 1-8 */
        else if (d1 >= 41 && d1 <= 72) base = 8;         /* loops 9-16 */
        else return;
        const rel = (base === 0) ? (d1 - 1) : (d1 - 41);   /* 0..31 */
        const loop = base + (rel % 8), row = (rel / 8) | 0, norm = d2 / 127;
        let key, val;
        if (row === 0)      { key = 'pit'; val = (d2 >= 62 && d2 <= 66) ? 0 : norm * 4 - 2; }  /* Speed, small centre snap to 1x */
        else if (row === 1) { key = 'fil'; val = norm; }             /* Filter */
        else if (row === 2) { key = 'pan'; val = norm * 2 - 1; }     /* Pan */
        else                { key = 'vol'; val = norm; }             /* Volume */
        sp('v' + loop + '.' + key, val.toFixed(4));
        return;
    }
    if (st === 0x90 && d2 > 0) {                         /* buttons */
        let loop = -1, mute = false;
        if      (d1 >= 68 && d1 <= 75) loop = d1 - 68;                        /* Track Focus row = tap */
        else if (d1 >= 76 && d1 <= 83) loop = 8 + (d1 - 76);
        else if (d1 >= 36 && d1 <= 43) { loop = d1 - 36;     mute = true; }    /* Track Control row = mute */
        else if (d1 >= 44 && d1 <= 51) { loop = 8 + (d1 - 44); mute = true; }
        else return;
        if (mute) { mutes[loop] = !mutes[loop]; spCmd('mute:' + loop); enqLED(LEFT_NOTES[loop], padColor(loop)); }
        else { spCmd('tap:' + loop); voiceState[loop] = nextTap(voiceState[loop]); enqLED(LEFT_NOTES[loop], padColor(loop)); }
        dirty = true;
    }
};
