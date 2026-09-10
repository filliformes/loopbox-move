/*
 * loopbox.c — LoopBox v0.2.1 for Ableton Move (Schwung)
 *
 * 16-track asynchronous STEREO tape looper.
 * Inspired by: 1010music BlackBox, Kinotone Ribbons, Puremagnetik LAPS,
 *   Chase Bliss Blooper, Mood MK2, Generation Loss MK2.
 *
 * Per-voice DSP chain:
 *   Variable-Rate Pitch -> DJ Filter (Isolator3) -> Saturation (tube) ->
 *   Wow/Flutter (Flutter2) -> Glitch/Beat-Repeat (+ random oct + bitcrush) ->
 *   Tilt EQ (Tonelux) -> Studer 962 EQ -> Pan/Vol -> Send
 *
 * Shared DSP:
 *   Input: Preamp (12 models) -> [stereo rec buf]
 *   Global Saturation (IronOxide reel-to-reel preamp)
 *   Clock: Global speed x0.25-x2.0 with SR degradation + aliasing noise (Mood mk2)
 *   Stability: Cumulative tape degradation (Blooper)
 *   Send Bus -> Tape Delay (100% wet) + Plate Reverb (100% wet)
 *   Master: Compressor -> Lo/Hi Cut -> Soft Limiter -> Output
 *
 * Overdub modes: Replace / Multiply / Disintegration
 * MIDI: Move pads (internal, notes 36-51) + LaunchControl XL (external, hard-coded CCs)
 *
 * License: GPL-3.0
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <dirent.h>
#include <time.h>
#include "plugin_api_v1.h"
#include "palette_fx.h"   /* 24-effect Palette engine for the send buses */

static const host_api_v1_t *g_host = NULL;

#define SR              44100.0
#define TWOPI           (2.0 * M_PI)
#define NUM_VOICES      16
#define LOOP_SECONDS    45
#define LOOP_SAMPLES    ((int)(SR * LOOP_SECONDS))
#define FLUTTER_BUF     1024
#define DELAY_BUF       88200
#define DPRESS_FRAMES   ((int)(SR * 0.4))
#define SMP_MAX_DIRS    32
#define SMP_MAX_FILES   64
#define SMP_NAME_LEN    48
#define SMP_PATH_LEN    256
#define SAMPLES_ROOT    "/data/UserData/UserLibrary/Samples"
#define PRV_EA 61
#define PRV_EB 499
#define PRV_EC 107
#define PRV_ED 127
#define PRV_EE 607
#define PRV_EF 313
#define PRV_A 631
#define PRV_B 281
#define PRV_C 97
#define PRV_D 709
#define PRV_E 307
#define PRV_F 149
#define PRV_G 313
#define PRV_H 37
#define PRV_I 659
#define PRV_J 701
#define PRV_K 733
#define PRV_L 787
#define PRV_PRE 8820

/* ---- Utilities ---- */
static inline float lb_clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
static inline double lb_clampd(double x, double lo, double hi) { return x < lo ? lo : (x > hi ? hi : x); }
static inline double lb_tanh(double x) {
    if (x > 3.0) return 1.0; if (x < -3.0) return -1.0;
    double x2 = x * x; return x * (27.0 + x2) / (27.0 + 9.0 * x2);
}
static inline double lb_rand(uint32_t *rng) {
    *rng = 214013u * (*rng) + 2531011u;
    return ((double)((*rng >> 16) & 0x7FFF) / 32768.0) * 2.0 - 1.0;
}

/* ---- Biquad ---- */
typedef struct { double b0, b1, b2, a1, a2, z1L, z2L, z1R, z2R; } Biquad;
static void bq_reset(Biquad *f) { memset(f, 0, sizeof(Biquad)); }
static void bq_set_lp(Biquad *f, double freq, double Q) {
    double w0=TWOPI*freq/SR,cosW=cos(w0),alpha=sin(w0)/(2.0*Q),a0=1.0+alpha;
    f->b0=(1.0-cosW)/2.0/a0;f->b1=(1.0-cosW)/a0;f->b2=f->b0;f->a1=(-2.0*cosW)/a0;f->a2=(1.0-alpha)/a0;
}
static void bq_set_hp(Biquad *f, double freq, double Q) {
    double w0=TWOPI*freq/SR,cosW=cos(w0),alpha=sin(w0)/(2.0*Q),a0=1.0+alpha;
    f->b0=(1.0+cosW)/2.0/a0;f->b1=-(1.0+cosW)/a0;f->b2=f->b0;f->a1=(-2.0*cosW)/a0;f->a2=(1.0-alpha)/a0;
}
static void bq_set_lowshelf(Biquad *f, double freq, double db, double S) {
    double A=pow(10.0,db/40.0),w0=TWOPI*freq/SR,cosW=cos(w0),sinW=sin(w0);
    double al=sinW/2.0*sqrt((A+1.0/A)*(1.0/S-1.0)+2.0),sqA=2.0*sqrt(A)*al;
    double a0=(A+1.0)+(A-1.0)*cosW+sqA;
    f->b0=(A*((A+1.0)-(A-1.0)*cosW+sqA))/a0;f->b1=(2.0*A*((A-1.0)-(A+1.0)*cosW))/a0;
    f->b2=(A*((A+1.0)-(A-1.0)*cosW-sqA))/a0;f->a1=(-2.0*((A-1.0)+(A+1.0)*cosW))/a0;
    f->a2=((A+1.0)+(A-1.0)*cosW-sqA)/a0;
}
static void bq_set_highshelf(Biquad *f, double freq, double db, double S) {
    double A=pow(10.0,db/40.0),w0=TWOPI*freq/SR,cosW=cos(w0),sinW=sin(w0);
    double al=sinW/2.0*sqrt((A+1.0/A)*(1.0/S-1.0)+2.0),sqA=2.0*sqrt(A)*al;
    double a0=(A+1.0)-(A-1.0)*cosW+sqA;
    f->b0=(A*((A+1.0)+(A-1.0)*cosW+sqA))/a0;f->b1=(-2.0*A*((A-1.0)+(A+1.0)*cosW))/a0;
    f->b2=(A*((A+1.0)+(A-1.0)*cosW-sqA))/a0;f->a1=(2.0*((A-1.0)-(A+1.0)*cosW))/a0;
    f->a2=((A+1.0)-(A-1.0)*cosW-sqA)/a0;
}
static void bq_set_peak(Biquad *f, double freq, double db, double Q) {
    double A=pow(10.0,db/40.0),w0=TWOPI*freq/SR,cosW=cos(w0),alpha=sin(w0)/(2.0*Q);
    double a0=1.0+alpha/A;
    f->b0=(1.0+alpha*A)/a0;f->b1=(-2.0*cosW)/a0;f->b2=(1.0-alpha*A)/a0;f->a1=f->b1;f->a2=(1.0-alpha/A)/a0;
}
static void bq_set_bp(Biquad *f, double freq, double Q) {
    double w0=TWOPI*freq/SR,alpha=sin(w0)/(2.0*Q),a0=1.0+alpha;
    f->b0=alpha/a0;f->b1=0.0;f->b2=-alpha/a0;f->a1=(-2.0*cos(w0))/a0;f->a2=(1.0-alpha)/a0;
}
static inline double bq_tick(Biquad *f, double x, double *z1, double *z2) {
    double out=f->b0*x+*z1;*z1=f->b1*x-f->a1*out+*z2;*z2=f->b2*x-f->a2*out;return out;
}
#define bq_L(f,x) bq_tick(f,x,&(f)->z1L,&(f)->z2L)
#define bq_R(f,x) bq_tick(f,x,&(f)->z1R,&(f)->z2R)
/* copy filter coefficients only (leaves the destination's own z-state intact) */
static inline void bq_copy_coeffs(Biquad *d, const Biquad *s){ d->b0=s->b0; d->b1=s->b1; d->b2=s->b2; d->a1=s->a1; d->a2=s->a2; }

/* ---- Tape saturation (IronOxide-inspired) ---- */
static inline double tape_sat(double x) {
    double ax=fabs(x);if(ax<0.0001)return x;double mojo=pow(ax,0.25);return sin(x*mojo*1.5707963)/mojo;
}

/* ---- Preamp models (12 types) ---- */
static inline void apply_preamp_sample(double *l, double *r, int model, double *casLpL, double *casLpR, uint32_t *rng) {
    double noise=lb_rand(rng)*0.008;
    switch(model){
    case 0:*l+=noise*0.2;*r+=noise*0.2;break;
    case 1:{*l=lb_tanh(*l*1.8)+noise;*r=lb_tanh(*r*1.8)+noise;double k=0.45;*casLpL+=k*(*l-*casLpL);*l=*casLpL;*casLpR+=k*(*r-*casLpR);*r=*casLpR;break;}
    case 2:{*l=atan(*l*1.6)*0.6366+noise;*r=atan(*r*1.6)*0.6366+noise;double k=0.38;*casLpL+=k*(*l-*casLpL);*l=*casLpL;*casLpR+=k*(*r-*casLpR);*r=*casLpR;break;}
    case 3:{*l=lb_tanh(*l*1.3)+noise*1.5;*r=lb_tanh(*r*1.3)+noise*1.5;double k=0.55;*casLpL+=k*(*l-*casLpL);*l=*casLpL;*casLpR+=k*(*r-*casLpR);*r=*casLpR;break;}
    case 4:{*l=tape_sat(*l*1.1)+noise*0.5;*r=tape_sat(*r*1.1)+noise*0.5;double k=0.3;*casLpL+=k*(*l-*casLpL);*l=*casLpL;*casLpR+=k*(*r-*casLpR);*r=*casLpR;break;}
    case 5:*l=tape_sat(*l)+noise*0.15;*r=tape_sat(*r)+noise*0.15;break;
    case 6:{*l=tape_sat(*l*1.15)+noise*0.3;*r=tape_sat(*r*1.15)+noise*0.3;double k=0.25;*casLpL+=k*(*l-*casLpL);*l=*casLpL;*casLpR+=k*(*r-*casLpR);*r=*casLpR;break;}
    case 7:{*l=sin(lb_clampd(*l*1.5,-1.5,1.5))+noise*0.5;*r=sin(lb_clampd(*r*1.5,-1.5,1.5))+noise*0.5;double k=0.5;*casLpL+=k*(*l-*casLpL);*l=*casLpL;*casLpR+=k*(*r-*casLpR);*r=*casLpR;break;}
    case 8:{*l=lb_tanh(*l*1.6)+noise*0.6;*r=lb_tanh(*r*1.6)+noise*0.6;double k=0.42;*casLpL+=k*(*l-*casLpL);*l=*casLpL;*casLpR+=k*(*r-*casLpR);*r=*casLpR;break;}
    case 9:{*l=sin(lb_clampd(*l*1.8,-1.5,1.5))+noise*0.8;*r=sin(lb_clampd(*r*1.8,-1.5,1.5))+noise*0.8;double k=0.48;*casLpL+=k*(*l-*casLpL);*l=*casLpL;*casLpR+=k*(*r-*casLpR);*r=*casLpR;break;}
    case 10:{*l=lb_tanh(*l*2.2)*0.85+noise*0.4;*r=lb_tanh(*r*2.2)*0.85+noise*0.4;double k=0.52;*casLpL+=k*(*l-*casLpL);*l=*casLpL;*casLpR+=k*(*r-*casLpR);*r=*casLpR;break;}
    case 11:{double al=fabs(*l),ar=fabs(*r);double spL=(al>0.001)?sin(*l*al)/al:*l;double spR=(ar>0.001)?sin(*r*ar)/ar:*r;
        *l=lb_tanh(spL*0.8+sin(*l)*0.5)+noise;*r=lb_tanh(spR*0.8+sin(*r)*0.5)+noise;break;}
    default:break;}
}

/* ---- Global effects ---- */
static inline double global_saturate(double x, double amt) {
    if(amt<0.005)return x;double gain=1.0+amt*4.0;x*=gain;
    double br=fabs(x);if(br>1.5707963)br=1.5707963;br=sin(br);x=(x>0.0)?br:-br;
    double bump=sin(x*0.3)*amt*0.08;x=(x+bump)/(gain*0.45+0.55);return x;
}
/* clock 0.5 = 1.0x (normal), 0 = 0.25x, 1 = 4.0x — so loops default to normal speed */
static inline double clock_to_speed(float clock) { return 0.25*pow(16.0,(double)clock); }
static inline double apply_stability(double x, double amt, uint32_t *rng) {
    if(amt<0.005)return x;x=lb_tanh(x*(1.0+amt*0.3));x+=lb_rand(rng)*amt*0.015;return x;
}

typedef enum { OD_REPLACE=0, OD_MULTIPLY=1, OD_DISINTEGRATION=2 } OverdubMode;
typedef enum { VS_EMPTY=0, VS_RECORDING, VS_PLAYING, VS_PAUSED, VS_OVERDUBBING } VoiceState;

/* ---- Voice (stereo buffers) ---- */
typedef struct {
    int16_t *bufferL, *bufferR;
    int loopLen, playHead, recHead;
    VoiceState state; int pushCount;
    float loopStart, loopEnd, reverse;
    float saturation, wowFlutter, send, glitch, tiltEQ;   /* 'send' = Send A */
    float sendB, scatter; int scatterCnt;                 /* Send B + per-voice Scatter */
    float eqBass, eqPresFreq, eqPresAmt, eqTreble;
    float pitch, filter, pan, volume, decay;
    Biquad djLpA, djLpB, djLpC, djHpA, djHpB, djHpC;
    Biquad eqLow, eqMid, eqHigh, tiltLo, tiltHi;
    double flutBufL[FLUTTER_BUF], flutBufR[FLUTTER_BUF];
    int flutWr; double flutSweep, flutNextMax;
    int glN, glOrder[16], glRev[16], glLastSlice; double glPrevAbs; float glKnobCache;  /* Seed slice-reorder */
    double playPhase, stabLpStateL, stabLpStateR;
    uint32_t rng, lastPadFrame;
    int smpDir, smpFile;
    int djMode;   /* -1 = LP active, +1 = HP active, 0 = bypass (transparent at centre) */
    double playEnv; /* click-free start/stop envelope (ramps 0<->1 on play/pause) */
    double scatXfadePhase; int scatXfade; /* scatter jump crossfade (declick): old read head + countdown */
    int muted;                            /* quick mute: gate output, playhead keeps running */
    int savedLoopLen;                     /* clear-undo: last loop length before a clear */
    float djReso;                         /* DJ filter resonance (Q) */
    float ampAtk, ampRel;                 /* amp envelope attack/release times (0..1) */
    float ampAtkK, ampRelK, ampAtkCache, ampRelCache;  /* cached one-pole coeffs */
} Voice;

/* (Tape Delay, Plate Reverb and Chorus removed from the core — the two send buses
 * now run through the Palette engine; the Plate lives there as PFX "Plate".) */

/* ---- Polyphonic MIDI-keyboard voice: plays a loop's buffer pitched ---- */
#define POLY_VOICES 8
typedef struct {
    int active, releasing, note, loopIdx;
    double phase, rate, env; float vel;
    Biquad djA, eqLow, eqMid, eqHigh, tiltLo, tiltHi;  /* own state, coeffs copied from the loop */
    int djMode;
} PolyVoice;

/* ---- Punch-in FX (master insert): 16 pad effects reading a 2s capture ring ---- */
#define PUNCH_BUF 88200
#define NUM_PUNCH 16
enum { PM_REPEAT=0, PM_PITCH, PM_REVERSE, PM_SCRATCH, PM_CRUSH, PM_HAZE, PM_SHIMMER, PM_STRETCH, PM_SAT, PM_STUTTER, PM_NONE };
#define SHBUF 8192
typedef struct { int mech; double param; } PunchDef;   /* param = division (REPEAT/STUTTER) or ratio/default */
static const PunchDef PUNCH_DEFS[NUM_PUNCH] = {   /* right 4x4, top->bottom, grouped by family */
    {PM_REPEAT,4},{PM_REPEAT,3},{PM_REPEAT,8},{PM_REPEAT,16},        /* Loops:   Loop16 Loop12 LoopSh LoopSr */
    {PM_STUTTER,4},{PM_STUTTER,3},{PM_STUTTER,16},{PM_STUTTER,6},    /* Stutter: Stut4 Stut3 Retrig Q6/8 */
    {PM_PITCH,2.0},{PM_PITCH,0.5},{PM_HAZE,0.0},{PM_SHIMMER,0.0},    /* Pitch/tex: Oct+ Oct- Haze Shimmer */
    {PM_STRETCH,0.5},{PM_STRETCH,1.0},{PM_REVERSE,1.0},{PM_SAT,0.0}  /* Time/char: Stretch Freeze Reverse Sat */
};
/* One active punch slot: its own capture ring + running state (up to 4 in series). */
typedef struct {
    int idx;                                   /* effect 0..15, or -1 = empty */
    double env; int releasing;                 /* click-free fade-in / fade-out on press/release */
    float ringL[PUNCH_BUF], ringR[PUNCH_BUF]; int w;
    double readPhase, sliceStart, sliceLen;
    double scratchPos, scratchDir, scratchLo, scratchHi;
    int crushCnt; double crushL, crushR;
    Biquad toneFilt;
    /* granular (Haze + Stretch share this grain pool) */
    double gPos[4],gAge[4],gDur[4],gRate[4],gGl[4],gGr[4]; int gAct[4];
    double gSched, stGrid; uint32_t gRng;
    /* shimmer 2-head pitch-shift + LP feedback */
    float shL[SHBUF], shR[SHBUF]; int shW; double shR1, shFbL, shFbR;
} PunchSlot;

typedef struct {
    float globalSat,masterComp,masterLoCut,masterHiCut,clock,masterVol;
    float preamp,overdubMode,stability;int selTrack;
    float globalWowFlut,inputMonitor,inputGain;
    double inputPeakL,inputPeakR;
    Voice voice[NUM_VOICES];
    Biquad masterLo,masterHi;
    /* Global FX: two selectable send buses (0=Delay 1=Reverb 2=Chorus) + Macro1/Macro2/Drift */
    int sendAType,sendBType; float sendAM1,sendAM2,sendADrift,sendBM1,sendBM2,sendBDrift;
    /* Perform: Stumble (master stochastic glitch, Forgetful-style) + Dropout */
    float stRingL[PUNCH_BUF],stRingR[PUNCH_BUF]; int stW;
    float stMix,stStep,stOdds,stSize,stReach; int stKind;
    int stStepLeft,stStepLen,stStepPos,stActive,stEffect,stGatePos,stSliceStart,stSliceLen;
    double stReadPhase,stRate; uint32_t stRng;
    float dropAmt; int dropActive,dropLeft; uint32_t dropRng;
    int scanTimer;   /* Perform: Scan gesture — playheads run fast while >0 */
    /* Palette send buses (block-processed, 1-block latency) + master limiter */
    pfx_slot *busA,*busB;
    float sbufAL[128],sbufAR[128],sbufBL[128],sbufBR[128];
    float sretAL[128],sretAR[128],sretBL[128],sretBR[128];
    double limEnv;
    double compEnvL,compEnvR,casLpL,casLpR,clockHoldL,clockHoldR;int clockCounter;uint32_t rng;
    double cpuPct;   /* smoothed render_block load, % of block budget (Overtake CPU meter) */
    PolyVoice poly[POLY_VOICES]; int rootNote;   /* MIDI-keyboard playback layer */
    /* Punch-in FX (master insert): up to 4 slots in series, per-effect params */
    float punchParams[NUM_PUNCH][4];   /* [effect][Rate,Pitch,Tone,Mix] */
    float punchPress[NUM_PUNCH];        /* per-effect pad pressure */
    PunchSlot pslot[4];
    /* Input FX (record chain): full EQ + record tape speed */
    float inLow,inMid,inMidFreq,inHigh,inHighFreq;
    Biquad inEqLo,inEqMid,inEqHi;
    double gFlutBufL[FLUTTER_BUF],gFlutBufR[FLUTTER_BUF];int gFlutWr;double gFlutSweep,gFlutNextMax;
    uint32_t frameClock;
    struct { char dirs[SMP_MAX_DIRS][SMP_NAME_LEN];char dirPaths[SMP_MAX_DIRS][SMP_PATH_LEN];int numDirs;
             char files[SMP_MAX_FILES][SMP_NAME_LEN];char filePaths[SMP_MAX_FILES][SMP_PATH_LEN];
             int numFiles,curDirIdx; } browser;
} loopbox_t;

/* ---- DJ Filter (single-pole-smooth, Essaim-style: continuous sweep, low Q,
 * transparent at centre, one 12dB/oct biquad per side, state reset on LP<->HP) ---- */
static void dj_filter_update(Voice *v) {
    double f=(double)v->filter;
    int newMode = (f < 0.485) ? -1 : ((f > 0.515) ? 1 : 0);
    if(newMode!=v->djMode){ if(newMode<0)bq_reset(&v->djLpA); else if(newMode>0)bq_reset(&v->djHpA); v->djMode=newMode; }
    double Q=0.70710678+(double)v->djReso*5.3;   /* resonance: Butterworth -> ~6 */
    if(newMode<0){ double lpF=200.0*pow(18000.0/200.0, f/0.485); if(lpF>18000.0)lpF=18000.0; bq_set_lp(&v->djLpA,lpF,Q); }
    else if(newMode>0){ double t=(f-0.515)/0.485; double hpF=20.0*pow(2000.0/20.0, t); if(hpF>2000.0)hpF=2000.0; bq_set_hp(&v->djHpA,hpF,Q); }
}
static inline void dj_filter_stereo(Voice *v, double *l, double *r) {
    if(v->djMode<0){ *l=bq_L(&v->djLpA,*l); *r=bq_R(&v->djLpA,*r); }
    else if(v->djMode>0){ *l=bq_L(&v->djHpA,*l); *r=bq_R(&v->djHpA,*r); }
}

/* ---- Studer 962 EQ ---- */
static void studer_eq_update(Voice *v) {
    double bDb=v->eqBass*15.0,tDb=v->eqTreble*15.0,pDb=v->eqPresAmt*11.0;
    double pF=150.0*pow(7000.0/150.0,(double)v->eqPresFreq);
    if(fabs(bDb)>0.1)bq_set_lowshelf(&v->eqLow,20.0,bDb,0.7);else bq_reset(&v->eqLow);
    if(fabs(pDb)>0.1)bq_set_peak(&v->eqMid,pF,pDb,0.6);else bq_reset(&v->eqMid);
    if(fabs(tDb)>0.1)bq_set_highshelf(&v->eqHigh,20000.0,tDb,0.7);else bq_reset(&v->eqHigh);
}
static inline void studer_eq_stereo(Voice *v, double *l, double *r) {
    if(fabs(v->eqBass)>0.007){*l=bq_L(&v->eqLow,*l);*r=bq_R(&v->eqLow,*r);}
    if(fabs(v->eqPresAmt)>0.007){*l=bq_L(&v->eqMid,*l);*r=bq_R(&v->eqMid,*r);}
    if(fabs(v->eqTreble)>0.007){*l=bq_L(&v->eqHigh,*l);*r=bq_R(&v->eqHigh,*r);}
}

/* ---- Tilt EQ (Tonelux 800Hz pivot) ---- */
static void tilt_eq_update(Voice *v) {
    double t=(double)v->tiltEQ;
    if(fabs(t)<0.01){bq_reset(&v->tiltLo);bq_reset(&v->tiltHi);return;}
    double db=t*6.0;bq_set_lowshelf(&v->tiltLo,800.0,-db,0.7);bq_set_highshelf(&v->tiltHi,800.0,db,0.7);
}
static inline void tilt_eq_stereo(Voice *v, double *l, double *r) {
    if(fabs(v->tiltEQ)<0.01)return;
    *l=bq_L(&v->tiltLo,*l);*l=bq_L(&v->tiltHi,*l);*r=bq_R(&v->tiltLo,*r);*r=bq_R(&v->tiltHi,*r);
}

static inline double voice_saturate(double x, double amt) {
    if(amt<0.005)return x;double drive=1.0+amt*3.0;double wet=tape_sat(x*drive);return x*(1.0-amt*0.5)+wet*(amt*0.5);
}

/* ---- Wow/Flutter (Flutter2 stereo) ---- */
static inline void voice_wowflutter_stereo(Voice *v, double *l, double *r, double amt) {
    if(amt<0.005)return;int wr=v->flutWr;v->flutBufL[wr]=*l;v->flutBufR[wr]=*r;
    double depth=amt*amt*40.0,freq=0.02*amt*amt*amt;
    double offset=depth+depth*sin(v->flutSweep);v->flutSweep+=v->flutNextMax*freq;
    if(v->flutSweep>TWOPI){v->flutSweep-=TWOPI;v->flutNextMax=0.24+(lb_rand(&v->rng)*0.5+0.5)*0.74;}
    int count=wr+(int)floor(offset);double frac=offset-floor(offset);
    int i0=count&(FLUTTER_BUF-1),i1=(count+1)&(FLUTTER_BUF-1);
    double oL=v->flutBufL[i0]*(1.0-frac)+v->flutBufL[i1]*frac;
    double oR=v->flutBufR[i0]*(1.0-frac)+v->flutBufR[i1]*frac;
    v->flutWr=(wr-1+FLUTTER_BUF)&(FLUTTER_BUF-1);
    *l=*l*(1.0-amt*0.7)+oL*(amt*0.7);*r=*r*(1.0-amt*0.7)+oR*(amt*0.7);
}

/* ---- Master Wow/Flutter (global tape wobble) ---- */
static inline void master_wowflutter_stereo(loopbox_t *s, double *l, double *r, double amt) {
    if(amt<0.005)return;int wr=s->gFlutWr;s->gFlutBufL[wr]=*l;s->gFlutBufR[wr]=*r;
    double depth=amt*amt*40.0,freq=0.02*amt*amt*amt;
    double offset=depth+depth*sin(s->gFlutSweep);s->gFlutSweep+=s->gFlutNextMax*freq;
    if(s->gFlutSweep>TWOPI){s->gFlutSweep-=TWOPI;s->gFlutNextMax=0.24+(lb_rand(&s->rng)*0.5+0.5)*0.74;}
    int count=wr+(int)floor(offset);double frac=offset-floor(offset);
    int i0=count&(FLUTTER_BUF-1),i1=(count+1)&(FLUTTER_BUF-1);
    double oL=s->gFlutBufL[i0]*(1.0-frac)+s->gFlutBufL[i1]*frac;
    double oR=s->gFlutBufR[i0]*(1.0-frac)+s->gFlutBufR[i1]*frac;
    s->gFlutWr=(wr-1+FLUTTER_BUF)&(FLUTTER_BUF-1);
    *l=*l*(1.0-amt*0.7)+oL*(amt*0.7);*r=*r*(1.0-amt*0.7)+oR*(amt*0.7);
}

/* ---- Voice Clear ---- */
static inline void voice_clear(Voice *v) {
    if(v->loopLen>0)v->savedLoopLen=v->loopLen;   /* remember for undo (buffer samples are kept) */
    v->state=VS_EMPTY;v->loopLen=0;v->recHead=0;v->playHead=0;v->playPhase=0.0;
    v->glLastSlice=-1;v->stabLpStateL=0.0;v->stabLpStateR=0.0;v->muted=0;
}
/* Undo a clear: the buffer was never wiped, so restore length + playback. */
static inline void voice_unclear(Voice *v) {
    if(v->state==VS_EMPTY && v->savedLoopLen>0){ v->loopLen=v->savedLoopLen; v->playHead=0; v->playPhase=0.0; v->state=VS_PLAYING; }
}

/* ---- Seed glitch (Smack-style): a seeded permutation of the loop's slices +
 * per-slice reverse. The knob IS the seed — turning it re-rolls the pattern; the
 * value also sets slice count (2/4/8/16). Applied as a read-position remap in
 * voice_render (loop timing preserved), declicked by the scatter crossfade. */
static void glitch_regen(Voice *v){
    float g=v->glitch; int n = (g<0.25f)?2 : (g<0.5f)?4 : (g<0.75f)?8 : 16;
    v->glN=n; v->glLastSlice=-1;
    for(int i=0;i<n;i++){ v->glOrder[i]=i; v->glRev[i]=0; }
    uint32_t r=(uint32_t)(g*997.0f)*2654435761u+12345u;   /* seed from knob value */
    for(int i=n-1;i>0;i--){ r=1664525u*r+1013904223u; int j=(int)((r>>16)%(uint32_t)(i+1));
        int t=v->glOrder[i]; v->glOrder[i]=v->glOrder[j]; v->glOrder[j]=t; }
    for(int i=0;i<n;i++){ r=1664525u*r+1013904223u; v->glRev[i]=((r>>20)&1u)?1:0; }
    v->glKnobCache=g;
}

/* ---- Master Compressor (Logical4-inspired) ---- */
static inline void master_comp(double *l, double *r, double amt, double *envL, double *envR) {
    if(amt<0.005)return;double det=fmax(fabs(*l),fabs(*r));
    static double atkC=0.0,relC=0.0; if(atkC==0.0){atkC=exp(-1.0/(SR*0.003));relC=exp(-1.0/(SR*0.15));}  /* SR-constant: compute once */
    double env=fmax(*envL,*envR);env=(det>env)?atkC*env+(1.0-atkC)*det:relC*env+(1.0-relC)*det;
    *envL=*envR=env;double thDb=-6.0-amt*18.0,db=20.0*log10(env+1e-12);
    double ratio=2.0+amt*6.0;
    if(db>thDb){double gr=(db-thDb)*(1.0-1.0/ratio);
        double gain=pow(10.0,-gr/20.0);*l*=gain;*r*=gain;}
    /* Makeup gain: compensate for gain reduction */
    double makeupDb=(0.0-thDb)*(1.0-1.0/ratio)*0.5;
    double makeup=pow(10.0,makeupDb/20.0);*l*=makeup;*r*=makeup;
}

/* ---- Punch-in FX engine (master insert) ---- */
static inline float ring_read(const float *ring, double pos){
    while(pos<0)pos+=PUNCH_BUF; while(pos>=PUNCH_BUF)pos-=PUNCH_BUF;
    int i0=(int)pos,i1=i0+1; if(i1>=PUNCH_BUF)i1=0; double f=pos-(double)i0;
    return (float)((double)ring[i0]*(1.0-f)+(double)ring[i1]*f);
}
static inline float buf_read(const float *b, double pos, int n){
    while(pos<0)pos+=n; while(pos>=n)pos-=n;
    int i0=(int)pos,i1=i0+1; if(i1>=n)i1=0; double f=pos-(double)i0;
    return (float)((double)b[i0]*(1.0-f)+(double)b[i1]*f);
}
#define PRND(r) ((lb_rand(&(r))*0.5)+0.5)   /* 0..1 */
/* Start a slot on effect idx, using that effect's stored params (s->punchParams[idx]). */
static void punch_slot_start(loopbox_t *s, PunchSlot *ps, int idx){
    ps->idx=idx; ps->env=0.0; ps->releasing=0; const PunchDef *d=&PUNCH_DEFS[idx]; float *P=s->punchParams[idx];
    double bpm=(g_host&&g_host->get_bpm)?(double)g_host->get_bpm():120.0; if(bpm<20.0)bpm=120.0;
    double beat=SR*60.0/bpm, rmul=pow(4.0,((double)P[0]-0.5)*2.0);
    if(d->mech==PM_REPEAT||d->mech==PM_STUTTER){ double sl=beat/d->param*rmul; if(sl<256.0)sl=256.0; if(sl>PUNCH_BUF/2)sl=PUNCH_BUF/2;
        ps->sliceLen=sl; int st=(((int)ps->w-(int)sl)%PUNCH_BUF+PUNCH_BUF)%PUNCH_BUF; ps->sliceStart=(double)st; ps->readPhase=0.0; }
    else if(d->mech==PM_REVERSE){ double rl=beat*(0.25+(double)P[0]*1.75); if(rl<256.0)rl=256.0; if(rl>PUNCH_BUF/2)rl=PUNCH_BUF/2;
        ps->sliceLen=rl; int st=(((int)ps->w-(int)rl)%PUNCH_BUF+PUNCH_BUF)%PUNCH_BUF; ps->sliceStart=(double)st; ps->readPhase=rl-1.0; }
    else if(d->mech==PM_PITCH){ ps->readPhase=2048.0; }   /* mid-window delay (2-head shifter) */
    else if(d->mech==PM_SCRATCH){ double win=beat*0.5; ps->scratchHi=(double)ps->w; ps->scratchLo=(double)ps->w-win; ps->scratchPos=(double)ps->w; ps->scratchDir=-1.0; }
    else if(d->mech==PM_HAZE||d->mech==PM_STRETCH){ for(int i=0;i<4;i++)ps->gAct[i]=0; ps->gSched=0.0; ps->stGrid=(double)ps->w-4000.0; if(ps->gRng==0)ps->gRng=0x1234567u+(uint32_t)idx*2654435761u; }
    else if(d->mech==PM_SHIMMER){ ps->shR1=(double)ps->shW; ps->shFbL=ps->shFbR=0.0; }
    ps->crushCnt=0;
}
static void punch_on(loopbox_t *s, int idx){
    if(idx<0||idx>=NUM_PUNCH)return;
    for(int i=0;i<4;i++) if(s->pslot[i].idx==idx){ punch_slot_start(s,&s->pslot[i],idx); return; }  /* retrigger (also un-releases) */
    for(int i=0;i<4;i++) if(s->pslot[i].idx<0){ punch_slot_start(s,&s->pslot[i],idx); return; }      /* first free */
}
static void punch_off(loopbox_t *s, int idx){
    for(int i=0;i<4;i++) if(s->pslot[i].idx==idx){ s->pslot[i].releasing=1; return; }  /* fade out, freed in render */
}
/* per-block: set each active slot's tone LP coeffs from its effect's Tone param */
static void punch_prep(loopbox_t *s){
    for(int i=0;i<4;i++){ PunchSlot *ps=&s->pslot[i]; if(ps->idx<0)continue; float t=s->punchParams[ps->idx][2];
        if(t<0.98f){ double cut=500.0*pow(18000.0/500.0,(double)t); bq_set_lp(&ps->toneFilt,cut,0.707); } }
}
/* per-sample: one slot reads its own ring and produces wet (ring already written by caller) */
static inline void punch_slot_process(loopbox_t *s, PunchSlot *ps, double *outL, double *outR){
    const PunchDef *d=&PUNCH_DEFS[ps->idx]; float *P=s->punchParams[ps->idx]; int toneOn=(P[2]<0.98f);
    if(d->mech==PM_CRUSH){
        int period=1+(int)((1.0-(double)P[0])*63.0);
        if(ps->crushCnt<=0){ ps->crushCnt=period; double bits=2.0+(double)P[1]*10.0, lv=pow(2.0,bits);
            ps->crushL=floor((double)ps->ringL[ps->w]*lv+0.5)/lv; ps->crushR=floor((double)ps->ringR[ps->w]*lv+0.5)/lv; }
        ps->crushCnt--; double l=ps->crushL,r=ps->crushR;
        if(toneOn){ l=bq_L(&ps->toneFilt,l); r=bq_R(&ps->toneFilt,r); } *outL=l; *outR=r; return; }
    if(d->mech==PM_SAT){   /* warm drive: Tape (self-norm) / Tube / Cassette by Character=P[1] */
        double drv=1.0+(double)P[0]*5.0, ch=(double)P[1];
        double l=(double)ps->ringL[ps->w]*drv, r=(double)ps->ringR[ps->w]*drv;
        if(ch<0.34){ l=tape_sat(l); r=tape_sat(r); }
        else if(ch<0.67){ l=(l>0?lb_tanh(l):l*0.55); r=(r>0?lb_tanh(r):r*0.55); double mk=1.3/(1.0+(double)P[0]*1.5); l*=mk;r*=mk; }
        else { l=lb_tanh(l); r=lb_tanh(r); double mk=1.2/(1.0+(double)P[0]*1.5); l*=mk;r*=mk; }
        l*=0.25; r*=0.25;   /* SAT output trim: drive runs hot, tape branch has no makeup */
        if(toneOn){ l=bq_L(&ps->toneFilt,l); r=bq_R(&ps->toneFilt,r); } *outL=l; *outR=r; return; }
    if(d->mech==PM_HAZE){   /* granular cloud: P0=Size P1=Pitch P2=Density */
        double dur=(0.03+(double)P[0]*0.4)*SR, dens=2.0+(double)P[2]*40.0, pr=pow(2.0,((double)P[1]-0.5)*2.0);
        ps->gSched+=dens/SR;
        if(ps->gSched>=1.0){ ps->gSched-=1.0; for(int i=0;i<4;i++) if(!ps->gAct[i]){ ps->gAct[i]=1; ps->gAge[i]=0.0; ps->gDur[i]=dur; ps->gRate[i]=pr;
            double back=dur*1.5+PRND(ps->gRng)*dur*3.0; ps->gPos[i]=(double)ps->w-back; double pan=(PRND(ps->gRng)*2.0-1.0)*0.6; ps->gGl[i]=0.5*(1.0-pan); ps->gGr[i]=0.5*(1.0+pan); break; } }
        double sl=0.0,sr=0.0;
        for(int i=0;i<4;i++){ if(!ps->gAct[i])continue; double wph=ps->gAge[i]/ps->gDur[i]; if(wph>=1.0){ps->gAct[i]=0;continue;}
            double win=0.5-0.5*cos(TWOPI*wph), rp=ps->gPos[i]+ps->gAge[i]*ps->gRate[i];
            sl+=(double)ring_read(ps->ringL,rp)*win*ps->gGl[i]; sr+=(double)ring_read(ps->ringR,rp)*win*ps->gGr[i]; ps->gAge[i]+=1.0; }
        *outL=sl*0.9; *outR=sr*0.9; return; }
    if(d->mech==PM_STRETCH){   /* 2-grain OLA stretch/freeze: P0=stretch(1=freeze) P1=Pitch P2=Grain */
        double dur=(0.04+(double)P[2]*0.3)*SR, pr=pow(2.0,((double)P[1]-0.5)*2.0), srate=1.0-(double)P[0];
        ps->stGrid+=srate;
        for(int i=0;i<2;i++){ if(!ps->gAct[i]||ps->gAge[i]>=ps->gDur[i]){ ps->gAct[i]=1; ps->gAge[i]=(i==1)?(-dur*0.5):0.0; ps->gDur[i]=dur; ps->gPos[i]=ps->stGrid; } }
        double sl=0.0,sr=0.0,wsum=0.0;
        for(int i=0;i<2;i++){ double a=ps->gAge[i]; if(a<0.0){ps->gAge[i]+=1.0;continue;} double wph=a/ps->gDur[i]; if(wph>=1.0){ps->gAct[i]=0;continue;}
            double win=0.5-0.5*cos(TWOPI*wph), rp=ps->gPos[i]+a*pr;
            sl+=(double)ring_read(ps->ringL,rp)*win; sr+=(double)ring_read(ps->ringR,rp)*win; wsum+=win; ps->gAge[i]+=1.0; }
        double n=(wsum>0.01)?1.0/wsum:1.0; *outL=sl*n; *outR=sr*n; return; }
    if(d->mech==PM_SHIMMER){   /* 2-head pitch-shift in band-limited feedback: P0=Regen P1=Pitch P2=Tone */
        double ratio=1.0+(double)P[1], regen=(double)P[0]*0.6;
        double inL=(double)ps->ringL[ps->w]+ps->shFbL, inR=(double)ps->ringR[ps->w]+ps->shFbR;
        ps->shL[ps->shW]=(float)inL; ps->shR[ps->shW]=(float)inR;
        ps->shR1+=ratio; while(ps->shR1>=SHBUF)ps->shR1-=SHBUF; while(ps->shR1<0)ps->shR1+=SHBUF;
        double r2=ps->shR1+SHBUF/2; if(r2>=SHBUF)r2-=SHBUF;
        double g1=(double)ps->shW-ps->shR1; if(g1<0)g1+=SHBUF; double g2=(double)ps->shW-r2; if(g2<0)g2+=SHBUF;
        double a=0.5-0.5*cos(TWOPI*g1/SHBUF), b=0.5-0.5*cos(TWOPI*g2/SHBUF), ab=a+b+1e-6;
        double l=((double)buf_read(ps->shL,ps->shR1,SHBUF)*a+(double)buf_read(ps->shL,r2,SHBUF)*b)/ab;
        double r=((double)buf_read(ps->shR,ps->shR1,SHBUF)*a+(double)buf_read(ps->shR,r2,SHBUF)*b)/ab;
        ps->shW++; if(ps->shW>=SHBUF)ps->shW=0;
        double fl=l,fr=r; if(toneOn){ fl=bq_L(&ps->toneFilt,fl); fr=bq_R(&ps->toneFilt,fr); }
        ps->shFbL=lb_tanh(fl*regen); ps->shFbR=lb_tanh(fr*regen);
        *outL=l; *outR=r; return; }
    if(d->mech==PM_PITCH){   /* delay-line pitch shift: 2 heads a half-window apart, Hann-crossfaded (click-free) */
        double ratio=d->param*pow(2.0,((double)P[1]-0.5)*2.0)*(1.0+((double)P[0]-0.5)*0.1);
        const double W=4096.0;
        ps->readPhase+=(1.0-ratio); while(ps->readPhase>=W)ps->readPhase-=W; while(ps->readPhase<0)ps->readPhase+=W;
        double d1=ps->readPhase, d2=d1+W*0.5; if(d2>=W)d2-=W;
        double w1=0.5-0.5*cos(TWOPI*d1/W), w2=0.5-0.5*cos(TWOPI*d2/W), ws=w1+w2+1e-9;
        double rp1=(double)ps->w-d1, rp2=(double)ps->w-d2;
        double l=((double)ring_read(ps->ringL,rp1)*w1+(double)ring_read(ps->ringL,rp2)*w2)/ws;
        double r=((double)ring_read(ps->ringR,rp1)*w1+(double)ring_read(ps->ringR,rp2)*w2)/ws;
        if(toneOn){ l=bq_L(&ps->toneFilt,l); r=bq_R(&ps->toneFilt,r); } *outL=l; *outR=r; return; }
    double pm=pow(2.0,((double)P[1]-0.5)*2.0), pos, gate=1.0, bf=1.0; const double FD=64.0;
    if(d->mech==PM_REPEAT||d->mech==PM_STUTTER){ pos=ps->sliceStart+ps->readPhase; ps->readPhase+=pm;
        if(ps->readPhase>=ps->sliceLen)ps->readPhase-=ps->sliceLen; if(ps->readPhase<0)ps->readPhase+=ps->sliceLen;
        double e=ps->readPhase<ps->sliceLen-ps->readPhase?ps->readPhase:ps->sliceLen-ps->readPhase; if(e<FD)bf=e/FD;  /* fade slice edges */
        if(d->mech==PM_STUTTER){ double gp=ps->readPhase/ps->sliceLen; gate=(gp<0.55)?1.0:0.0;
            if(gp<0.03)gate=gp/0.03; else if(gp>0.52&&gp<0.55)gate=(0.55-gp)/0.03; } }
    else if(d->mech==PM_REVERSE){ pos=ps->sliceStart+ps->readPhase; ps->readPhase-=pm;
        if(ps->readPhase<0)ps->readPhase+=ps->sliceLen; if(ps->readPhase>=ps->sliceLen)ps->readPhase-=ps->sliceLen;
        double e=ps->readPhase<ps->sliceLen-ps->readPhase?ps->readPhase:ps->sliceLen-ps->readPhase; if(e<FD)bf=e/FD; }
    else { pos=ps->scratchPos; ps->scratchPos+=ps->scratchDir*d->param*3.0*pm;
        if(ps->scratchPos<ps->scratchLo){ps->scratchPos=ps->scratchLo;ps->scratchDir=1.0;}
        else if(ps->scratchPos>ps->scratchHi){ps->scratchPos=ps->scratchHi;ps->scratchDir=-1.0;} }
    double l=(double)ring_read(ps->ringL,pos)*gate*bf, r=(double)ring_read(ps->ringR,pos)*gate*bf;
    if(toneOn){ l=bq_L(&ps->toneFilt,l); r=bq_R(&ps->toneFilt,r); } *outL=l; *outR=r;
}

/* ---- Voice Render (stereo) ---- */
static void voice_render(Voice *v, loopbox_t *s, double *outL, double *outR, double *sendAL, double *sendAR, double *sendBL, double *sendBR, double clockSpeed) {
    *outL=*outR=*sendAL=*sendAR=*sendBL=*sendBR=0.0;
    int playing=(v->state==VS_PLAYING||v->state==VS_OVERDUBBING);
    if((!playing && v->playEnv<0.0005)||v->loopLen<=0||!v->bufferL||!v->bufferR){ if(!playing)v->playEnv=0.0; return; }
    int effStart=(int)(v->loopStart*(float)v->loopLen); if(effStart<0)effStart=0; if(effStart>v->loopLen-1)effStart=v->loopLen-1;
    int avail=v->loopLen-effStart; if(avail<1)avail=1;                 /* End is loop LENGTH from Start */
    int effLen=(int)(v->loopEnd*(float)avail); if(effLen<256)effLen=256; if(effLen>avail)effLen=avail;
    int effEnd=effStart+effLen;
    double rate=pow(2.0,(double)v->pitch)*clockSpeed;if(v->reverse>0.5f)rate=-rate;
    if(s->scanTimer>0)rate*=3.5;   /* Perform: Scan gesture (fast sweep) */
    if(playing && v->scatter>0.01f){ if(--v->scatterCnt<=0){
        int slices=4+(int)(v->scatter*12.0f); int sliceLen=effLen/slices; if(sliceLen<256)sliceLen=256;
        v->scatterCnt=sliceLen;
        if((lb_rand(&v->rng)*0.5+0.5)<(double)v->scatter){ int sl=(int)((lb_rand(&v->rng)*0.5+0.5)*(double)slices); if(sl>=slices)sl=slices-1;
            v->scatXfadePhase=v->playPhase; v->scatXfade=64;   /* crossfade out of the old position */
            v->playPhase=(double)(effStart+sl*sliceLen); } } }
    int glOn=(v->glitch>=0.02f); if(glOn && v->glitch!=v->glKnobCache) glitch_regen(v);
    double relPhase=v->playPhase-(double)effStart;
    while(relPhase<0)relPhase+=(double)effLen;while(relPhase>=(double)effLen)relPhase-=(double)effLen;
    double boundRel=relPhase;   /* linear phase for the loop-boundary fade (pre-remap) */
    if(glOn && playing && v->glN>0){   /* Seed: play slices in a seeded order (some reversed) */
        int gN=v->glN; while(gN>2 && effLen/gN<128) gN>>=1;   /* keep slice >= ~3ms so edges can crossfade */
        double sl=(double)effLen/(double)gN; int slice=(int)(relPhase/sl);
        if(slice<0)slice=0; if(slice>=gN)slice=gN-1;
        double within=relPhase-(double)slice*sl; int mapped=v->glOrder[slice]%gN;
        double mRel=v->glRev[slice]?((double)mapped*sl+(sl-1.0-within)):((double)mapped*sl+within);
        if(slice!=v->glLastSlice){ if(v->glLastSlice>=0){v->scatXfadePhase=v->glPrevAbs;v->scatXfade=64;} v->glLastSlice=slice; }
        relPhase=mRel; if(relPhase<0)relPhase=0; if(relPhase>=(double)effLen)relPhase=(double)effLen-1.0;
    }
    double absPhase=(double)effStart+relPhase; v->glPrevAbs=absPhase;
    int i0=(int)absPhase%v->loopLen;int i1=(i0+1)%v->loopLen;double frac=absPhase-floor(absPhase);
    double rawL=((double)v->bufferL[i0]*(1.0-frac)+(double)v->bufferL[i1]*frac)/32768.0;
    double rawR=((double)v->bufferR[i0]*(1.0-frac)+(double)v->bufferR[i1]*frac)/32768.0;
    if(v->scatXfade>0){   /* raised-cosine crossfade from the pre-jump position (scatter declick) */
        double op=v->scatXfadePhase; while(op<0)op+=(double)v->loopLen; while(op>=(double)v->loopLen)op-=(double)v->loopLen;
        int oi0=(int)op%v->loopLen,oi1=(oi0+1)%v->loopLen; double ofr=op-floor(op);
        double oL=((double)v->bufferL[oi0]*(1.0-ofr)+(double)v->bufferL[oi1]*ofr)/32768.0;
        double oR=((double)v->bufferR[oi0]*(1.0-ofr)+(double)v->bufferR[oi1]*ofr)/32768.0;
        double t=0.5-0.5*cos(M_PI*(1.0-(double)v->scatXfade/64.0));
        rawL=oL*(1.0-t)+rawL*t; rawR=oR*(1.0-t)+rawR*t;
        v->scatXfadePhase+=rate; v->scatXfade--; }
    if(playing){ v->playPhase+=rate;double dEnd=(double)effEnd,dStart=(double)effStart;
        while(v->playPhase>=dEnd)v->playPhase-=(double)effLen;while(v->playPhase<dStart)v->playPhase+=(double)effLen;
        v->playHead=(int)v->playPhase; }
    /* Amp envelope: attack fades in on trigger/unmute, release fades out on mute/pause/stop.
     * Attack 3ms..3s, Release 3ms..5s (param 0 = click-free floor); coeffs cached, recomputed on change. */
    if(v->ampAtk!=v->ampAtkCache){ double T=0.003*pow(1000.0,(double)v->ampAtk);    double k=1.0/(T*SR); if(k>1.0)k=1.0; v->ampAtkK=(float)k; v->ampAtkCache=v->ampAtk; }
    if(v->ampRel!=v->ampRelCache){ double T=0.003*pow(1666.667,(double)v->ampRel); double k=1.0/(T*SR); if(k>1.0)k=1.0; v->ampRelK=(float)k; v->ampRelCache=v->ampRel; }
    double gate=(playing && !v->muted)?1.0:0.0;
    double ek=(gate>v->playEnv)?(double)v->ampAtkK:(double)v->ampRelK;
    v->playEnv += (gate-v->playEnv)*ek;
    double _bf=1.0; const double _FD=128.0;                /* loop-boundary fade (click-free wrap), from linear phase */
    if(boundRel<_FD)_bf=boundRel/_FD; else if(boundRel>(double)effLen-_FD)_bf=((double)effLen-boundRel)/_FD;
    if(_bf<0.0)_bf=0.0; double _amp=v->playEnv*_bf;
    double sL=rawL,sR=rawR;
    sL=voice_saturate(sL,(double)v->saturation);sR=voice_saturate(sR,(double)v->saturation);
    voice_wowflutter_stereo(v,&sL,&sR,(double)v->wowFlutter);
    dj_filter_stereo(v,&sL,&sR);
    tilt_eq_stereo(v,&sL,&sR);studer_eq_stereo(v,&sL,&sR);
    if(s->stability>0.005f){sL=apply_stability(sL,(double)s->stability,&v->rng);sR=apply_stability(sR,(double)s->stability,&v->rng);
        double stabK=0.2+(double)s->stability*0.6;
        v->stabLpStateL+=stabK*(sL-v->stabLpStateL);sL=v->stabLpStateL;
        v->stabLpStateR+=stabK*(sR-v->stabLpStateR);sR=v->stabLpStateR;}
    double vol=(double)v->volume*_amp,pn=(double)v->pan;   /* _amp = click-free env x boundary fade */
    double panL=cos((pn+1.0)*0.25*M_PI),panR=sin((pn+1.0)*0.25*M_PI);
    *outL=sL*vol*panL;*outR=sR*vol*panR;
    double sSig=(sL+sR)*0.5*vol;
    *sendAL=sSig*(double)v->send;  *sendAR=*sendAL;    /* Send A -> delay bus */
    *sendBL=sSig*(double)v->sendB; *sendBR=*sendBL;    /* Send B -> reverb bus */
}

/* ---- Disintegration pass ---- */
static void voice_disintegrate_pass(Voice *v) {
    if(v->loopLen<=0||!v->bufferL||!v->bufferR)return;
    for(int i=0;i<v->loopLen;i++){
        double xL=(double)v->bufferL[i]/32768.0,xR=(double)v->bufferR[i]/32768.0;
        xL=voice_saturate(xL,(double)v->saturation*0.3);xR=voice_saturate(xR,(double)v->saturation*0.3);
        voice_wowflutter_stereo(v,&xL,&xR,(double)v->wowFlutter*0.2);
        double stabK=0.15;v->stabLpStateL+=stabK*(xL-v->stabLpStateL);xL=v->stabLpStateL;
        v->stabLpStateR+=stabK*(xR-v->stabLpStateR);xR=v->stabLpStateR;xL*=0.98;xR*=0.98;
        v->bufferL[i]=(int16_t)lb_clampd(xL*32767.0,-32767.0,32767.0);
        v->bufferR[i]=(int16_t)lb_clampd(xR*32767.0,-32767.0,32767.0);}
}

/* ---- MIDI-keyboard polyphony (plays a loop's buffer pitched, through its FX) ---- */
static void poly_note_on(loopbox_t *s, int loopIdx, int note, int vel) {
    if(loopIdx<0||loopIdx>=NUM_VOICES)return;
    Voice *lp=&s->voice[loopIdx]; if(lp->loopLen<=0)return;   /* nothing recorded yet */
    int slot=-1; for(int i=0;i<POLY_VOICES;i++) if(!s->poly[i].active){slot=i;break;}
    if(slot<0){ double lo=1e9; for(int i=0;i<POLY_VOICES;i++) if(s->poly[i].env<lo){lo=s->poly[i].env;slot=i;} } /* steal quietest */
    PolyVoice *pv=&s->poly[slot];
    int effStart=(int)(lp->loopStart*(float)lp->loopLen);
    pv->active=1; pv->releasing=0; pv->note=note; pv->loopIdx=loopIdx;
    pv->phase=(double)effStart; pv->rate=pow(2.0,(double)(note-s->rootNote)/12.0);
    pv->env=0.0; pv->vel=(float)vel/127.0f; pv->djMode=lp->djMode;
    bq_reset(&pv->djA);bq_reset(&pv->eqLow);bq_reset(&pv->eqMid);bq_reset(&pv->eqHigh);bq_reset(&pv->tiltLo);bq_reset(&pv->tiltHi);
}
static void poly_note_off(loopbox_t *s, int note) {   /* match by note only -> never a stuck note */
    for(int i=0;i<POLY_VOICES;i++){ PolyVoice *pv=&s->poly[i]; if(pv->active&&!pv->releasing&&pv->note==note)pv->releasing=1; }
}
/* per-block: refresh each active poly voice's filter/EQ coeffs from its loop */
static void poly_prep(loopbox_t *s) {
    for(int i=0;i<POLY_VOICES;i++){ PolyVoice *pv=&s->poly[i]; if(!pv->active)continue;
        if(pv->loopIdx<0||pv->loopIdx>=NUM_VOICES){pv->active=0;continue;}
        Voice *lp=&s->voice[pv->loopIdx]; pv->djMode=lp->djMode;
        if(lp->djMode<0)bq_copy_coeffs(&pv->djA,&lp->djLpA); else if(lp->djMode>0)bq_copy_coeffs(&pv->djA,&lp->djHpA);
        bq_copy_coeffs(&pv->eqLow,&lp->eqLow);bq_copy_coeffs(&pv->eqMid,&lp->eqMid);bq_copy_coeffs(&pv->eqHigh,&lp->eqHigh);
        bq_copy_coeffs(&pv->tiltLo,&lp->tiltLo);bq_copy_coeffs(&pv->tiltHi,&lp->tiltHi);
    }
}
/* per-sample: read buffer pitched, apply loop FX (sat/filter/tilt/EQ), pan/vol, accumulate */
static inline void poly_sample(loopbox_t *s, double *mixL, double *mixR, double *sAL, double *sAR, double *sBL, double *sBR) {
    for(int pi=0;pi<POLY_VOICES;pi++){ PolyVoice *pv=&s->poly[pi]; if(!pv->active)continue;
        Voice *lp=&s->voice[pv->loopIdx]; if(lp->loopLen<=0){pv->active=0;continue;}
        int effStart=(int)(lp->loopStart*(float)lp->loopLen); if(effStart<0)effStart=0; if(effStart>lp->loopLen-1)effStart=lp->loopLen-1;
        int avail=lp->loopLen-effStart; if(avail<1)avail=1;
        int effLen=(int)(lp->loopEnd*(float)avail); if(effLen<256)effLen=256; if(effLen>avail)effLen=avail;
        int effEnd=effStart+effLen;
        while(pv->phase>=(double)effEnd)pv->phase-=(double)effLen; while(pv->phase<(double)effStart)pv->phase+=(double)effLen;
        int i0=(int)pv->phase%lp->loopLen,i1=(i0+1)%lp->loopLen; double frac=pv->phase-floor(pv->phase);
        double l=((double)lp->bufferL[i0]*(1.0-frac)+(double)lp->bufferL[i1]*frac)/32768.0;
        double r=((double)lp->bufferR[i0]*(1.0-frac)+(double)lp->bufferR[i1]*frac)/32768.0;
        pv->phase+=pv->rate;
        if(pv->releasing){ pv->env+=(0.0-pv->env)*0.00015; if(pv->env<0.0004){pv->active=0;continue;} }
        else pv->env+=(1.0-pv->env)*0.0045;
        double g=pv->env*(double)pv->vel; l*=g; r*=g;
        l=voice_saturate(l,(double)lp->saturation); r=voice_saturate(r,(double)lp->saturation);
        if(pv->djMode!=0){ l=bq_L(&pv->djA,l); r=bq_R(&pv->djA,r); }
        if(fabs(lp->tiltEQ)>=0.01){ l=bq_L(&pv->tiltLo,l);l=bq_L(&pv->tiltHi,l); r=bq_R(&pv->tiltLo,r);r=bq_R(&pv->tiltHi,r); }
        if(fabs(lp->eqBass)>0.007){l=bq_L(&pv->eqLow,l);r=bq_R(&pv->eqLow,r);}
        if(fabs(lp->eqPresAmt)>0.007){l=bq_L(&pv->eqMid,l);r=bq_R(&pv->eqMid,r);}
        if(fabs(lp->eqTreble)>0.007){l=bq_L(&pv->eqHigh,l);r=bq_R(&pv->eqHigh,r);}
        double vol=(double)lp->volume,pn=(double)lp->pan;
        double panL=cos((pn+1.0)*0.25*M_PI),panR=sin((pn+1.0)*0.25*M_PI);
        *mixL+=l*vol*panL; *mixR+=r*vol*panR;
        double ssig=(l+r)*0.5*vol; *sAL+=ssig*(double)lp->send; *sAR+=ssig*(double)lp->send; *sBL+=ssig*(double)lp->sendB; *sBR+=ssig*(double)lp->sendB;
    }
}

/* ---- Sample Browser ---- */
static int str_endswith_wav(const char *s){int n=(int)strlen(s);if(n<4)return 0;
    const char *e=s+n-4;return(e[0]=='.'&&(e[1]=='w'||e[1]=='W')&&(e[2]=='a'||e[2]=='A')&&(e[3]=='v'||e[3]=='V'));}

/* Recursively collect all subdirectories that contain .wav files (leaf folders) */
static void smp_scan_dir_recursive(loopbox_t *s, const char *path, const char *display) {
    if(s->browser.numDirs>=SMP_MAX_DIRS)return;
    DIR *d=opendir(path);if(!d)return;struct dirent *e;
    int hasWav=0,hasSub=0;
    /* First pass: check what's in this dir */
    while((e=readdir(d))){if(e->d_name[0]=='.')continue;
        if(e->d_type==DT_DIR)hasSub=1;
        if(str_endswith_wav(e->d_name))hasWav=1;}
    closedir(d);
    /* If this folder has WAV files, add it as a browsable folder */
    if(hasWav&&s->browser.numDirs<SMP_MAX_DIRS){
        strncpy(s->browser.dirs[s->browser.numDirs],display,SMP_NAME_LEN-1);
        s->browser.dirs[s->browser.numDirs][SMP_NAME_LEN-1]='\0';
        strncpy(s->browser.dirPaths[s->browser.numDirs],path,SMP_PATH_LEN-1);
        s->browser.dirPaths[s->browser.numDirs][SMP_PATH_LEN-1]='\0';
        s->browser.numDirs++;}
    /* Recurse into subdirectories */
    if(hasSub){d=opendir(path);if(!d)return;
        while((e=readdir(d))&&s->browser.numDirs<SMP_MAX_DIRS){
            if(e->d_name[0]=='.'||e->d_type!=DT_DIR)continue;
            char sub[SMP_PATH_LEN],lbl[SMP_NAME_LEN];
            snprintf(sub,SMP_PATH_LEN,"%s/%s",path,e->d_name);
            snprintf(lbl,SMP_NAME_LEN,"%s/%s",display,e->d_name);
            smp_scan_dir_recursive(s,sub,lbl);}
        closedir(d);}
}

static void smp_scan_dirs(loopbox_t *s){s->browser.numDirs=0;
    DIR *d=opendir(SAMPLES_ROOT);if(!d)return;struct dirent *e;
    while((e=readdir(d))&&s->browser.numDirs<SMP_MAX_DIRS){
        if(e->d_name[0]=='.'||e->d_type!=DT_DIR)continue;
        char sub[SMP_PATH_LEN];snprintf(sub,SMP_PATH_LEN,"%s/%s",SAMPLES_ROOT,e->d_name);
        smp_scan_dir_recursive(s,sub,e->d_name);}
    closedir(d);}

static void smp_scan_files(loopbox_t *s, int dirIdx){s->browser.numFiles=0;s->browser.curDirIdx=dirIdx;
    if(dirIdx<0||dirIdx>=s->browser.numDirs)return;
    char dirPath[SMP_PATH_LEN];strncpy(dirPath,s->browser.dirPaths[dirIdx],SMP_PATH_LEN-1);dirPath[SMP_PATH_LEN-1]='\0';
    DIR *d=opendir(dirPath);if(!d)return;struct dirent *e;
    while((e=readdir(d))&&s->browser.numFiles<SMP_MAX_FILES){
        if(e->d_name[0]=='.')continue;if(!str_endswith_wav(e->d_name))continue;
        strncpy(s->browser.files[s->browser.numFiles],e->d_name,SMP_NAME_LEN-1);
        s->browser.files[s->browser.numFiles][SMP_NAME_LEN-1]='\0';
        snprintf(s->browser.filePaths[s->browser.numFiles],SMP_PATH_LEN,"%s/%s",dirPath,e->d_name);
        s->browser.numFiles++;}
    closedir(d);}

static int smp_load_wav(const char *path, int16_t *bufL, int16_t *bufR, int maxSamples){
    FILE *f=fopen(path,"rb");if(!f)return 0;
    char hdr[4];uint32_t fSize,dummy32;fread(hdr,1,4,f);fread(&fSize,4,1,f);fread(hdr,1,4,f);
    if(memcmp(hdr,"WAVE",4)!=0){fclose(f);return 0;}
    int16_t audioFmt=0,numCh=0,bps=0;int32_t sr=0;uint32_t dataSize=0;int foundData=0;
    char cid[4];uint32_t csz;
    while(fread(cid,1,4,f)==4){fread(&csz,4,1,f);
        if(memcmp(cid,"fmt ",4)==0){fread(&audioFmt,2,1,f);fread(&numCh,2,1,f);fread(&sr,4,1,f);
            fread(&dummy32,4,1,f);fread(&dummy32,4,1,f);/* skip byteRate(4)+blockAlign(2) — reading 4+4=8, but blockAlign is 2... */
            /* Actually: byteRate=4, blockAlign=2, bitsPerSample=2 = 8 bytes after sampleRate */
            /* We read 4 (dummy32) above. Need to back up and re-read properly */
            fseek(f,-(long)(4+4),SEEK_CUR);/* back to after sr */
            fread(&dummy32,4,1,f);/* byteRate */
            int16_t blockAlign;fread(&blockAlign,2,1,f);fread(&bps,2,1,f);
            long remaining=(long)csz-16;if(remaining>0)fseek(f,remaining,SEEK_CUR);
        }else if(memcmp(cid,"data",4)==0){dataSize=csz;foundData=1;break;
        }else{fseek(f,(long)csz,SEEK_CUR);}}
    if(!foundData||audioFmt!=1||bps!=16||numCh<1){fclose(f);return 0;}
    int total=(int)(dataSize/(numCh*2));if(total>maxSamples)total=maxSamples;
    if(numCh==1){for(int i=0;i<total;i++){int16_t sv;fread(&sv,2,1,f);bufL[i]=sv;bufR[i]=sv;}}
    else{for(int i=0;i<total;i++){int16_t sL,sR;fread(&sL,2,1,f);fread(&sR,2,1,f);bufL[i]=sL;bufR[i]=sR;
        if(numCh>2)fseek(f,(long)(numCh-2)*2,SEEK_CUR);}}
    fclose(f);return total;}

/* ---- Lifecycle ---- */
static void *create_instance(const char *module_dir, const char *json_defaults) {
    (void)module_dir;(void)json_defaults;
    loopbox_t *s=(loopbox_t*)calloc(1,sizeof(loopbox_t));if(!s)return NULL;
    for(int i=0;i<NUM_VOICES;i++){Voice *v=&s->voice[i];
        v->bufferL=(int16_t*)calloc(LOOP_SAMPLES,sizeof(int16_t));
        v->bufferR=(int16_t*)calloc(LOOP_SAMPLES,sizeof(int16_t));
        if(!v->bufferL||!v->bufferR){for(int j=0;j<=i;j++){free(s->voice[j].bufferL);free(s->voice[j].bufferR);}free(s);return NULL;}
        v->state=VS_EMPTY;v->loopStart=0.0f;v->loopEnd=1.0f;v->reverse=0.0f;
        v->pitch=0.0f;v->filter=0.5f;v->pan=0.0f;v->volume=0.8f;
        v->saturation=0.0f;v->wowFlutter=0.0f;v->send=0.0f;v->glitch=0.0f;
        v->tiltEQ=0.0f;v->decay=1.0f;v->eqBass=0.0f;v->eqPresFreq=0.5f;v->eqPresAmt=0.0f;v->eqTreble=0.0f;
        v->flutNextMax=0.5;v->rng=12345+i*7919;v->smpDir=0;v->smpFile=-1;v->glLastSlice=-1;
        v->djReso=0.0f;v->ampAtk=0.0f;v->ampRel=0.0f;v->ampAtkCache=-1.0f;v->ampRelCache=-1.0f;
        bq_reset(&v->djLpA);bq_reset(&v->djLpB);bq_reset(&v->djLpC);
        bq_reset(&v->djHpA);bq_reset(&v->djHpB);bq_reset(&v->djHpC);
        bq_reset(&v->eqLow);bq_reset(&v->eqMid);bq_reset(&v->eqHigh);
        bq_reset(&v->tiltLo);bq_reset(&v->tiltHi);
        dj_filter_update(v);studer_eq_update(v);tilt_eq_update(v);}
    s->globalSat=0.0f;s->masterComp=0.0f;s->masterLoCut=20.0f;s->masterHiCut=20000.0f;s->clock=0.5f;s->masterVol=1.0f;
    s->preamp=0.0f;s->overdubMode=0.0f;s->stability=0.0f;s->selTrack=1;s->rng=42;
    s->globalWowFlut=0.0f;s->inputMonitor=0.75f;s->inputGain=1.0f;s->gFlutNextMax=0.5;s->frameClock=100000;
    bq_reset(&s->masterLo);bq_reset(&s->masterHi);
    s->cpuPct=0.0; s->rootNote=60;   /* C3 plays the loop at its recorded speed */
    for(int i=0;i<4;i++){s->pslot[i].idx=-1;bq_reset(&s->pslot[i].toneFilt);}
    for(int i=0;i<NUM_PUNCH;i++){s->punchParams[i][0]=0.5f;s->punchParams[i][1]=0.5f;s->punchParams[i][2]=1.0f;s->punchParams[i][3]=1.0f;s->punchPress[i]=0.0f;}
    s->inLow=0.0f;s->inMid=0.0f;s->inMidFreq=0.5f;s->inHigh=0.0f;s->inHighFreq=0.5f;
    bq_reset(&s->inEqLo);bq_reset(&s->inEqMid);bq_reset(&s->inEqHi);
    s->busA=pfx_create(44100.0f); s->busB=pfx_create(44100.0f); s->limEnv=0.0;
    s->sendAType=14;s->sendBType=17;s->sendAM1=0.4f;s->sendAM2=0.5f;s->sendADrift=0.2f;s->sendBM1=0.5f;s->sendBM2=0.5f;s->sendBDrift=0.2f;
    if(s->busA)pfx_select(s->busA,s->sendAType); if(s->busB)pfx_select(s->busB,s->sendBType);
    s->stMix=0.0f;s->stStep=0.3f;s->stOdds=0.5f;s->stSize=0.5f;s->stReach=0.3f;s->stKind=0;s->stStepLeft=1;s->stRng=0x2233aa55u;
    s->dropAmt=0.0f;s->dropLeft=1;s->dropRng=0x9911bb77u;
    /* Overtake: no sample browser (file I/O forbidden on the audio callback; live
     * looping records from the input). Browser stays empty and harmless. */
    return s;
}
static void destroy_instance(void *inst){loopbox_t *s=(loopbox_t*)inst;if(!s)return;
    for(int i=0;i<NUM_VOICES;i++){free(s->voice[i].bufferL);free(s->voice[i].bufferR);}
    if(s->busA)pfx_destroy(s->busA); if(s->busB)pfx_destroy(s->busB); free(s);}

/* ---- LCXL CC mapping (remapped to avoid standard MIDI CCs 0-31) ---- */
/* Template 1 (voices 1-8): Pitch CC33-40, Filter CC41-48, Pan CC49-56, Vol CC57-64 */
/* Template 2 (voices 9-16): Pitch CC65-72, Filter CC73-80, Pan CC81-88, Vol CC89-96 */
static inline int lcxl_cc_to_voice(int cc, int *paramType) {
    if(cc>=33&&cc<=40){*paramType=0;return cc-33;}if(cc>=41&&cc<=48){*paramType=1;return cc-41;}
    if(cc>=49&&cc<=56){*paramType=2;return cc-49;}if(cc>=57&&cc<=64){*paramType=3;return cc-57;}
    if(cc>=65&&cc<=72){*paramType=0;return cc-65+8;}if(cc>=73&&cc<=80){*paramType=1;return cc-73+8;}
    if(cc>=81&&cc<=88){*paramType=2;return cc-81+8;}if(cc>=89&&cc<=96){*paramType=3;return cc-89+8;}
    return -1;
}
static inline int lcxl_note_to_rec(int note) {
    if(note>=68&&note<=75)return note-68;if(note>=76&&note<=83)return note-76+8;return -1;
}
static inline int lcxl_note_to_play(int note) {
    if(note>=36&&note<=43)return note-36;if(note>=44&&note<=51)return note-44+8;return -1;
}

/* ---- MIDI Handler ---- */
static void on_midi(void *inst, const uint8_t *msg, int len, int source) {
    loopbox_t *s=(loopbox_t*)inst;if(len<3)return;
    uint8_t status=msg[0]&0xF0,d1=msg[1],d2=msg[2];

    /* Overtake: internal pad/knob input is owned by ui.js, which drives the DSP
     * via set_param("cmd", "tap:N" / "odub:N" / "clear:N" / "sel:N"). The block
     * below is kept for reference but disabled so pads never double-trigger. */
    if(source==MOVE_MIDI_SOURCE_INTERNAL) return;

    if(0 && source==MOVE_MIDI_SOURCE_INTERNAL) {
        /* Move pads: Note On 36-51 (lower) + 52-67 (upper, mirrored) */
        if(status==0x90&&d2>0){int vIdx=(int)d1-36;
            if(vIdx>=16)vIdx-=16; /* upper 16 pads mirror lower 16 */
            if(vIdx<0||vIdx>=NUM_VOICES)return;Voice *v=&s->voice[vIdx];s->selTrack=vIdx+1;
            uint32_t now=s->frameClock,elapsed=now-v->lastPadFrame;v->lastPadFrame=now;
            int dblTap=(elapsed>128&&elapsed<(uint32_t)DPRESS_FRAMES);
            switch(v->state){
            case VS_EMPTY:v->state=VS_RECORDING;v->recHead=0;v->loopLen=0;break;
            case VS_RECORDING:v->loopLen=v->recHead;v->playHead=0;v->playPhase=0.0;v->state=VS_PLAYING;break;
            case VS_PLAYING:if(dblTap){v->state=VS_OVERDUBBING;v->recHead=v->playHead;}else{v->state=VS_PAUSED;}break;
            case VS_PAUSED:if(dblTap){v->state=VS_OVERDUBBING;v->recHead=v->playHead;}else{v->state=VS_PLAYING;}break;
            case VS_OVERDUBBING:if((int)s->overdubMode==OD_DISINTEGRATION)voice_disintegrate_pass(v);v->state=VS_PLAYING;break;}}
        return;
    }

    if(source==MOVE_MIDI_SOURCE_EXTERNAL) {
        /* MIDI keyboard playing: ch1 -> selected loop, ch2..16 -> loops 2..16; 8-voice poly */
        int chan = msg[0] & 0x0F;
        int loopIdx = (chan==0) ? (s->selTrack-1) : chan;
        if(status==0x90 && d2>0) { poly_note_on(s, loopIdx, (int)d1, (int)d2); return; }
        if(status==0x80 || (status==0x90 && d2==0)) { poly_note_off(s, (int)d1); return; }
        return;   /* LCXL handling below is disabled (next wave) */
        /* LCXL CCs: per-voice Pitch/Filter/Pan/Volume */
        if(status==0xB0){int pt=-1;int vIdx=lcxl_cc_to_voice((int)d1,&pt);
            if(vIdx>=0&&vIdx<NUM_VOICES){Voice *v=&s->voice[vIdx];double val=(double)d2/127.0;
                switch(pt){case 0:v->pitch=(float)(val*4.0-2.0);break;case 1:v->filter=(float)val;dj_filter_update(v);break;
                    case 2:v->pan=(float)(val*2.0-1.0);break;case 3:v->volume=(float)val;break;}
                s->selTrack=vIdx+1;}return;}
        /* LCXL buttons: Note On for Rec/OD and Play/Stop */
        if(status==0x90&&d2>0){
            int vIdx=lcxl_note_to_rec((int)d1);
            if(vIdx>=0&&vIdx<NUM_VOICES){Voice *v=&s->voice[vIdx];s->selTrack=vIdx+1;
                switch(v->state){
                case VS_EMPTY:v->state=VS_RECORDING;v->recHead=0;v->loopLen=0;break;
                case VS_RECORDING:v->loopLen=v->recHead;v->playHead=0;v->playPhase=0.0;v->state=VS_PLAYING;break;
                case VS_PLAYING:case VS_PAUSED:v->state=VS_OVERDUBBING;v->recHead=v->playHead;break;
                case VS_OVERDUBBING:if((int)s->overdubMode==OD_DISINTEGRATION)voice_disintegrate_pass(v);v->state=VS_PLAYING;break;}
                return;}
            vIdx=lcxl_note_to_play((int)d1);
            if(vIdx>=0&&vIdx<NUM_VOICES){Voice *v=&s->voice[vIdx];s->selTrack=vIdx+1;
                switch(v->state){
                case VS_PLAYING:v->state=VS_PAUSED;break;case VS_PAUSED:v->state=VS_PLAYING;break;
                case VS_RECORDING:v->loopLen=v->recHead;v->playHead=0;v->playPhase=0.0;v->state=VS_PLAYING;break;
                case VS_OVERDUBBING:if((int)s->overdubMode==OD_DISINTEGRATION)voice_disintegrate_pass(v);v->state=VS_PLAYING;break;
                default:break;}return;}}
    }
}

/* ---- Input EQ (record chain) ---- */
static void input_eq_update(loopbox_t *s){
    double lDb=(double)s->inLow*15.0, mDb=(double)s->inMid*11.0, hDb=(double)s->inHigh*15.0;
    double mF=150.0*pow(7000.0/150.0,(double)s->inMidFreq), hF=3000.0+(double)s->inHighFreq*12000.0;
    if(fabs(lDb)>0.1)bq_set_lowshelf(&s->inEqLo,120.0,lDb,0.7); else bq_reset(&s->inEqLo);
    if(fabs(mDb)>0.1)bq_set_peak(&s->inEqMid,mF,mDb,0.7); else bq_reset(&s->inEqMid);
    if(fabs(hDb)>0.1)bq_set_highshelf(&s->inEqHi,hF,hDb,0.7); else bq_reset(&s->inEqHi);
}

/* ---- Perform: Stumble (free-running probabilistic step glitch on the master) ---- */
static inline void stumble_sample(loopbox_t *s, double *mixL, double *mixR){
    if(s->stMix<=0.001f)return;
    s->stRingL[s->stW]=(float)*mixL; s->stRingR[s->stW]=(float)*mixR;
    if(--s->stStepLeft<=0){
        int stepSamp=(int)((20.0+(double)s->stStep*980.0)*44.1); if(stepSamp<441)stepSamp=441; if(stepSamp>PUNCH_BUF/2)stepSamp=PUNCH_BUF/2;
        s->stStepLen=stepSamp; s->stStepLeft=stepSamp; s->stStepPos=0;
        s->stActive = (PRND(s->stRng) < (double)s->stOdds);
        if(s->stActive){
            s->stEffect = (s->stKind==0)? (int)(PRND(s->stRng)*5.0) : (s->stKind-1); if(s->stEffect>4)s->stEffect=4; if(s->stEffect<0)s->stEffect=0;
            int slice=(int)((double)stepSamp*(0.05+0.95*(double)s->stSize)); if(slice<256)slice=256; if(slice>PUNCH_BUF/2)slice=PUNCH_BUF/2; s->stSliceLen=slice;
            int back=slice; if(s->stReach>0.0f){ int span=PUNCH_BUF-slice-2; if(span>0)back+=(int)(PRND(s->stRng)*(double)span*(double)s->stReach); }
            s->stSliceStart=((s->stW-back)%PUNCH_BUF+PUNCH_BUF)%PUNCH_BUF;
            s->stReadPhase=(s->stEffect==1)?(double)(slice-1):0.0; s->stGatePos=0; s->stRate=1.0;
        }
    }
    s->stStepPos++;
    if(s->stActive){
        double wl=*mixL,wr=*mixR; int e=s->stEffect;
        if(e==3){ int half=s->stSliceLen/2; double env=(s->stGatePos<half)?1.0:0.0; wl=*mixL*env; wr=*mixR*env; if(++s->stGatePos>=s->stSliceLen)s->stGatePos=0; }
        else if(e==4){ double lv=64.0; wl=floor(*mixL*lv+0.5)/lv; wr=floor(*mixR*lv+0.5)/lv; }
        else { double pos=(double)s->stSliceStart+s->stReadPhase; wl=(double)ring_read(s->stRingL,pos); wr=(double)ring_read(s->stRingR,pos);
            double sp=s->stReadPhase, ep=(sp<s->stSliceLen-sp)?sp:s->stSliceLen-sp; const double SEF=48.0;  /* fade slice-loop edges */
            if(ep<SEF){ double f=ep/SEF; wl*=f; wr*=f; }
            if(e==1){ s->stReadPhase-=1.0; if(s->stReadPhase<0)s->stReadPhase+=s->stSliceLen; }
            else { s->stReadPhase+=s->stRate; if(s->stReadPhase>=s->stSliceLen)s->stReadPhase-=s->stSliceLen; if(e==2)s->stRate*=0.99997; } }
        double fade=1.0; const int FD=66;
        if(s->stStepPos<FD)fade=(double)s->stStepPos/FD; int rem=s->stStepLen-s->stStepPos; if(rem>=0&&rem<FD){double f=(double)rem/FD; if(f<fade)fade=f;}
        double m=(double)s->stMix*fade;
        *mixL=*mixL+(wl-*mixL)*m; *mixR=*mixR+(wr-*mixR)*m;
    }
    s->stW++; if(s->stW>=PUNCH_BUF)s->stW=0;
}
static inline void dropout_sample(loopbox_t *s, double *mixL, double *mixR){
    if(s->dropAmt<=0.001f)return;
    if(--s->dropLeft<=0){ s->dropLeft=(int)(SR*(0.02+PRND(s->dropRng)*0.15)); s->dropActive=(PRND(s->dropRng)<(double)s->dropAmt*0.4); }
    if(s->dropActive){ double d=1.0-(double)s->dropAmt; *mixL*=d; *mixR*=d; }
}

/* ---- Master limiter: analog-style fast-attack peak duck + soft ceiling ---- */
static inline void master_limiter(double *l, double *r, double *env){
    double det=fmax(fabs(*l),fabs(*r));
    const double atk=0.9285, rel=0.99977;   /* ~0.3ms attack / ~100ms release */
    *env = (det>*env)? atk*(*env)+(1.0-atk)*det : rel*(*env)+(1.0-rel)*det;
    const double ceil=0.90; double gr=(*env>ceil)? ceil/(*env):1.0;
    *l=lb_tanh(*l*gr); *r=lb_tanh(*r*gr);   /* duck peaks, then smooth soft-clip */
}

/* ---- Render Block ---- */
static void render_block(void *inst, int16_t *out_interleaved_lr, int frames) {
    loopbox_t *s=(loopbox_t*)inst;s->frameClock+=(uint32_t)frames;
    struct timespec _t0; clock_gettime(CLOCK_MONOTONIC,&_t0);
    int16_t *micBuf=NULL;if(g_host&&g_host->mapped_memory)micBuf=(int16_t*)(g_host->mapped_memory+g_host->audio_in_offset);
    int selIdx=s->selTrack-1;
    if(selIdx>=0&&selIdx<NUM_VOICES){Voice *sv=&s->voice[selIdx];dj_filter_update(sv);studer_eq_update(sv);tilt_eq_update(sv);}
    if(s->masterLoCut>21.0f)bq_set_hp(&s->masterLo,(double)s->masterLoCut,0.707);
    if(s->masterHiCut<19999.0f)bq_set_lp(&s->masterHi,(double)s->masterHiCut,0.707);
    double clockSpeed=clock_to_speed(s->clock);int decimFactor=(clockSpeed<0.99)?(int)(1.0/clockSpeed):1;
    if(decimFactor<1)decimFactor=1;if(decimFactor>4)decimFactor=4;
    OverdubMode odMode=(OverdubMode)(int)lb_clampf(s->overdubMode,0.0f,2.0f);
    poly_prep(s);   /* refresh keyboard-poly FX coeffs from their loops */
    if(s->scanTimer>0){ s->scanTimer-=frames; if(s->scanTimer<0)s->scanTimer=0; }
    input_eq_update(s);   /* record-chain EQ + tape-speed */
    punch_prep(s);   /* per-block: punch slot tone-filter coeffs */

    for(int n=0;n<frames;n++){
        double inL=0.0,inR=0.0;
        if(micBuf){double ig=(double)s->inputGain;inL=(double)micBuf[n*2]/32768.0*ig;inR=(double)micBuf[n*2+1]/32768.0*ig;
            double aL=fabs(inL),aR=fabs(inR);if(aL>s->inputPeakL)s->inputPeakL=aL;if(aR>s->inputPeakR)s->inputPeakR=aR;}
        int preModel=(int)lb_clampf(s->preamp,0.0f,11.0f);
        apply_preamp_sample(&inL,&inR,preModel,&s->casLpL,&s->casLpR,&s->rng);
        if(fabs(s->inLow)>0.007f){inL=bq_L(&s->inEqLo,inL);inR=bq_R(&s->inEqLo,inR);}
        if(fabs(s->inMid)>0.007f){inL=bq_L(&s->inEqMid,inL);inR=bq_R(&s->inEqMid,inR);}
        if(fabs(s->inHigh)>0.007f){inL=bq_L(&s->inEqHi,inL);inR=bq_R(&s->inEqHi,inR);}
        int16_t inSL=(int16_t)lb_clampd(inL*32767.0,-32767.0,32767.0);
        int16_t inSR=(int16_t)lb_clampd(inR*32767.0,-32767.0,32767.0);

        for(int vi=0;vi<NUM_VOICES;vi++){Voice *v=&s->voice[vi];
            if(v->state==VS_RECORDING){if(v->recHead<LOOP_SAMPLES){v->bufferL[v->recHead]=inSL;v->bufferR[v->recHead]=inSR;v->recHead++;}
                if(v->recHead>=LOOP_SAMPLES){v->loopLen=LOOP_SAMPLES;v->playHead=0;v->playPhase=0.0;v->state=VS_PLAYING;}}
            else if(v->state==VS_OVERDUBBING&&v->playHead<v->loopLen){switch(odMode){
                case OD_REPLACE:v->bufferL[v->playHead]=inSL;v->bufferR[v->playHead]=inSR;break;
                case OD_MULTIPLY:{double oL=(double)v->bufferL[v->playHead]/32768.0,oR=(double)v->bufferR[v->playHead]/32768.0;
                    double dg=0.5+(double)v->decay*0.5;
                    v->bufferL[v->playHead]=(int16_t)lb_clampd((oL*dg+inL)*32767.0,-32767.0,32767.0);
                    v->bufferR[v->playHead]=(int16_t)lb_clampd((oR*dg+inR)*32767.0,-32767.0,32767.0);break;}
                case OD_DISINTEGRATION:{double oL=(double)v->bufferL[v->playHead]/32768.0,oR=(double)v->bufferR[v->playHead]/32768.0;
                    v->bufferL[v->playHead]=(int16_t)lb_clampd((oL*0.85+inL)*32767.0,-32767.0,32767.0);
                    v->bufferR[v->playHead]=(int16_t)lb_clampd((oR*0.85+inR)*32767.0,-32767.0,32767.0);break;}}}}

        double mixL=0.0,mixR=0.0,sAL=0.0,sAR=0.0,sBL=0.0,sBR=0.0;
        for(int vi=0;vi<NUM_VOICES;vi++){double vL,vR,aL,aR,bL,bR;
            voice_render(&s->voice[vi],s,&vL,&vR,&aL,&aR,&bL,&bR,clockSpeed);mixL+=vL;mixR+=vR;sAL+=aL;sAR+=aR;sBL+=bL;sBR+=bR;}
        poly_sample(s,&mixL,&mixR,&sAL,&sAR,&sBL,&sBR);   /* MIDI-keyboard poly layer */
        if(s->inputMonitor>0.005f){double mg=(double)s->inputMonitor;mixL+=inL*mg;mixR+=inR*mg;}

        /* Clock SR decimation + aliasing noise (Mood mk2 style) */
        s->clockCounter++;if(s->clockCounter>=decimFactor){s->clockHoldL=mixL;s->clockHoldR=mixR;s->clockCounter=0;}
        mixL=s->clockHoldL;mixR=s->clockHoldR;
        if(decimFactor>1){double nAmt=(double)(decimFactor-1)*0.008;mixL+=lb_rand(&s->rng)*nAmt;mixR+=lb_rand(&s->rng)*nAmt;}

        /* Global FX: add the previous block's Palette send returns; capture this block's inputs */
        mixL += (double)s->sretAL[n] + (double)s->sretBL[n];
        mixR += (double)s->sretAR[n] + (double)s->sretBR[n];
        s->sbufAL[n]=(float)sAL; s->sbufAR[n]=(float)sAR;
        s->sbufBL[n]=(float)sBL; s->sbufBR[n]=(float)sBR;
        mixL=global_saturate(mixL,(double)s->globalSat);mixR=global_saturate(mixR,(double)s->globalSat);
        master_wowflutter_stereo(s,&mixL,&mixR,(double)s->globalWowFlut);
        master_comp(&mixL,&mixR,(double)s->masterComp,&s->compEnvL,&s->compEnvR);
        if(s->masterLoCut>21.0f){mixL=bq_L(&s->masterLo,mixL);mixR=bq_R(&s->masterLo,mixR);}
        if(s->masterHiCut<19999.0f){mixL=bq_L(&s->masterHi,mixL);mixR=bq_R(&s->masterHi,mixR);}
        stumble_sample(s,&mixL,&mixR);   /* Perform: master stochastic glitch */
        dropout_sample(s,&mixL,&mixR);
        /* Punch-in FX: up to 4 slots in series, each with its own capture ring */
        { double xl=mixL,xr=mixR;
          for(int si=0;si<4;si++){ PunchSlot *ps=&s->pslot[si];
              if(ps->idx<0){ ps->ringL[ps->w]=(float)mixL; ps->ringR[ps->w]=(float)mixR; }   /* idle: cache dry master */
              else { ps->ringL[ps->w]=(float)xl; ps->ringR[ps->w]=(float)xr;                  /* active: capture chain input */
                  double wl,wr; punch_slot_process(s,ps,&wl,&wr);
                  ps->env += ((ps->releasing?0.0:1.0)-ps->env)*0.02;   /* ~2ms click-free fade */
                  float *P=s->punchParams[ps->idx]; double em=((double)P[3]+(1.0-(double)P[3])*(double)s->punchPress[ps->idx])*ps->env; if(em>1.0)em=1.0;
                  xl=xl+(wl-xl)*em; xr=xr+(wr-xr)*em;
                  if(ps->releasing && ps->env<0.004) ps->idx=-1; }
              ps->w++; if(ps->w>=PUNCH_BUF)ps->w=0; }
          mixL=xl; mixR=xr; }
        mixL*=(double)s->masterVol; mixR*=(double)s->masterVol;   /* master output level */
        master_limiter(&mixL,&mixR,&s->limEnv);   /* analog-style soft limiter (tames glitch clicks) */
        out_interleaved_lr[n*2]=(int16_t)lb_clampd(mixL*32767.0,-32767.0,32767.0);
        out_interleaved_lr[n*2+1]=(int16_t)lb_clampd(mixR*32767.0,-32767.0,32767.0);
    }
    /* Process the two Palette send buses over the whole block (result feeds the next block) */
    if(s->busA){ pfx_process(s->busA,s->sbufAL,s->sbufAR,frames,s->sendAM1,s->sendAM2,s->sendADrift);
        memcpy(s->sretAL,s->sbufAL,(size_t)frames*sizeof(float)); memcpy(s->sretAR,s->sbufAR,(size_t)frames*sizeof(float)); }
    if(s->busB){ pfx_process(s->busB,s->sbufBL,s->sbufBR,frames,s->sendBM1,s->sendBM2,s->sendBDrift);
        memcpy(s->sretBL,s->sbufBL,(size_t)frames*sizeof(float)); memcpy(s->sretBR,s->sbufBR,(size_t)frames*sizeof(float)); }

    /* Decay input peak meters (~50ms decay) */
    s->inputPeakL*=0.95;s->inputPeakR*=0.95;

    /* CPU meter: render time as % of the block budget (frames/SR), smoothed. */
    struct timespec _t1; clock_gettime(CLOCK_MONOTONIC,&_t1);
    double _us=(double)(_t1.tv_sec-_t0.tv_sec)*1e6+(double)(_t1.tv_nsec-_t0.tv_nsec)/1e3;
    double _budget=(double)frames/SR*1e6;
    double _pct=(_budget>0.0)?100.0*_us/_budget:0.0;
    s->cpuPct+=0.1*(_pct-s->cpuPct);
}

/* ---- Parameters ---- */
#define SETFR(k,field,lo,hi) if(strcmp(key,k)==0){s->field=lb_clampf((float)atof(val),(float)(lo),(float)(hi));return;}
#define SETVFR(k,field,lo,hi) if(strcmp(key,k)==0){v->field=lb_clampf((float)atof(val),(float)(lo),(float)(hi));return;}
static const char *preamp_opts[]={"Clean","Cass1","Cass2","VHS1","VHS2","Reel15","Reel7","Reel3","4trk","Porta","Dub","Warp"};
static const char *odmode_opts[]={"Replace","Multiply","Disint"};
static const char *reverse_opts[]={"Normal","Reverse"};
static const char *stkind_opts[]={"Tumble","Stutter","Reverse","Tape","Gate","Crush"};
static int match_enum(const char *value, const char **opts, int count){for(int i=0;i<count;i++)if(strcmp(value,opts[i])==0)return i;return -1;}

/* ---- Overtake control (driven from ui.js via set_param("cmd", ...)) ---- */
/* Single-tap gesture: cycle Empty->Rec->Play<->Pause; from Odub -> Play. */
static void voice_tap(loopbox_t *s, int vi) {
    if(vi<0||vi>=NUM_VOICES)return; Voice *v=&s->voice[vi]; s->selTrack=vi+1;
    switch(v->state){
    case VS_EMPTY: v->state=VS_RECORDING; v->recHead=0; v->loopLen=0; break;
    case VS_RECORDING: v->loopLen=v->recHead; v->playHead=0; v->playPhase=0.0; v->state=VS_PLAYING; break;
    case VS_PLAYING: v->state=VS_PAUSED; break;
    case VS_PAUSED: v->state=VS_PLAYING; break;
    case VS_OVERDUBBING: if((int)s->overdubMode==OD_DISINTEGRATION)voice_disintegrate_pass(v); v->state=VS_PLAYING; break;
    }
}
/* Overdub gesture (double-tap): Play/Pause -> Overdub; Odub -> Play; Rec -> Play. */
static void voice_odub(loopbox_t *s, int vi) {
    if(vi<0||vi>=NUM_VOICES)return; Voice *v=&s->voice[vi]; s->selTrack=vi+1;
    switch(v->state){
    case VS_PLAYING: case VS_PAUSED: v->state=VS_OVERDUBBING; v->recHead=v->playHead; break;
    case VS_OVERDUBBING: if((int)s->overdubMode==OD_DISINTEGRATION)voice_disintegrate_pass(v); v->state=VS_PLAYING; break;
    case VS_RECORDING: v->loopLen=v->recHead; v->playHead=0; v->playPhase=0.0; v->state=VS_PLAYING; break;
    default: break;
    }
}

static void set_param(void *inst, const char *key, const char *val) {
    loopbox_t *s=(loopbox_t*)inst;if(!key||!val)return;
    if(strcmp(key,"cmd")==0){
        const char *c=strchr(val,':'); int vi=c?atoi(c+1):-1;
        if(strncmp(val,"tap",3)==0)        voice_tap(s,vi);
        else if(strncmp(val,"odub",4)==0)  voice_odub(s,vi);
        else if(strncmp(val,"clear",5)==0){ if(vi>=0&&vi<NUM_VOICES)voice_clear(&s->voice[vi]); }
        else if(strncmp(val,"unclr",5)==0){ if(vi>=0&&vi<NUM_VOICES)voice_unclear(&s->voice[vi]); }
        else if(strncmp(val,"mute",4)==0){ if(vi>=0&&vi<NUM_VOICES)s->voice[vi].muted=!s->voice[vi].muted; }
        else if(strncmp(val,"sel",3)==0){ if(vi>=0&&vi<NUM_VOICES)s->selTrack=vi+1; }
        return;
    }
    if(strcmp(key,"punch")==0){ const char *c=strchr(val,':'); int n=c?atoi(c+1):-1;
        if(strncmp(val,"on",2)==0)punch_on(s,n); else if(strncmp(val,"off",3)==0)punch_off(s,n); return; }
    if(strcmp(key,"pfx")==0){ int idx=atoi(val); const char *c1=strchr(val,':'); if(!c1)return; int p=atoi(c1+1);
        const char *c2=strchr(c1+1,':'); if(!c2)return; float v=lb_clampf((float)atof(c2+1),0.0f,1.0f);
        if(idx>=0&&idx<NUM_PUNCH&&p>=0&&p<4){ s->punchParams[idx][p]=v;
            if(p==0){ for(int i=0;i<4;i++) if(s->pslot[i].idx==idx) punch_slot_start(s,&s->pslot[i],idx); } }
        return; }
    if(strcmp(key,"punchPress")==0){ int idx=atoi(val); const char *c=strchr(val,':'); float v=c?lb_clampf((float)atof(c+1),0.0f,1.0f):0.0f; if(idx>=0&&idx<NUM_PUNCH)s->punchPress[idx]=v; return; }
    SETFR("globalSat",globalSat,0.0,1.0) SETFR("masterComp",masterComp,0.0,1.0)
    SETFR("masterLoCut",masterLoCut,20.0,500.0) SETFR("masterHiCut",masterHiCut,1000.0,20000.0)
    SETFR("clock",clock,0.0,1.0) SETFR("masterVol",masterVol,0.0,1.5)
    if(strcmp(key,"preamp")==0){int idx=match_enum(val,preamp_opts,12);if(idx>=0)s->preamp=(float)idx;else s->preamp=lb_clampf((float)atof(val),0.0f,11.0f);return;}
    if(strcmp(key,"overdubMode")==0){int idx=match_enum(val,odmode_opts,3);if(idx>=0)s->overdubMode=(float)idx;else s->overdubMode=lb_clampf((float)atof(val),0.0f,2.0f);return;}
    SETFR("stability",stability,0.0,1.0) SETFR("globalWowFlut",globalWowFlut,0.0,1.0) SETFR("inputMonitor",inputMonitor,0.0,1.0) SETFR("inputGain",inputGain,0.0,2.0)
    SETFR("inLow",inLow,-1.0,1.0) SETFR("inMid",inMid,-1.0,1.0) SETFR("inMidFreq",inMidFreq,0.0,1.0)
    SETFR("inHigh",inHigh,-1.0,1.0) SETFR("inHighFreq",inHighFreq,0.0,1.0)
    if(strcmp(key,"sendAType")==0){int id=-1; for(int i=0;i<PFX_NUM;i++) if(strcmp(val,pfx_name(i))==0){id=i;break;} if(id<0)id=(int)lb_clampf((float)atof(val),0.0f,(float)(PFX_NUM-1)); s->sendAType=id; if(s->busA)pfx_select(s->busA,id); return;}
    if(strcmp(key,"sendBType")==0){int id=-1; for(int i=0;i<PFX_NUM;i++) if(strcmp(val,pfx_name(i))==0){id=i;break;} if(id<0)id=(int)lb_clampf((float)atof(val),0.0f,(float)(PFX_NUM-1)); s->sendBType=id; if(s->busB)pfx_select(s->busB,id); return;}
    SETFR("sendAM1",sendAM1,0.0,1.0) SETFR("sendAM2",sendAM2,0.0,1.0) SETFR("sendADrift",sendADrift,0.0,1.0)
    SETFR("sendBM1",sendBM1,0.0,1.0) SETFR("sendBM2",sendBM2,0.0,1.0) SETFR("sendBDrift",sendBDrift,0.0,1.0)
    SETFR("stMix",stMix,0.0,1.0) SETFR("stStep",stStep,0.0,1.0) SETFR("stOdds",stOdds,0.0,1.0)
    SETFR("stSize",stSize,0.0,1.0) SETFR("stReach",stReach,0.0,1.0) SETFR("dropAmt",dropAmt,0.0,1.0)
    if(strcmp(key,"stKind")==0){int i=match_enum(val,stkind_opts,6); s->stKind=(i>=0)?i:(int)lb_clampf((float)atof(val),0.0f,5.0f); return;}
    if(strcmp(key,"jump")==0){ if((float)atof(val)>0.5f){ for(int i=0;i<NUM_VOICES;i++){Voice *v=&s->voice[i];
        if((v->state==VS_PLAYING||v->state==VS_OVERDUBBING)&&v->loopLen>0){ int es=(int)(v->loopStart*(float)v->loopLen); if(es<0)es=0; if(es>v->loopLen-1)es=v->loopLen-1;
            int av=v->loopLen-es; if(av<1)av=1; int el=(int)(v->loopEnd*(float)av); if(el<256)el=256; if(el>av)el=av;
            double frac=lb_rand(&v->rng)*0.5+0.5; v->playPhase=(double)es+frac*(double)el; } } } return; }
    if(strcmp(key,"scan")==0){ if((float)atof(val)>0.5f)s->scanTimer=(int)(SR*0.4); return; }
    if(strcmp(key,"rootNote")==0){s->rootNote=(int)lb_clampf((float)atof(val),24.0f,96.0f); return;}
    if(strcmp(key,"selTrack")==0){s->selTrack=(int)lb_clampf((float)atof(val),1.0f,16.0f);
        int si=s->selTrack-1;if(si>=0&&si<NUM_VOICES)smp_scan_files(s,s->voice[si].smpDir);return;}
    if(strcmp(key,"clearSel")==0){float t=(float)atof(val);if(t>0.5f){int ci=s->selTrack-1;
        if(ci>=0&&ci<NUM_VOICES)voice_clear(&s->voice[ci]);}return;}
    if(strcmp(key,"clearAll")==0){float t=(float)atof(val);if(t>0.5f){for(int ci=0;ci<NUM_VOICES;ci++)voice_clear(&s->voice[ci]);}return;}
    int selIdx=s->selTrack-1;if(selIdx<0||selIdx>=NUM_VOICES)return;Voice *v=&s->voice[selIdx];
    SETVFR("v_start",loopStart,0.0,1.0) SETVFR("v_end",loopEnd,0.0,1.0)
    if(strcmp(key,"v_reverse")==0){int idx=match_enum(val,reverse_opts,2);if(idx>=0)v->reverse=(float)idx;else v->reverse=lb_clampf((float)atof(val),0.0f,1.0f);return;}
    SETVFR("v_sat",saturation,0.0,1.0) SETVFR("v_wowflut",wowFlutter,0.0,1.0)
    SETVFR("v_send",send,0.0,1.0) SETVFR("v_sendA",send,0.0,1.0) SETVFR("v_sendB",sendB,0.0,1.0)
    SETVFR("v_scatter",scatter,0.0,1.0) SETVFR("v_glitch",glitch,0.0,1.0) SETVFR("v_tilt",tiltEQ,-1.0,1.0)
    SETVFR("v_eqBass",eqBass,-1.0,1.0) SETVFR("v_eqPresFrq",eqPresFreq,0.0,1.0)
    SETVFR("v_eqPresAmt",eqPresAmt,-1.0,1.0) SETVFR("v_eqTreble",eqTreble,-1.0,1.0)
    SETVFR("v_pitch",pitch,-2.0,2.0) SETVFR("v_filter",filter,0.0,1.0)
    SETVFR("v_pan",pan,-1.0,1.0) SETVFR("v_volume",volume,0.0,1.0) SETVFR("v_decay",decay,0.0,1.0)
    SETVFR("v_atk",ampAtk,0.0,1.0) SETVFR("v_rel",ampRel,0.0,1.0)
    if(strcmp(key,"v_djReso")==0){v->djReso=lb_clampf((float)atof(val),0.0f,1.0f);dj_filter_update(v);return;}
    if(strcmp(key,"v_smpDir")==0){int idx=atoi(val);if(idx<0)idx=0;
        if(idx>=s->browser.numDirs)idx=s->browser.numDirs>0?s->browser.numDirs-1:0;
        v->smpDir=idx;smp_scan_files(s,idx);v->smpFile=-1;return;}
    if(strcmp(key,"v_smpFile")==0){int idx=atoi(val);
        if(idx<0||idx>=s->browser.numFiles){v->smpFile=-1;return;}v->smpFile=idx;
        int loaded=smp_load_wav(s->browser.filePaths[idx],v->bufferL,v->bufferR,LOOP_SAMPLES);
        if(loaded>0){v->loopLen=loaded;v->playHead=0;v->playPhase=0.0;v->state=VS_PLAYING;
            v->loopStart=0.0f;v->loopEnd=1.0f;}return;}
    /* State restore: indexed voice params v0.pitch=0.0 ... v15.volume=0.8 */
    if(key[0]=='v'&&key[1]>='0'&&key[1]<='9'){
        int vi=-1;const char *sub=NULL;
        if(key[2]=='.'){vi=key[1]-'0';sub=key+3;}
        else if(key[2]>='0'&&key[2]<='9'&&key[3]=='.'){vi=(key[1]-'0')*10+(key[2]-'0');sub=key+4;}
        if(vi>=0&&vi<NUM_VOICES&&sub){Voice *vr=&s->voice[vi];float fv=(float)atof(val);
            if(strcmp(sub,"srt")==0)vr->loopStart=lb_clampf(fv,0,1);
            else if(strcmp(sub,"end")==0)vr->loopEnd=lb_clampf(fv,0,1);
            else if(strcmp(sub,"rev")==0)vr->reverse=lb_clampf(fv,0,1);
            else if(strcmp(sub,"sat")==0)vr->saturation=lb_clampf(fv,0,1);
            else if(strcmp(sub,"wf")==0)vr->wowFlutter=lb_clampf(fv,0,1);
            else if(strcmp(sub,"snd")==0)vr->send=lb_clampf(fv,0,1);
            else if(strcmp(sub,"gli")==0)vr->glitch=lb_clampf(fv,0,1);
            else if(strcmp(sub,"tlt")==0)vr->tiltEQ=lb_clampf(fv,-1,1);
            else if(strcmp(sub,"eB")==0)vr->eqBass=lb_clampf(fv,-1,1);
            else if(strcmp(sub,"ePF")==0)vr->eqPresFreq=lb_clampf(fv,0,1);
            else if(strcmp(sub,"ePA")==0)vr->eqPresAmt=lb_clampf(fv,-1,1);
            else if(strcmp(sub,"eT")==0)vr->eqTreble=lb_clampf(fv,-1,1);
            else if(strcmp(sub,"pit")==0)vr->pitch=lb_clampf(fv,-2,2);
            else if(strcmp(sub,"fil")==0){vr->filter=lb_clampf(fv,0,1);dj_filter_update(vr);}
            else if(strcmp(sub,"pan")==0)vr->pan=lb_clampf(fv,-1,1);
            else if(strcmp(sub,"vol")==0)vr->volume=lb_clampf(fv,0,1);
            else if(strcmp(sub,"dec")==0)vr->decay=lb_clampf(fv,0,1);
            else if(strcmp(sub,"rso")==0){vr->djReso=lb_clampf(fv,0,1);dj_filter_update(vr);}
            else if(strcmp(sub,"atk")==0)vr->ampAtk=lb_clampf(fv,0,1);
            else if(strcmp(sub,"rel")==0)vr->ampRel=lb_clampf(fv,0,1);
            studer_eq_update(vr);tilt_eq_update(vr);}return;}
}

#define GETP(k,f) if(strcmp(key,k)==0)return snprintf(buf,buf_len,"%.4f",(double)s->f);
#define GETVP(k,f) if(strcmp(key,k)==0)return snprintf(buf,buf_len,"%.4f",(double)v->f);
#define GETE(k,f,opts,cnt) do{if(strcmp(key,k)==0){int _i=(int)roundf(s->f);if(_i<0)_i=0;if(_i>=(cnt))_i=(cnt)-1;return snprintf(buf,buf_len,"%s",(opts)[_i]);}}while(0)

/* ui_hierarchy JSON - MUST be returned from get_param for sound generators */
static const char *UI_HIERARCHY_JSON =
    "{\"modes\":null,\"levels\":{"
    "\"root\":{\"name\":\"LoopBox\","
    "\"knobs\":[\"globalSat\",\"masterComp\",\"masterLoCut\",\"masterHiCut\",\"clock\",\"stability\",\"globalWowFlut\",\"inputMonitor\"],"
    "\"params\":[{\"level\":\"MAIN\",\"label\":\"Main\"},{\"level\":\"FX\",\"label\":\"FX\"},{\"level\":\"CONTROL\",\"label\":\"Control\"},{\"level\":\"LOOP\",\"label\":\"Loop\"}]},"
    "\"MAIN\":{\"label\":\"Main\","
    "\"knobs\":[\"globalSat\",\"masterComp\",\"masterLoCut\",\"masterHiCut\",\"clock\",\"stability\",\"globalWowFlut\",\"inputMonitor\"],"
    "\"params\":[\"globalSat\",\"masterComp\",\"masterLoCut\",\"masterHiCut\",\"clock\",\"stability\",\"globalWowFlut\",\"inputMonitor\"]},"
    "\"FX\":{\"label\":\"FX\","
    "\"knobs\":[\"sendAType\",\"sendAM1\",\"sendAM2\",\"sendADrift\",\"sendBType\",\"sendBM1\",\"sendBM2\",\"sendBDrift\"],"
    "\"params\":[\"sendAType\",\"sendAM1\",\"sendAM2\",\"sendADrift\",\"sendBType\",\"sendBM1\",\"sendBM2\",\"sendBDrift\",\"masterVol\"]},"
    "\"CONTROL\":{\"label\":\"Control\","
    "\"knobs\":[\"preamp\",\"overdubMode\",\"inputGain\",\"clearSel\",\"clearAll\",\"selTrack\",\"selTrack\",\"selTrack\"],"
    "\"params\":[\"preamp\",\"overdubMode\",\"inputGain\",\"clearSel\",\"clearAll\",\"selTrack\"]},"
    "\"LOOP\":{\"label\":\"Loop\","
    "\"knobs\":[\"v_start\",\"v_end\",\"v_reverse\",\"v_sat\",\"v_wowflut\",\"v_send\",\"v_glitch\",\"v_tilt\"],"
    "\"params\":[\"v_start\",\"v_end\",\"v_reverse\",\"v_sat\",\"v_wowflut\",\"v_send\",\"v_glitch\",\"v_tilt\","
    "\"v_eqBass\",\"v_eqPresFrq\",\"v_eqPresAmt\",\"v_eqTreble\","
    "\"v_pitch\",\"v_filter\",\"v_pan\",\"v_volume\",\"v_decay\","
    "\"v_smpDir\",\"v_smpFile\"]}"
    "}}";

/* chain_params JSON */
static const char *CHAIN_PARAMS_JSON =
    "[{\"key\":\"globalSat\",\"name\":\"Sat\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"masterComp\",\"name\":\"Comp\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"masterLoCut\",\"name\":\"LoCut\",\"type\":\"int\",\"min\":20,\"max\":500,\"step\":1},"
    "{\"key\":\"masterHiCut\",\"name\":\"HiCut\",\"type\":\"int\",\"min\":1000,\"max\":20000,\"step\":200},"
    "{\"key\":\"clock\",\"name\":\"Clock\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"stability\",\"name\":\"Stabil\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"globalWowFlut\",\"name\":\"W/Flut\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"inputMonitor\",\"name\":\"InMon\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"masterVol\",\"name\":\"Out\",\"type\":\"float\",\"min\":0,\"max\":1.5,\"step\":0.01},"
    "{\"key\":\"sendAType\",\"name\":\"A Fx\",\"type\":\"enum\",\"options\":[\"Off\",\"Drive\",\"Sweeten\",\"Fuzz\",\"Howl\",\"Fold\",\"Swell\",\"Doubler\",\"Vibrato\",\"Phaser\",\"Tremolo\",\"Pitch\",\"Shift\",\"Cascade\",\"Reels\",\"Collage\",\"Reverse\",\"Space\",\"Bloom\",\"Filter\",\"Squash\",\"Cassette\",\"Broken\",\"Interference\",\"Halo\",\"Plate\"]},"
    "{\"key\":\"sendAM1\",\"name\":\"A Amt\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"sendAM2\",\"name\":\"A Mac\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"sendADrift\",\"name\":\"A Drf\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"sendBType\",\"name\":\"B Fx\",\"type\":\"enum\",\"options\":[\"Off\",\"Drive\",\"Sweeten\",\"Fuzz\",\"Howl\",\"Fold\",\"Swell\",\"Doubler\",\"Vibrato\",\"Phaser\",\"Tremolo\",\"Pitch\",\"Shift\",\"Cascade\",\"Reels\",\"Collage\",\"Reverse\",\"Space\",\"Bloom\",\"Filter\",\"Squash\",\"Cassette\",\"Broken\",\"Interference\",\"Halo\",\"Plate\"]},"
    "{\"key\":\"sendBM1\",\"name\":\"B Amt\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"sendBM2\",\"name\":\"B Mac\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"sendBDrift\",\"name\":\"B Drf\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"preamp\",\"name\":\"Preamp\",\"type\":\"enum\",\"options\":[\"Clean\",\"Cass1\",\"Cass2\",\"VHS1\",\"VHS2\",\"Reel15\",\"Reel7\",\"Reel3\",\"4trk\",\"Porta\",\"Dub\",\"Warp\"]},"
    "{\"key\":\"overdubMode\",\"name\":\"OdMode\",\"type\":\"enum\",\"options\":[\"Replace\",\"Multiply\",\"Disint\"]},"
    "{\"key\":\"inputGain\",\"name\":\"InGain\",\"type\":\"float\",\"min\":0,\"max\":2,\"step\":0.01},"
    "{\"key\":\"clearSel\",\"name\":\"ClrSel\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"clearAll\",\"name\":\"ClrAll\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"selTrack\",\"name\":\"Track\",\"type\":\"int\",\"min\":1,\"max\":16,\"step\":1},"
    "{\"key\":\"v_start\",\"name\":\"Start\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"v_end\",\"name\":\"End\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"v_reverse\",\"name\":\"Rev\",\"type\":\"enum\",\"options\":[\"Normal\",\"Reverse\"]},"
    "{\"key\":\"v_sat\",\"name\":\"Sat\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"v_wowflut\",\"name\":\"W/Flut\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"v_send\",\"name\":\"Send\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"v_glitch\",\"name\":\"Seed\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"v_tilt\",\"name\":\"Tilt\",\"type\":\"float\",\"min\":-1,\"max\":1,\"step\":0.01},"
    "{\"key\":\"v_eqBass\",\"name\":\"Bass\",\"type\":\"float\",\"min\":-1,\"max\":1,\"step\":0.01},"
    "{\"key\":\"v_eqPresFrq\",\"name\":\"MidF\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"v_eqPresAmt\",\"name\":\"MidG\",\"type\":\"float\",\"min\":-1,\"max\":1,\"step\":0.01},"
    "{\"key\":\"v_eqTreble\",\"name\":\"Treble\",\"type\":\"float\",\"min\":-1,\"max\":1,\"step\":0.01},"
    "{\"key\":\"v_pitch\",\"name\":\"Pitch\",\"type\":\"float\",\"min\":-2,\"max\":2,\"step\":0.01},"
    "{\"key\":\"v_filter\",\"name\":\"Filter\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"v_pan\",\"name\":\"Pan\",\"type\":\"float\",\"min\":-1,\"max\":1,\"step\":0.01},"
    "{\"key\":\"v_volume\",\"name\":\"Vol\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"v_decay\",\"name\":\"Decay\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"v_djReso\",\"name\":\"Reso\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"v_atk\",\"name\":\"Atk\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"v_rel\",\"name\":\"Rel\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"v_smpDir\",\"name\":\"SmpDir\",\"type\":\"enum\",\"options\":[\"0\",\"1\",\"2\",\"3\",\"4\",\"5\",\"6\",\"7\",\"8\",\"9\",\"10\",\"11\",\"12\",\"13\",\"14\",\"15\",\"16\",\"17\",\"18\",\"19\",\"20\",\"21\",\"22\",\"23\",\"24\",\"25\",\"26\",\"27\",\"28\",\"29\",\"30\",\"31\"]},"
    "{\"key\":\"v_smpFile\",\"name\":\"Sample\",\"type\":\"enum\",\"options\":[\"-1\",\"0\",\"1\",\"2\",\"3\",\"4\",\"5\",\"6\",\"7\",\"8\",\"9\",\"10\",\"11\",\"12\",\"13\",\"14\",\"15\",\"16\",\"17\",\"18\",\"19\",\"20\",\"21\",\"22\",\"23\",\"24\",\"25\",\"26\",\"27\",\"28\",\"29\",\"30\",\"31\",\"32\",\"33\",\"34\",\"35\",\"36\",\"37\",\"38\",\"39\",\"40\",\"41\",\"42\",\"43\",\"44\",\"45\",\"46\",\"47\",\"48\",\"49\",\"50\",\"51\",\"52\",\"53\",\"54\",\"55\",\"56\",\"57\",\"58\",\"59\",\"60\",\"61\",\"62\",\"63\"]}]";

static int get_param(void *inst, const char *key, char *buf, int buf_len) {
    loopbox_t *s=(loopbox_t*)inst;if(!key)return -1;

    /* Overtake: Manager discovery + UI feedback */
    if(strcmp(key,"module_id")==0)return snprintf(buf,buf_len,"loopbox");
    if(strcmp(key,"cpu")==0)return snprintf(buf,buf_len,"%.1f",s->cpuPct);
    if(strcmp(key,"states")==0){ int p=0;
        for(int i=0;i<NUM_VOICES&&p<buf_len-1;i++) buf[p++]=(char)('0'+(int)s->voice[i].state);
        buf[p]='\0'; return p; }

    /* Sound generators MUST return ui_hierarchy from get_param */
    if(strcmp(key,"ui_hierarchy")==0){int len=(int)strlen(UI_HIERARCHY_JSON);if(len>=buf_len)return -1;
        memcpy(buf,UI_HIERARCHY_JSON,len+1);return len;}
    if(strcmp(key,"chain_params")==0){int len=(int)strlen(CHAIN_PARAMS_JSON);if(len>=buf_len)return -1;
        memcpy(buf,CHAIN_PARAMS_JSON,len+1);return len;}
    if(strcmp(key,"name")==0)return snprintf(buf,buf_len,"LoopBox");

    GETP("globalSat",globalSat) GETP("masterComp",masterComp)
    if(strcmp(key,"masterLoCut")==0)return snprintf(buf,buf_len,"%d",(int)s->masterLoCut);
    if(strcmp(key,"masterHiCut")==0)return snprintf(buf,buf_len,"%d",(int)s->masterHiCut);
    GETP("clock",clock) GETP("masterVol",masterVol)
    GETE("preamp",preamp,preamp_opts,12); GETE("overdubMode",overdubMode,odmode_opts,3);
    GETP("stability",stability) GETP("globalWowFlut",globalWowFlut) GETP("inputMonitor",inputMonitor) GETP("inputGain",inputGain)
    GETP("inLow",inLow) GETP("inMid",inMid) GETP("inMidFreq",inMidFreq) GETP("inHigh",inHigh) GETP("inHighFreq",inHighFreq)
    if(strcmp(key,"rootNote")==0)return snprintf(buf,buf_len,"%d",s->rootNote);
    if(strcmp(key,"sendAType")==0)return snprintf(buf,buf_len,"%s",pfx_name(s->sendAType));
    if(strcmp(key,"sendBType")==0)return snprintf(buf,buf_len,"%s",pfx_name(s->sendBType));
    GETP("sendAM1",sendAM1) GETP("sendAM2",sendAM2) GETP("sendADrift",sendADrift)
    GETP("sendBM1",sendBM1) GETP("sendBM2",sendBM2) GETP("sendBDrift",sendBDrift)
    GETP("stMix",stMix) GETP("stStep",stStep) GETP("stOdds",stOdds) GETP("stSize",stSize) GETP("stReach",stReach) GETP("dropAmt",dropAmt)
    if(strcmp(key,"stKind")==0)return snprintf(buf,buf_len,"%s",stkind_opts[s->stKind]);
    if(strcmp(key,"jump")==0||strcmp(key,"scan")==0)return snprintf(buf,buf_len,"0");
    if(strcmp(key,"inputPeak")==0)return snprintf(buf,buf_len,"%.3f",fmax(s->inputPeakL,s->inputPeakR));
    if(strcmp(key,"selTrack")==0)return snprintf(buf,buf_len,"%d",s->selTrack);
    if(strcmp(key,"clearSel")==0||strcmp(key,"clearAll")==0)return snprintf(buf,buf_len,"0");

    int selIdx=s->selTrack-1;if(selIdx<0||selIdx>=NUM_VOICES)return -1;Voice *v=&s->voice[selIdx];
    GETVP("v_start",loopStart) GETVP("v_end",loopEnd)
    if(strcmp(key,"v_reverse")==0){int _i=(int)roundf(v->reverse);if(_i<0)_i=0;if(_i>1)_i=1;return snprintf(buf,buf_len,"%s",reverse_opts[_i]);}
    GETVP("v_sat",saturation) GETVP("v_wowflut",wowFlutter) GETVP("v_send",send)
    GETVP("v_sendA",send) GETVP("v_sendB",sendB) GETVP("v_scatter",scatter)
    GETVP("v_glitch",glitch) GETVP("v_tilt",tiltEQ)
    GETVP("v_eqBass",eqBass) GETVP("v_eqPresFrq",eqPresFreq) GETVP("v_eqPresAmt",eqPresAmt)
    GETVP("v_eqTreble",eqTreble) GETVP("v_pitch",pitch) GETVP("v_filter",filter)
    GETVP("v_pan",pan) GETVP("v_volume",volume) GETVP("v_decay",decay)
    GETVP("v_djReso",djReso) GETVP("v_atk",ampAtk) GETVP("v_rel",ampRel)

    if(strcmp(key,"v_smpDir")==0){if(s->browser.numDirs==0)return snprintf(buf,buf_len,"(empty)");
        int idx=v->smpDir;if(idx<0||idx>=s->browser.numDirs)idx=0;
        return snprintf(buf,buf_len,"%s",s->browser.dirs[idx]);}
    if(strcmp(key,"v_smpFile")==0){if(v->smpFile<0||v->smpFile>=s->browser.numFiles)return snprintf(buf,buf_len,"(none)");
        return snprintf(buf,buf_len,"%s",s->browser.files[v->smpFile]);}
    if(strcmp(key,"v_state")==0){static const char *sts[]={"Empty","Rec","Play","Pause","Odub"};
        int st=(int)v->state;if(st<0||st>4)st=0;return snprintf(buf,buf_len,"%s",sts[st]);}
    if(strcmp(key,"v_loopLen")==0)return snprintf(buf,buf_len,"%.1f",(double)v->loopLen/SR);

    /* State serialization: dump all global + all 16 voices' params */
    if(strcmp(key,"state")==0){int p=0;
        #define WF(k,val) p+=snprintf(buf+p,buf_len-p,"%s=%.4f\n",k,(double)(val))
        #define WI(k,val) p+=snprintf(buf+p,buf_len-p,"%s=%d\n",k,(int)(val))
        WF("globalSat",s->globalSat);WF("masterComp",s->masterComp);
        WI("masterLoCut",(int)s->masterLoCut);WI("masterHiCut",(int)s->masterHiCut);
        WF("clock",s->clock);WF("masterVol",s->masterVol);
        WI("preamp",(int)s->preamp);WI("overdubMode",(int)s->overdubMode);
        WF("stability",s->stability);WF("globalWowFlut",s->globalWowFlut);WF("inputMonitor",s->inputMonitor);WF("inputGain",s->inputGain);
        WI("selTrack",s->selTrack);
        for(int i=0;i<NUM_VOICES;i++){Voice *vi=&s->voice[i];
            p+=snprintf(buf+p,buf_len-p,"v%d.srt=%.4f\nv%d.end=%.4f\nv%d.rev=%.4f\n",i,(double)vi->loopStart,i,(double)vi->loopEnd,i,(double)vi->reverse);
            p+=snprintf(buf+p,buf_len-p,"v%d.sat=%.4f\nv%d.wf=%.4f\nv%d.snd=%.4f\n",i,(double)vi->saturation,i,(double)vi->wowFlutter,i,(double)vi->send);
            p+=snprintf(buf+p,buf_len-p,"v%d.gli=%.4f\nv%d.tlt=%.4f\n",i,(double)vi->glitch,i,(double)vi->tiltEQ);
            p+=snprintf(buf+p,buf_len-p,"v%d.eB=%.4f\nv%d.ePF=%.4f\nv%d.ePA=%.4f\nv%d.eT=%.4f\n",i,(double)vi->eqBass,i,(double)vi->eqPresFreq,i,(double)vi->eqPresAmt,i,(double)vi->eqTreble);
            p+=snprintf(buf+p,buf_len-p,"v%d.pit=%.4f\nv%d.fil=%.4f\nv%d.pan=%.4f\nv%d.vol=%.4f\nv%d.dec=%.4f\n",
                i,(double)vi->pitch,i,(double)vi->filter,i,(double)vi->pan,i,(double)vi->volume,i,(double)vi->decay);
            p+=snprintf(buf+p,buf_len-p,"v%d.rso=%.4f\nv%d.atk=%.4f\nv%d.rel=%.4f\n",i,(double)vi->djReso,i,(double)vi->ampAtk,i,(double)vi->ampRel);}
        #undef WF
        #undef WI
        return p;}

    /* CRITICAL: return -1 for unknown keys, NOT 0 */
    return -1;
}

static int get_error(void *inst, char *buf, int buf_len) { (void)inst;(void)buf;(void)buf_len; return 0; }

/* ---- API v2 export ---- */
static plugin_api_v2_t api = {
    .api_version      = MOVE_PLUGIN_API_VERSION_2,
    .create_instance  = create_instance,
    .destroy_instance = destroy_instance,
    .on_midi          = on_midi,
    .set_param        = set_param,
    .get_param        = get_param,
    .get_error        = get_error,   /* REQUIRED - missing shifts render_block pointer = SIGSEGV */
    .render_block     = render_block,
};

plugin_api_v2_t *move_plugin_init_v2(const host_api_v1_t *host) { g_host = host; return &api; }
