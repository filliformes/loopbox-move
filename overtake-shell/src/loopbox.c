/*
 * loopbox.c — LoopBox for Ableton Move (Schwung Overtake module)
 *
 * 16-track asynchronous STEREO tape looper.
 * Inspired by: 1010music BlackBox, Kinotone Ribbons, Puremagnetik LAPS,
 *   Chase Bliss Blooper, Mood MK2, Generation Loss MK2, Magneto, norns loopers.
 *
 * Per-voice DSP chain (4 playheads per loop, head 1 = the main play position):
 *   Variable-rate playheads (fwd/bwd/ping, per-head speed) -> Seed slice-reorder
 *   -> Scatter (crossfaded jumps) -> Amp envelope (Atk/Rel, quick mute) ->
 *   DJ filter (+reso) -> Saturation -> Wow/Flutter -> Tilt EQ -> Studer EQ ->
 *   per-track Clock (SR hold + hiss) -> per-track Compressor -> Pan/Vol -> Sends A/B
 *
 * Shared DSP:
 *   Input: 13 preamp/tape models (Tapeless, Clean, cassette, VHS, reel, ...) with
 *     drive / noise / HF loss / lo-cut / wow / flutter / generation loss + 3-band EQ
 *   Global saturation, Stability (tape degradation), Dropout
 *   Palette send buses A/B (26 effects, block-processed, 1-block latency)
 *   Punch-in FX: 16 pad effects over a 2 s capture ring, up to 4 in series
 *   Perform: Stumble / Jump / Scan; MIDI-keyboard poly layer (off by default)
 *   Master: Lo/Hi cut -> Compressor -> Soft limiter -> Output
 *   Sessions: 32 slots, all disk work on a SCHED_OTHER worker (cores 0-2)
 *
 * Overdub modes: Replace / Multiply / Disintegration
 *
 * Realtime rule: every entry point runs on the SPI audio callback. No file I/O,
 * allocation or logging outside create/destroy and the session worker.
 *
 * License: GPL-3.0
 */

#define _GNU_SOURCE
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sched.h>
#include <unistd.h>
#include <sys/stat.h>
#include "plugin_api_v1.h"
#include "palette_fx.h"   /* 24-effect Palette engine for the send buses */

static const host_api_v1_t *g_host = NULL;

#define SR              44100.0
#define TWOPI           (2.0 * M_PI)
#define NUM_VOICES      16
#define LOOP_SECONDS    45
#define LOOP_SAMPLES    ((int)(SR * LOOP_SECONDS))
#define FLUTTER_BUF     1024
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

/* ---- Preamp models (13 types; 0 = Tapeless bypass) ----
 * `tapeNoise` scales the whole noise floor (Tape menu); base floor lowered for a
 * cleaner front end, and Clean's contribution cut hard. */
static inline void apply_preamp_sample(double *l, double *r, int model, double *casLpL, double *casLpR, uint32_t *rng, double nAmt) {
    if(model<=0) return;                       /* Tapeless: true bypass, no colour, no noise */
    double noise=lb_rand(rng)*0.005*nAmt;
    switch(model){
    case 1:*l+=noise*0.05;*r+=noise*0.05;break;   /* Clean */
    case 2:{*l=lb_tanh(*l*1.8)+noise;*r=lb_tanh(*r*1.8)+noise;double k=0.45;*casLpL+=k*(*l-*casLpL);*l=*casLpL;*casLpR+=k*(*r-*casLpR);*r=*casLpR;break;}
    case 3:{*l=atan(*l*1.6)*0.6366+noise;*r=atan(*r*1.6)*0.6366+noise;double k=0.38;*casLpL+=k*(*l-*casLpL);*l=*casLpL;*casLpR+=k*(*r-*casLpR);*r=*casLpR;break;}
    case 4:{*l=lb_tanh(*l*1.3)+noise*1.5;*r=lb_tanh(*r*1.3)+noise*1.5;double k=0.55;*casLpL+=k*(*l-*casLpL);*l=*casLpL;*casLpR+=k*(*r-*casLpR);*r=*casLpR;break;}
    case 5:{*l=tape_sat(*l*1.1)+noise*0.5;*r=tape_sat(*r*1.1)+noise*0.5;double k=0.3;*casLpL+=k*(*l-*casLpL);*l=*casLpL;*casLpR+=k*(*r-*casLpR);*r=*casLpR;break;}
    case 6:*l=tape_sat(*l)+noise*0.15;*r=tape_sat(*r)+noise*0.15;break;
    case 7:{*l=tape_sat(*l*1.15)+noise*0.3;*r=tape_sat(*r*1.15)+noise*0.3;double k=0.25;*casLpL+=k*(*l-*casLpL);*l=*casLpL;*casLpR+=k*(*r-*casLpR);*r=*casLpR;break;}
    case 8:{*l=sin(lb_clampd(*l*1.5,-1.5,1.5))+noise*0.5;*r=sin(lb_clampd(*r*1.5,-1.5,1.5))+noise*0.5;double k=0.5;*casLpL+=k*(*l-*casLpL);*l=*casLpL;*casLpR+=k*(*r-*casLpR);*r=*casLpR;break;}
    case 9:{*l=lb_tanh(*l*1.6)+noise*0.6;*r=lb_tanh(*r*1.6)+noise*0.6;double k=0.42;*casLpL+=k*(*l-*casLpL);*l=*casLpL;*casLpR+=k*(*r-*casLpR);*r=*casLpR;break;}
    case 10:{*l=sin(lb_clampd(*l*1.8,-1.5,1.5))+noise*0.8;*r=sin(lb_clampd(*r*1.8,-1.5,1.5))+noise*0.8;double k=0.48;*casLpL+=k*(*l-*casLpL);*l=*casLpL;*casLpR+=k*(*r-*casLpR);*r=*casLpR;break;}
    case 11:{*l=lb_tanh(*l*2.2)*0.85+noise*0.4;*r=lb_tanh(*r*2.2)*0.85+noise*0.4;double k=0.52;*casLpL+=k*(*l-*casLpL);*l=*casLpL;*casLpR+=k*(*r-*casLpR);*r=*casLpR;break;}
    case 12:{double al=fabs(*l),ar=fabs(*r);double spL=(al>0.001)?sin(*l*al)/al:*l;double spR=(ar>0.001)?sin(*r*ar)/ar:*r;
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

/* One read head over a loop: 0=off 1=fwd 2=bwd 3=ping-pong. Head 0 shares the
 * voice's playPhase (so scatter/Seed/scrub/jump keep driving it). */
typedef struct { int mode; float spd, spdCache; double mult, phase, env; int dir; } Playhead;
typedef enum { OD_REPLACE=0, OD_MULTIPLY=1, OD_DISINTEGRATION=2 } OverdubMode;
typedef enum { VS_EMPTY=0, VS_RECORDING, VS_PLAYING, VS_PAUSED, VS_OVERDUBBING } VoiceState;

/* ---- Voice (stereo buffers) ---- */
typedef struct {
    int16_t *bufferL, *bufferR;
    int loopLen, playHead, recHead;
    VoiceState state;
    float loopStart, loopEnd, reverse;
    float saturation, wowFlutter, send, glitch, tiltEQ;   /* 'send' = Send A */
    float sendB, scatter; int scatterCnt;                 /* Send B + per-voice Scatter */
    float eqBass, eqPresFreq, eqPresAmt, eqTreble;
    float pitch, filter, pan, volume, decay;
    Biquad djLpA, djHpA;
    Biquad eqLow, eqMid, eqHigh, tiltLo, tiltHi;
    double flutBufL[FLUTTER_BUF], flutBufR[FLUTTER_BUF];
    int flutWr; double flutSweep, flutNextMax;
    int glN, glOrder[16], glRev[16], glLastSlice; double glPrevAbs; float glKnobCache;  /* Seed slice-reorder */
    double playPhase, stabLpStateL, stabLpStateR;
    uint32_t rng;
    int djMode;   /* -1 = LP active, +1 = HP active, 0 = bypass (transparent at centre) */
    double playEnv; /* click-free start/stop envelope (ramps 0<->1 on play/pause) */
    double scatXfadePhase; int scatXfade; /* scatter jump crossfade (declick): old read head + countdown */
    int muted;                            /* quick mute: gate output, playhead keeps running */
    int armed;                            /* threshold-armed record: waiting for input to cross */
    Playhead ph[4];                       /* up to 4 simultaneous read heads */
    int scrubTimer; double scrubRate;     /* jog scrub: audible tape rock */
    float filterSm; double volSm, panSm;  /* 10ms smoothing on the steppy knobs */
    int savedLoopLen;                     /* clear-undo: last loop length before a clear */
    float djReso;                         /* DJ filter resonance (Q) */
    float comp, clock;                    /* per-track compressor amount, per-track clock (0.5 = 1x) */
    double cEnvL, cEnvR, ckHoldL, ckHoldR; int ckCnt;
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
enum { PM_REPEAT=0, PM_PITCH, PM_REVERSE, PM_HAZE, PM_SHIMMER, PM_STRETCH,
       PM_MOSAIC, PM_SMEAR, PM_STRUM, PM_GLIDE, PM_CHOP, PM_NONE };
#define SHBUF 8192
typedef struct { int mech; double param; } PunchDef;   /* param = division (REPEAT) or ratio/default */
static const PunchDef PUNCH_DEFS[NUM_PUNCH] = {   /* right 4x4, top->bottom, grouped by family */
    {PM_REPEAT,4},{PM_REPEAT,3},{PM_REPEAT,8},{PM_REPEAT,16},        /* Loops:  Loop16 Loop12 LoopSh LoopSr */
    {PM_HAZE,0},{PM_MOSAIC,0},{PM_SMEAR,0},{PM_STRUM,0},              /* Grains: Haze Mosaic Smear Strum */
    {PM_PITCH,2.0},{PM_PITCH,0.5},{PM_GLIDE,0},{PM_SHIMMER,0},        /* Pitch:  Oct+ Oct- Glide Shimmer */
    {PM_STRETCH,0.5},{PM_STRETCH,1.0},{PM_REVERSE,1.0},{PM_CHOP,0}    /* Time:   Stretch Freeze Reverse Chop */
};
/* One active punch slot: its own capture ring + running state (up to 4 in series). */
typedef struct {
    int idx;                                   /* effect 0..15, or -1 = empty */
    double env; int releasing;                 /* click-free fade-in / fade-out on press/release */
    float ringL[PUNCH_BUF], ringR[PUNCH_BUF]; int w;
    double readPhase, sliceStart, sliceLen;
    Biquad toneFilt;
    /* granular pool (Haze / Mosaic / Smear / Strum / Stretch share it) */
    double gPos[4],gAge[4],gDur[4],gRate[4],gGl[4],gGr[4]; int gAct[4];
    double gSched, stGrid; uint32_t gRng; int gIdx;
    /* glide: per-repeat rate ramp; chop: onset slice + pattern */
    double glRate; int glCycle; int chopStep; uint32_t chopPat;
    /* shimmer 2-head pitch-shift + LP feedback */
    float shL[SHBUF], shR[SHBUF]; int shW; double shR1, shFbL, shFbR;
} PunchSlot;

typedef struct {
    float globalSat,masterComp,masterLoCut,masterHiCut,masterVol;
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
    double compEnvL,compEnvR,casLpL,casLpR;uint32_t rng;
    double cpuPct;   /* smoothed render_block load, % of block budget (Overtake CPU meter) */
    PolyVoice poly[POLY_VOICES]; int rootNote;   /* MIDI-keyboard playback layer */
    /* Punch-in FX (master insert): up to 4 slots in series, per-effect params */
    float punchParams[NUM_PUNCH][4];   /* [effect][Rate,Pitch,Tone,Mix] */
    float punchPress[NUM_PUNCH];        /* per-effect pad pressure */
    PunchSlot pslot[4];
    /* Input FX (record chain): full EQ + record tape speed */
    float inLow,inMid,inMidFreq,inHigh,inHighFreq;
    /* Tape menu (Capture button) — modelled on Magneto's Tape page */
    float tapeNoise,tapeDrive,tapeHF,tapeLoCut,tapeWow,tapeFlut,tapeGen;
    int midiIn;              /* 0 = ignore external MIDI (default), 1 = keyboard layer on */
    float armThresh;         /* threshold-armed record level (0..1) */
    Biquad inEqLo,inEqMid,inEqHi,inTapeLp,inTapeHp;
    double iFlutBufL[FLUTTER_BUF],iFlutBufR[FLUTTER_BUF]; int iFlutWr; double iFlutPhW,iFlutPhF;
    double genLpL,genLpR;
    /* Session save/load — all disk work happens on a SCHED_OTHER worker (cores 0-2),
     * never on the audio callback. Handshake is atomics only. */
    struct {
        atomic_int request;      /* 0 idle, 1 save, 2 load */
        atomic_int slot, busy, cancel, applyState, status;   /* status: 0 idle 1 sav 2 load 3 ok 4 err */
        atomic_int cloneSrc, cloneDst;                        /* request 3 = clone a loop */
        pthread_t th; int active;
        char stateBuf[16384];
        char names[33][40];      /* slot names (1-based), filled by the worker; '' = empty */
    } sio;
    double gFlutBufL[FLUTTER_BUF],gFlutBufR[FLUTTER_BUF];int gFlutWr;double gFlutSweep,gFlutNextMax;
} loopbox_t;

static void set_param(void *inst, const char *key, const char *val);
static int  get_param(void *inst, const char *key, char *buf, int buf_len);

/* ---- Session store: settings blob + raw int16 loop buffers, 8 numbered slots ----
 * Layout: /data/UserData/schwung/loopbox-sessions/slotN/{state.txt,meta.txt,tKK.raw}
 * (outside the module dir so reinstalls keep sessions — per the Overtake SDK). */
#define SESS_DIR_BASE "/data/UserData/schwung/loopbox-sessions"
#define NUM_SLOTS 32
/* Worker-only: refresh the slot-name cache from disk. */
static void session_scan_names(loopbox_t *s){
    char path[352];
    for(int n=1;n<=NUM_SLOTS;n++){ s->sio.names[n][0]=0;
        snprintf(path,sizeof path,"%s/slot%d/name.txt",SESS_DIR_BASE,n);
        FILE *f=fopen(path,"r"); if(!f)continue;
        if(fgets(s->sio.names[n],(int)sizeof(s->sio.names[n]),f)){ size_t L=strlen(s->sio.names[n]);
            while(L&&(s->sio.names[n][L-1]=='\n'||s->sio.names[n][L-1]=='\r')) s->sio.names[n][--L]=0; }
        fclose(f); }
}
static void *session_worker(void *arg){
    loopbox_t *s=(loopbox_t*)arg;
    char dir[256],path[352];
    while(1){
        while(!atomic_load(&s->sio.request)&&!atomic_load(&s->sio.cancel)) usleep(50000);
        if(atomic_load(&s->sio.cancel)) break;
        int req=atomic_exchange(&s->sio.request,0);
        int slot=atomic_load(&s->sio.slot);
        atomic_store(&s->sio.busy,1);
        snprintf(dir,sizeof dir,"%s/slot%d",SESS_DIR_BASE,slot);
        if(req==1){                                   /* ---- SAVE ---- */
            atomic_store(&s->sio.status,1);
            mkdir("/data/UserData/schwung",0777); mkdir(SESS_DIR_BASE,0777); mkdir(dir,0777);
            snprintf(path,sizeof path,"%s/state.txt",dir);
            FILE *f=fopen(path,"w"); if(f){ fputs(s->sio.stateBuf,f); fclose(f); }
            for(int i=0;i<NUM_VOICES;i++){
                Voice *v=&s->voice[i]; int len=v->loopLen;
                snprintf(path,sizeof path,"%s/t%02d.raw",dir,i);
                if(len<=0){ remove(path); continue; }
                FILE *g=fopen(path,"wb");
                if(g){ fwrite(v->bufferL,sizeof(int16_t),(size_t)len,g);
                       fwrite(v->bufferR,sizeof(int16_t),(size_t)len,g); fclose(g); }
            }
            snprintf(path,sizeof path,"%s/meta.txt",dir);
            FILE *m=fopen(path,"w");
            if(m){ for(int i=0;i<NUM_VOICES;i++) fprintf(m,"%d %d\n",s->voice[i].loopLen,(int)s->voice[i].state); fclose(m); }
            { time_t t=time(NULL); struct tm tmv; localtime_r(&t,&tmv); char nm[40];
              snprintf(nm,sizeof nm,"%02d_%04d%02d%02d_%02d%02d",slot,tmv.tm_year+1900,tmv.tm_mon+1,tmv.tm_mday,tmv.tm_hour,tmv.tm_min);
              snprintf(path,sizeof path,"%s/name.txt",dir); FILE *nf=fopen(path,"w"); if(nf){ fputs(nm,nf); fclose(nf); } }
            session_scan_names(s);
            atomic_store(&s->sio.status,3);
        } else if(req==2){                            /* ---- LOAD ---- */
            atomic_store(&s->sio.status,2);
            int lens[NUM_VOICES],sts[NUM_VOICES];
            snprintf(path,sizeof path,"%s/meta.txt",dir);
            FILE *m=fopen(path,"r");
            if(!m){ atomic_store(&s->sio.status,4); atomic_store(&s->sio.busy,0); continue; }
            for(int i=0;i<NUM_VOICES;i++) if(fscanf(m,"%d %d",&lens[i],&sts[i])!=2){ lens[i]=0; sts[i]=0; }
            fclose(m);
            for(int i=0;i<NUM_VOICES;i++){
                Voice *v=&s->voice[i];
                v->state=VS_EMPTY; v->loopLen=0;      /* render now skips this voice — safe to fill */
                v->playPhase=0.0; v->playHead=0; v->playEnv=0.0; v->muted=0; v->glLastSlice=-1;
                int len=lens[i]; if(len>LOOP_SAMPLES)len=LOOP_SAMPLES; if(len<=0) continue;
                snprintf(path,sizeof path,"%s/t%02d.raw",dir,i);
                FILE *g=fopen(path,"rb"); if(!g) continue;
                size_t gl=fread(v->bufferL,sizeof(int16_t),(size_t)len,g);
                size_t gr=fread(v->bufferR,sizeof(int16_t),(size_t)len,g);
                fclose(g);
                int n=(int)(gl<gr?gl:gr);
                v->savedLoopLen=n;
                v->loopLen=n;                          /* publish length LAST */
                v->state=(sts[i]==VS_PLAYING||sts[i]==VS_OVERDUBBING)?VS_PLAYING:VS_PAUSED;
            }
            snprintf(path,sizeof path,"%s/state.txt",dir);
            FILE *f=fopen(path,"r");
            if(f){ size_t n=fread(s->sio.stateBuf,1,sizeof(s->sio.stateBuf)-1,f);
                   s->sio.stateBuf[n]='\0'; fclose(f); atomic_store(&s->sio.applyState,1); }
            session_scan_names(s);
            atomic_store(&s->sio.status,3);
        } else if(req==4){ session_scan_names(s); atomic_store(&s->sio.status,0); }   /* scan only */
        else if(req==3){                              /* ---- CLONE a loop (off-callback memcpy) ---- */
            int a=atomic_load(&s->sio.cloneSrc), b=atomic_load(&s->sio.cloneDst);
            if(a>=0&&a<NUM_VOICES&&b>=0&&b<NUM_VOICES&&a!=b){
                Voice *src=&s->voice[a],*dst=&s->voice[b];
                int len=src->loopLen; if(len>LOOP_SAMPLES)len=LOOP_SAMPLES;
                dst->state=VS_EMPTY; dst->loopLen=0;   /* render skips it while we copy */
                dst->playPhase=0.0; dst->playHead=0; dst->playEnv=0.0; dst->glLastSlice=-1; dst->muted=0;
                if(len>0){
                    memcpy(dst->bufferL,src->bufferL,(size_t)len*sizeof(int16_t));
                    memcpy(dst->bufferR,src->bufferR,(size_t)len*sizeof(int16_t));
                    dst->loopStart=src->loopStart; dst->loopEnd=src->loopEnd; dst->reverse=src->reverse;
                    dst->pitch=src->pitch; dst->filter=src->filter; dst->pan=src->pan; dst->volume=src->volume;
                    dst->saturation=src->saturation; dst->wowFlutter=src->wowFlutter;
                    dst->send=src->send; dst->sendB=src->sendB; dst->glitch=src->glitch; dst->scatter=src->scatter;
                    dst->savedLoopLen=len;
                    dst->loopLen=len;                  /* publish length LAST */
                    dst->state=VS_PLAYING;
                }
            }
            atomic_store(&s->sio.status,3);
        }
        atomic_store(&s->sio.busy,0);
    }
    return NULL;
}

/* ---- DJ Filter (single-pole-smooth, Essaim-style: continuous sweep, low Q,
 * transparent at centre, one 12dB/oct biquad per side, state reset on LP<->HP) ---- */
static void dj_filter_update(Voice *v) {
    double f=(double)v->filterSm;
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
    double mk=(amt<0.05)?amt/0.05:1.0;   /* fade the makeup in: no level step when the knob leaves 0 */
    double makeupDb=(0.0-thDb)*(1.0-1.0/ratio)*0.5*mk;
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
/* Synced autopan: `ph` is the effect's own cycle phase (0..1), so the image
 * swings in time with the repeat/grain rate. Equal-power, with a little makeup. */
static inline void punch_autopan(double ph, double depth, double *l, double *r){
    double a=0.5+0.5*sin(TWOPI*ph)*depth;               /* 0..1 position */
    double gl=cos(a*0.5*M_PI)*1.2, gr=sin(a*0.5*M_PI)*1.2;
    *l*=gl; *r*=gr;
}
static inline double punch_beat(void){
    double bpm=(g_host&&g_host->get_bpm)?(double)g_host->get_bpm():120.0; if(bpm<20.0)bpm=120.0;
    return SR*60.0/bpm;
}
/* Slice geometry for the slice-based mechs (REPEAT / REVERSE / GLIDE / CHOP). */
static double punch_slice_len(const PunchDef *d, const float *P, double beat){
    double sl;
    if(d->mech==PM_REVERSE)      sl=beat*(0.25+(double)P[0]*1.75);
    else if(d->mech==PM_CHOP)    sl=beat/(double)(1<<(int)((double)P[0]*3.99));
    else if(d->mech==PM_GLIDE)   sl=beat/4.0*pow(4.0,((double)P[0]-0.5)*2.0);
    else                         sl=beat/d->param*pow(4.0,((double)P[0]-0.5)*2.0);   /* REPEAT */
    if(sl<256.0)sl=256.0; if(sl>PUNCH_BUF/2)sl=PUNCH_BUF/2;
    return sl;
}
/* Start a slot on effect idx, using that effect's stored params (s->punchParams[idx]). */
static void punch_slot_start(loopbox_t *s, PunchSlot *ps, int idx){
    ps->idx=idx; ps->env=0.0; ps->releasing=0; const PunchDef *d=&PUNCH_DEFS[idx]; float *P=s->punchParams[idx];
    double beat=punch_beat();
    if(ps->gRng==0)ps->gRng=0x1234567u+(uint32_t)idx*2654435761u;
    if(d->mech==PM_REPEAT||d->mech==PM_REVERSE||d->mech==PM_GLIDE||d->mech==PM_CHOP){
        double sl=punch_slice_len(d,P,beat);
        ps->sliceLen=sl; int st=(((int)ps->w-(int)sl)%PUNCH_BUF+PUNCH_BUF)%PUNCH_BUF; ps->sliceStart=(double)st;
        ps->readPhase=(d->mech==PM_REVERSE)?(sl-1.0):0.0;
        ps->glRate=1.0; ps->glCycle=0; ps->chopStep=0;
        if(d->mech==PM_CHOP){
            /* Onset: the loudest moment in the last beat becomes the hit we chop. */
            int span=(int)beat; if(span>PUNCH_BUF-64)span=PUNCH_BUF-64; int best=0; float pk=0.0f;
            for(int j=0;j<span;j+=32){ int ix=(ps->w-1-j+PUNCH_BUF)%PUNCH_BUF; float a=ps->ringL[ix]; if(a<0)a=-a; if(a>pk){pk=a;best=j;} }
            int st2=(ps->w-1-best-160+PUNCH_BUF)%PUNCH_BUF; ps->sliceStart=(double)st2;   /* 160-sample pre-roll */
            /* Rhythm from P1: a seeded 16-step pattern whose density follows the knob. */
            uint32_t r=(uint32_t)((double)P[1]*997.0)*2654435761u+7u; ps->chopPat=0;
            for(int b=0;b<16;b++){ r=1664525u*r+1013904223u; if(((r>>16)&255)<(uint32_t)(40+(double)P[1]*200.0)) ps->chopPat|=(1u<<b); }
            ps->chopPat|=1u;   /* the downbeat always hits */
        }
    }
    else if(d->mech==PM_PITCH){ ps->readPhase=2048.0; }   /* mid-window delay (2-head shifter) */
    else if(d->mech==PM_HAZE||d->mech==PM_STRETCH||d->mech==PM_MOSAIC||d->mech==PM_SMEAR||d->mech==PM_STRUM){
        for(int i=0;i<4;i++)ps->gAct[i]=0; ps->gSched=0.0; ps->gIdx=0; ps->stGrid=(double)ps->w-4000.0; }
    else if(d->mech==PM_SHIMMER){ ps->shR1=(double)ps->shW; ps->shFbL=ps->shFbR=0.0;
        memset(ps->shL,0,sizeof ps->shL); memset(ps->shR,0,sizeof ps->shR); }   /* stale buffer = burst/click on engage */
}
/* Re-tune slice geometry WITHOUT restarting the slot (keeps env + relative phase),
 * so turning Rate on a held effect no longer re-triggers it and clicks. */
static void punch_slot_retune(loopbox_t *s, PunchSlot *ps, int idx){
    const PunchDef *d=&PUNCH_DEFS[idx]; float *P=s->punchParams[idx];
    if(d->mech!=PM_REPEAT&&d->mech!=PM_REVERSE&&d->mech!=PM_GLIDE&&d->mech!=PM_CHOP) return;
    double sl=punch_slice_len(d,P,punch_beat());
    double frac=(ps->sliceLen>1.0)?(ps->readPhase/ps->sliceLen):0.0;   /* keep relative position */
    ps->sliceLen=sl;
    if(d->mech!=PM_CHOP){ int st=(((int)ps->w-(int)sl)%PUNCH_BUF+PUNCH_BUF)%PUNCH_BUF; ps->sliceStart=(double)st; }
    ps->readPhase=frac*sl;
    if(ps->readPhase>=sl)ps->readPhase=sl-1.0; if(ps->readPhase<0.0)ps->readPhase=0.0;
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
/* Spawn a grain in the shared pool. rate<0 reads backwards. Returns the slot or -1. */
static inline int punch_grain(PunchSlot *ps, double pos, double dur, double rate, double pan){
    for(int i=0;i<4;i++) if(!ps->gAct[i]){ ps->gAct[i]=1; ps->gAge[i]=0.0; ps->gDur[i]=dur; ps->gRate[i]=rate; ps->gPos[i]=pos;
        ps->gGl[i]=0.5*(1.0-pan); ps->gGr[i]=0.5*(1.0+pan); return i; }
    return -1;
}
/* Sum the active grains (Hann windows). */
static inline void punch_grains_out(PunchSlot *ps, double *sl, double *sr){
    double l=0.0,r=0.0;
    for(int i=0;i<4;i++){ if(!ps->gAct[i])continue; double wph=ps->gAge[i]/ps->gDur[i]; if(wph>=1.0){ps->gAct[i]=0;continue;}
        double win=0.5-0.5*cos(TWOPI*wph), rp=ps->gPos[i]+ps->gAge[i]*ps->gRate[i];
        l+=(double)ring_read(ps->ringL,rp)*win*ps->gGl[i]; r+=(double)ring_read(ps->ringR,rp)*win*ps->gGr[i]; ps->gAge[i]+=1.0; }
    *sl=l; *sr=r;
}
/* per-sample: one slot reads its own ring and produces wet (ring already written by caller).
 * Pressure (s->punchPress) drives each effect's most musical parameter — see the UI map. */
static inline void punch_slot_process(loopbox_t *s, PunchSlot *ps, double *outL, double *outR){
    const PunchDef *d=&PUNCH_DEFS[ps->idx]; float *P=s->punchParams[ps->idx]; int toneOn=(P[2]<0.98f);
    double press=(double)s->punchPress[ps->idx];
    double pm=pow(2.0,((double)P[1]-0.5)*2.0);
    if(d->mech==PM_HAZE||d->mech==PM_SMEAR){   /* granular clouds: P0=Size P1=Pitch P2=Density; pressure = density */
        int smear=(d->mech==PM_SMEAR);
        double dur =smear?((0.15+(double)P[0]*0.65)*SR):((0.03+(double)P[0]*0.4)*SR);
        double dens=(smear?(1.0+(double)P[2]*12.0):(2.0+(double)P[2]*40.0))*(1.0+press*3.0);
        double pr=pm;
        ps->gSched+=dens/SR;
        if(ps->gSched>=1.0){ ps->gSched-=1.0;
            double back=dur*1.5+PRND(ps->gRng)*dur*3.0; double pos=(double)ps->w-back;
            double pan=(PRND(ps->gRng)*2.0-1.0)*(smear?0.9:0.6);
            double rate=pr; if(smear&&PRND(ps->gRng)<0.5){ rate=-pr; pos+=dur*rate*-1.0; }   /* reversed grains land ahead of their read */
            punch_grain(ps,pos,dur,rate,pan); }
        double sl,sr; punch_grains_out(ps,&sl,&sr);
        double g=smear?0.8:0.9; *outL=sl*g; *outR=sr*g; return; }
    if(d->mech==PM_MOSAIC){   /* grid-synced re-sequencer: P0=Grid P1=Pitch P2=Var; pressure = grid x2 */
        double beat=punch_beat(); int dv=1<<(int)((double)P[0]*3.99); double grid=beat/(double)dv;
        if(press>0.5)grid*=0.5;
        ps->gSched+=1.0;
        if(ps->gSched>=grid){ ps->gSched-=grid;
            double var=(double)P[2];
            int cell=1+(int)(PRND(ps->gRng)*(1.0+var*7.0));            /* which earlier grid cell to quote */
            double pos=(double)ps->w-(double)cell*grid;
            if(var>0.0) pos-=PRND(ps->gRng)*grid*var*0.5;
            double rate=(PRND(ps->gRng)<var*0.6)?-pm:pm;
            if(rate<0) pos+=grid;                                     /* read backwards from the cell end */
            double pan=(ps->gIdx&1)?0.6:-0.6; ps->gIdx++;
            punch_grain(ps,pos,grid*0.95,rate,pan); }
        double sl,sr; punch_grains_out(ps,&sl,&sr); *outL=sl; *outR=sr; return; }
    if(d->mech==PM_STRUM){   /* arpeggiated grain cascade: P0=Rate P1=Dir/Range P2=Tone; pressure = faster + wider */
        double ivl=(0.06+(1.0-(double)P[0])*0.4)*SR*(1.0-press*0.6);
        ps->gSched+=1.0;
        if(ps->gSched>=ivl){ ps->gSched-=ivl;
            static const double up[4]={0,7,12,19}, dn[4]={0,-5,-12,-17};
            int k=ps->gIdx&3; double semis=((double)P[1]>=0.5)?up[k]:dn[k];
            if(press>0.7) semis+=((double)P[1]>=0.5)?12.0:-12.0;
            double rate=pow(2.0,semis/12.0);
            double pos=(double)ps->w-ivl*2.0-200.0; if(rate>1.0) pos-=ivl*rate;   /* faster reads need more room */
            double pan=-0.7+(double)k/3.0*1.4;
            punch_grain(ps,pos,ivl*1.6,rate,pan); ps->gIdx++; }
        double sl,sr; punch_grains_out(ps,&sl,&sr);
        if(toneOn){ sl=bq_L(&ps->toneFilt,sl); sr=bq_R(&ps->toneFilt,sr); } *outL=sl; *outR=sr; return; }
    if(d->mech==PM_STRETCH){   /* 2-grain OLA stretch/freeze: P0=stretch(1=freeze) P1=Pitch P2=Grain; pressure = toward freeze */
        double dur=(0.04+(double)P[2]*0.3)*SR, pr=pm, srate=(1.0-(double)P[0])*(1.0-press);
        ps->stGrid+=srate;
        for(int i=0;i<2;i++){ if(!ps->gAct[i]||ps->gAge[i]>=ps->gDur[i]){ ps->gAct[i]=1; ps->gAge[i]=(i==1)?(-dur*0.5):0.0; ps->gDur[i]=dur; ps->gPos[i]=ps->stGrid; } }
        double sl=0.0,sr=0.0,wsum=0.0;
        for(int i=0;i<2;i++){ double a=ps->gAge[i]; if(a<0.0){ps->gAge[i]+=1.0;continue;} double wph=a/ps->gDur[i]; if(wph>=1.0){ps->gAct[i]=0;continue;}
            double win=0.5-0.5*cos(TWOPI*wph), rp=ps->gPos[i]+a*pr;
            sl+=(double)ring_read(ps->ringL,rp)*win; sr+=(double)ring_read(ps->ringR,rp)*win; wsum+=win; ps->gAge[i]+=1.0; }
        double n=(wsum>0.01)?1.0/wsum:1.0; *outL=sl*n; *outR=sr*n; return; }
    if(d->mech==PM_SHIMMER){   /* 2-head pitch-shift in band-limited feedback: P0=Regen P1=Pitch P2=Tone; pressure = regen */
        double ratio=1.0+(double)P[1], regen=(double)P[0]*0.6+press*0.35; if(regen>0.95)regen=0.95;
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
        double ratio=d->param*pm*(1.0+((double)P[0]-0.5)*0.1);
        const double W=4096.0;
        ps->readPhase+=(1.0-ratio); while(ps->readPhase>=W)ps->readPhase-=W; while(ps->readPhase<0)ps->readPhase+=W;
        double d1=ps->readPhase, d2=d1+W*0.5; if(d2>=W)d2-=W;
        double w1=0.5-0.5*cos(TWOPI*d1/W), w2=0.5-0.5*cos(TWOPI*d2/W), ws=w1+w2+1e-9;
        double rp1=(double)ps->w-d1, rp2=(double)ps->w-d2;
        double l=((double)ring_read(ps->ringL,rp1)*w1+(double)ring_read(ps->ringL,rp2)*w2)/ws;
        double r=((double)ring_read(ps->ringR,rp1)*w1+(double)ring_read(ps->ringR,rp2)*w2)/ws;
        if(toneOn){ l=bq_L(&ps->toneFilt,l); r=bq_R(&ps->toneFilt,r); } *outL=l; *outR=r; return; }
    /* ---- slice-based mechs: REPEAT / REVERSE / GLIDE / CHOP (all autopanned to their cycle) ---- */
    double pos, gate=1.0, bf=1.0, el=ps->sliceLen; const double FD=64.0;
    if(d->mech==PM_REPEAT){          /* pressure subdivides the loop: 1/1 -> 1/2 -> 1/4 */
        el=ps->sliceLen*pow(0.5,floor(press*2.99)); if(el<128.0)el=128.0;
        pos=ps->sliceStart+ps->readPhase; ps->readPhase+=pm;
        while(ps->readPhase>=el)ps->readPhase-=el; while(ps->readPhase<0)ps->readPhase+=el; }
    else if(d->mech==PM_REVERSE){    /* pressure shortens the slice */
        el=ps->sliceLen*(1.0-press*0.6); if(el<128.0)el=128.0;
        if(ps->readPhase>=el)ps->readPhase=el-1.0;
        pos=ps->sliceStart+ps->readPhase; ps->readPhase-=pm;
        while(ps->readPhase<0)ps->readPhase+=el; while(ps->readPhase>=el)ps->readPhase-=el; }
    else if(d->mech==PM_GLIDE){      /* each repeat re-pitches: P1 = down/up, pressure = harder glide */
        pos=ps->sliceStart+ps->readPhase; ps->readPhase+=pm*ps->glRate;
        if(ps->readPhase>=el||ps->readPhase<0){
            ps->readPhase=(ps->readPhase>=el)?(ps->readPhase-el):(ps->readPhase+el);
            double amt=((double)P[1]-0.5)*(0.5+press*1.0);              /* -0.75..+0.75 per cycle */
            ps->glRate*=pow(2.0,amt*0.5); ps->glCycle++;
            if(ps->glRate<0.2||ps->glRate>5.0||ps->glCycle>=8){ ps->glRate=1.0; ps->glCycle=0; } } }
    else {                           /* CHOP: repeat the last hit on a seeded pattern; pressure doubles the rate */
        if(press>0.5) el=ps->sliceLen*0.5;
        pos=ps->sliceStart+ps->readPhase; ps->readPhase+=pm;
        if(ps->readPhase>=el){ ps->readPhase-=el; ps->chopStep++; }
        gate=((ps->chopPat>>(ps->chopStep&15))&1u)?1.0:0.0;
        double gp=ps->readPhase/el; if(gp>0.85)gate*=(1.0-gp)/0.15;     /* short tail so hits stay separate */ }
    double e=ps->readPhase<el-ps->readPhase?ps->readPhase:el-ps->readPhase; if(e<FD)bf=e/FD;   /* fade slice edges */
    double l=(double)ring_read(ps->ringL,pos)*gate*bf, r=(double)ring_read(ps->ringR,pos)*gate*bf;
    double cyc=ps->readPhase/el; if(d->mech==PM_CHOP) cyc=(double)(ps->chopStep&1);   /* chop: alternate L/R per hit */
    punch_autopan(cyc,0.7,&l,&r);
    if(toneOn){ l=bq_L(&ps->toneFilt,l); r=bq_R(&ps->toneFilt,r); } *outL=l; *outR=r;
}

/* Read a voice's buffer at an absolute phase (wrapped + linear-interpolated). */
static inline void vbuf_read(Voice *v,double ph,double *l,double *r){
    double q=ph; while(q<0)q+=(double)v->loopLen; while(q>=(double)v->loopLen)q-=(double)v->loopLen;
    int i0=(int)q%v->loopLen,i1=(i0+1)%v->loopLen; double f=q-floor(q);
    *l=((double)v->bufferL[i0]*(1.0-f)+(double)v->bufferL[i1]*f)/32768.0;
    *r=((double)v->bufferR[i0]*(1.0-f)+(double)v->bufferR[i1]*f)/32768.0;
}

/* ---- Voice Render (stereo) ---- */
static void voice_render(Voice *v, loopbox_t *s, double *outL, double *outR, double *sendAL, double *sendAR, double *sendBL, double *sendBR) {
    *outL=*outR=*sendAL=*sendAR=*sendBL=*sendBR=0.0;
    int playing=(v->state==VS_PLAYING||v->state==VS_OVERDUBBING);
    int scrubbing=(v->scrubTimer>0);
    if((!playing && !scrubbing && v->playEnv<0.0005)||v->loopLen<=0||!v->bufferL||!v->bufferR){ if(!playing)v->playEnv=0.0; return; }
    int effStart=(int)(v->loopStart*(float)v->loopLen); if(effStart<0)effStart=0; if(effStart>v->loopLen-1)effStart=v->loopLen-1;
    int avail=v->loopLen-effStart; if(avail<1)avail=1;                 /* End is loop LENGTH from Start */
    int effLen=(int)(v->loopEnd*(float)avail); if(effLen<256)effLen=256; if(effLen>avail)effLen=avail;
    int effEnd=effStart+effLen;
    double vcs=clock_to_speed(v->clock);
    double rate=pow(2.0,(double)v->pitch)*vcs;if(v->reverse>0.5f)rate=-rate;
    if(s->scanTimer>0)rate*=3.5;   /* Perform: Scan gesture (fast sweep) */
    if(scrubbing){ rate=v->scrubRate; v->scrubTimer--; }   /* jog rocks the tape, audibly */
    /* head 0 mode/speed */
    { Playhead *P0=&v->ph[0];
      if(P0->spd!=P0->spdCache){ P0->mult=0.25*pow(16.0,(double)P0->spd); P0->spdCache=P0->spd; }
      if(!scrubbing){ rate*=P0->mult;
        if(P0->mode==2) rate=-rate;
        else if(P0->mode==3) rate*=(double)P0->dir; } }
    double baseRate=rate;
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
    if(playing||scrubbing){ v->playPhase+=rate;double dEnd=(double)effEnd,dStart=(double)effStart;
        if(v->ph[0].mode==3&&!scrubbing){ if(v->playPhase>=dEnd){v->playPhase=dEnd-1.0;v->ph[0].dir=-1;}
            else if(v->playPhase<dStart){v->playPhase=dStart;v->ph[0].dir=1;} }
        else { while(v->playPhase>=dEnd)v->playPhase-=(double)effLen;while(v->playPhase<dStart)v->playPhase+=(double)effLen; }
        v->playHead=(int)v->playPhase; }
    /* Amp envelope: attack fades in on trigger/unmute, release fades out on mute/pause/stop.
     * Attack 3ms..3s, Release 3ms..5s (param 0 = click-free floor); coeffs cached, recomputed on change. */
    if(v->ampAtk!=v->ampAtkCache){ double T=0.003*pow(1000.0,(double)v->ampAtk);    double k=1.0/(T*SR); if(k>1.0)k=1.0; v->ampAtkK=(float)k; v->ampAtkCache=v->ampAtk; }
    if(v->ampRel!=v->ampRelCache){ double T=0.003*pow(1666.667,(double)v->ampRel); double k=1.0/(T*SR); if(k>1.0)k=1.0; v->ampRelK=(float)k; v->ampRelCache=v->ampRel; }
    double gate=((playing||scrubbing) && !v->muted)?1.0:0.0;
    double ek=(gate>v->playEnv)?(double)v->ampAtkK:(double)v->ampRelK;
    v->playEnv += (gate-v->playEnv)*ek;
    double _bf=1.0; const double _FD=128.0;                /* loop-boundary fade (click-free wrap), from linear phase */
    if(boundRel<_FD)_bf=boundRel/_FD; else if(boundRel>(double)effLen-_FD)_bf=((double)effLen-boundRel)/_FD;
    if(_bf<0.0)_bf=0.0;
    /* Head 0 fades with its own mode; heads 1-3 add their own reads. */
    { Playhead *P0=&v->ph[0]; double t0=(P0->mode>0)?1.0:0.0; P0->env+=(t0-P0->env)*0.002;
      rawL*=P0->env; rawR*=P0->env; }
    double envSum=v->ph[0].env;
    for(int k=1;k<4;k++){ Playhead *P=&v->ph[k];
        double tg=(P->mode>0)?1.0:0.0; P->env+=(tg-P->env)*0.002;
        if(P->env<0.0005&&P->mode==0) continue;
        if(P->spd!=P->spdCache){ P->mult=0.25*pow(16.0,(double)P->spd); P->spdCache=P->spd; }
        double hl,hr; vbuf_read(v,P->phase,&hl,&hr);
        double hrel=P->phase-(double)effStart;
        while(hrel<0)hrel+=(double)effLen; while(hrel>=(double)effLen)hrel-=(double)effLen;
        double hbf=1.0; if(hrel<_FD)hbf=hrel/_FD; else if(hrel>(double)effLen-_FD)hbf=((double)effLen-hrel)/_FD;
        if(hbf<0.0)hbf=0.0;
        rawL+=hl*P->env*hbf; rawR+=hr*P->env*hbf; envSum+=P->env;
        if(playing||scrubbing){ double pr=baseRate*P->mult;
            if(P->mode==2) pr=-pr; else if(P->mode==3) pr*=(double)P->dir;
            P->phase+=pr;
            if(P->mode==3){ if(P->phase>=(double)effEnd){P->phase=(double)effEnd-1.0;P->dir=-1;}
                            else if(P->phase<(double)effStart){P->phase=(double)effStart;P->dir=1;} }
            else { while(P->phase>=(double)effEnd)P->phase-=(double)effLen;
                   while(P->phase<(double)effStart)P->phase+=(double)effLen; } } }
    if(envSum>1.0){ double nrm=1.0/sqrt(envSum); rawL*=nrm; rawR*=nrm; }   /* keep level sane as heads stack */
    double _amp=v->playEnv*_bf;
    double sL=rawL,sR=rawR;
    sL=voice_saturate(sL,(double)v->saturation);sR=voice_saturate(sR,(double)v->saturation);
    voice_wowflutter_stereo(v,&sL,&sR,(double)v->wowFlutter);
    dj_filter_stereo(v,&sL,&sR);
    tilt_eq_stereo(v,&sL,&sR);studer_eq_stereo(v,&sL,&sR);
    if(s->stability>0.005f){sL=apply_stability(sL,(double)s->stability,&v->rng);sR=apply_stability(sR,(double)s->stability,&v->rng);
        double stabK=0.2+(double)s->stability*0.6;
        v->stabLpStateL+=stabK*(sL-v->stabLpStateL);sL=v->stabLpStateL;
        v->stabLpStateR+=stabK*(sR-v->stabLpStateR);sR=v->stabLpStateR;}
    /* Per-track clock degradation (SR hold + aliasing hiss, Mood-style) and compressor. */
    if(vcs<0.99){ int df=(int)(1.0/vcs); if(df<1)df=1; if(df>4)df=4;
        v->ckCnt++; if(v->ckCnt>=df){ v->ckHoldL=sL; v->ckHoldR=sR; v->ckCnt=0; } sL=v->ckHoldL; sR=v->ckHoldR;
        double degr=1.0-vcs, nA=degr*degr*0.012; sL+=lb_rand(&v->rng)*nA; sR+=lb_rand(&v->rng)*nA; }
    master_comp(&sL,&sR,(double)v->comp,&v->cEnvL,&v->cEnvR);
    v->volSm+=((double)v->volume-v->volSm)*0.00227;   /* ~10ms smoothing (no zipper) */
    v->panSm+=((double)v->pan   -v->panSm)*0.00227;
    double vol=v->volSm*_amp,pn=v->panSm;   /* _amp = click-free env x boundary fade */
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
        v->flutNextMax=0.5;v->rng=12345+i*7919;v->glLastSlice=-1;
        v->djReso=0.0f;v->ampAtk=0.0f;v->ampRel=0.0f;v->ampAtkCache=-1.0f;v->ampRelCache=-1.0f;
        v->comp=0.0f;v->clock=0.5f;
        v->filterSm=0.5f;v->volSm=(double)v->volume;v->panSm=0.0;
        /* heads: 1 on @1x, 2 off @0.5x, 3 off @2x, 4 off @1x  (spd: 0.5=1x, 0.25=0.5x, 0.75=2x) */
        for(int k=0;k<4;k++){ v->ph[k].mode=0; v->ph[k].dir=1; v->ph[k].env=0.0; v->ph[k].phase=0.0; v->ph[k].spdCache=-1.0f; }
        v->ph[0].mode=1; v->ph[0].spd=0.5f; v->ph[0].env=1.0;
        v->ph[1].spd=0.25f; v->ph[2].spd=0.75f; v->ph[3].spd=0.5f;
        bq_reset(&v->djLpA);bq_reset(&v->djHpA);
        bq_reset(&v->eqLow);bq_reset(&v->eqMid);bq_reset(&v->eqHigh);
        bq_reset(&v->tiltLo);bq_reset(&v->tiltHi);
        dj_filter_update(v);studer_eq_update(v);tilt_eq_update(v);}
    s->globalSat=0.0f;s->masterComp=0.0f;s->masterLoCut=20.0f;s->masterHiCut=20000.0f;s->masterVol=1.0f;
    s->preamp=1.0f;s->overdubMode=0.0f;s->stability=0.0f;s->selTrack=1;s->rng=42;   /* 1 = Clean (0 = Tapeless) */
    s->midiIn=0;s->armThresh=0.08f;
    s->tapeNoise=0.5f;s->tapeDrive=0.0f;s->tapeHF=1.0f;s->tapeLoCut=0.0f;s->tapeWow=0.0f;s->tapeFlut=0.0f;s->tapeGen=0.0f;
    s->globalWowFlut=0.0f;s->inputMonitor=0.75f;s->inputGain=1.0f;s->gFlutNextMax=0.5;
    bq_reset(&s->masterLo);bq_reset(&s->masterHi);
    s->cpuPct=0.0; s->rootNote=60;   /* C3 plays the loop at its recorded speed */
    for(int i=0;i<4;i++){s->pslot[i].idx=-1;bq_reset(&s->pslot[i].toneFilt);}
    for(int i=0;i<NUM_PUNCH;i++){s->punchParams[i][0]=0.5f;s->punchParams[i][1]=0.5f;s->punchParams[i][2]=1.0f;s->punchParams[i][3]=1.0f;s->punchPress[i]=0.0f;}
    s->inLow=0.0f;s->inMid=0.0f;s->inMidFreq=0.5f;s->inHigh=0.0f;s->inHighFreq=0.5f;
    bq_reset(&s->inEqLo);bq_reset(&s->inEqMid);bq_reset(&s->inEqHi);bq_reset(&s->inTapeLp);bq_reset(&s->inTapeHp);
    s->busA=pfx_create(44100.0f); s->busB=pfx_create(44100.0f); s->limEnv=0.0;
    s->sendAType=14;s->sendBType=17;s->sendAM1=0.4f;s->sendAM2=0.5f;s->sendADrift=0.2f;s->sendBM1=0.5f;s->sendBM2=0.5f;s->sendBDrift=0.2f;
    if(s->busA)pfx_select(s->busA,s->sendAType); if(s->busB)pfx_select(s->busB,s->sendBType);
    s->stMix=0.0f;s->stStep=0.3f;s->stOdds=0.5f;s->stSize=0.5f;s->stReach=0.3f;s->stKind=0;s->stStepLeft=1;s->stRng=0x2233aa55u;
    s->dropAmt=0.0f;s->dropLeft=1;s->dropRng=0x9911bb77u;
    /* Session worker: demoted to SCHED_OTHER and pinned to cores 0-2 so it can never
     * starve the FIFO-70 audio callback (it would otherwise inherit that priority). */
    atomic_store(&s->sio.request,0); atomic_store(&s->sio.cancel,0);
    atomic_store(&s->sio.busy,0); atomic_store(&s->sio.applyState,0);
    atomic_store(&s->sio.status,0); atomic_store(&s->sio.slot,1);
    if(pthread_create(&s->sio.th,NULL,session_worker,s)==0){
        s->sio.active=1;
        struct sched_param sp; memset(&sp,0,sizeof sp);
        pthread_setschedparam(s->sio.th,SCHED_OTHER,&sp);
        cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(0,&cs); CPU_SET(1,&cs); CPU_SET(2,&cs);
        pthread_setaffinity_np(s->sio.th,sizeof(cs),&cs);
        atomic_store(&s->sio.request,4);   /* populate the slot-name cache off the callback */
    }
    /* Overtake: no sample browser (file I/O forbidden on the audio callback; live
     * looping records from the input). Browser stays empty and harmless. */
    return s;
}
static void destroy_instance(void *inst){loopbox_t *s=(loopbox_t*)inst;if(!s)return;
    if(s->sio.active){ atomic_store(&s->sio.cancel,1); atomic_store(&s->sio.request,1); /* wake */
        pthread_join(s->sio.th,NULL); s->sio.active=0; }   /* join BEFORE freeing buffers */
    for(int i=0;i<NUM_VOICES;i++){free(s->voice[i].bufferL);free(s->voice[i].bufferR);}
    if(s->busA)pfx_destroy(s->busA); if(s->busB)pfx_destroy(s->busB); free(s);}

/* ---- MIDI Handler ---- */
static void on_midi(void *inst, const uint8_t *msg, int len, int source) {
    loopbox_t *s=(loopbox_t*)inst;if(len<3)return;
    uint8_t status=msg[0]&0xF0,d1=msg[1],d2=msg[2];

    /* Overtake: internal pad/knob input is owned by ui.js, which drives the DSP
     * via set_param("cmd", "tap:N" / "odub:N" / "clear:N" / "sel:N"). */
    if(source==MOVE_MIDI_SOURCE_INTERNAL) return;

    if(source==MOVE_MIDI_SOURCE_EXTERNAL) {
        if(!s->midiIn) return;   /* external MIDI off by default (Move track MIDI-out would play loops) */
        /* MIDI keyboard playing: ch1 -> selected loop, ch2..16 -> loops 2..16; 8-voice poly */
        int chan = msg[0] & 0x0F;
        int loopIdx = (chan==0) ? (s->selTrack-1) : chan;
        if(status==0x90 && d2>0) { poly_note_on(s, loopIdx, (int)d1, (int)d2); return; }
        if(status==0x80 || (status==0x90 && d2==0)) { poly_note_off(s, (int)d1); return; }
        return;
    }
}

/* ---- Input EQ (record chain) ---- */
static void input_eq_update(loopbox_t *s){
    double lDb=(double)s->inLow*15.0, mDb=(double)s->inMid*11.0, hDb=(double)s->inHigh*15.0;
    double mF=150.0*pow(7000.0/150.0,(double)s->inMidFreq), hF=3000.0+(double)s->inHighFreq*12000.0;
    if(fabs(lDb)>0.1)bq_set_lowshelf(&s->inEqLo,120.0,lDb,0.7); else bq_reset(&s->inEqLo);
    if(fabs(mDb)>0.1)bq_set_peak(&s->inEqMid,mF,mDb,0.7); else bq_reset(&s->inEqMid);
    if(fabs(hDb)>0.1)bq_set_highshelf(&s->inEqHi,hF,hDb,0.7); else bq_reset(&s->inEqHi);
    if(s->tapeHF<0.99f){ double cut=2000.0*pow(20000.0/2000.0,(double)s->tapeHF); bq_set_lp(&s->inTapeLp,cut,0.707); }  /* tape HF rolloff */
    if(s->tapeLoCut>0.01f){ bq_set_hp(&s->inTapeHp,20.0+(double)s->tapeLoCut*780.0,0.707); }                            /* tape low cut  */
}

/* ---- Tape transport wobble on the RECORD path (Wow = slow, Flutter = fast) ---- */
static inline void input_wowflutter(loopbox_t *s, double *l, double *r, double wow, double flut){
    if(wow<0.005&&flut<0.005)return;
    int wr=s->iFlutWr; s->iFlutBufL[wr]=*l; s->iFlutBufR[wr]=*r;
    s->iFlutPhW+=TWOPI*0.7/SR;  if(s->iFlutPhW>TWOPI)s->iFlutPhW-=TWOPI;   /* ~0.7 Hz wow    */
    s->iFlutPhF+=TWOPI*7.3/SR;  if(s->iFlutPhF>TWOPI)s->iFlutPhF-=TWOPI;   /* ~7.3 Hz flutter */
    double off=2.0+wow*wow*60.0*(0.5+0.5*sin(s->iFlutPhW))+flut*flut*12.0*(0.5+0.5*sin(s->iFlutPhF));
    int cnt=wr+(int)floor(off); double frac=off-floor(off);
    int i0=cnt&(FLUTTER_BUF-1),i1=(cnt+1)&(FLUTTER_BUF-1);
    *l=s->iFlutBufL[i0]*(1.0-frac)+s->iFlutBufL[i1]*frac;
    *r=s->iFlutBufR[i0]*(1.0-frac)+s->iFlutBufR[i1]*frac;
    s->iFlutWr=(wr-1+FLUTTER_BUF)&(FLUTTER_BUF-1);
}
/* Generations: approximate N tape dubs — soft-sat + progressive darkening + hiss. */
static inline void input_generations(loopbox_t *s, double *l, double *r, double g, uint32_t *rng){
    if(g<0.005)return;
    double mk=1.0/(1.0+g*0.4);
    *l=lb_tanh(*l*(1.0+g*0.8))*mk; *r=lb_tanh(*r*(1.0+g*0.8))*mk;
    double k=1.0-g*0.55;                                  /* darker each generation */
    s->genLpL+=k*(*l-s->genLpL); *l=s->genLpL;
    s->genLpR+=k*(*r-s->genLpR); *r=s->genLpR;
    double n=lb_rand(rng)*0.004*g; *l+=n; *r+=n;
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
    loopbox_t *s=(loopbox_t*)inst;
    struct timespec _t0; clock_gettime(CLOCK_MONOTONIC,&_t0);
    /* A finished session load hands back a settings blob — apply it here (bounded
     * string parse, no I/O), once, at block start. */
    if(atomic_load(&s->sio.applyState)){ atomic_store(&s->sio.applyState,0); set_param(s,"state",s->sio.stateBuf); }
    int16_t *micBuf=NULL;if(g_host&&g_host->mapped_memory)micBuf=(int16_t*)(g_host->mapped_memory+g_host->audio_in_offset);
    int selIdx=s->selTrack-1;
    for(int vi=0;vi<NUM_VOICES;vi++){ Voice *fv=&s->voice[vi];
        fv->filterSm+=(fv->filter-fv->filterSm)*0.25f;      /* ~10ms at 2.9ms/block */
        if(fabsf(fv->filter-fv->filterSm)>0.0002f) dj_filter_update(fv); }
    if(selIdx>=0&&selIdx<NUM_VOICES){Voice *sv=&s->voice[selIdx];dj_filter_update(sv);studer_eq_update(sv);tilt_eq_update(sv);}
    if(s->masterLoCut>21.0f)bq_set_hp(&s->masterLo,(double)s->masterLoCut,0.707);
    if(s->masterHiCut<19999.0f)bq_set_lp(&s->masterHi,(double)s->masterHiCut,0.707);
    OverdubMode odMode=(OverdubMode)(int)lb_clampf(s->overdubMode,0.0f,2.0f);
    poly_prep(s);   /* refresh keyboard-poly FX coeffs from their loops */
    if(s->scanTimer>0){ s->scanTimer-=frames; if(s->scanTimer<0)s->scanTimer=0; }
    input_eq_update(s);   /* record-chain EQ + tape-speed */
    punch_prep(s);   /* per-block: punch slot tone-filter coeffs */

    for(int n=0;n<frames;n++){
        double inL=0.0,inR=0.0;
        if(micBuf){double ig=(double)s->inputGain;inL=(double)micBuf[n*2]/32768.0*ig;inR=(double)micBuf[n*2+1]/32768.0*ig;
            double aL=fabs(inL),aR=fabs(inR);if(aL>s->inputPeakL)s->inputPeakL=aL;if(aR>s->inputPeakR)s->inputPeakR=aR;}
        int preModel=(int)lb_clampf(s->preamp,0.0f,12.0f);
        double tdG=1.0+(double)s->tapeDrive*3.0;                  /* tape drive w/ unity makeup */
        if(preModel>0&&s->tapeDrive>0.001f){ inL*=tdG; inR*=tdG; }
        apply_preamp_sample(&inL,&inR,preModel,&s->casLpL,&s->casLpR,&s->rng,(double)s->tapeNoise);
        if(preModel>0&&s->tapeDrive>0.001f){ inL/=tdG; inR/=tdG; }
        if(preModel>0&&s->tapeHF<0.99f){ inL=bq_L(&s->inTapeLp,inL); inR=bq_R(&s->inTapeLp,inR); }
        if(preModel>0&&s->tapeLoCut>0.01f){ inL=bq_L(&s->inTapeHp,inL); inR=bq_R(&s->inTapeHp,inR); }
        if(preModel>0) input_wowflutter(s,&inL,&inR,(double)s->tapeWow,(double)s->tapeFlut);
        if(preModel>0) input_generations(s,&inL,&inR,(double)s->tapeGen,&s->rng);
        if(fabs(s->inLow)>0.007f){inL=bq_L(&s->inEqLo,inL);inR=bq_R(&s->inEqLo,inR);}
        if(fabs(s->inMid)>0.007f){inL=bq_L(&s->inEqMid,inL);inR=bq_R(&s->inEqMid,inR);}
        if(fabs(s->inHigh)>0.007f){inL=bq_L(&s->inEqHi,inL);inR=bq_R(&s->inEqHi,inR);}
        int16_t inSL=(int16_t)lb_clampd(inL*32767.0,-32767.0,32767.0);
        int16_t inSR=(int16_t)lb_clampd(inR*32767.0,-32767.0,32767.0);

        /* Threshold-armed record: an armed track starts the moment input crosses. */
        { double lvl=fabs(inL)>fabs(inR)?fabs(inL):fabs(inR);
          if(lvl>(double)s->armThresh){
            for(int vi=0;vi<NUM_VOICES;vi++){ Voice *v=&s->voice[vi];
                if(v->armed){ v->armed=0; v->state=VS_RECORDING; v->recHead=0; v->loopLen=0; } } } }
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
            voice_render(&s->voice[vi],s,&vL,&vR,&aL,&aR,&bL,&bR);mixL+=vL;mixR+=vR;sAL+=aL;sAR+=aR;sBL+=bL;sBR+=bR;}
        poly_sample(s,&mixL,&mixR,&sAL,&sAR,&sBL,&sBR);   /* MIDI-keyboard poly layer */
        if(s->inputMonitor>0.005f){double mg=(double)s->inputMonitor;mixL+=inL*mg;mixR+=inR*mg;}

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
static const char *preamp_opts[]={"Tapeless","Clean","Cass1","Cass2","VHS1","VHS2","Reel15","Reel7","Reel3","4trk","Porta","Dub","Warp"};
#define NUM_PREAMP 13
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
    /* State restore: the host hands back the whole blob from get_param("state").
     * Split "key=value" lines and replay each through this same dispatcher.
     * Bounded string work only — no I/O, no alloc — so it is callback-safe. */
    if(strcmp(key,"state")==0){
        const char *p=val; char k[64],v[192];
        while(*p){
            const char *eol=strchr(p,'\n'); if(!eol)eol=p+strlen(p);
            while(p<eol&&(*p==' '||*p=='\t'||*p=='\r'))p++;            /* skip leading space */
            const char *eq=(const char*)memchr(p,'=',(size_t)(eol-p));
            if(eq){
                int kl=(int)(eq-p);      if(kl>63) kl=63;
                int vl=(int)(eol-eq-1);  if(vl>191)vl=191; if(vl<0)vl=0;
                memcpy(k,p,(size_t)kl); k[kl]='\0';
                memcpy(v,eq+1,(size_t)vl); v[vl]='\0';
                while(kl>0&&(k[kl-1]==' '||k[kl-1]=='\t'||k[kl-1]=='\r'))k[--kl]='\0';
                while(vl>0&&(v[vl-1]==' '||v[vl-1]=='\t'||v[vl-1]=='\r'))v[--vl]='\0';
                if(k[0]&&strcmp(k,"state")!=0) set_param(inst,k,v);    /* never recurse on state */
            }
            p=(*eol)?eol+1:eol;
        }
        return;
    }
    if(strcmp(key,"cmd")==0){
        const char *c=strchr(val,':'); int vi=c?atoi(c+1):-1;
        if(strncmp(val,"tap",3)==0)        voice_tap(s,vi);
        else if(strncmp(val,"odub",4)==0)  voice_odub(s,vi);
        else if(strncmp(val,"clear",5)==0){ if(vi>=0&&vi<NUM_VOICES)voice_clear(&s->voice[vi]); }
        else if(strncmp(val,"unclr",5)==0){ if(vi>=0&&vi<NUM_VOICES)voice_unclear(&s->voice[vi]); }
        else if(strncmp(val,"mute",4)==0){ if(vi>=0&&vi<NUM_VOICES)s->voice[vi].muted=!s->voice[vi].muted; }
        else if(strncmp(val,"sel",3)==0){ if(vi>=0&&vi<NUM_VOICES)s->selTrack=vi+1; }
        else if(strncmp(val,"arm",3)==0){ if(vi>=0&&vi<NUM_VOICES){ Voice *v=&s->voice[vi];
            v->armed=!v->armed; if(v->armed){ if(v->loopLen>0)voice_clear(v); else { v->state=VS_EMPTY; v->recHead=0; } } } }
        else if(strncmp(val,"clone",5)==0){   /* "clone:SRC:DST" — the worker does the big copy */
            const char *c2=c?strchr(c+1,':'):NULL; int dst=c2?atoi(c2+1):-1;
            if(vi>=0&&vi<NUM_VOICES&&dst>=0&&dst<NUM_VOICES&&vi!=dst&&s->sio.active&&!atomic_load(&s->sio.busy)){
                atomic_store(&s->sio.cloneSrc,vi); atomic_store(&s->sio.cloneDst,dst);
                atomic_store(&s->sio.request,3); } }
        return;
    }
    /* Sessions: "session" = "save:N" / "load:N" (N = 1..32). Only sets atomics —
     * the worker does every file operation off the callback. */
    if(strcmp(key,"session")==0){
        const char *c=strchr(val,':'); int n=c?atoi(c+1):1; if(n<1)n=1; if(n>32)n=32;
        if(atomic_load(&s->sio.busy)||!s->sio.active) return;
        atomic_store(&s->sio.slot,n);
        if(strncmp(val,"save",4)==0){
            get_param(s,"state",s->sio.stateBuf,(int)sizeof(s->sio.stateBuf));  /* snapshot settings */
            atomic_store(&s->sio.request,1);
        } else if(strncmp(val,"load",4)==0) atomic_store(&s->sio.request,2);
        return; }
    if(strcmp(key,"punch")==0){ const char *c=strchr(val,':'); int n=c?atoi(c+1):-1;
        if(strncmp(val,"on",2)==0)punch_on(s,n); else if(strncmp(val,"off",3)==0)punch_off(s,n); return; }
    if(strcmp(key,"pfx")==0){ int idx=atoi(val); const char *c1=strchr(val,':'); if(!c1)return; int p=atoi(c1+1);
        const char *c2=strchr(c1+1,':'); if(!c2)return; float v=lb_clampf((float)atof(c2+1),0.0f,1.0f);
        if(idx>=0&&idx<NUM_PUNCH&&p>=0&&p<4){ s->punchParams[idx][p]=v;
            if(p==0){ for(int i=0;i<4;i++) if(s->pslot[i].idx==idx) punch_slot_retune(s,&s->pslot[i],idx); } }
        return; }
    /* State restore for punch params: "pfx0" .. "pfx15" = "rate,pitch,tone,mix" */
    if(strncmp(key,"pfx",3)==0&&key[3]>='0'&&key[3]<='9'){
        int idx=atoi(key+3);
        if(idx>=0&&idx<NUM_PUNCH){ float a=0,b=0,c=0,d=0;
            if(sscanf(val,"%f,%f,%f,%f",&a,&b,&c,&d)==4){
                s->punchParams[idx][0]=lb_clampf(a,0.0f,1.0f); s->punchParams[idx][1]=lb_clampf(b,0.0f,1.0f);
                s->punchParams[idx][2]=lb_clampf(c,0.0f,1.0f); s->punchParams[idx][3]=lb_clampf(d,0.0f,1.0f); } }
        return; }
    if(strcmp(key,"punchPress")==0){ int idx=atoi(val); const char *c=strchr(val,':'); float v=c?lb_clampf((float)atof(c+1),0.0f,1.0f):0.0f; if(idx>=0&&idx<NUM_PUNCH)s->punchPress[idx]=v; return; }
    SETFR("globalSat",globalSat,0.0,1.0) SETFR("masterComp",masterComp,0.0,1.0)
    SETFR("masterLoCut",masterLoCut,20.0,500.0) SETFR("masterHiCut",masterHiCut,1000.0,20000.0)
    SETFR("masterVol",masterVol,0.0,1.5)
    if(strcmp(key,"preamp")==0){int idx=match_enum(val,preamp_opts,NUM_PREAMP);if(idx>=0)s->preamp=(float)idx;else s->preamp=lb_clampf((float)atof(val),0.0f,(float)(NUM_PREAMP-1));return;}
    if(strcmp(key,"overdubMode")==0){int idx=match_enum(val,odmode_opts,3);if(idx>=0)s->overdubMode=(float)idx;else s->overdubMode=lb_clampf((float)atof(val),0.0f,2.0f);return;}
    SETFR("stability",stability,0.0,1.0) SETFR("globalWowFlut",globalWowFlut,0.0,1.0) SETFR("inputMonitor",inputMonitor,0.0,1.0) SETFR("inputGain",inputGain,0.0,2.0)
    SETFR("inLow",inLow,-1.0,1.0) SETFR("inMid",inMid,-1.0,1.0) SETFR("inMidFreq",inMidFreq,0.0,1.0)
    SETFR("inHigh",inHigh,-1.0,1.0) SETFR("inHighFreq",inHighFreq,0.0,1.0)
    SETFR("tapeNoise",tapeNoise,0.0,1.0) SETFR("tapeDrive",tapeDrive,0.0,1.0) SETFR("tapeHF",tapeHF,0.0,1.0)
    if(strcmp(key,"midiIn")==0){ s->midiIn=(strcmp(val,"On")==0||atof(val)>0.5)?1:0; return; }
    SETFR("armThresh",armThresh,0.0,1.0)
    SETFR("tapeLoCut",tapeLoCut,0.0,1.0) SETFR("tapeWow",tapeWow,0.0,1.0) SETFR("tapeFlut",tapeFlut,0.0,1.0) SETFR("tapeGen",tapeGen,0.0,1.0)
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
            double frac=lb_rand(&v->rng)*0.5+0.5;
            v->scatXfadePhase=v->playPhase; v->scatXfade=64;   /* declick the jump */
            v->playPhase=(double)es+frac*(double)el; } } } return; }
    if(strcmp(key,"scan")==0){ if((float)atof(val)>0.5f)s->scanTimer=(int)(SR*0.4); return; }
    /* Jog scrub: nudge the selected loop's playhead, declicked by the scatter crossfade. */
    if(strcmp(key,"scrub")==0){ int si=s->selTrack-1; if(si<0||si>=NUM_VOICES)return; Voice *v=&s->voice[si];
        if(v->loopLen<=0)return;
        /* Magneto-style: the jog rocks the tape — the head travels for ~120ms and you hear it. */
        double dist=(double)atof(val)*(double)v->loopLen*0.01, win=SR*0.12;
        v->scrubRate=dist/win; v->scrubTimer=(int)win;
        return; }
    if(strcmp(key,"headpos")==0){ int si=s->selTrack-1; if(si<0||si>=NUM_VOICES)return; Voice *v=&s->voice[si];
        const char *c=strchr(val,':'); int hi=atoi(val); double d=c?atof(c+1):0.0;
        if(hi<0||hi>3||v->loopLen<=0)return;
        double mv=d*(double)v->loopLen*0.01;
        if(hi==0){ v->scatXfadePhase=v->playPhase; v->scatXfade=64; v->playPhase+=mv;
            while(v->playPhase>=(double)v->loopLen)v->playPhase-=(double)v->loopLen;
            while(v->playPhase<0.0)v->playPhase+=(double)v->loopLen; }
        else { v->ph[hi].phase+=mv;
            while(v->ph[hi].phase>=(double)v->loopLen)v->ph[hi].phase-=(double)v->loopLen;
            while(v->ph[hi].phase<0.0)v->ph[hi].phase+=(double)v->loopLen; }
        return; }
    if(strcmp(key,"rootNote")==0){s->rootNote=(int)lb_clampf((float)atof(val),24.0f,96.0f); return;}
    if(strcmp(key,"selTrack")==0){s->selTrack=(int)lb_clampf((float)atof(val),1.0f,16.0f);return;}
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
    if(strncmp(key,"v_ph",4)==0&&key[4]>='1'&&key[4]<='4'){
        int hi=key[4]-'1'; const char *sub=key+5;
        if(strcmp(sub,"mode")==0){
            static const char *mo[]={"Off","Fwd","Bwd","Ping"};
            int m=match_enum(val,mo,4); if(m<0)m=(int)lb_clampf((float)atof(val),0.0f,3.0f);
            int was=v->ph[hi].mode; v->ph[hi].mode=m;
            if(m>0&&was==0){   /* turning a head on always restarts it at the loop start */
                int es=(int)(v->loopStart*(float)v->loopLen); if(es<0)es=0;
                v->ph[hi].phase=(double)es; v->ph[hi].dir=1;
                if(hi==0){ v->playPhase=(double)es; v->scatXfade=0; } }
            return; }
        if(strcmp(sub,"spd")==0){ v->ph[hi].spd=lb_clampf((float)atof(val),0.0f,1.0f); return; }
        return; }
    if(strcmp(key,"v_djReso")==0){v->djReso=lb_clampf((float)atof(val),0.0f,1.0f);dj_filter_update(v);return;}
    SETVFR("v_comp",comp,0.0,1.0) SETVFR("v_clock",clock,0.0,1.0)
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
            else if(strcmp(sub,"cmp")==0)vr->comp=lb_clampf(fv,0,1);
            else if(strcmp(sub,"clk")==0)vr->clock=lb_clampf(fv,0,1);
            else if(strcmp(sub,"ph")==0){ int m[4]; float sp[4];
                if(sscanf(val,"%d,%f,%d,%f,%d,%f,%d,%f",&m[0],&sp[0],&m[1],&sp[1],&m[2],&sp[2],&m[3],&sp[3])==8)
                    for(int k=0;k<4;k++){ vr->ph[k].mode=(m[k]<0||m[k]>3)?0:m[k];
                        vr->ph[k].spd=lb_clampf(sp[k],0,1); vr->ph[k].env=(vr->ph[k].mode>0)?1.0:0.0; } }
            studer_eq_update(vr);tilt_eq_update(vr);}return;}
}

#define GETP(k,f) if(strcmp(key,k)==0)return snprintf(buf,buf_len,"%.4f",(double)s->f);
#define GETVP(k,f) if(strcmp(key,k)==0)return snprintf(buf,buf_len,"%.4f",(double)v->f);
#define GETE(k,f,opts,cnt) do{if(strcmp(key,k)==0){int _i=(int)roundf(s->f);if(_i<0)_i=0;if(_i>=(cnt))_i=(cnt)-1;return snprintf(buf,buf_len,"%s",(opts)[_i]);}}while(0)

/* ui_hierarchy JSON - MUST be returned from get_param for sound generators */
static const char *UI_HIERARCHY_JSON =
    "{\"modes\":null,\"levels\":{"
    "\"root\":{\"name\":\"LoopBox\","
    "\"knobs\":[\"v_pitch\",\"v_filter\",\"v_pan\",\"v_volume\",\"v_start\",\"v_end\",\"v_reverse\",\"v_sendA\"],"
    "\"params\":[{\"level\":\"LOOP\",\"label\":\"Loop\"},{\"level\":\"INPUT\",\"label\":\"Input\"},{\"level\":\"FX\",\"label\":\"Global FX\"},{\"level\":\"MASTER\",\"label\":\"Master\"}]},"
    "\"LOOP\":{\"label\":\"Loop\","
    "\"knobs\":[\"v_pitch\",\"v_filter\",\"v_pan\",\"v_volume\",\"v_start\",\"v_end\",\"v_reverse\",\"v_sendA\"],"
    "\"params\":[\"v_pitch\",\"v_filter\",\"v_pan\",\"v_volume\",\"v_start\",\"v_end\",\"v_reverse\",\"v_sendA\","
    "\"v_clock\",\"v_djReso\",\"v_sat\",\"v_comp\",\"v_wowflut\",\"v_scatter\",\"v_glitch\",\"v_sendB\","
    "\"v_eqBass\",\"v_eqPresFrq\",\"v_eqPresAmt\",\"v_eqTreble\",\"v_tilt\",\"v_atk\",\"v_rel\"]},"
    "\"INPUT\":{\"label\":\"Input\","
    "\"knobs\":[\"inputMonitor\",\"preamp\",\"inputGain\",\"inLow\",\"inMid\",\"inMidFreq\",\"inHigh\",\"inHighFreq\"],"
    "\"params\":[\"inputMonitor\",\"preamp\",\"inputGain\",\"inLow\",\"inMid\",\"inMidFreq\",\"inHigh\",\"inHighFreq\","
    "\"tapeDrive\",\"tapeWow\",\"tapeFlut\",\"tapeHF\",\"tapeLoCut\",\"tapeNoise\",\"tapeGen\"]},"
    "\"FX\":{\"label\":\"Global FX\","
    "\"knobs\":[\"sendAType\",\"sendAM1\",\"sendAM2\",\"sendADrift\",\"sendBType\",\"sendBM1\",\"sendBM2\",\"sendBDrift\"],"
    "\"params\":[\"sendAType\",\"sendAM1\",\"sendAM2\",\"sendADrift\",\"sendBType\",\"sendBM1\",\"sendBM2\",\"sendBDrift\","
    "\"stMix\",\"stStep\",\"stOdds\",\"stSize\",\"stReach\",\"stKind\",\"dropAmt\"]},"
    "\"MASTER\":{\"label\":\"Master\","
    "\"knobs\":[\"masterVol\",\"rootNote\",\"overdubMode\",\"masterLoCut\",\"masterHiCut\",\"globalSat\",\"midiIn\",\"armThresh\"],"
    "\"params\":[\"masterVol\",\"rootNote\",\"overdubMode\",\"masterLoCut\",\"masterHiCut\",\"globalSat\",\"midiIn\",\"armThresh\","
    "\"masterComp\",\"stability\",\"globalWowFlut\"]}"
    "}}";

/* chain_params JSON */
static const char *CHAIN_PARAMS_JSON =
    "[{\"key\":\"globalSat\",\"name\":\"Sat\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"masterComp\",\"name\":\"Comp\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"masterLoCut\",\"name\":\"LoCut\",\"type\":\"int\",\"min\":20,\"max\":500,\"step\":1},"
    "{\"key\":\"masterHiCut\",\"name\":\"HiCut\",\"type\":\"int\",\"min\":1000,\"max\":20000,\"step\":200},"
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
    "{\"key\":\"preamp\",\"name\":\"Preamp\",\"type\":\"enum\",\"options\":[\"Tapeless\",\"Clean\",\"Cass1\",\"Cass2\",\"VHS1\",\"VHS2\",\"Reel15\",\"Reel7\",\"Reel3\",\"4trk\",\"Porta\",\"Dub\",\"Warp\"]},"
    "{\"key\":\"tapeNoise\",\"name\":\"Noise\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"tapeDrive\",\"name\":\"TpDrv\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"tapeHF\",\"name\":\"TpHF\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"tapeLoCut\",\"name\":\"TpLoC\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"tapeWow\",\"name\":\"TpWow\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"tapeFlut\",\"name\":\"TpFlt\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"tapeGen\",\"name\":\"TpGen\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
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
    "{\"key\":\"v_comp\",\"name\":\"Comp\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"v_clock\",\"name\":\"Clock\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01}]";

static int get_param(void *inst, const char *key, char *buf, int buf_len) {
    loopbox_t *s=(loopbox_t*)inst;if(!key)return -1;

    /* Overtake: Manager discovery + UI feedback */
    if(strcmp(key,"module_id")==0)return snprintf(buf,buf_len,"loopbox");
    if(strcmp(key,"cpu")==0)return snprintf(buf,buf_len,"%.1f",s->cpuPct);
    if(strcmp(key,"sessStatus")==0){ static const char *st[]={"","Saving..","Loading..","OK","Empty"};
        int i=atomic_load(&s->sio.status); if(i<0||i>4)i=0; return snprintf(buf,buf_len,"%s",st[i]); }
    if(strcmp(key,"sessSlot")==0)return snprintf(buf,buf_len,"%d",atomic_load(&s->sio.slot));
    if(strcmp(key,"sessName")==0){ int n=atomic_load(&s->sio.slot); if(n<1||n>NUM_SLOTS)n=1; return snprintf(buf,buf_len,"%s",s->sio.names[n]); }
    if(strcmp(key,"sessNames")==0){ int p=0; for(int n=1;n<=NUM_SLOTS&&p<buf_len-2;n++) p+=snprintf(buf+p,buf_len-p,"%s%s",s->sio.names[n],(n<NUM_SLOTS)?";":""); return p; }
    if(strcmp(key,"wave")==0){   /* 128 columns x (max,lo) min/max envelope, auto-normalised */
        int si=s->selTrack-1; if(si<0||si>=NUM_VOICES)return -1; Voice *v=&s->voice[si];
        if(v->loopLen<=0||buf_len<258){ buf[0]=0; return 0; }
        int per=v->loopLen/128; if(per<1)per=1;
        int step=per/96; if(step<1)step=1;
        int peak=1;                                  /* scale to the loop own peak, like the Move display */
        for(int i=0;i<v->loopLen;i+=step*4){
            int a=v->bufferL[i]; if(a<0)a=-a;
            int r=v->bufferR[i]; if(r<0)r=-r; if(r>a)a=r;
            if(a>peak)peak=a; }
        if(peak<200)peak=200;                        /* near-silence floor: do not amplify noise */
        for(int b=0;b<128;b++){
            int base=b*per, mx=-32768, mn=32767;
            for(int j=0;j<per;j+=step){ int idx=base+j; if(idx>=v->loopLen)break;
                int a=v->bufferL[idx]; if(a>mx)mx=a; if(a<mn)mn=a;
                int r=v->bufferR[idx]; if(r>mx)mx=r; if(r<mn)mn=r; }
            if(mx<mn){ mx=0; mn=0; }
            int hi=(mx*31)/peak; if(hi>31)hi=31; if(hi<-31)hi=-31;
            int lo=(mn*31)/peak; if(lo>31)lo=31; if(lo<-31)lo=-31;
            buf[b*2]  =(char)(48+hi+31);
            buf[b*2+1]=(char)(48+lo+31); }
        buf[256]=0; return 256; }
    if(strcmp(key,"heads")==0){   /* "m,pos" x4 — pos 0..999 across the loop */
        int si=s->selTrack-1; if(si<0||si>=NUM_VOICES)return -1; Voice *v=&s->voice[si];
        int p=0; double L=(v->loopLen>0)?(double)v->loopLen:1.0;
        for(int k=0;k<4;k++){ double ph=(k==0)?v->playPhase:v->ph[k].phase;
            while(ph<0)ph+=L; while(ph>=L)ph-=L;
            int pos=(int)(ph/L*999.0); if(pos<0)pos=0; if(pos>999)pos=999;
            p+=snprintf(buf+p,buf_len-p,"%d,%d%s",v->ph[k].mode,pos,(k<3)?";":""); }
        return p; }
    if(strcmp(key,"armed")==0){ int p=0;
        for(int i=0;i<NUM_VOICES&&p<buf_len-1;i++) buf[p++]=(char)('0'+(s->voice[i].armed?1:0));
        buf[p]='\0'; return p; }
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
    GETP("masterVol",masterVol)
    GETE("preamp",preamp,preamp_opts,NUM_PREAMP); GETE("overdubMode",overdubMode,odmode_opts,3);
    GETP("stability",stability) GETP("globalWowFlut",globalWowFlut) GETP("inputMonitor",inputMonitor) GETP("inputGain",inputGain)
    GETP("inLow",inLow) GETP("inMid",inMid) GETP("inMidFreq",inMidFreq) GETP("inHigh",inHigh) GETP("inHighFreq",inHighFreq)
    GETP("tapeNoise",tapeNoise) GETP("tapeDrive",tapeDrive) GETP("tapeHF",tapeHF)
    if(strcmp(key,"midiIn")==0)return snprintf(buf,buf_len,"%s",s->midiIn?"On":"Off");
    GETP("armThresh",armThresh)
    GETP("tapeLoCut",tapeLoCut) GETP("tapeWow",tapeWow) GETP("tapeFlut",tapeFlut) GETP("tapeGen",tapeGen)
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
    GETVP("v_djReso",djReso) GETVP("v_atk",ampAtk) GETVP("v_rel",ampRel) GETVP("v_comp",comp) GETVP("v_clock",clock)
    if(strncmp(key,"v_ph",4)==0&&key[4]>='1'&&key[4]<='4'){
        int hi=key[4]-'1'; const char *sub=key+5;
        static const char *mo[]={"Off","Fwd","Bwd","Ping"};
        if(strcmp(sub,"mode")==0){ int m=v->ph[hi].mode; if(m<0||m>3)m=0; return snprintf(buf,buf_len,"%s",mo[m]); }
        if(strcmp(sub,"spd")==0) return snprintf(buf,buf_len,"%.4f",(double)v->ph[hi].spd); }

    if(strcmp(key,"v_state")==0){static const char *sts[]={"Empty","Rec","Play","Pause","Odub"};
        int st=(int)v->state;if(st<0||st>4)st=0;return snprintf(buf,buf_len,"%s",sts[st]);}
    if(strcmp(key,"v_loopLen")==0)return snprintf(buf,buf_len,"%.1f",(double)v->loopLen/SR);

    /* State serialization: dump all global + all 16 voices' params */
    if(strcmp(key,"state")==0){int p=0;
        #define WF(k,val) p+=snprintf(buf+p,buf_len-p,"%s=%.4f\n",k,(double)(val))
        #define WI(k,val) p+=snprintf(buf+p,buf_len-p,"%s=%d\n",k,(int)(val))
        WF("globalSat",s->globalSat);WF("masterComp",s->masterComp);
        WI("masterLoCut",(int)s->masterLoCut);WI("masterHiCut",(int)s->masterHiCut);
        WI("preamp",(int)s->preamp);WI("overdubMode",(int)s->overdubMode);
        WF("stability",s->stability);WF("globalWowFlut",s->globalWowFlut);WF("inputMonitor",s->inputMonitor);WF("inputGain",s->inputGain);
        WI("selTrack",s->selTrack);
        /* Global FX send buses (by effect NAME so ids stay stable across versions) */
        p+=snprintf(buf+p,buf_len-p,"sendAType=%s\nsendBType=%s\n",pfx_name(s->sendAType),pfx_name(s->sendBType));
        WF("sendAM1",s->sendAM1);WF("sendAM2",s->sendAM2);WF("sendADrift",s->sendADrift);
        WF("sendBM1",s->sendBM1);WF("sendBM2",s->sendBM2);WF("sendBDrift",s->sendBDrift);
        /* Perform */
        WF("stMix",s->stMix);WF("stStep",s->stStep);WF("stOdds",s->stOdds);
        WF("stSize",s->stSize);WF("stReach",s->stReach);WI("stKind",s->stKind);WF("dropAmt",s->dropAmt);
        /* Input / Tape chain */
        WF("inLow",s->inLow);WF("inMid",s->inMid);WF("inMidFreq",s->inMidFreq);
        WF("inHigh",s->inHigh);WF("inHighFreq",s->inHighFreq);
        WF("tapeNoise",s->tapeNoise);WF("tapeDrive",s->tapeDrive);WF("tapeHF",s->tapeHF);
        WI("midiIn",s->midiIn);WF("armThresh",s->armThresh);WF("tapeLoCut",s->tapeLoCut);WF("tapeWow",s->tapeWow);WF("tapeFlut",s->tapeFlut);WF("tapeGen",s->tapeGen);
        /* Master + keyboard */
        WF("masterVol",s->masterVol);WI("rootNote",s->rootNote);
        /* Punch-in FX per-effect params */
        for(int pi=0;pi<NUM_PUNCH;pi++)
            p+=snprintf(buf+p,buf_len-p,"pfx%d=%.4f,%.4f,%.4f,%.4f\n",pi,
                (double)s->punchParams[pi][0],(double)s->punchParams[pi][1],
                (double)s->punchParams[pi][2],(double)s->punchParams[pi][3]);
        for(int i=0;i<NUM_VOICES;i++){Voice *vi=&s->voice[i];
            p+=snprintf(buf+p,buf_len-p,"v%d.srt=%.4f\nv%d.end=%.4f\nv%d.rev=%.4f\n",i,(double)vi->loopStart,i,(double)vi->loopEnd,i,(double)vi->reverse);
            p+=snprintf(buf+p,buf_len-p,"v%d.sat=%.4f\nv%d.wf=%.4f\nv%d.snd=%.4f\n",i,(double)vi->saturation,i,(double)vi->wowFlutter,i,(double)vi->send);
            p+=snprintf(buf+p,buf_len-p,"v%d.gli=%.4f\nv%d.tlt=%.4f\n",i,(double)vi->glitch,i,(double)vi->tiltEQ);
            p+=snprintf(buf+p,buf_len-p,"v%d.eB=%.4f\nv%d.ePF=%.4f\nv%d.ePA=%.4f\nv%d.eT=%.4f\n",i,(double)vi->eqBass,i,(double)vi->eqPresFreq,i,(double)vi->eqPresAmt,i,(double)vi->eqTreble);
            p+=snprintf(buf+p,buf_len-p,"v%d.pit=%.4f\nv%d.fil=%.4f\nv%d.pan=%.4f\nv%d.vol=%.4f\nv%d.dec=%.4f\n",
                i,(double)vi->pitch,i,(double)vi->filter,i,(double)vi->pan,i,(double)vi->volume,i,(double)vi->decay);
            p+=snprintf(buf+p,buf_len-p,"v%d.rso=%.4f\nv%d.atk=%.4f\nv%d.rel=%.4f\n",i,(double)vi->djReso,i,(double)vi->ampAtk,i,(double)vi->ampRel);
            p+=snprintf(buf+p,buf_len-p,"v%d.cmp=%.4f\nv%d.clk=%.4f\n",i,(double)vi->comp,i,(double)vi->clock);}
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
