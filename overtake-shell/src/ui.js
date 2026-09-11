/*
 * LoopBox — Overtake UI (QuickJS). Drives the loopbox.c engine via string params.
 *
 * Left 16 pads  = loop tracks: tap cycles Empty->Rec->Play<->Pause; double-tap
 *                 (from Play/Pause) = Overdub; LED colored by state.
 * Right 16 pads = reserved (punch-FX, next wave) — shown dim.
 * Step row      = select track. 8 knobs = selected-track params over 2 pages
 *                 (Down arrow = page 2, Up arrow = page 1). Screen shows strip + CPU%.
 * Back = clean exit.
 */

import {
    Black, White, LightGrey, DarkGrey,
    BrightRed, NeonGreen, Purple, AzureBlue, VividYellow,
    MoveKnob1, MoveShift, MoveBack, MoveUp, MoveDown, MoveLeft, MoveRight, MoveUndo, MoveMute,
    MoveSample, MoveCapture, MoveCopy, MoveLoop, MoveMainKnob,
    MoveSteps, MoveRow1, MoveRow2, MoveRow3, MoveRow4,
    WhiteLedOff, WhiteLedDim, WhiteLedBright,
} from '/data/UserData/schwung/shared/constants.mjs';

import { setLED, setButtonLED, decodeDelta }
    from '/data/UserData/schwung/shared/input_filter.mjs';

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
const PUNCH_NAMES = ['Loop16','Loop12','LoopSh','LoopSr','Stut4','Stut3','Retrig','Q6/8',
                     'Oct+','Oct-','Haze','Shmr','Strch','Freez','Revrse','Sat'];
const PUNCH_PAD_COLORS = [AzureBlue,AzureBlue,AzureBlue,AzureBlue, BrightRed,BrightRed,BrightRed,BrightRed,
                          Purple,Purple,NeonGreen,NeonGreen, VividYellow,VividYellow,Purple,BrightRed];
const PUNCH_DSP_KEYS = ['punchRate','punchPitch','punchTone','punchMix'];  /* knobs 5-8 -> these DSP slots */
const PUNCH_PARAMS = [ /* per-effect labels for knobs 5,6,7,8 */
  ['Rate','Pit','Tone','Mix'],['Rate','Pit','Tone','Mix'],['Rate','Pit','Tone','Mix'],['Rate','Pit','Tone','Mix'],
  ['Rate','Pit','Tone','Mix'],['Rate','Pit','Tone','Mix'],['Rate','Pit','Tone','Mix'],['Rate','Pit','Tone','Mix'],
  ['Fine','Pit','Tone','Mix'],['Fine','Pit','Tone','Mix'],['Size','Pit','Dens','Mix'],['Regn','Pit','Tone','Mix'],
  ['Strch','Pit','Grn','Mix'],['Frz','Pit','Grn','Mix'],['Len','Pit','Tone','Mix'],['Drive','Char','Tone','Mix']
];
let punchMode = false, punchActive = -1;
const heldPunch = [];  /* currently-held punch pads (up to 4, in press order) */
const punchLatched = new Array(NV).fill(false);  /* Shift+pad = latch on (hands-free) */
const punchVals = [];  /* [16][4] per-effect stored values */
for (let i = 0; i < 16; i++) punchVals.push([0.5, 0.5, 1.0, 1.0]);
punchVals[10] = [0.4, 0.5, 0.5, 1.0];   /* Haze:   size, pitch, density */
punchVals[11] = [0.4, 0.0, 0.4, 1.0];   /* Shimmer: regen, pitch 1x (ratio=1+P1), darker tone */
punchVals[12] = [0.5, 0.5, 0.3, 1.0];   /* Stretch: mid stretch, small grain */
punchVals[13] = [1.0, 0.5, 0.3, 1.0];   /* Freeze:  full freeze */
punchVals[15] = [0.4, 0.0, 0.7, 1.0];   /* Saturate: drive, Tape character, open tone */

/* ---- Track-button menus (MoveRow1..4) ---- */
const ROW_CCS = [MoveRow1, MoveRow2, MoveRow3, MoveRow4];   /* Track buttons 1..4 */
const MENU_NAMES = ['Input FX', 'Global FX', 'Perform', 'Settings', 'Tape', 'Sessions'];
const PFX_NAMES = ['Off','Drive','Sweeten','Fuzz','Howl','Fold','Swell','Doubler','Vibrato','Phaser','Tremolo','Pitch','Shift',
                   'Cascade','Reels','Collage','Reverse','Space','Bloom','Filter','Squash','Cassette','Broken','Interference','Halo','Plate'];
const PREAMP_NAMES = ['Tapeless','Clean','Cass1','Cass2','VHS1','VHS2','Reel15','Reel7','Reel3','4trk','Porta','Dub','Warp'];
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
    [ /* Track 3 — Perform: Stumble master glitch + Dropout */
      { k:'stMix', lo:0, hi:1, lbl:'Stmb' }, { k:'stStep', lo:0, hi:1, lbl:'Step' },
      { k:'stOdds', lo:0, hi:1, lbl:'Odds' }, { k:'stSize', lo:0, hi:1, lbl:'Size' },
      { k:'stKind', opts:['Tumble','Stutter','Reverse','Tape','Gate','Crush'], lbl:'Kind' }, { k:'stReach', lo:0, hi:1, lbl:'Reach' },
      { k:'jump', trig:true, lbl:'Jump' }, { k:'scan', trig:true, lbl:'Scan' },
    ],
    [ /* Track 4 — Settings */
      { k:'masterVol', lo:0, hi:1.5, lbl:'Out' },     { k:'rootNote', lo:24, hi:96, lbl:'Root', int:true },
      { k:'overdubMode', opts:['Replace','Multiply','Disint'], lbl:'ODub' }, { k:'masterLoCut', lo:20, hi:500, lbl:'LoCut', int:true },
      { k:'masterHiCut', lo:1000, hi:20000, lbl:'HiCut', int:true }, { k:'globalSat', lo:0, hi:1, lbl:'gSat' },
      { k:'midiIn', opts:['Off','On'], lbl:'MIDI' },  { k:'armThresh', lo:0, hi:1, lbl:'ArmTh' },
    ],
    [ /* 4 — Tape (Capture button): the record-path tape machine, Magneto-style */
      { k:'preamp', opts:PREAMP_NAMES, lbl:'Tape' },  { k:'tapeDrive', lo:0, hi:1, lbl:'Drive' },
      { k:'tapeWow', lo:0, hi:1, lbl:'Wow' },         { k:'tapeFlut', lo:0, hi:1, lbl:'Flut' },
      { k:'tapeHF', lo:0, hi:1, lbl:'HF' },           { k:'tapeLoCut', lo:0, hi:1, lbl:'LoCut' },
      { k:'tapeNoise', lo:0, hi:1, lbl:'Hiss' },      { k:'tapeGen', lo:0, hi:1, lbl:'Gen' },
    ],
    [ /* 5 — Sessions (Rec button): slot select + save/load (worker thread does the disk I/O) */
      { k:'sessSlot', lo:1, hi:8, lbl:'Slot', int:true, local:true },
      { k:'sessSave', trig:true, lbl:'Save' },
      { k:'sessLoad', trig:true, lbl:'Load' },
    ],
];
let sessSlot = 1, sessLast = '';
let armThreshVal = 0.08;
let copyHeld = false, loopHeld = false, cloneSrc = -1;
const armedArr = new Array(NV).fill(false);
let blinkOn = false, resumeRepaint = 0;
let view = 'main', viewUntil = 0;          /* 'main' | 'knobs' | 'wave' */
const VIEW_TICKS = 85;                     /* ~5s before falling back to main */
let sampleHeld = false, jogHead = -1;      /* Shift+Sample+jog = arm threshold; P4 touch = head to move */
let waveStr = '', headsStr = '';
function showView(v) { view = v; viewUntil = tickCount + VIEW_TICKS; dirty = true; }
const LOOP_MULTS = [1.0, 0.5, 0.25, 0.125];
const loopMultIdx = new Array(NV).fill(0);
let menu = -1, menuReload = false;
const menuVals = [0,0,0,0,0,0,0,0];

let knobVals = new Array(8).fill(0);
let needReload = true;
let lastKnob = -1, lastKnobLbl = '', lastKnobVal = '';
let cpu = '0', loopLen = '0', inPeak = '0';

const PAGE0 = [   /* Loop page 1 (Up arrow) — knob 8 = Send A */
    { k: 'v_pitch', lo: -2, hi: 2, lbl: 'Pit', spd: true, step: 0.1 / 12 }, { k: 'v_filter', lo: 0, hi: 1, lbl: 'Fil', step: 0.002 },
    { k: 'v_pan', lo: -1, hi: 1, lbl: 'Pan' },   { k: 'v_volume', lo: 0, hi: 1, lbl: 'Vol' },
    { k: 'v_start', lo: 0, hi: 1, lbl: 'Srt' },  { k: 'v_end', lo: 0, hi: 1, lbl: 'End' },
    { k: 'v_reverse', lo: 0, hi: 1, lbl: 'Rev', e2: ['Nrm', 'Rev'] }, { k: 'v_sendA', lo: 0, hi: 1, lbl: 'SndA' },
];
const PAGE1 = [   /* Loop page 2 (Down arrow) — knobs 6/7 = Scatter/Seed, knob 8 = Send B */
    { k: 'clock', lo: 0, hi: 1, lbl: 'Clk', clk: true },  { k: 'v_djReso', lo: 0, hi: 1, lbl: 'Reso' },
    { k: 'v_sat', lo: 0, hi: 1, lbl: 'Sat' },             { k: 'masterComp', lo: 0, hi: 1, lbl: 'Cmp' },
    { k: 'v_wowflut', lo: 0, hi: 1, lbl: 'WF' },          { k: 'v_scatter', lo: 0, hi: 1, lbl: 'Scat' },
    { k: 'v_glitch', lo: 0, hi: 1, lbl: 'Seed' },         { k: 'v_sendB', lo: 0, hi: 1, lbl: 'SndB' },
];
const PAGE2 = [   /* Loop page 3 / Tone (Right arrow) — Studer EQ + DJ reso + amp envelope */
    { k: 'v_eqBass', lo: -1, hi: 1, lbl: 'Bass' },    { k: 'v_eqPresFrq', lo: 0, hi: 1, lbl: 'MidF' },
    { k: 'v_eqPresAmt', lo: -1, hi: 1, lbl: 'MidG' }, { k: 'v_eqTreble', lo: -1, hi: 1, lbl: 'Treb' },
    { k: 'v_tilt', lo: -1, hi: 1, lbl: 'Tilt' },      { k: '_heads', page: 3, lbl: 'Heads' },
    { k: 'v_atk', lo: 0, hi: 1, lbl: 'Atk' },         { k: 'v_rel', lo: 0, hi: 1, lbl: 'Dec' },
];
const HEAD_MODES = ['Off', 'Fwd', 'Bwd', 'Ping'];
const PAGE3 = [   /* Loop page 4 — Playheads: mode + speed per head (touch one, jog moves it) */
    { k: 'v_ph1mode', opts: HEAD_MODES, lbl: 'H1' },  { k: 'v_ph1spd', lo: 0, hi: 1, lbl: 'H1spd', clk: true },
    { k: 'v_ph2mode', opts: HEAD_MODES, lbl: 'H2' },  { k: 'v_ph2spd', lo: 0, hi: 1, lbl: 'H2spd', clk: true },
    { k: 'v_ph3mode', opts: HEAD_MODES, lbl: 'H3' },  { k: 'v_ph3spd', lo: 0, hi: 1, lbl: 'H3spd', clk: true },
    { k: 'v_ph4mode', opts: HEAD_MODES, lbl: 'H4' },  { k: 'v_ph4spd', lo: 0, hi: 1, lbl: 'H4spd', clk: true },
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
    paintNav();
    paintTrackLEDs();
}
/* Open (or toggle off) a menu by index; 0-3 are the track buttons, 4=Tape, 5=Sessions */
function openMenu(idx) {
    if (menu === idx) menu = -1;
    else { menu = idx; menuReload = true; setMsg(MENU_NAMES[idx]); showView('knobs'); }
    paintTrackLEDs(); paintNav(); dirty = true;
}
/* Nav buttons: arrows lit (current page's jump-arrow bright), Undo dim, Mute bright while held */
function paintNav() {
    setButtonLED(MoveDown,  loopPage < NPAGES - 1 ? WhiteLedBright : WhiteLedDim, true);
    setButtonLED(MoveUp,    loopPage > 0 ? WhiteLedBright : WhiteLedDim, true);
    setButtonLED(MoveUndo,  WhiteLedDim, true);
    setButtonLED(MoveMute,  muteHeld ? WhiteLedBright : WhiteLedDim, true);
    setButtonLED(MoveCapture, menu === 4 ? WhiteLedBright : WhiteLedDim, true);
    setButtonLED(MoveSample,  menu === 5 ? WhiteLedBright : WhiteLedDim, true);
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
        const r = gp(defs[i].k);
        if (defs[i].e2) knobVals[i] = (r === 'Reverse') ? 1 : 0;
        else { const f = parseFloat(r); knobVals[i] = isNaN(f) ? defs[i].lo : f; }
    }
    needReload = false;
}

/* ---- menu helpers ---- */
function reloadMenu() {
    const defs = MENU_DEFS[menu]; if (!defs) return;
    for (let i = 0; i < 8; i++) {
        const d = defs[i]; if (!d) { menuVals[i] = 0; continue; }
        if (d.local) { menuVals[i] = sessSlot; continue; }
        const r = gp(d.k);
        if (d.opts) { let idx = d.opts.indexOf(r); if (idx < 0) idx = parseInt(r) || 0; menuVals[i] = Math.max(0, Math.min(d.opts.length - 1, idx)); }
        else { const f = parseFloat(r); menuVals[i] = isNaN(f) ? (d.lo || 0) : f; }
    }
    menuReload = false;
}
function menuKnob(k, delta) {
    const d = MENU_DEFS[menu][k]; if (!d) return;
    if (d.local) {                    /* UI-local (session slot) */
        const dir = delta > 0 ? 1 : (delta < 0 ? -1 : 0);
        const nv = Math.max(d.lo, Math.min(d.hi, Math.round(menuVals[k]) + dir));
        menuVals[k] = nv; sessSlot = nv; lastKnob = k; lastKnobLbl = d.lbl; lastKnobVal = String(nv); return;
    }
    if (d.trig) {
        if (delta !== 0) {
            if (d.k === 'sessSave')      { sp('session', 'save:' + sessSlot); setMsg('Saving slot ' + sessSlot); }
            else if (d.k === 'sessLoad') { sp('session', 'load:' + sessSlot); setMsg('Loading slot ' + sessSlot); }
            else sp(d.k, '1');
            lastKnob = k; lastKnobLbl = d.lbl; lastKnobVal = 'fire';
        }
        return;
    }
    if (d.opts) {
        const dir = delta > 0 ? 1 : (delta < 0 ? -1 : 0);
        let idx = Math.max(0, Math.min(d.opts.length - 1, Math.round(menuVals[k]) + dir));
        menuVals[k] = idx; sp(d.k, d.opts[idx]); lastKnobVal = d.opts[idx];
    } else if (d.int) {
        const step = Math.max(1, Math.round((d.hi - d.lo) * 0.02));
        const nv = Math.max(d.lo, Math.min(d.hi, Math.round(menuVals[k] + delta * step)));
        menuVals[k] = nv; sp(d.k, String(nv)); lastKnobVal = String(nv);
    } else {
        const step = (d.hi - d.lo) * 0.02, c = (d.lo + d.hi) / 2;
        let nv = c + Math.round((menuVals[k] + delta * step - c) / step) * step; nv = clampf(nv, d.lo, d.hi);
        menuVals[k] = nv; sp(d.k, nv.toFixed(4)); lastKnobVal = nv.toFixed(2);
    }
    lastKnob = k; lastKnobLbl = d.lbl;
}
function paintTrackLEDs() { for (let i = 0; i < 4; i++) setButtonLED(ROW_CCS[i], (menu === i) ? WhiteLedBright : WhiteLedDim); }

/* capacitive knob touch (notes 0-7 = E1-E8): show the param it affects, without changing it */
function handleKnobTouch(d1) {
    const k = d1; if (k < 0 || k > 7) return;
    if (menu >= 0 && MENU_DEFS[menu]) {
        const d = MENU_DEFS[menu][k]; if (!d) return; lastKnobLbl = d.lbl;
        lastKnobVal = d.local ? String(sessSlot) : d.trig ? '(fire)' : (d.opts ? d.opts[Math.round(menuVals[k])] : (d.int ? String(Math.round(menuVals[k])) : Number(menuVals[k]).toFixed(2)));
    } else if (punchMode && k >= 4 && punchActive >= 0) {
        const j = k - 4; lastKnobLbl = PUNCH_PARAMS[punchActive][j]; lastKnobVal = punchVals[punchActive][j].toFixed(2);
    } else if (menu < 0 && !punchMode) {
        const d = PAGES[page()][k]; lastKnobLbl = d.lbl;
        lastKnobVal = d.e2 ? d.e2[knobVals[k] > 0.5 ? 1 : 0] : (d.spd ? Math.pow(2, knobVals[k]).toFixed(2) + 'x' : (d.clk ? (0.25 * Math.pow(16, knobVals[k])).toFixed(2) + 'x' : Number(knobVals[k]).toFixed(2)));
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
}

/* ---- screen ---- */
/* Full 8-knob page, Schwung-style: 2 rows x 4 cells, label over value. */
function drawKnobView() {
    clear_screen();
    const defs = (menu >= 0) ? MENU_DEFS[menu] : PAGES[page()];
    const title = (menu >= 0) ? MENU_NAMES[menu] : ('T' + (sel + 1) + '  P' + (page() + 1));
    print(0, 0, title, 1);
    draw_line(0, 9, SCREEN_W, 9, 1);
    for (let i = 0; i < 8; i++) {
        const d = defs[i]; if (!d) continue;
        const col = i % 4, row = Math.floor(i / 4);
        const x = col * 32, y = 13 + row * 26;
        print(x, y, d.lbl.substring(0, 5), 1);
        let val;
        if (d.page !== undefined) val = '>';
        else if (d.local) val = String(sessSlot);
        else if (d.trig) val = '--';
        else if (d.opts) val = String(d.opts[Math.round((menu >= 0 ? menuVals[i] : knobVals[i]))] || '').substring(0, 5);
        else {
            const raw = (menu >= 0) ? menuVals[i] : knobVals[i];
            if (d.e2) val = d.e2[raw > 0.5 ? 1 : 0];
            else if (d.spd) val = Math.pow(2, raw).toFixed(2) + 'x';
            else if (d.clk) val = (0.25 * Math.pow(16, raw)).toFixed(2) + 'x';
            else if (d.int) val = String(Math.round(raw));
            else val = Number(raw).toFixed(2);
        }
        print(x, y + 10, String(val).substring(0, 6), 1);
        if (i === lastKnob) fill_rect(x, y + 20, 28, 1, 1);
    }
    host_flush_display();
}

/* Loop waveform with the active playheads riding over it. */
function drawWaveView() {
    clear_screen();
    print(0, 0, 'T' + (sel + 1) + '  ' + STATE_NAMES[voiceState[sel]] + '  ' + loopLen + 's', 1);
    draw_line(0, 9, SCREEN_W, 9, 1);
    const midY = 34, halfH = 20;
    if (waveStr && waveStr.length >= 64) {
        for (let b = 0; b < 64; b++) {
            const lv = waveStr.charCodeAt(b) - 48;          /* 0..15 */
            const h = Math.max(1, Math.round((lv / 15) * halfH));
            fill_rect(b * 2, midY - h, 2, h * 2, 1);
        }
    } else {
        print(0, 30, '(empty loop)', 1);
    }
    /* playheads: vertical lines, numbered */
    if (headsStr) {
        const parts = headsStr.split(';');
        for (let k = 0; k < parts.length && k < 4; k++) {
            const kv = parts[k].split(',');
            const mode = parseInt(kv[0]) || 0; if (mode === 0) continue;
            const pos = parseInt(kv[1]) || 0;
            const x = Math.min(127, Math.round((pos / 999) * 127));
            fill_rect(x, 12, 1, 44, 1);
            print(Math.min(122, x), 57, String(k + 1), 1);
        }
    }
    host_flush_display();
}

function drawUI() {
    if (view === 'knobs') { drawKnobView(); return; }
    if (view === 'wave')  { drawWaveView(); return; }
    clear_screen();
    print(0, 0, 'LB T' + (sel + 1) + ' ' + STATE_NAMES[voiceState[sel]] + '  C' + cpu + '%', 1);
    draw_line(0, 9, SCREEN_W, 9, 1);
    for (let i = 0; i < NV; i++) {                 /* 16-track strip */
        const st = voiceState[i], x = i * 8;
        const h = (st === 0) ? 1 : (st === 3 ? 3 : 6);
        fill_rect(x, 11, 6, h, 1);
        if (i === sel) fill_rect(x, 19, 6, 1, 1);   /* selected underline */
    }
    draw_line(0, 23, SCREEN_W, 23, 1);
    if (menu >= 0) print(0, 27, MENU_NAMES[menu] + (lastKnob < 0 ? '' : '  ' + lastKnobLbl + ' ' + lastKnobVal), 1);
    else if (punchMode) print(0, 27, 'PUNCH ' + PUNCH_NAMES[punchActive] + (lastKnob < 0 ? '' : '  ' + lastKnobLbl + ' ' + lastKnobVal), 1);
    else print(0, 27, 'P' + (page() + 1) + (lastKnob < 0 ? '  turn a knob' : '  E' + (lastKnob + 1) + ' ' + lastKnobLbl + ' ' + lastKnobVal), 1);
    print(0, 39, 'Loop ' + loopLen + 's   In ' + inPeak, 1);
    if (tickCount < statusMsgUntil) print(0, 54, statusMsg, 1);
    else print(0, 54, (muteHeld ? 'MUTE+pad  ' : 'hold=clr Undo ') + 'Sh+FX=latch', 1);
    host_flush_display();
}

/* ---- lifecycle ---- */
globalThis.init = function () {
    for (let i = 0; i < NV; i++) voiceState[i] = 0;
    pollStates();
    reloadKnobs();
    paintAll(true);
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
    if (menu === 5 && tickCount % 4 === 0) {
        const st = gp('sessStatus');
        if (st && st !== sessLast) { sessLast = st; if (st === 'OK') { needReload = true; menuReload = true; }
            if (st) setMsg('Slot ' + sessSlot + ' ' + st); }
    }
    if (resumeRepaint > 0) { resumeRepaint--; paintAll(true); }   /* force LEDs back after resume */
    if (tickCount % 5 === 0) {                                   /* blink driver for armed / clone-src */
        blinkOn = !blinkOn;
        for (let i = 0; i < NV; i++) if (armedArr[i] || i === cloneSrc) setLED(LEFT_NOTES[i], padColor(i), true);
    }
    if (tickCount % 10 === 4) {                                  /* armed state from the DSP */
        const a = gp('armed');
        if (a && a.length >= NV) for (let i = 0; i < NV; i++) {
            const on = a.charAt(i) === '1';
            if (on !== armedArr[i]) { armedArr[i] = on; setLED(LEFT_NOTES[i], padColor(i), true); }
        }
    }
    if (view !== 'main' && tickCount >= viewUntil) { view = 'main'; dirty = true; }
    if (view === 'wave') {
        if (tickCount % 12 === 0) { const w = gp('wave'); if (w) waveStr = w; }
        const h = gp('heads'); if (h) headsStr = h;
    }
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
        if (d1 === MoveBack && d2 > 0) { if (menu >= 0) { menu = -1; paintTrackLEDs(); return; } clearAllLEDs(); host_exit_module(); return; }
        if (d1 === MoveShift) { shiftHeld = d2 > 0; return; }
        if (d1 === MoveMute)  { muteHeld = d2 > 0; paintNav(); return; }     /* Mute modifier (lights the button) */
        if (d1 === MoveCapture && d2 > 0) { openMenu(4); return; }           /* Capture = Tape menu */
        if (d1 === MoveSample) { sampleHeld = d2 > 0; paintNav(); if (d2 === 0) return; }
        if (d1 === MoveSample  && d2 > 0) {                                  /* Sample/Record button */
            if (shiftHeld) { spCmd('arm:' + sel); armedArr[sel] = !armedArr[sel];
                setMsg('T' + (sel + 1) + (armedArr[sel] ? ' ARMED' : ' disarmed')); return; }
            openMenu(5); return;                                             /* Sessions menu */
        }
        if (d1 === MoveCopy) { copyHeld = d2 > 0; if (!copyHeld) cloneSrc = -1; paintNav(); return; }
        if (d1 === MoveLoop) { loopHeld = d2 > 0; paintNav(); return; }
        if (d1 === MoveMainKnob) {
            const dv = decodeDelta(d2); if (dv === 0) return;
            if (shiftHeld && sampleHeld) {                                   /* arm threshold */
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
        if (d1 === MoveDown  && d2 > 0) { setPage(loopPage + 1); return; }   /* next loop page */
        if (d1 === MoveUp    && d2 > 0) { setPage(loopPage - 1); return; }   /* prev loop page */
        if (d1 === MoveUndo  && d2 > 0) {                                    /* Undo the last clear */
            if (lastCleared >= 0) { spCmd('unclr:' + lastCleared); voiceState[lastCleared] = 2;
                enqLED(LEFT_NOTES[lastCleared], padColor(lastCleared)); setMsg('T' + (lastCleared + 1) + ' restored'); lastCleared = -1; }
            else setMsg('nothing to undo');
            return;
        }
        const rowIdx = ROW_CCS.indexOf(d1);
        if (rowIdx >= 0) {                          /* track buttons = menus */
            if (d2 > 0) {
                if (menu === rowIdx) menu = -1;
                else if (!MENU_DEFS[rowIdx]) { setMsg(MENU_NAMES[rowIdx] + ': soon'); menu = -1; }
                else { menu = rowIdx; menuReload = true; setMsg(MENU_NAMES[rowIdx]); }
                paintTrackLEDs();
            }
            return;
        }
        const k = d1 - MoveKnob1;
        if (k >= 0 && k < 8) {
            if (menu >= 0 && MENU_DEFS[menu]) { menuKnob(k, decodeDelta(d2)); return; }
            if (punchMode) {                       /* knobs 5-8 (above the right pads) control the held effect */
                if (k >= 4 && punchActive >= 0) {
                    const j = k - 4;
                    let nv = punchVals[punchActive][j] + decodeDelta(d2) * 0.02;
                    nv = 0.5 + Math.round((nv - 0.5) / 0.02) * 0.02;   /* grid aligned to centre */
                    nv = clampf(nv, 0, 1);
                    punchVals[punchActive][j] = nv; sp('pfx', punchActive + ':' + j + ':' + nv.toFixed(4));
                    lastKnob = k; lastKnobLbl = PUNCH_PARAMS[punchActive][j]; lastKnobVal = nv.toFixed(2);
                }
                return;
            }
            const def = PAGES[page()][k];
            if (def.page !== undefined) { if (decodeDelta(d2) !== 0) setPage(def.page); return; }
            const step = (def.step !== undefined) ? def.step : (def.hi - def.lo) * 0.02;
            const center = (def.lo + def.hi) / 2;
            let nv = knobVals[k] + decodeDelta(d2) * step;
            nv = center + Math.round((nv - center) / step) * step;   /* grid aligned to centre -> exact 0.5 / 1.0x */
            nv = clampf(nv, def.lo, def.hi);
            knobVals[k] = nv;
            if (def.e2) { sp(def.k, nv > 0.5 ? '1' : '0'); lastKnobVal = def.e2[nv > 0.5 ? 1 : 0]; }
            else { sp(def.k, nv.toFixed(4)); lastKnobVal = def.spd ? Math.pow(2, nv).toFixed(2) + 'x' : (def.clk ? (0.25 * Math.pow(16, nv)).toFixed(2) + 'x' : nv.toFixed(2)); }
            lastKnob = k; lastKnobLbl = def.lbl;
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
            if (shiftHeld) { cycleSpeed(i); return; }   /* Shift+tap = cycle speed */
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
        if (d1 in STEP_TO_TRACK) {
            const t = STEP_TO_TRACK[d1];
            if (menu >= 0) { menu = -1; paintTrackLEDs(); paintNav(); }
            if (t === sel) setPage((loopPage + 1) % NPAGES);   /* same step again = next loop page */
            else { selectTrack(t); showView('wave'); }
            return;
        }
        if (d1 in NOTE_TO_RIGHT) {                  /* punch-in FX: hold to apply, knobs edit it */
            const i = NOTE_TO_RIGHT[d1];
            if (shiftHeld && punchLatched[i]) {     /* Shift+pad on a latched effect = unlatch (stop) */
                punchLatched[i] = false;
                const hi0 = heldPunch.indexOf(i); if (hi0 >= 0) heldPunch.splice(hi0, 1);
                sp('punch', 'off:' + i); sp('punchPress', i + ':0');
                if (heldPunch.length) punchActive = heldPunch[heldPunch.length - 1];
                else { punchActive = -1; punchMode = false; needReload = true; }
                enqLED(RIGHT_NOTES[i], rightColor(i)); setMsg('Unlatch ' + PUNCH_NAMES[i]); return;
            }
            if (shiftHeld) punchLatched[i] = true;  /* Shift+pad = latch on (stays after release) */
            if (heldPunch.indexOf(i) < 0 && heldPunch.length < 4) heldPunch.push(i);
            punchActive = i; punchMode = true;
            for (let j = 0; j < 4; j++) sp('pfx', i + ':' + j + ':' + punchVals[i][j].toFixed(4)); /* push this pad's params */
            sp('punch', 'on:' + i);
            enqLED(RIGHT_NOTES[i], rightColor(i));
            setMsg((punchLatched[i] ? 'Latch ' : 'Punch ') + PUNCH_NAMES[i]);
            return;
        }
        return;
    }

    if (status === 0xa0) {                          /* pad pressure -> punch intensity (per held effect) */
        if (d1 in NOTE_TO_RIGHT) { const i = NOTE_TO_RIGHT[d1]; if (heldPunch.indexOf(i) >= 0) sp('punchPress', i + ':' + (d2 / 127).toFixed(3)); }
        return;
    }

    if (status === 0x80 || (status === 0x90 && d2 === 0)) {   /* note-off */
        if (d1 in NOTE_TO_LEFT) {
            const i = NOTE_TO_LEFT[d1];
            if (mutePressed[i]) { mutePressed[i] = false; return; }   /* release of a Mute+tap — never clears */
            if (now() - pressMs[i] >= CLEAR_HOLD_MS) {         /* long-press = clear loop */
                spCmd('clear:' + i);
                voiceState[i] = 0; lastTapMs[i] = 0; mutes[i] = false; lastCleared = i;
                enqLED(LEFT_NOTES[i], padColor(i));
                setMsg('T' + (i + 1) + ' cleared (Undo)');
            }
            return;
        }
        if (d1 in NOTE_TO_RIGHT) {                  /* release punch effect */
            const i = NOTE_TO_RIGHT[d1];
            if (punchLatched[i]) return;            /* latched: ignore release, keep running */
            const hi = heldPunch.indexOf(i); if (hi >= 0) heldPunch.splice(hi, 1);
            sp('punch', 'off:' + i); sp('punchPress', i + ':0');
            if (heldPunch.length) punchActive = heldPunch[heldPunch.length - 1];
            else { punchActive = -1; punchMode = false; needReload = true; }
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

globalThis.onMidiMessageExternal = function (data) { /* LaunchControl XL — next wave */ };
