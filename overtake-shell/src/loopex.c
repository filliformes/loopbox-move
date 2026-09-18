/*
 * loopex.c — Loopex for Ableton Move (Schwung Overtake module)
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
 *   Sessions: 64 slots, all disk work on a SCHED_OTHER worker (cores 0-2)
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
#include <sys/mman.h>
#include <fcntl.h>
#include "plugin_api_v1.h"
#include "palette_fx.h"   /* 24-effect Palette engine for the send buses */

/* ---- Link Audio input SHM (/schwung-link-in): the Schwung host's link-subscriber
 * writes Move's per-track audio here (slots 0-3 = tracks 1-4, 4 = Main), one SPSC
 * ring per channel. We read with our OWN cursor and never touch read_pos (that is
 * the shim's). Layout mirrors the host header; magic guards a version mismatch. */
#define LA_IN_SHM_NAME     "/schwung-link-in"
#define LA_IN_MAGIC        0x4C41494Eu   /* "LAIN" */
#define LA_IN_RING_SAMPLES 16384         /* 64 blocks * 128 frames * 2 ch */
#define LA_IN_RING_MASK    (LA_IN_RING_SAMPLES-1)
#define LA_IN_SLOTS        5
typedef struct {
    int16_t  ring[LA_IN_RING_SAMPLES];
    volatile uint32_t write_pos, read_pos;
    volatile int      active;
    char     name[32];
    volatile uint32_t starve_count, catchup_count, catchup_samples_dropped, max_avail_seen;
    volatile uint32_t produced_count, would_overrun_count, max_frames_seen;
    uint32_t _stats_pad[1];
} la_in_slot_t;
typedef struct { volatile uint32_t magic, version; la_in_slot_t slots[LA_IN_SLOTS]; } la_in_shm_t;
_Static_assert(sizeof(la_in_shm_t)==164228, "la_in_shm_t must match /schwung-link-in (host link_audio.h)");

/* ---- Schwung published stems SHM (/schwung-pub-audio): the host publishes its
 * OWN audio here (slots 0-3 = the four Schwung mixer slots, 4 = master) as stereo
 * stems, one SPSC ring per slot, when link_audio_publish is on. This is distinct
 * from /schwung-link-in (which carries the reconstructed OG Move tracks). Same read
 * discipline: private cursor, never touch read_pos. Geometry ("BPAL", 12-byte header,
 * 5 x 8204-byte slots, 4096-int16 stereo ring, write_pos in int16 samples) was
 * verified live against the running host. */
#define BPA_SHM_NAME     "/schwung-pub-audio"
#define BPA_MAGIC        0x4C415042u   /* "BPAL" */
#define BPA_RING_SAMPLES 4096          /* 2048 stereo frames */
#define BPA_RING_MASK    (BPA_RING_SAMPLES-1)
#define BPA_SLOTS        5
typedef struct {
    int16_t  ring[BPA_RING_SAMPLES];
    volatile uint32_t write_pos, read_pos, active;
} bpa_slot_t;
typedef struct { volatile uint32_t magic, version, reserved; bpa_slot_t slots[BPA_SLOTS]; } bpa_shm_t;
_Static_assert(sizeof(bpa_shm_t)==41032, "bpa_shm_t must match /schwung-pub-audio");
/* pitch_shift.cc — Signalsmith Stretch behind a C API (per-loop Pitch) */
void *ps_create(float sr, int blockMax, int staggerSamples); void ps_destroy(void *h); void ps_reset(void *h);
int ps_latency(void *h); void ps_set_semitones(void *h, float semitones); void ps_process(void *h, float *l, float *r, int n);

static const host_api_v1_t *g_host = NULL;

#define SR              44100.0
#define TWOPI           (2.0 * M_PI)
#define NUM_VOICES      16
#define LOOP_SECONDS    45
#define LOOP_SAMPLES    ((int)(SR * LOOP_SECONDS))
#define FLUTTER_BUF     1024
/* ---- Utilities ---- */
static inline float lb_clampf(float x, float lo, float hi) { if(x!=x)return lo; return x < lo ? lo : (x > hi ? hi : x); }
static inline double lb_clampd(double x, double lo, double hi) { if(x!=x)return lo; return x < lo ? lo : (x > hi ? hi : x); }
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
    if(amt<0.005)return x;double x0=x,gain=1.0+amt*4.0;x*=gain;
    double br=fabs(x);if(br>1.5707963)br=1.5707963;br=sin(br);x=(x>0.0)?br:-br;
    double bump=sin(x*0.3)*amt*0.08;x=(x+bump)/(gain*0.45+0.55);
    double w=(amt<0.05)?amt/0.05:1.0; return x0+(x-x0)*w;   /* blend in: no curve step when the knob leaves 0 */
}
/* clock 0.5 = 1.0x (normal), 0 = 0.25x, 1 = 4.0x — so loops default to normal speed */
static inline double apply_stability(double x, double amt, uint32_t *rng) {
    if(amt<0.005)return x;double x0=x;x=lb_tanh(x*(1.0+amt*0.3));x+=lb_rand(rng)*amt*0.015;
    double w=(amt<0.05)?amt/0.05:1.0; return x0+(x-x0)*w;
}

/* One read head over a loop: 0=off 1=fwd 2=bwd 3=ping-pong. Head 0 shares the
 * voice's playPhase (so scatter/Seed/scrub/jump keep driving it). */
typedef struct { int mode; float spd, spdCache; double mult, phase, env; int dir; double xfPhase; int xf; int jumpCd; } Playhead;   /* xf: crossfade samples left; jumpCd: samples to next random jump (Jump mode) */
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
    float lfStL[6], lfStR[6], lfG; int lfModeCache;   /* per-loop analog LP (mf_run voicing) */
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
    float hVol[4], hPan[4];               /* per-head Vol/Pan (heads 1-3; head 0 uses the loop's volume/pan) */
    int scrubTimer; double scrubRate;     /* jog scrub: audible tape rock */
    float filterSm; double volSm, panSm;  /* 10ms smoothing on the steppy knobs */
    int savedLoopLen;                     /* clear-undo: last loop length before a clear */
    float djReso;                         /* DJ filter resonance (Q) */
    float comp, clock;                    /* per-track compressor amount; clock = independent PITCH shift in octaves (-2..2), not the playback rate */
    void *ps; float psInL[128], psInR[128], psOutL[128], psOutR[128];   /* Signalsmith Stretch (pitch_shift.cc), one block late */
    int psActive, psWarm, psLat, psNudge; double psMix; float psCache;            /* engage state: warm-up count, latency, dry/wet ramp */
    double cEnvL, cEnvR;
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
       PM_MOSAIC, PM_SMEAR, PM_STRUM, PM_GLIDE, PM_CHOP, PM_PALETTE, PM_NONE };
#define SHBUF 8192
#define NUM_PSLOTS 5   /* punch effects in series */
typedef struct { int mech; double param; } PunchDef;   /* param = division (REPEAT) or ratio/default */
static const PunchDef PUNCH_DEFS[NUM_PUNCH] = {   /* right 4x4, top->bottom, grouped by family */
    {PM_REPEAT,3},{PM_REPEAT,4},{PM_REPEAT,8},{PM_CHOP,0},            /* Loops:  Loop12 Loop16 LoopSh Chop */
    {PM_HAZE,0},{PM_MOSAIC,0},{PM_SMEAR,0},{PM_STRUM,0},              /* Grains: Haze Mosaic Smear Strum */
    {PM_PITCH,0.5},{PM_PITCH,2.0},{PM_GLIDE,0},{PM_SHIMMER,0},        /* Pitch:  Oct- Oct+ Glide Shimmer */
    {PM_STRETCH,0.5},{PM_STRETCH,1.0},{PM_REVERSE,1.0},{PM_PALETTE,0} /* Time:   Stretch Freeze Reverse Palette */
};
/* Chop patterns (from Signal): Patrn knob picks one; steps advance one per slice. */

#define NUM_CHOP_PAT 32
static const uint8_t CHOP_PAT[NUM_CHOP_PAT][16] = {
    {1,0,1,0,0,1,0,0,0,0,1,0,1,0,0,0},  /* V0P15 */
    {1,1,0,1,0,0,1,0,0,0,0,1,0,0,0,0},  /* V0P16 */
    {1,1,1,0,1,0,1,0,0,1,0,0,0,0,0,0},  /* V0P01 */
    {1,1,0,1,0,1,0,0,0,0,0,1,0,0,1,0},  /* V1P20 */
    {1,0,0,1,0,1,0,0,1,0,0,1,0,1,0,0},  /* V2P14 */
    {1,0,0,1,0,1,0,0,1,0,0,0,0,0,0,0},  /* V2P05 */
    {1,1,1,1,0,0,0,0,1,1,1,0,0,0,0,0},  /* V0P12 */
    {1,1,0,1,0,1,0,0,0,1,0,1,0,0,1,0},  /* V3P15 */
    {1,1,0,0,1,1,0,0,1,1,0,0,1,1,0,0},  /* V0P11 */
    {1,0,0,1,0,0,0,0,1,1,1,1,1,1,0,0},  /* V0P20 */
    {1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0},  /* V1P04 */
    {1,0,1,0,1,0,1,0,0,0,0,0,0,0,0,0},  /* V1P10 */
    {1,0,1,1,0,0,0,1,0,1,1,1,0,0,1,0},  /* V1P22 */
    {1,0,0,1,1,0,1,0,0,1,0,1,1,0,0,1},  /* V2P20 */
    {1,1,0,1,0,1,0,0,1,1,0,1,0,1,0,0},  /* V2P24 */
    {1,0,0,1,1,0,1,0,0,1,0,1,1,0,1,0},  /* V3P13 */
    {1,0,0,1,0,1,1,0,0,1,1,0,1,0,0,1},  /* V3P20 */
    {1,0,0,1,1,0,1,0,0,1,0,0,1,1,0,1},  /* V3P23 */
    {1,0,1,1,1,0,1,0,1,0,1,1,1,0,0,0},  /* V0P04 */
    {1,0,1,1,1,0,1,1,0,0,0,0,1,0,1,1},  /* V1P21 */
    {0,1,1,0,1,0,1,0,1,1,1,0,1,0,1,0},  /* V1P23 */
    {1,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1},  /* V1P24 */
    {1,1,1,0,1,0,1,0,1,0,1,1,1,0,0,0},  /* V2P03 */
    {1,1,0,1,1,0,0,1,1,1,0,0,1,0,0,1},  /* V3P22 */
    {1,0,1,1,0,1,1,1,1,1,1,0,0,0,0,1},  /* V0P21 */
    {1,0,0,1,1,1,1,0,0,0,1,1,0,1,1,1},  /* V0P22 */
    {1,1,0,1,1,1,0,0,1,0,1,1,1,1,0,0},  /* V0P24 */
    {1,1,0,1,1,0,1,0,0,0,0,0,0,0,0,0},  /* V2P12 */
    {1,0,1,1,0,1,0,1,1,0,1,1,0,1,0,1},  /* V3P21 */
    {1,0,1,0,0,1,1,0,1,1,1,0,0,1,1,1},  /* V3P24 */
    {1,1,1,0,1,1,1,0,1,1,1,0,1,1,1,0},  /* V2P21 */
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},  /* V2P10 */
};

/* ---- FX sequencer (Delete button): ONE shared 16-step pattern of punch pads ----
 * Modelled on Polyend MESS: a step holds up to 5 pads with their four knobs and
 * pressure locked at write time, a per-step play chance (Always / 10..90% /
 * Like Last / Play X Skip Y), extension steps that hold the previous step, a
 * length, a speed division, gate, swing and direction. Runs per block from the
 * Move tempo; pads it triggers go through the same punch_on/off as a finger. */
#define FXSEQ_STEPS 16
#define FXSEQ_MAXPADS 5
static const double FXSEQ_DIV[7]={0.125,0.25,1.0/3.0,0.5,1.0,2.0,4.0};   /* beats per step: 1/32 1/16 1/8T 1/8 1/4 1/2 1 */
static const char *fxspeed_opts[7]={"1/32","1/16","1/8T","1/8","1/4","1/2","1"};
static const char *fxdir_opts[4]={"Fwd","Bwd","Ping","Rand"};
typedef struct { uint8_t n, ext, chance; uint8_t pad[FXSEQ_MAXPADS]; float lock[FXSEQ_MAXPADS][4]; float press[FXSEQ_MAXPADS]; int cycles; } FxStep;
typedef struct {
    int run, speed, len, dir; float gate, swing;
    FxStep st[FXSEQ_STEPS];
    int pos, pingDir, counter, gateTimer, stepLen, lastTrig, odd, restart; uint32_t rng;
    uint8_t seqHeld[NUM_PUNCH], userHeld[NUM_PUNCH];
} FxSeq;

/* One active punch slot: its own capture ring + running state (up to 5 in series). */
typedef struct {
    int idx;                                   /* effect 0..15, or -1 = empty */
    double env; int releasing;                 /* click-free fade-in / fade-out on press/release */
    float ringL[PUNCH_BUF], ringR[PUNCH_BUF]; int w;
    double readPhase, sliceStart, sliceLen;
    double dPow, wPow, mkGain;                  /* loudness match: dry vs wet power, makeup in use */
    int restartPend; float pSm[4];              /* retrigger queued behind a quick fade; smoothed knob values */
    double lfoPh; float lfoTgt, lfoCur; uint32_t lfoRng; int lfoInit;   /* per-effect LFO (knobs 1-4) */
    double elCur, sliceStartT, pressSm;         /* slice length in use (changes only at a wrap), pending start, smoothed pad pressure */
    Biquad toneFilt;
    /* granular pool (Haze / Mosaic / Smear / Strum / Stretch share it) */
    double gPos[4],gAge[4],gDur[4],gRate[4],gGl[4],gGr[4]; int gAct[4];
    double gSched, stGrid; uint32_t gRng; int gIdx, gPrime;   /* gPrime: first grain fires now + a mid-window one */
    /* glide: per-repeat rate ramp; chop: onset slice + pattern */
    double glRate; int glCycle; int chopStep, chopIdx;
    /* Palette punch (block-processed, 1-block latency like the send buses) */
    float pinL[128],pinR[128],poutL[128],poutR[128];
    /* shimmer 2-head pitch-shift + LP feedback */
    float shL[SHBUF], shR[SHBUF]; int shW, shFill; double shR1, shFbL, shFbR;
    float dblBuf[1024]; int dblW, dblHold; double dblPh; float dblGain;   /* shared stereo microshifter */
} PunchSlot;

typedef struct {
    float globalSat,masterComp,masterLoCut,masterHiCut,masterVol;
    float preamp,overdubMode,stability;int selTrack;
    float globalWowFlut,inputMonitor,inputGain;
    int inSource;                          /* 0 Line, 1 Master, 2-5 = Schwung S1-4 (pub), 6-9 = Move M1-4 (link-in) */
    la_in_shm_t *laShm; int laFd; uint32_t laRead; int laSlotCur;   /* Link Audio track source (OG Move tracks) */
    bpa_shm_t *bpaShm; int bpaFd; uint32_t bpaRead; int bpaSlotCur; /* Schwung published stems source */
    float selfPrevL[128], selfPrevR[128];  /* last block's own output, subtracted when recording the master (feedback guard) */
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
    float dropAmt; int dropActive,dropLeft; uint32_t dropRng; double dropG;
    int scanTimer;   /* Perform: Scan gesture — playheads run fast while >0 */
    /* Palette send buses (block-processed, 1-block latency) + master limiter */
    pfx_slot *busA,*busB,*punchFx;
    atomic_int fxSel[3], fxBusy[3];     /* pending effect id per bus (A, B, punch) for the worker; busy = bus muted while it swaps */
    float sbufAL[128],sbufAR[128],sbufBL[128],sbufBR[128];
    float sretAL[128],sretAR[128],sretBL[128],sretBR[128];
    /* Perform 2: master filter + Mood clock + creative */
    float mfCut, mfReso; int mfMode; float mfStL[6], mfStR[6]; float mfCutSm, mfResoSm;
    float mClock; int mClockMode, mClockSpot; float mclkRatioSm, mclkWet;
    float mclkBufL[4096], mclkBufR[4096]; int mclkW; double mclkPh; double mclkHoldL,mclkHoldR; int mclkCnt;
    float perfTrem, perfTremRate; double tremPh;
    /* Settings 2: character EQ + glue + tape limiter */
    int masterEQ; float masterGlue, tapeLimit;
    int loopFilterMode;                    /* Settings p2: analog character for every loop's LP */
    /* Drift: global COSMOS-style shifting-delay memory station (Sample-button menu) */
    float driftAmt, driftRate, driftSize, driftFb, driftSupr, driftBlur, driftDamp, driftMix;
    float driftSm[8];                 /* smoothed controls, no stepping */
    float *drL[4], *drR[4]; int drLen[4]; int drW[4];
    double drPh[4]; float drDL[4], drDR[4]; double drInEnv;
    int drSilent; float drBleed;   /* abandoned-tail silence bleed */
    Biquad eqLoSh, eqMidPk, eqHiSh; int eqCrush; float eqSat, eqMakeup; double eqCrushHoldL,eqCrushHoldR; int eqCrushCnt;
    double glueEnvL,glueEnvR, tapeLimEnv;
    double compEnvL,compEnvR,casLpL,casLpR;uint32_t rng;
    double cpuPct;   /* smoothed render_block load, % of block budget (Overtake CPU meter) */
    PolyVoice poly[POLY_VOICES]; int rootNote;   /* MIDI-keyboard playback layer */
    /* Punch-in FX (master insert): up to 4 slots in series, per-effect params */
    float punchParams[NUM_PUNCH][4];   /* [effect][Rate,Pitch,Tone,Mix] */
    float punchPress[NUM_PUNCH];        /* per-effect pad pressure */
    float punchLfo[NUM_PUNCH][4];       /* [effect][dest,shape,speed,depth] — knobs 1-4 */
    float punchWidth;                   /* punch-FX stereo width (Settings 2) */
    PunchSlot pslot[NUM_PSLOTS];
    FxSeq fx; int punchFxId;           /* FX sequencer (Delete button) + the Palette punch slot's current effect */
    /* Overdub undo: copy-on-write of the samples an overdub overwrites (one shared buffer) */
    int16_t *undoL,*undoR; uint16_t *undoGen, undoCur; int undoTrack, undoCount, undoLen; atomic_int undoReq;   /* gen stamp per sample: no memset at overdub start */
    atomic_int disintReq;   /* voiceIndex+1: run voice_disintegrate_pass on the worker */
    /* Tape stop / speed-up (Left / Right arrows): log2 speed, smoothed per sample */
    int tapeHold; double tapeLs, tapeSpd, tapeGain;
    /* Input FX (record chain): full EQ + record tape speed */
    float inLow,inMid,inMidFreq,inHigh,inHighFreq;
    /* Tape menu (Capture button) — modelled on Magneto's Tape page */
    float tapeNoise,tapeDrive,tapeHF,tapeLoCut,tapeWow,tapeFlut,tapeGen;
    int midiIn;              /* 0 = ignore external MIDI (default), 1 = keyboard layer on */
    int midiOut, midiOutPrev;              /* mirror loop state to a LaunchControl XL's LEDs */
    uint8_t ledFocusCache[16], ledMuteCache[16];
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
        char names[65][40];      /* slot names (1-based), filled by the worker; '' = empty */
    } sio;
    double gFlutBufL[FLUTTER_BUF],gFlutBufR[FLUTTER_BUF];int gFlutWr;double gFlutSweep,gFlutNextMax;
} loopex_t;

static void set_param(void *inst, const char *key, const char *val);
static void master_eq_update(loopex_t *s);   /* master character EQ (defined near render) */
static int  get_param(void *inst, const char *key, char *buf, int buf_len);

/* ---- Session store: settings blob + raw int16 loop buffers, 8 numbered slots ----
 * Layout: /data/UserData/schwung/loopex-sessions/slotN/{state.txt,meta.txt,tKK.raw}
 * (outside the module dir so reinstalls keep sessions — per the Overtake SDK). */
#define SESS_DIR_BASE "/data/UserData/schwung/loopex-sessions"
#define NUM_SLOTS 64
/* Worker-only: refresh the slot-name cache from disk. */
static void session_scan_names(loopex_t *s){
    char path[352];
    for(int n=1;n<=NUM_SLOTS;n++){ s->sio.names[n][0]=0;
        snprintf(path,sizeof path,"%s/slot%d/name.txt",SESS_DIR_BASE,n);
        FILE *f=fopen(path,"r");
        if(!f){ snprintf(path,sizeof path,"%s/slot%d/state.txt",SESS_DIR_BASE,n);   /* saved before names existed: still a session */
                FILE *g=fopen(path,"r"); if(g){ fclose(g); snprintf(s->sio.names[n],sizeof(s->sio.names[n]),"%02d_saved",n); } continue; }
        if(fgets(s->sio.names[n],(int)sizeof(s->sio.names[n]),f)){ size_t L=strlen(s->sio.names[n]);
            while(L&&(s->sio.names[n][L-1]=='\n'||s->sio.names[n][L-1]=='\r')) s->sio.names[n][--L]=0; }
        fclose(f); }
}
static void voice_disintegrate_pass(Voice *v);   /* fwd: worker runs it off-callback */
static void *session_worker(void *arg){
    loopex_t *s=(loopex_t*)arg;
    char dir[256],path[352];
    while(1){
        while(!atomic_load(&s->sio.request)&&!atomic_load(&s->sio.cancel)&&!atomic_load(&s->undoReq)&&!atomic_load(&s->disintReq)

              &&atomic_load(&s->fxSel[0])<0&&atomic_load(&s->fxSel[1])<0&&atomic_load(&s->fxSel[2])<0) usleep(20000);

        if(atomic_load(&s->sio.cancel)) break;

        /* Effect switches: pfx_select may allocate, so it runs here; the bus is muted meanwhile. */

        for(int b=0;b<3;b++){ int id=atomic_exchange(&s->fxSel[b],-1); if(id<0)continue;

            pfx_slot *sl=(b==0)?s->busA:(b==1)?s->busB:s->punchFx; if(!sl)continue;

            atomic_store(&s->fxBusy[b],1); usleep(4000); pfx_select(sl,id); atomic_store(&s->fxBusy[b],0); }

        /* Overdub undo: put the overwritten samples back (memcpy-sized, off the callback). */

        { int dr=atomic_exchange(&s->disintReq,0); if(dr>0){ int t=dr-1; if(t>=0&&t<NUM_VOICES)voice_disintegrate_pass(&s->voice[t]); } }
        if(atomic_exchange(&s->undoReq,0)){ int t=s->undoTrack;

            if(t>=0&&t<NUM_VOICES&&s->undoCount>0&&s->voice[t].loopLen==s->undoLen){ Voice *v=&s->voice[t];

                for(int i=0;i<s->undoLen;i++) if(s->undoGen[i]==s->undoCur){ v->bufferL[i]=s->undoL[i]; v->bufferR[i]=s->undoR[i]; } }

            s->undoCount=0; s->undoTrack=-1; s->undoCur++; if(!s->undoCur)s->undoCur=1; }

        int req=atomic_exchange(&s->sio.request,0); if(!req) continue;
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
                __atomic_store_n(&v->loopLen,n,__ATOMIC_RELEASE);   /* publish length LAST, buffers first */
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
                    for(int hk=0;hk<4;hk++){ dst->hVol[hk]=src->hVol[hk]; dst->hPan[hk]=src->hPan[hk]; }
                    dst->saturation=src->saturation; dst->wowFlutter=src->wowFlutter;
                    dst->send=src->send; dst->sendB=src->sendB; dst->glitch=src->glitch; dst->scatter=src->scatter;
                    dst->savedLoopLen=len;
                    __atomic_store_n(&dst->loopLen,len,__ATOMIC_RELEASE);   /* publish length LAST */
                    dst->state=VS_PLAYING;
                }
            }
            atomic_store(&s->sio.status,3);
        }
        else if(req==5){                              /* ---- DELETE a slot (worker: unlink the files) ---- */
            snprintf(path,sizeof path,"%s/state.txt",dir); remove(path);
            snprintf(path,sizeof path,"%s/meta.txt",dir);  remove(path);
            snprintf(path,sizeof path,"%s/name.txt",dir);  remove(path);
            for(int i=0;i<NUM_VOICES;i++){ snprintf(path,sizeof path,"%s/t%02d.raw",dir,i); remove(path); }
            rmdir(dir);
            session_scan_names(s);
            atomic_store(&s->sio.status,3);
        }
        atomic_store(&s->sio.busy,0);
    }
    return NULL;
}

/* ---- DJ Filter (single-pole-smooth, Essaim-style: continuous sweep, low Q,
 * transparent at centre, one 12dB/oct biquad per side, state reset on LP<->HP) ---- */
#define MF_NVOICE 12   /* master/loop filter voicings (fwd for the per-loop analog LP) */
static inline float mf_run(float *st, float x, float g, float reso, int voicing);
static void dj_filter_update(Voice *v) {
    double f=(double)v->filterSm;
    int newMode = (f < 0.485) ? -1 : ((f > 0.515) ? 1 : 0);
    if(newMode!=v->djMode){ if(newMode<0){ bq_reset(&v->djLpA); v->lfModeCache=-99; } else if(newMode>0)bq_reset(&v->djHpA); v->djMode=newMode; }
    double Q=0.70710678+(double)v->djReso*5.3;   /* resonance: Butterworth -> ~6 */
    if(newMode<0){ double lpF=200.0*pow(18000.0/200.0, f/0.485); if(lpF>18000.0)lpF=18000.0; bq_set_lp(&v->djLpA,lpF,Q); v->lfG=tanf((float)(M_PI*lpF/SR)); }
    else if(newMode>0){ double t=(f-0.515)/0.485; double hpF=20.0*pow(2000.0/20.0, t); if(hpF>2000.0)hpF=2000.0; bq_set_hp(&v->djHpA,hpF,Q); }
}
static inline void dj_filter_stereo(Voice *v, int voicing, double *l, double *r) {
    if(v->djMode<0){                       /* low-pass side runs the chosen analog voicing */
        if(voicing<0)voicing=0; if(voicing>=MF_NVOICE)voicing=MF_NVOICE-1;
        if(voicing!=v->lfModeCache){ for(int i=0;i<6;i++){ v->lfStL[i]=0.0f; v->lfStR[i]=0.0f; } v->lfModeCache=voicing; }
        *l=(double)mf_run(v->lfStL,(float)*l,v->lfG,v->djReso,voicing);
        *r=(double)mf_run(v->lfStR,(float)*r,v->lfG,v->djReso,voicing);
    }
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
static inline void master_wowflutter_stereo(loopex_t *s, double *l, double *r, double amt) {
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
        double gain=pow(10.0,-gr/20.0); double w=(amt<0.05)?amt/0.05:1.0; gain=1.0+(gain-1.0)*w; *l*=gain;*r*=gain;}
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
static void punch_slot_start(loopex_t *s, PunchSlot *ps, int idx){
    ps->idx=idx; ps->env=0.0; ps->releasing=0; ps->restartPend=0; const PunchDef *d=&PUNCH_DEFS[idx]; float *P=s->punchParams[idx];
    memcpy(ps->pSm,P,sizeof ps->pSm);   /* no glide on engage: start at the pad's values */
    ps->elCur=0.0; ps->sliceStartT=-1.0; ps->pressSm=0.0; ps->dPow=ps->wPow=0.0; ps->mkGain=1.0;
    ps->dblW=0; ps->dblHold=(int)(SR*0.02); ps->dblPh=0.0; ps->dblGain=0.0f; memset(ps->dblBuf,0,sizeof ps->dblBuf);
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
            ps->chopIdx=(int)((double)P[1]*(NUM_CHOP_PAT-0.01)); if(ps->chopIdx<0)ps->chopIdx=0; if(ps->chopIdx>=NUM_CHOP_PAT)ps->chopIdx=NUM_CHOP_PAT-1;
        }
    }
    else if(d->mech==PM_PALETTE){ memset(ps->poutL,0,sizeof ps->poutL); memset(ps->poutR,0,sizeof ps->poutR); }
    else if(d->mech==PM_PITCH){ ps->readPhase=2048.0; }   /* mid-window delay (2-head shifter) */
    else if(d->mech==PM_HAZE||d->mech==PM_STRETCH||d->mech==PM_MOSAIC||d->mech==PM_SMEAR||d->mech==PM_STRUM){
        for(int i=0;i<4;i++)ps->gAct[i]=0; ps->gSched=1e12; ps->gIdx=0; ps->gPrime=1; ps->stGrid=(double)ps->w-4000.0;
        memset(ps->shL,0,sizeof ps->shL); memset(ps->shR,0,sizeof ps->shR); ps->shW=0; ps->shR1=0.0; }   /* clear the Stretch doubler ring */   /* gSched primed: no wait before the first grain */
    else if(d->mech==PM_SHIMMER){ ps->shR1=2048.0; ps->shFbL=ps->shFbR=0.0; ps->shW=0; ps->shFill=0;
        memset(ps->shL,0,sizeof ps->shL); memset(ps->shR,0,sizeof ps->shR); }   /* stale buffer = burst/click on engage */
}
/* Re-tune slice geometry WITHOUT restarting the slot (keeps env + relative phase),
 * so turning Rate on a held effect no longer re-triggers it and clicks. */
static void punch_slot_retune(loopex_t *s, PunchSlot *ps, int idx){
    const PunchDef *d=&PUNCH_DEFS[idx]; float *P=s->punchParams[idx];
    if(d->mech!=PM_REPEAT&&d->mech!=PM_REVERSE&&d->mech!=PM_GLIDE&&d->mech!=PM_CHOP) return;
    double sl=punch_slice_len(d,P,punch_beat());
    ps->sliceLen=sl;   /* elCur picks this up at the next wrap (the slice edge is faded there) */
    /* the ring is frozen while a slice mech holds, so w is still the capture end: end-anchored re-cut */
    if(d->mech!=PM_CHOP){ int st=(((int)ps->w-(int)sl)%PUNCH_BUF+PUNCH_BUF)%PUNCH_BUF; ps->sliceStartT=(double)st; }
}
/* Slice mechs and a frozen Stretch read a snapshot: the ring stops writing while they
 * hold, so a loop held longer than the 2 s ring is never overwritten under the head. */
static inline int punch_holds_ring(const loopex_t *s, const PunchSlot *ps){
    if(ps->idx<0||ps->releasing) return 0;
    int m=PUNCH_DEFS[ps->idx].mech;
    if(m==PM_REPEAT||m==PM_REVERSE||m==PM_GLIDE||m==PM_CHOP) return 1;
    if(m==PM_STRETCH){ const float *P=s->punchParams[ps->idx]; return ((1.0-(double)P[0])*(1.0-ps->pressSm))<0.02; }
    return 0;
}
static void punch_on(loopex_t *s, int idx){
    if(idx<0||idx>=NUM_PUNCH)return;
    for(int i=0;i<NUM_PSLOTS;i++) if(s->pslot[i].idx==idx){ PunchSlot *ps=&s->pslot[i];   /* retrigger (also un-releases) */
        if(ps->releasing||ps->env<0.05){ punch_slot_start(s,ps,idx); } else ps->restartPend=1;   /* running: fade down first, restart in render */
        return; }
    for(int i=0;i<NUM_PSLOTS;i++) if(s->pslot[i].idx<0){ punch_slot_start(s,&s->pslot[i],idx); return; }      /* first free */
}
static void punch_off(loopex_t *s, int idx){
    for(int i=0;i<NUM_PSLOTS;i++) if(s->pslot[i].idx==idx){ s->pslot[i].releasing=1; return; }  /* fade out, freed in render */
}
/* per-block: set each active slot's tone LP coeffs from its effect's Tone param */
static void punch_prep(loopex_t *s){
    for(int i=0;i<NUM_PSLOTS;i++){ PunchSlot *ps=&s->pslot[i]; if(ps->idx<0)continue; float t=ps->pSm[2];
        if(t<0.98f){ double cut=500.0*pow(18000.0/500.0,(double)t); bq_set_lp(&ps->toneFilt,cut,0.707); } }
}

static void fxseq_init(FxSeq *f){ memset(f,0,sizeof *f); f->speed=1; f->len=16; f->gate=1.0f; f->swing=0.5f; f->pos=-1; f->pingDir=1; f->gateTimer=-1; f->rng=0x5eedf00du; f->lastTrig=1; }
static void fxseq_off_all(loopex_t *s){
    for(int p=0;p<NUM_PUNCH;p++) if(s->fx.seqHeld[p]){ s->fx.seqHeld[p]=0; if(!s->fx.userHeld[p]) punch_off(s,p); }
}
static int fxseq_peek(const FxSeq *f, int pos){   /* the step after `pos` in the current direction */
    int len=f->len<1?1:f->len;
    if(f->dir==1) return (pos-1+len)%len;
    if(f->dir==2){ int n=pos+f->pingDir; if(n>=len)n=len>1?len-2:0; if(n<0)n=len>1?1:0; return n; }
    return (pos+1)%len;
}
static void fxseq_next(FxSeq *f){
    int len=f->len<1?1:f->len;
    if(f->dir==3){ f->pos=(int)(PRND(f->rng)*len); if(f->pos>=len)f->pos=len-1; return; }
    if(f->dir==2){ int n=f->pos+f->pingDir; if(n>=len){ f->pingDir=-1; n=len>1?len-2:0; } if(n<0){ f->pingDir=1; n=len>1?1:0; } f->pos=n; return; }
    f->pos=fxseq_peek(f,f->pos<0?(f->dir==1?0:len-1):f->pos);
    if(f->pos>=len)f->pos=0;
}
static int fxseq_chance(FxSeq *f, FxStep *st){
    int c=st->chance, trig=1; st->cycles++;
    if(c>=1&&c<=9)      trig=(PRND(f->rng)<(double)c*0.1);
    else if(c==10)      trig=f->lastTrig;
    else if(c==11)      trig=((st->cycles-1)%2)==0;    /* Play 1 Skip 1 */
    else if(c==12)      trig=((st->cycles-1)%3)<2;     /* Play 2 Skip 1 */
    else if(c==13)      trig=((st->cycles-1)%2)==1;    /* Skip 1 Play 1 */
    f->lastTrig=trig; return trig;
}
static void fxseq_apply(loopex_t *s, int idx){
    FxSeq *f=&s->fx; FxStep *st=&f->st[idx];
    if(st->ext){ f->gateTimer=-1; return; }                     /* extension: the previous step keeps holding */
    int trig=(st->n>0)?fxseq_chance(f,st):0;
    uint8_t keep[NUM_PUNCH]; memset(keep,0,sizeof keep);
    if(trig) for(int k=0;k<st->n&&k<FXSEQ_MAXPADS;k++) keep[st->pad[k]&15]=1;
    for(int p=0;p<NUM_PUNCH;p++) if(f->seqHeld[p]&&!keep[p]){ f->seqHeld[p]=0; if(!f->userHeld[p]) punch_off(s,p); }
    if(trig) for(int k=0;k<st->n&&k<FXSEQ_MAXPADS;k++){ int p=st->pad[k]&15;
        memcpy(s->punchParams[p],st->lock[k],sizeof st->lock[k]); s->punchPress[p]=st->press[k];
        if(PUNCH_DEFS[p].mech==PM_PALETTE){ int id=(int)(st->lock[k][0]*(PFX_NUM-1)+0.5f); if(id<0)id=0; if(id>=PFX_NUM)id=PFX_NUM-1;
            if(id!=s->punchFxId){ s->punchFxId=id; atomic_store(&s->fxSel[2],id); } }
        if(!f->userHeld[p]) punch_on(s,p);                        /* retriggers a running one, as a finger would */
        f->seqHeld[p]=1; }
    int nextExt=f->st[fxseq_peek(f,idx)].ext;
    f->gateTimer=(trig&&f->gate<0.99f&&!nextExt)?(int)((double)f->stepLen*(double)f->gate):-1;
}
static void fxseq_tick(loopex_t *s, int frames){
    FxSeq *f=&s->fx;
    if(f->restart){ f->restart=0; f->pos=-1; f->pingDir=1; f->counter=0; f->odd=0; f->gateTimer=-1; f->lastTrig=1;
        for(int i=0;i<FXSEQ_STEPS;i++) f->st[i].cycles=0; fxseq_off_all(s); }
    if(!f->run){ fxseq_off_all(s); return; }
    if(f->gateTimer>0){ f->gateTimer-=frames; if(f->gateTimer<=0){ f->gateTimer=-1; fxseq_off_all(s); } }
    f->counter-=frames;
    while(f->counter<=0){
        double sl=punch_beat()*FXSEQ_DIV[f->speed]; f->stepLen=(int)sl;
        fxseq_next(f); fxseq_apply(s,f->pos);
        double sw=((double)f->swing-0.5)*2.0; int even=(f->odd==0); f->odd^=1;   /* swing: even steps long, odd short */
        int d=(int)(sl*(even?(1.0+sw):(1.0-sw))); if(d<64)d=64; f->counter+=d;
    }
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
/* Stereo microshifter: two slowly-detuned taps off a mono ring, widening a mono
 * effect into stereo. A hold + fade-in on engage means the ring is primed before
 * the taps read it, so there is no zero-to-signal step (the old engage click). */
#define DBL_BUF 1024
static inline void punch_doubler(PunchSlot *ps, double *sl, double *sr){
    float mono=(float)(0.5*(*sl+*sr)); int wr=ps->dblW; ps->dblBuf[wr]=mono;
    ps->dblW++; if(ps->dblW>=DBL_BUF)ps->dblW=0;
    if(ps->dblHold>0){ ps->dblHold--; return; }             /* fill the ring past the max tap first */
    ps->dblGain += (1.0f-ps->dblGain)*0.0008f;              /* ~14 ms fade-in */
    ps->dblPh+=0.13/SR; if(ps->dblPh>=1.0)ps->dblPh-=1.0;
    double m1=sin(TWOPI*ps->dblPh), m2=sin(TWOPI*(ps->dblPh*1.46+0.25));
    double dl=(double)(SR*0.009)+m1*(SR*0.0016), dr=(double)(SR*0.013)+m2*(SR*0.0016);
    double rpl=(double)wr-dl; while(rpl<0)rpl+=DBL_BUF; double rpr=(double)wr-dr; while(rpr<0)rpr+=DBL_BUF;
    int il0=(int)rpl, il1=(il0+1)%DBL_BUF; double lf=rpl-(double)il0; il0%=DBL_BUF;   /* interpolate the LFO'd taps */
    int ir0=(int)rpr, ir1=(ir0+1)%DBL_BUF; double rf=rpr-(double)ir0; ir0%=DBL_BUF;
    double dblL=ps->dblBuf[il0]*(1.0-lf)+ps->dblBuf[il1]*lf;
    double dblR=ps->dblBuf[ir0]*(1.0-rf)+ps->dblBuf[ir1]*rf;
    double g=(double)ps->dblGain;
    *sl=*sl*(1.0-0.2*g)+dblL*0.5*g; *sr=*sr*(1.0-0.2*g)+dblR*0.5*g;   /* dry unity at g=0: no step when the hold ends */
}
static inline void punch_slot_process(loopex_t *s, PunchSlot *ps, int n, double *outL, double *outR){
    const PunchDef *d=&PUNCH_DEFS[ps->idx];
    for(int j=0;j<4;j++) ps->pSm[j]+=(s->punchParams[ps->idx][j]-ps->pSm[j])*0.004f;   /* ~6 ms: knob detents do not step the sound */
    float *P=ps->pSm; int toneOn=(P[2]<0.98f);
    if(d->mech==PM_PALETTE){   /* one Palette effect as a punch: this block in, last block out */
        ps->pinL[n]=ps->ringL[ps->w]; ps->pinR[n]=ps->ringR[ps->w];
        *outL=(double)ps->poutL[n]; *outR=(double)ps->poutR[n]; return; }
    ps->pressSm+=((double)s->punchPress[ps->idx]-ps->pressSm)*0.002;   /* ~11 ms */
    double press=ps->pressSm;
    /* per-effect LFO (knobs 1-4 = dest, shape, speed, depth): modulates one fx param */
    { const float *Lf=s->punchLfo[ps->idx]; int dest=(int)(Lf[0]*4.99f);
      if(dest>0){ if(!ps->lfoInit){ ps->lfoInit=1; ps->lfoRng=0x2545f491u+(uint32_t)ps->idx*2654435761u; ps->lfoTgt=ps->lfoCur=0.0f; }
        int shape=(int)(Lf[1]*7.99f); double hz=0.05*pow(400.0,(double)Lf[2]); double ph=ps->lfoPh;
        ps->lfoPh+=hz/SR; int wrapped=0; if(ps->lfoPh>=1.0){ ps->lfoPh-=1.0; wrapped=1; }
        double v;
        switch(shape){
          case 1: v=1.0-4.0*fabs(ph-0.5); break;                         /* triangle */
          case 2: v=1.0-2.0*ph; break;                                   /* saw down */
          case 3: v=2.0*ph-1.0; break;                                   /* ramp up */
          case 4: v=(ph<0.5)?1.0:-1.0; break;                            /* square */
          case 5: v=(ph<0.25)?1.0:-1.0; break;                           /* pulse 25% */
          case 6: if(wrapped) ps->lfoCur=(float)(lb_rand(&ps->lfoRng)*2.0); v=ps->lfoCur; break;   /* S&H */
          case 7: if(wrapped) ps->lfoTgt=(float)(lb_rand(&ps->lfoRng)*2.0); ps->lfoCur+=(ps->lfoTgt-ps->lfoCur)*0.002f; v=ps->lfoCur; break; /* smoothed random */
          default: v=sin(TWOPI*ph); }                                    /* sine */
        int di=dest-1; float mv=P[di]+(float)(v*(double)Lf[3]*0.5); ps->pSm[di]=mv<0?0:(mv>1?1:mv); } }
    double pm=(d->mech==PM_CHOP)?1.0:pow(2.0,((double)P[1]-0.5)*2.0);   /* Chop: knob 2 is the pattern, not pitch */
    if(d->mech==PM_HAZE||d->mech==PM_SMEAR){   /* granular clouds: P0=Size P1=Pitch P2=Density; pressure = density */
        int smear=(d->mech==PM_SMEAR);
        double dur =smear?((0.15+(double)P[0]*0.65)*SR):((0.03+(double)P[0]*0.4)*SR);
        double dens=(smear?(1.0+(double)P[2]*12.0):(2.0+(double)P[2]*40.0))*(1.0+press*3.0);
        double pr=pm;
        ps->gSched+=dens/SR;
        if(ps->gSched>=1.0){ ps->gSched-=1.0; if(ps->gSched>1.0)ps->gSched=0.0;
            double back=dur*(0.5+(pr>1.0?pr:1.0))+PRND(ps->gRng)*dur*3.0; double pos=(double)ps->w-back;   /* room for the whole read, even pitched up */
            double pan=(PRND(ps->gRng)*2.0-1.0)*(smear?0.9:0.6);
            double rate=pr; if(smear&&PRND(ps->gRng)<0.5){ rate=-pr; pos+=dur*rate*-1.0; }   /* reversed grains land ahead of their read */
            punch_grain(ps,pos,dur,rate,pan);
            if(ps->gPrime){ ps->gPrime=0;   /* engage: a second grain already at full window so the cloud is audible at once */
                int gi=punch_grain(ps,pos-dur*0.5*rate,dur,rate,-pan); if(gi>=0)ps->gAge[gi]=dur*0.5; } }
        double sl,sr; punch_grains_out(ps,&sl,&sr);
        double g=smear?0.8:0.9; *outL=sl*g; *outR=sr*g; return; }
    if(d->mech==PM_MOSAIC){   /* grid-synced re-sequencer: P0=Grid P1=Pitch P2=Var; pressure = grid x2 */
        double beat=punch_beat(); int dv=1<<(int)((double)P[0]*3.99); double grid=beat/(double)dv;
        if(press>0.5)grid*=0.5;
        ps->gSched+=1.0;
        if(ps->gSched>=grid){ ps->gSched-=grid; if(ps->gSched>grid)ps->gSched=0.0;
            double var=(double)P[2];
            int cell=1+(int)(PRND(ps->gRng)*(1.0+var*7.0));            /* which earlier grid cell to quote */
            double pos=(double)ps->w-(double)cell*grid;
            if(var>0.0) pos-=PRND(ps->gRng)*grid*var*0.5;
            double rate=(PRND(ps->gRng)<var*0.6)?-pm:pm;
            if(rate<0) pos+=grid;                                     /* read backwards from the cell end */
            { double need=grid*0.95*(rate>0?rate:0.0)+64.0; if((double)ps->w-pos<need) pos=(double)ps->w-need; }   /* stay behind the write head */
            double pan=(ps->gIdx&1)?0.6:-0.6; ps->gIdx++;
            punch_grain(ps,pos,grid*0.95,rate,pan);
            if(ps->gPrime){ ps->gPrime=0; int gi=punch_grain(ps,pos-grid*0.475*rate,grid*0.95,rate,-pan); if(gi>=0)ps->gAge[gi]=grid*0.475; } }
        double sl,sr; punch_grains_out(ps,&sl,&sr); *outL=sl; *outR=sr; return; }
    if(d->mech==PM_STRUM){   /* arpeggiated grain cascade: P0=Rate P1=Dir/Range P2=Tone; pressure = faster + wider */
        double ivl=(0.06+(1.0-(double)P[0])*0.4)*SR*(1.0-press*0.6);
        ps->gSched+=1.0;
        if(ps->gSched>=ivl){ ps->gSched-=ivl; if(ps->gSched>ivl)ps->gSched=0.0;
            static const double up[4]={0,7,12,19}, dn[4]={0,-5,-12,-17};
            int k=ps->gIdx&3; double semis=((double)P[1]>=0.5)?up[k]:dn[k];
            if(press>0.7) semis+=((double)P[1]>=0.5)?12.0:-12.0;
            double rate=pow(2.0,semis/12.0);
            double pos=(double)ps->w-ivl*2.0-200.0; if(rate>1.0) pos-=ivl*rate;   /* faster reads need more room */
            double pan=-0.7+(double)k/3.0*1.4;
            punch_grain(ps,pos,ivl*1.6,rate,pan); ps->gIdx++;
            if(ps->gPrime){ ps->gPrime=0; int gi=punch_grain(ps,pos-ivl*0.8*rate,ivl*1.6,rate,pan); if(gi>=0)ps->gAge[gi]=ivl*0.8; } }
        double sl,sr; punch_grains_out(ps,&sl,&sr);
        if(toneOn){ sl=bq_L(&ps->toneFilt,sl); sr=bq_R(&ps->toneFilt,sr); } *outL=sl; *outR=sr; return; }
    if(d->mech==PM_STRETCH){   /* Freeze/Stretch as a lush multi-grain wash: FOUR grains, each with a
                                * randomised length and (as the hold approaches freeze) a read position that
                                * wanders backward through the held 2 s ring, given its own stereo place and a
                                * small L/R micro-offset. Summed as a weighted average (floored) so the level
                                * stays flat and never rings or clicks, and the random lengths keep the four
                                * respawns from ever sharing a period -> a non-periodic hall-of-mirrors hold.
                                * P0 = stretch (1 = freeze) · P1 = Pitch · P2 = Grain size; pressure -> freeze */
        double dur=(0.09+(double)P[2]*0.5)*SR, pr=pm;                 /* 90..590 ms: longer, smoother windows */
        double srate=(1.0-(double)P[0])*(1.0-press);
        ps->stGrid+=srate;
        { double ahead=fmod((double)ps->w-ps->stGrid,(double)PUNCH_BUF); if(ahead<0)ahead+=PUNCH_BUF;   /* keep the read span behind the head */
          double need=dur*(pr>1.0?pr:1.0)+256.0; if(ahead<need) ps->stGrid=(double)ps->w-need; }
        double frz=1.0-srate; if(frz<0.0)frz=0.0;                    /* 0 moving -> 1 frozen: more wander + wider spread when held */
        for(int i=0;i<4;i++){ if(!ps->gAct[i]||ps->gAge[i]>=ps->gDur[i]){   /* (re)spawn grain i */
            ps->gAct[i]=1;
            ps->gDur[i]=dur*(0.75+0.5*PRND(ps->gRng));               /* random length: respawns never share a period */
            ps->gRate[i]=pr;                                          /* latched: Pitch takes effect at the next grain */
            ps->gPos[i]=ps->stGrid-PRND(ps->gRng)*frz*dur*2.0;        /* wander backward into the frozen buffer */
            double pan=(PRND(ps->gRng)*2.0-1.0)*(0.35+0.5*frz);       /* stereo place, wider toward freeze */
            ps->gGl[i]=cos((pan+1.0)*0.25*M_PI); ps->gGr[i]=sin((pan+1.0)*0.25*M_PI);
            ps->gAge[i]=ps->gPrime?((double)i*0.25*ps->gDur[i]):0.0;  /* engage: spread the four across the window so they overlap at once */
        } }
        ps->gPrime=0;
        double numL=0.0,numR=0.0,wsum=0.5;                            /* wsum floor guards the divide (staggered grains keep it ~2) */
        for(int i=0;i<4;i++){ double a=ps->gAge[i]; double wph=a/ps->gDur[i]; if(wph>=1.0){ps->gAct[i]=0;continue;}
            double win=0.5-0.5*cos(TWOPI*wph), rp=ps->gPos[i]+a*ps->gRate[i];
            double roff=(double)(97+i*211);                          /* per-grain L/R micro-offset -> decorrelated stereo */
            numL+=(double)ring_read(ps->ringL,rp)*win*(double)ps->gGl[i];
            numR+=(double)ring_read(ps->ringR,rp+roff)*win*(double)ps->gGr[i];
            wsum+=win; ps->gAge[i]+=1.0; }
        double norm=1.6/wsum;                                         /* weighted average = flat level; 1.6 makeup for the constant-power spread */
        double sl=numL*norm, sr=numR*norm;
        punch_doubler(ps,&sl,&sr);   /* extra stereo width */
        *outL=sl; *outR=sr; return; }
    if(d->mech==PM_SHIMMER){   /* octave-up shifter in band-limited feedback: P0=Regen P1=Pitch (0.5 = +1 oct) P2=Tone; pressure = regen */
        double ratio=pow(2.0,(double)P[1]*2.0), regen=(double)P[0]*0.85+press*0.3; if(regen>0.95)regen=0.95;
        double inL=(double)ps->ringL[ps->w]+ps->shFbL, inR=(double)ps->ringR[ps->w]+ps->shFbR;
        ps->shL[ps->shW]=(float)inL; ps->shR[ps->shW]=(float)inR;
        /* same 2-head Hann-crossfaded delay-line shifter as Oct+/Oct-, on the feedback buffer */
        const double W=4096.0;
        ps->shR1+=(1.0-ratio); while(ps->shR1>=W)ps->shR1-=W; while(ps->shR1<0)ps->shR1+=W;
        double d1=ps->shR1, d2=d1+W*0.5; if(d2>=W)d2-=W;
        double lim=(double)ps->shFill-1.0; if(lim<1.0)lim=1.0;   /* engage: only read what has been written (no zero-seam click) */
        if(d1>lim)d1=lim; if(d2>lim)d2=lim;
        double w1=0.5-0.5*cos(TWOPI*d1/W), w2=0.5-0.5*cos(TWOPI*d2/W), ws=w1+w2+1e-9;
        double rp1=(double)ps->shW-d1, rp2=(double)ps->shW-d2;
        double l=((double)buf_read(ps->shL,rp1,SHBUF)*w1+(double)buf_read(ps->shL,rp2,SHBUF)*w2)/ws;
        double r=((double)buf_read(ps->shR,rp1,SHBUF)*w1+(double)buf_read(ps->shR,rp2,SHBUF)*w2)/ws;
        ps->shW++; if(ps->shW>=SHBUF)ps->shW=0; if(ps->shFill<SHBUF)ps->shFill++;
        double fl=l,fr=r; if(toneOn){ fl=bq_L(&ps->toneFilt,fl); fr=bq_R(&ps->toneFilt,fr); }
        ps->shFbL=lb_tanh(fl*regen); ps->shFbR=lb_tanh(fr*regen);   /* each pass climbs another interval: the shimmer */
        punch_doubler(ps,&l,&r);   /* stereo width */
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
        if(toneOn){ l=bq_L(&ps->toneFilt,l); r=bq_R(&ps->toneFilt,r); }
        punch_doubler(ps,&l,&r);   /* stereo width */
        *outL=l; *outR=r; return; }
    /* ---- slice-based mechs: REPEAT / REVERSE / GLIDE / CHOP (all autopanned to their cycle) ---- */
    /* Target slice length from the knob + pressure; the length IN USE (elCur) only
     * changes when a slice wraps, where the edge fade already sits at zero - so
     * pressure sweeps the repeat rate continuously (no stairs) and never mid-slice. */
    double elT=ps->sliceLen;
    if(d->mech==PM_REPEAT)       elT=ps->sliceLen*pow(0.5,press*2.0);       /* 1/1 .. 1/4, continuous */
    else if(d->mech==PM_REVERSE) elT=ps->sliceLen*(1.0-press*0.6);
    else if(d->mech==PM_CHOP)    elT=ps->sliceLen*(press>0.5?0.5:1.0);      /* rhythmic: stays on the grid */
    if(elT<128.0)elT=128.0;
    if(ps->elCur<=0.0)ps->elCur=elT;
    double pos, gate=1.0, bf=1.0, el=ps->elCur;
    #define PUNCH_LATCH() do{ ps->elCur=elT; el=elT; if(ps->sliceStartT>=0.0){ ps->sliceStart=ps->sliceStartT; ps->sliceStartT=-1.0; } }while(0)
    if(d->mech==PM_REPEAT){
        pos=ps->sliceStart+ps->readPhase; ps->readPhase+=pm;
        if(ps->readPhase>=el){ ps->readPhase-=el; PUNCH_LATCH(); while(ps->readPhase>=el)ps->readPhase-=el; } }
    else if(d->mech==PM_REVERSE){
        pos=ps->sliceStart+ps->readPhase; ps->readPhase-=pm;
        if(ps->readPhase<0){ PUNCH_LATCH(); ps->readPhase+=el; while(ps->readPhase<0)ps->readPhase+=el; if(ps->readPhase>=el)ps->readPhase=el-1.0; } }
    else if(d->mech==PM_GLIDE){      /* each repeat re-pitches: P1 = down/up, pressure = harder glide */
        pos=ps->sliceStart+ps->readPhase; ps->readPhase+=pm*ps->glRate;
        if(ps->readPhase>=el){
            ps->readPhase-=el; PUNCH_LATCH(); while(ps->readPhase>=el)ps->readPhase-=el;
            double amt=((double)P[1]-0.5)*(0.5+press*1.0);              /* -0.75..+0.75 per cycle */
            ps->glRate*=pow(2.0,amt*0.5); ps->glCycle++;
            if(ps->glRate<0.2||ps->glRate>5.0||ps->glCycle>=8){ ps->glRate=1.0; ps->glCycle=0; } } }
    else {                           /* CHOP: repeat the last hit on a seeded pattern; pressure doubles the rate */
        pos=ps->sliceStart+ps->readPhase; ps->readPhase+=pm;
        if(ps->readPhase>=el){ ps->readPhase-=el; PUNCH_LATCH(); while(ps->readPhase>=el)ps->readPhase-=el; ps->chopStep++; }
        { int ci=(int)((double)P[1]*(NUM_CHOP_PAT-0.01)); if(ci<0)ci=0; if(ci>=NUM_CHOP_PAT)ci=NUM_CHOP_PAT-1; ps->chopIdx=ci; }   /* Patrn knob, live */
        gate=CHOP_PAT[ps->chopIdx][ps->chopStep&15]?1.0:0.0;
        double gp=ps->readPhase/el; if(gp>0.85)gate*=(1.0-gp)/0.15;     /* short tail so hits stay separate */ }
    #undef PUNCH_LATCH
    double FD=el*0.25; if(FD>192.0)FD=192.0;                                 /* edge fade: up to ~4 ms */
    double e=ps->readPhase<el-ps->readPhase?ps->readPhase:el-ps->readPhase; if(e<FD)bf=e/FD;
    double l=(double)ring_read(ps->ringL,pos)*gate*bf, r=(double)ring_read(ps->ringR,pos)*gate*bf;
    double cyc=ps->readPhase/el; if(d->mech==PM_CHOP) cyc=(double)(ps->chopStep&1);   /* chop: alternate L/R per hit */
    punch_autopan(cyc,0.7,&l,&r);
    if(toneOn){ l=bq_L(&ps->toneFilt,l); r=bq_R(&ps->toneFilt,r); }
    if(d->mech==PM_CHOP) punch_doubler(ps,&l,&r);   /* stereo width */
    *outL=l; *outR=r;
}

/* Read a voice's buffer at an absolute phase (wrapped + linear-interpolated). */
static inline void vbuf_read(Voice *v,double ph,double *l,double *r){
    int L=__atomic_load_n(&v->loopLen,__ATOMIC_ACQUIRE); if(L<1||!v->bufferL||!v->bufferR){*l=0.0;*r=0.0;return;}
    double q=ph; while(q<0)q+=(double)L; while(q>=(double)L)q-=(double)L;
    int i0=(int)q%L,i1=(i0+1)%L; double f=q-floor(q);
    *l=((double)v->bufferL[i0]*(1.0-f)+(double)v->bufferL[i1]*f)/32768.0;
    *r=((double)v->bufferR[i0]*(1.0-f)+(double)v->bufferR[i1]*f)/32768.0;
}

/* Move a playhead by d samples click-free: head 0 through the scatter crossfade,
 * heads 1-3 through their own 64-sample crossfade from the old position. */
static inline void head_jump(Voice *v, int k, double d){
    double L=(double)v->loopLen; if(L<1.0)return;
    if(k==0){ v->scatXfadePhase=v->playPhase; v->scatXfade=64; v->playPhase+=d;
        while(v->playPhase>=L)v->playPhase-=L; while(v->playPhase<0.0)v->playPhase+=L; return; }
    Playhead *P=&v->ph[k]; P->xfPhase=P->phase; P->xf=64; P->phase+=d;
    while(P->phase>=L)P->phase-=L; while(P->phase<0.0)P->phase+=L;
}
/* ---- Voice Render (stereo) ---- */
static void voice_render(Voice *v, loopex_t *s, int n, double *outL, double *outR, double *sendAL, double *sendAR, double *sendBL, double *sendBR) {
    *outL=*outR=*sendAL=*sendAR=*sendBL=*sendBR=0.0;
    int playing=(v->state==VS_PLAYING||v->state==VS_OVERDUBBING);
    int scrubbing=(v->scrubTimer>0);
    int LEN=__atomic_load_n(&v->loopLen,__ATOMIC_ACQUIRE);            /* snapshot: the worker may zero it mid-render */
    if((!playing && !scrubbing && v->playEnv<0.0005)||LEN<=0||!v->bufferL||!v->bufferR){ if(!playing)v->playEnv=0.0; return; }
    int effStart=(int)(v->loopStart*(float)LEN); if(effStart<0)effStart=0; if(effStart>LEN-1)effStart=LEN-1;
    int avail=LEN-effStart; if(avail<1)avail=1;                        /* End is loop LENGTH from Start */
    int effLen=(int)(v->loopEnd*(float)avail); if(effLen<256)effLen=256; if(effLen>avail)effLen=avail;
    int effEnd=effStart+effLen;
    double rate=pow(2.0,(double)v->pitch)*s->tapeSpd;if(v->reverse>0.5f)rate=-rate;
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
    int i0=(int)absPhase%LEN;int i1=(i0+1)%LEN;double frac=absPhase-floor(absPhase);
    double rawL=((double)v->bufferL[i0]*(1.0-frac)+(double)v->bufferL[i1]*frac)/32768.0;
    double rawR=((double)v->bufferR[i0]*(1.0-frac)+(double)v->bufferR[i1]*frac)/32768.0;
    if(v->scatXfade>0){   /* raised-cosine crossfade from the pre-jump position (scatter declick) */
        double op=v->scatXfadePhase; while(op<0)op+=(double)LEN; while(op>=(double)LEN)op-=(double)LEN;
        int oi0=(int)op%LEN,oi1=(oi0+1)%LEN; double ofr=op-floor(op);
        double oL=((double)v->bufferL[oi0]*(1.0-ofr)+(double)v->bufferL[oi1]*ofr)/32768.0;
        double oR=((double)v->bufferR[oi0]*(1.0-ofr)+(double)v->bufferR[oi1]*ofr)/32768.0;
        double t=0.5-0.5*cos(M_PI*(1.0-(double)v->scatXfade/64.0));
        rawL=oL*(1.0-t)+rawL*t; rawR=oR*(1.0-t)+rawR*t;
        v->scatXfadePhase+=rate; v->scatXfade--; }
    if(playing||scrubbing){ v->playPhase+=rate;double dEnd=(double)effEnd,dStart=(double)effStart;
        if(v->ph[0].mode==3&&!scrubbing){ if(v->playPhase>=dEnd){v->playPhase=dEnd-1.0;v->ph[0].dir=-1;}
            else if(v->playPhase<dStart){v->playPhase=dStart;v->ph[0].dir=1;} }
        else { while(v->playPhase>=dEnd)v->playPhase-=(double)effLen;while(v->playPhase<dStart)v->playPhase+=(double)effLen; }
        if(v->ph[0].mode==4&&!scrubbing){ if(--v->ph[0].jumpCd<=0){         /* Jump: forward with periodic random leaps */
            v->ph[0].jumpCd=(int)(SR*(0.06+0.5*(lb_rand(&v->rng)*0.5+0.5)));
            v->scatXfadePhase=v->playPhase; v->scatXfade=64;                 /* declick the leap */
            v->playPhase=dStart+(lb_rand(&v->rng)*0.5+0.5)*(double)effLen;
            while(v->playPhase>=dEnd)v->playPhase-=(double)effLen; } }
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
        if(P->xf>0){ double ol,orr; vbuf_read(v,P->xfPhase,&ol,&orr); double w=(double)P->xf/64.0;   /* jump crossfade */
            hl=hl*(1.0-w)+ol*w; hr=hr*(1.0-w)+orr*w;
            if(playing||scrubbing) P->xfPhase+=baseRate*P->mult*((P->mode==2)?-1.0:(P->mode==3)?(double)P->dir:1.0);
            P->xf--; }
        double hrel=P->phase-(double)effStart;
        while(hrel<0)hrel+=(double)effLen; while(hrel>=(double)effLen)hrel-=(double)effLen;
        double hbf=1.0; if(hrel<_FD)hbf=hrel/_FD; else if(hrel>(double)effLen-_FD)hbf=((double)effLen-hrel)/_FD;
        if(hbf<0.0)hbf=0.0;
        double hv=(double)v->hVol[k], hp=(double)v->hPan[k];   /* per-head Vol + balance (center = unity, no level change) */
        double hpl=(hp<=0.0)?1.0:(1.0-hp), hpr=(hp>=0.0)?1.0:(1.0+hp);
        double hg=P->env*hbf*hv;
        rawL+=hl*hg*hpl; rawR+=hr*hg*hpr; envSum+=P->env;
        if(playing||scrubbing){ double pr=baseRate*P->mult;
            if(P->mode==2) pr=-pr; else if(P->mode==3) pr*=(double)P->dir;
            P->phase+=pr;
            if(P->mode==3){ if(P->phase>=(double)effEnd){P->phase=(double)effEnd-1.0;P->dir=-1;}
                            else if(P->phase<(double)effStart){P->phase=(double)effStart;P->dir=1;} }
            else { while(P->phase>=(double)effEnd)P->phase-=(double)effLen;
                   while(P->phase<(double)effStart)P->phase+=(double)effLen; }
            if(P->mode==4){ if(--P->jumpCd<=0){                              /* Jump: periodic random leaps */
                P->jumpCd=(int)(SR*(0.06+0.5*(lb_rand(&v->rng)*0.5+0.5)));
                P->xfPhase=P->phase; P->xf=64;
                P->phase=(double)effStart+(lb_rand(&v->rng)*0.5+0.5)*(double)effLen;
                while(P->phase>=(double)effEnd)P->phase-=(double)effLen; } } } }
    if(envSum>1.0){ double nrm=1.0/sqrt(envSum); rawL*=nrm; rawR*=nrm; }   /* keep level sane as heads stack */
    /* Pitch (P2 knob 1): Signalsmith Stretch, independent of the playback rate.
     * The shifter runs one 128-frame block late (psIn this block -> psOut next block).
     * Engage: feed it, wait its latency, then crossfade to its output while nudging
     * every playhead forward by that latency so the loop stays in time. Disengage:
     * crossfade back and nudge the heads back. */
    { int want=(v->clock>0.005f||v->clock<-0.005f);
      if(want&&!v->psActive&&v->ps){ v->psActive=1; v->psWarm=0; v->psMix=0.0; ps_reset(v->ps); v->psLat=ps_latency(v->ps)+128; v->psCache=-99.0f; }
      if(v->psActive){
        if(v->clock!=v->psCache&&want){ ps_set_semitones(v->ps,v->clock*12.0f); v->psCache=v->clock; }
        v->psInL[n]=(float)rawL; v->psInR[n]=(float)rawR;
        double target=0.0;
        if(want){ if(v->psWarm<v->psLat){ v->psWarm++; if(v->psWarm==v->psLat) v->psNudge=v->psLat; } else target=1.0; }
        else if(v->psWarm>=0){ int _n=(v->psWarm>=v->psLat); v->psWarm=-1; if(_n)v->psNudge=-v->psLat; }   /* only undo the forward nudge if it happened */
        v->psMix+=(target-v->psMix)*0.004;   /* ~6 ms ramp */
        if(v->psNudge){ double d=(double)v->psNudge; v->psNudge=0;   /* keep time: the shifted stream lags by psLat */
            for(int k=0;k<4;k++) head_jump(v,k,d); }
        rawL=rawL*(1.0-v->psMix)+(double)v->psOutL[n]*v->psMix; rawR=rawR*(1.0-v->psMix)+(double)v->psOutR[n]*v->psMix;
        if(!want&&v->psMix<0.0005){ v->psActive=0; v->psMix=0.0; }
      } }
    double _amp=v->playEnv*_bf;
    double sL=rawL,sR=rawR;
    sL=voice_saturate(sL,(double)v->saturation);sR=voice_saturate(sR,(double)v->saturation);
    voice_wowflutter_stereo(v,&sL,&sR,(double)v->wowFlutter);
    dj_filter_stereo(v,s->loopFilterMode,&sL,&sR);
    tilt_eq_stereo(v,&sL,&sR);studer_eq_stereo(v,&sL,&sR);
    if(s->stability>0.005f){sL=apply_stability(sL,(double)s->stability,&v->rng);sR=apply_stability(sR,(double)s->stability,&v->rng);
        double stabK=0.2+(double)s->stability*0.6;
        v->stabLpStateL+=stabK*(sL-v->stabLpStateL);sL=v->stabLpStateL;
        v->stabLpStateR+=stabK*(sR-v->stabLpStateR);sR=v->stabLpStateR;}
    /* Per-track clock degradation (SR hold + aliasing hiss, Mood-style) and compressor. */
    master_comp(&sL,&sR,(double)v->comp,&v->cEnvL,&v->cEnvR);
    sL*=s->tapeGain; sR*=s->tapeGain;
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
static void poly_note_on(loopex_t *s, int loopIdx, int note, int vel) {
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
static void poly_note_off(loopex_t *s, int note) {   /* match by note only -> never a stuck note */
    for(int i=0;i<POLY_VOICES;i++){ PolyVoice *pv=&s->poly[i]; if(pv->active&&!pv->releasing&&pv->note==note)pv->releasing=1; }
}
/* per-block: refresh each active poly voice's filter/EQ coeffs from its loop */
static void poly_prep(loopex_t *s) {
    for(int i=0;i<POLY_VOICES;i++){ PolyVoice *pv=&s->poly[i]; if(!pv->active)continue;
        if(pv->loopIdx<0||pv->loopIdx>=NUM_VOICES){pv->active=0;continue;}
        Voice *lp=&s->voice[pv->loopIdx]; pv->djMode=lp->djMode;
        if(lp->djMode<0)bq_copy_coeffs(&pv->djA,&lp->djLpA); else if(lp->djMode>0)bq_copy_coeffs(&pv->djA,&lp->djHpA);
        bq_copy_coeffs(&pv->eqLow,&lp->eqLow);bq_copy_coeffs(&pv->eqMid,&lp->eqMid);bq_copy_coeffs(&pv->eqHigh,&lp->eqHigh);
        bq_copy_coeffs(&pv->tiltLo,&lp->tiltLo);bq_copy_coeffs(&pv->tiltHi,&lp->tiltHi);
    }
}
/* per-sample: read buffer pitched, apply loop FX (sat/filter/tilt/EQ), pan/vol, accumulate */
static inline void poly_sample(loopex_t *s, double *mixL, double *mixR, double *sAL, double *sAR, double *sBL, double *sBR) {
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
#define DRIFT_N 4
static const int DRIFT_LEN[DRIFT_N] = { 24001, 41011, 62003, 89017 };  /* ~0.54/0.93/1.41/2.02 s, coprime line lengths */
static void *create_instance(const char *module_dir, const char *json_defaults) {
    (void)module_dir;(void)json_defaults;
    loopex_t *s=(loopex_t*)calloc(1,sizeof(loopex_t));if(!s)return NULL;
    for(int i=0;i<NUM_VOICES;i++){Voice *v=&s->voice[i];
        v->bufferL=(int16_t*)calloc(LOOP_SAMPLES,sizeof(int16_t));
        v->bufferR=(int16_t*)calloc(LOOP_SAMPLES,sizeof(int16_t));
        if(!v->bufferL||!v->bufferR){for(int j=0;j<=i;j++){free(s->voice[j].bufferL);free(s->voice[j].bufferR);}free(s);return NULL;}
        v->state=VS_EMPTY;v->loopStart=0.0f;v->loopEnd=1.0f;v->reverse=0.0f;
        v->pitch=0.0f;v->filter=0.5f;v->pan=0.0f;v->volume=0.8f;
        v->saturation=0.0f;v->wowFlutter=0.0f;v->send=0.0f;v->glitch=0.0f;
        v->tiltEQ=0.0f;v->decay=1.0f;v->eqBass=0.0f;v->eqPresFreq=0.5f;v->eqPresAmt=0.0f;v->eqTreble=0.0f;
        v->flutNextMax=0.5;v->rng=12345+i*7919;v->glLastSlice=-1;
        v->djReso=0.0f;v->ampAtk=0.0f;v->ampRel=0.0f;v->ampAtkCache=-1.0f;v->ampRelCache=-1.0f;v->lfModeCache=-99;v->lfG=0.5f;
        v->comp=0.0f;v->clock=0.0f;v->psCache=-99.0f;v->psActive=0;v->psMix=0.0;v->psNudge=0;
        v->ps=ps_create(44100.0f,128,(i*512)/NUM_VOICES);   /* staggered so the 16 STFTs do not land in one callback */
        v->filterSm=0.5f;v->volSm=(double)v->volume;v->panSm=0.0;
        /* heads: 1 on @1x, 2 off @0.5x, 3 off @2x, 4 off @1x  (spd: 0.5=1x, 0.25=0.5x, 0.75=2x) */
        for(int k=0;k<4;k++){ v->ph[k].mode=0; v->ph[k].dir=1; v->ph[k].env=0.0; v->ph[k].phase=0.0; v->ph[k].spdCache=-1.0f; v->ph[k].jumpCd=0; }
        v->ph[0].mode=1; v->ph[0].spd=0.5f; v->ph[0].env=1.0;
        v->ph[1].spd=0.25f; v->ph[2].spd=0.75f; v->ph[3].spd=0.5f;
        for(int k=0;k<4;k++){ v->hVol[k]=1.0f; v->hPan[k]=0.0f; }   /* per-head Vol unity, Pan center */
        bq_reset(&v->djLpA);bq_reset(&v->djHpA);
        bq_reset(&v->eqLow);bq_reset(&v->eqMid);bq_reset(&v->eqHigh);
        bq_reset(&v->tiltLo);bq_reset(&v->tiltHi);
        dj_filter_update(v);studer_eq_update(v);tilt_eq_update(v);}
    s->globalSat=0.0f;s->masterComp=0.0f;s->masterLoCut=20.0f;s->masterHiCut=20000.0f;s->masterVol=1.0f;
    s->preamp=1.0f;s->overdubMode=1.0f;s->stability=0.0f;s->selTrack=1;s->rng=42;   /* 1 = Clean; overdub 1 = Multiply default */
    s->midiIn=0;s->midiOut=0;s->midiOutPrev=0;s->armThresh=0.08f;
    s->tapeNoise=0.5f;s->tapeDrive=0.0f;s->tapeHF=1.0f;s->tapeLoCut=0.0f;s->tapeWow=0.0f;s->tapeFlut=0.0f;s->tapeGen=0.0f;
    s->globalWowFlut=0.0f;s->inputMonitor=0.5f;s->inputGain=1.0f;s->gFlutNextMax=0.5;
    bq_reset(&s->masterLo);bq_reset(&s->masterHi);
    s->cpuPct=0.0; s->rootNote=60;   /* C3 plays the loop at its recorded speed */
    for(int i=0;i<NUM_PSLOTS;i++){s->pslot[i].idx=-1;bq_reset(&s->pslot[i].toneFilt);}
    for(int i=0;i<NUM_PUNCH;i++){s->punchParams[i][0]=0.5f;s->punchParams[i][1]=0.5f;s->punchParams[i][2]=1.0f;s->punchParams[i][3]=1.0f;s->punchPress[i]=0.0f;}
    s->inLow=0.0f;s->inMid=0.0f;s->inMidFreq=0.5f;s->inHigh=0.0f;s->inHighFreq=0.5f;
    bq_reset(&s->inEqLo);bq_reset(&s->inEqMid);bq_reset(&s->inEqHi);bq_reset(&s->inTapeLp);bq_reset(&s->inTapeHp);
    s->busA=pfx_create(44100.0f); s->busB=pfx_create(44100.0f); s->punchFx=pfx_create(44100.0f);
    if(s->punchFx)pfx_select(s->punchFx,28);   /* Veil */
    for(int i=0;i<3;i++){ atomic_store(&s->fxSel[i],-1); atomic_store(&s->fxBusy[i],0); }
    s->undoL=(int16_t*)calloc(LOOP_SAMPLES,sizeof(int16_t)); s->undoR=(int16_t*)calloc(LOOP_SAMPLES,sizeof(int16_t));
    s->undoGen=(uint16_t*)calloc(LOOP_SAMPLES,sizeof(uint16_t)); s->undoCur=1; s->undoTrack=-1; atomic_store(&s->undoReq,0); atomic_store(&s->disintReq,0);
    s->tapeHold=0; s->tapeLs=0.0; s->tapeSpd=1.0; s->tapeGain=1.0;
    fxseq_init(&s->fx); s->punchFxId=28;   /* PalFX punch defaults to Veil */
    for(int i=0;i<DRIFT_N;i++){ s->drLen[i]=DRIFT_LEN[i];
        s->drL[i]=(float*)calloc((size_t)s->drLen[i],sizeof(float));
        s->drR[i]=(float*)calloc((size_t)s->drLen[i],sizeof(float));
        s->drW[i]=0; s->drPh[i]=0.25*i; s->drDL[i]=0.0f; s->drDR[i]=0.0f; }
    for(int i=0;i<DRIFT_N;i++) if(!s->drL[i]||!s->drR[i]){ for(int j=0;j<DRIFT_N;j++){ free(s->drL[j]); free(s->drR[j]); s->drL[j]=NULL; s->drR[j]=NULL; } break; }   /* partial alloc: disable Drift (guarded by drL[0]) */
    s->drInEnv=0.0; s->drSilent=0; s->drBleed=1.0f;
    s->driftAmt=0.0f; s->driftRate=0.30f; s->driftSize=0.50f; s->driftFb=0.60f;
    s->driftSupr=0.0f; s->driftBlur=0.40f; s->driftDamp=0.40f; s->driftMix=0.0f;
    s->driftSm[0]=0.0f; s->driftSm[1]=0.30f; s->driftSm[2]=0.50f; s->driftSm[3]=0.60f;
    s->driftSm[4]=0.0f; s->driftSm[5]=0.40f; s->driftSm[6]=0.40f; s->driftSm[7]=0.0f;
    s->mfCut=1.0f; s->mfReso=0.0f; s->mfCutSm=1.0f; s->mfResoSm=0.0f; s->mfMode=0; s->mClock=0.5f; s->mClockMode=0; s->mClockSpot=0; s->mclkRatioSm=1.0f; s->mclkWet=0.0f;
    s->perfTrem=0.0f; s->perfTremRate=0.08f; s->punchWidth=0.5f;   /* rate default -> di 0 = one pump per beat */
    s->masterEQ=0; s->masterGlue=0.0f; s->tapeLimit=0.0f; master_eq_update(s); s->inSource=0; s->loopFilterMode=0;
    s->laShm=NULL; s->laFd=-1; s->laRead=0; s->laSlotCur=-1;
    { int fd=shm_open(LA_IN_SHM_NAME,O_RDONLY,0);   /* map Move's per-track audio if the host is streaming it */
      if(fd>=0){ void *p=mmap(NULL,sizeof(la_in_shm_t),PROT_READ,MAP_SHARED,fd,0);
        if(p!=MAP_FAILED) { s->laShm=(la_in_shm_t*)p; s->laFd=fd; } else close(fd); } }
    s->bpaShm=NULL; s->bpaFd=-1; s->bpaRead=0; s->bpaSlotCur=-1;
    { int fd=shm_open(BPA_SHM_NAME,O_RDONLY,0);     /* map Schwung's published stems if link_audio_publish is on */
      if(fd>=0){ void *p=mmap(NULL,sizeof(bpa_shm_t),PROT_READ,MAP_SHARED,fd,0);
        if(p!=MAP_FAILED) { s->bpaShm=(bpa_shm_t*)p; s->bpaFd=fd; } else close(fd); } }
    s->sendAType=14;s->sendBType=17;s->sendAM1=0.4f;s->sendAM2=0.5f;s->sendADrift=0.2f;s->sendBM1=0.5f;s->sendBM2=0.5f;s->sendBDrift=0.2f;
    if(s->busA)pfx_select(s->busA,s->sendAType); if(s->busB)pfx_select(s->busB,s->sendBType);
    s->stMix=0.0f;s->stStep=0.3f;s->stOdds=0.5f;s->stSize=0.5f;s->stReach=0.3f;s->stKind=0;s->stStepLeft=1;s->stRng=0x2233aa55u;
    s->dropAmt=0.0f;s->dropLeft=1;s->dropRng=0x9911bb77u;s->dropG=1.0;
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
static void destroy_instance(void *inst){loopex_t *s=(loopex_t*)inst;if(!s)return;
    if(s->sio.active){ atomic_store(&s->sio.cancel,1); atomic_store(&s->sio.request,1); /* wake */
        pthread_join(s->sio.th,NULL); s->sio.active=0; }   /* join BEFORE freeing buffers */
    for(int i=0;i<NUM_VOICES;i++){free(s->voice[i].bufferL);free(s->voice[i].bufferR);ps_destroy(s->voice[i].ps);}
    for(int i=0;i<4;i++){free(s->drL[i]);free(s->drR[i]);}
    if(s->laShm)munmap(s->laShm,sizeof(la_in_shm_t)); if(s->laFd>=0)close(s->laFd);
    if(s->bpaShm)munmap(s->bpaShm,sizeof(bpa_shm_t)); if(s->bpaFd>=0)close(s->bpaFd);
    if(s->busA)pfx_destroy(s->busA); if(s->busB)pfx_destroy(s->busB); if(s->punchFx)pfx_destroy(s->punchFx);
    free(s->undoL); free(s->undoR); free(s->undoGen); free(s);}

/* ---- MIDI Handler ---- */
static void on_midi(void *inst, const uint8_t *msg, int len, int source) {
    loopex_t *s=(loopex_t*)inst;if(len<3)return;
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
static void input_eq_update(loopex_t *s){
    double lDb=(double)s->inLow*15.0, mDb=(double)s->inMid*11.0, hDb=(double)s->inHigh*15.0;
    double mF=150.0*pow(7000.0/150.0,(double)s->inMidFreq), hF=3000.0+(double)s->inHighFreq*12000.0;
    if(fabs(lDb)>0.1)bq_set_lowshelf(&s->inEqLo,120.0,lDb,0.7); else bq_reset(&s->inEqLo);
    if(fabs(mDb)>0.1)bq_set_peak(&s->inEqMid,mF,mDb,0.7); else bq_reset(&s->inEqMid);
    if(fabs(hDb)>0.1)bq_set_highshelf(&s->inEqHi,hF,hDb,0.7); else bq_reset(&s->inEqHi);
    if(s->tapeHF<0.99f){ double cut=2000.0*pow(20000.0/2000.0,(double)s->tapeHF); bq_set_lp(&s->inTapeLp,cut,0.707); }  /* tape HF rolloff */
    if(s->tapeLoCut>0.01f){ bq_set_hp(&s->inTapeHp,20.0+(double)s->tapeLoCut*780.0,0.707); }                            /* tape low cut  */
}

/* ---- Tape transport wobble on the RECORD path (Wow = slow, Flutter = fast) ---- */
static inline void input_wowflutter(loopex_t *s, double *l, double *r, double wow, double flut){
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
static inline void input_generations(loopex_t *s, double *l, double *r, double g, uint32_t *rng){
    if(g<0.005)return;
    double mk=1.0/(1.0+g*0.4);
    *l=lb_tanh(*l*(1.0+g*0.8))*mk; *r=lb_tanh(*r*(1.0+g*0.8))*mk;
    double k=1.0-g*0.55;                                  /* darker each generation */
    s->genLpL+=k*(*l-s->genLpL); *l=s->genLpL;
    s->genLpR+=k*(*r-s->genLpR); *r=s->genLpR;
    double n=lb_rand(rng)*0.004*g; *l+=n; *r+=n;
}

/* ---- Perform: Stumble (free-running probabilistic step glitch on the master) ---- */
static inline void stumble_sample(loopex_t *s, double *mixL, double *mixR){
    if(s->stMix<=0.001f)return;
    s->stRingL[s->stW]=(float)*mixL; s->stRingR[s->stW]=(float)*mixR;
    if(--s->stStepLeft<=0){
        int stepSamp=(int)((20.0+(double)s->stStep*980.0)*44.1); if(stepSamp<441)stepSamp=441; if(stepSamp>PUNCH_BUF/2)stepSamp=PUNCH_BUF/2;
        s->stStepLen=stepSamp; s->stStepLeft=stepSamp; s->stStepPos=0;
        s->stActive = (PRND(s->stRng) < (double)s->stOdds);
        if(s->stActive){
            s->stEffect = (s->stKind==0)? (int)(PRND(s->stRng)*4.0) : (s->stKind-1); if(s->stEffect>4)s->stEffect=4; if(s->stEffect<0)s->stEffect=0;   /* Random picks the 4 slice glitches, not the bitcrush */
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
        else if(e==4){ double lv=180.0; wl=floor(*mixL*lv+0.5)/lv; wr=floor(*mixR*lv+0.5)/lv; }   /* gentler crush */
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
static inline void dropout_sample(loopex_t *s, double *mixL, double *mixR){
    if(s->dropAmt<=0.001f && s->dropG>0.9995){ return; }
    if(--s->dropLeft<=0){ s->dropLeft=(int)(SR*(0.02+PRND(s->dropRng)*0.15)); s->dropActive=(PRND(s->dropRng)<(double)s->dropAmt*0.4); }
    double target=s->dropActive?(1.0-(double)s->dropAmt):1.0;
    s->dropG += (target - s->dropG)*0.02;   /* ~1 ms glide: no click on the random edges */
    *mixL*=s->dropG; *mixR*=s->dropG;
}

/* ---- Master limiter: analog-style fast-attack peak duck + soft ceiling ---- */
/* ═══ Perform page 2 + Settings page 2 master stages ═══════════════════════ */



/* ---- Master low-pass filter — 12 voicings ported from Fizzik ---------------
 * Every mode is a LOW-PASS; the mode picks the analog character (SVF family:
 * Clean/SEM/MS-20/Steiner/Sallen-Key with diode-clipped resonance on MS-20/K35;
 * ladder family: 1/2/4-pole, Prophet, Oberheim, Diode-303, Vintage — ZDF with
 * tanh feedback for bounded self-oscillation). Clean-room VA math (Cytomic SVF +
 * Zavalishin ladder), the same engine as Fizzik's global filter. */
static const char *mfmode_opts[MF_NVOICE] = {
    "Clean","SEM","MS-20","Steiner","Ladder4","Ladder2","Ladder1","Prophet","Oberheim","Diode","K35","Vintage" };
static inline float mf_tanh(float x){ if(x<-3.0f)return -1.0f; if(x>3.0f)return 1.0f; float x2=x*x; return x*(27.0f+x2)/(27.0f+9.0f*x2); }
/* one channel, one sample; st = {ic1,ic2,z1,z2,z3,z4} */
static inline float mf_run(float *st, float x, float g, float reso, int voicing){
    if(reso<0)reso=0; if(reso>1)reso=1;
    if(voicing==0||voicing==1||voicing==2||voicing==3||voicing==10){   /* SVF family, LP out */
        float kmin,krange,drive=1.0f,trim;
        switch(voicing){
            case 1:  kmin=0.20f;  krange=1.65f; trim=0.68f; break;              /* SEM */
            case 2:  kmin=0.030f; krange=1.97f; drive=1.8f; trim=0.82f; break;  /* MS-20 */
            case 3:  kmin=0.10f;  krange=1.85f; drive=1.2f; trim=0.75f; break;  /* Steiner */
            case 10: kmin=0.050f; krange=1.90f; drive=1.5f; trim=0.78f; break;  /* Sallen-Key K35 */
            default: kmin=0.060f; krange=1.94f; trim=0.68f; break;              /* Clean */
        }
        float k=kmin+krange*(1.0f-reso);
        float a1=1.0f/(1.0f+g*(g+k)), a2=g*a1, a3=g*a2;
        float xin=(drive>1.01f)? mf_tanh(x*drive)*(1.0f/drive) : x;
        float v3=xin-st[1], v1=a1*st[0]+a2*v3, v2=st[1]+a2*st[0]+a3*v3;
        st[0]=2.0f*v1-st[0]+1e-20f; st[1]=2.0f*v2-st[1];
        return v2*(1.0f-(1.0f-trim)*reso);   /* low-pass; trim compensates resonance only, unity at reso 0 */
    }
    float G=g/(1.0f+g); int poles; float kmax,drive;
    switch(voicing){
        case 5:  poles=2; kmax=3.6f; drive=1.2f;  break;   /* Ladder 2-pole (12 dB) */
        case 6:  poles=1; kmax=2.4f; drive=1.1f;  break;   /* Ladder 1-pole (6 dB) */
        case 7:  poles=4; kmax=3.7f; drive=1.0f;  break;   /* Prophet — clean, creamy 24 dB */
        case 8:  poles=2; kmax=4.3f; drive=1.15f; break;   /* Oberheim — resonant 12 dB (SEM-ish) */
        case 9:  poles=4; kmax=4.5f; drive=1.8f;  break;   /* Diode / 303 — squelchy */
        case 11: poles=4; kmax=4.0f; drive=2.6f;  break;   /* Vintage — warm, heavily driven */
        default: poles=4; kmax=4.0f; drive=1.3f;  break;   /* Ladder 4-pole (24 dB) */
    }
    float k=kmax*reso;
    float xin=(drive>1.01f)? mf_tanh(x*drive)*(1.0f/drive) : x;
    float G2=G*G,G3=G2*G,G4=G3*G;
    float S1=st[2]*(1.0f-G),S2=st[3]*(1.0f-G),S3=st[4]*(1.0f-G),S4=st[5]*(1.0f-G);
    float Sigma=G3*S1+G2*S2+G*S3+S4;
    float y4lin=(G4*xin+Sigma)/(1.0f+k*G4);
    float u=xin-k*mf_tanh(y4lin);
    float v,y1,y2,y3,y4;
    v=(u -st[2])*G; y1=v+st[2]; st[2]=y1+v+1e-20f;
    v=(y1-st[3])*G; y2=v+st[3]; st[3]=y2+v;
    v=(y2-st[4])*G; y3=v+st[4]; st[4]=y3+v;
    v=(y3-st[5])*G; y4=v+st[5]; st[5]=y4+v;
    float lp=(poles==1)?y1:(poles==2)?y2:y4;
    float comp=1.0f+0.5f*k;
    return lp*comp;
}
static inline void master_filter(loopex_t *s, double *l, double *r){
    /* ~5 ms one-pole glide on cutoff + reso: sweeps are continuous, never stepped */
    s->mfCutSm  += (s->mfCut  - s->mfCutSm ) * 0.006f;
    s->mfResoSm += (s->mfReso - s->mfResoSm) * 0.006f;
    if(s->mfCutSm>=0.998f && s->mfResoSm<0.002f) return;   /* wide open, no reso = transparent */
    double fc = 30.0*pow(20000.0/30.0,(double)s->mfCutSm); if(fc>18000.0)fc=18000.0;
    float g=(float)tan(M_PI*fc/SR);
    int voic=s->mfMode; if(voic<0)voic=0; if(voic>=MF_NVOICE)voic=MF_NVOICE-1;
    *l=(double)mf_run(s->mfStL,(float)*l,g,s->mfResoSm,voic);
    *r=(double)mf_run(s->mfStR,(float)*r,g,s->mfResoSm,voic);
}

/* ---- Master Clock (Chase Bliss Mood-style): pitch shift + SR-crush ---------

 * K4 sets the ratio; musical mode snaps it to wide intervals (octaves / fifths /

 * thirds), continuous mode slides. A 2-head crossfaded delay pitches the master

 * without drift (same shifter as the per-loop Pitch); the further from 1x, the

 * more sample-rate decimation crush, for Mood's lo-fi grind. */

#define MCLK_W 4096

static inline double mclk_rd(const float *b, double pos){

    while(pos<0)pos+=MCLK_W; while(pos>=MCLK_W)pos-=MCLK_W;

    int i0=(int)pos, i1=i0+1; if(i1>=MCLK_W)i1=0; double f=pos-(double)i0;

    return (double)b[i0]*(1.0-f)+(double)b[i1]*f;

}

static const int MCLK_SEMI[13] = { -24,-19,-17,-12,-7,-5,0,5,7,12,17,19,24 };

static inline void master_clock(loopex_t *s, double *l, double *r){

    double _dryL=*l,_dryR=*r;

    double semis = ((double)s->mClock-0.5)*48.0;             /* +-24 st */

    if(s->mClockMode==0){ int best=6; double bd=1e9;         /* snap to musical intervals */

        for(int i=0;i<13;i++){ double dd=fabs(semis-MCLK_SEMI[i]); if(dd<bd){bd=dd;best=i;} } semis=MCLK_SEMI[best]; }

    float _wetT=(fabs(semis)<0.5)?0.0f:1.0f; s->mclkWet+=(_wetT-s->mclkWet)*0.02f;   /* crossfade dry<->shifted */

    double ratio = pow(2.0, semis/12.0);   /* ~1 near unity; the wet crossfade fades it to true dry */

    s->mclkRatioSm += ((float)ratio - s->mclkRatioSm)*0.02f;  /* glide ratio: no zipper on K4 */

    double rr = s->mclkRatioSm;

    s->mclkBufL[s->mclkW]=(float)*l; s->mclkBufR[s->mclkW]=(float)*r;

    const double W=MCLK_W;

    s->mclkPh += (1.0 - rr); while(s->mclkPh>=W)s->mclkPh-=W; while(s->mclkPh<0)s->mclkPh+=W;

    double d1=s->mclkPh, d2=d1+W*0.5; if(d2>=W)d2-=W;

    double w1=0.5-0.5*cos(TWOPI*d1/W), w2=0.5-0.5*cos(TWOPI*d2/W), ws=w1+w2+1e-9;

    double rp1=(double)s->mclkW-d1, rp2=(double)s->mclkW-d2;

    double oL=(mclk_rd(s->mclkBufL,rp1)*w1+mclk_rd(s->mclkBufL,rp2)*w2)/ws;

    double oR=(mclk_rd(s->mclkBufR,rp1)*w1+mclk_rd(s->mclkBufR,rp2)*w2)/ws;

    s->mclkW=(s->mclkW+1)&(MCLK_W-1);

    /* SR-decimation crush: heavier as we pitch away from unity (down side hardest) */

    double dist = (rr<1.0)? (1.0/rr-1.0) : (rr-1.0);

    int df = 1 + (int)(dist*3.5); if(df<1)df=1; if(df>6)df=6;

    if(df>1){ if(++s->mclkCnt>=df){ s->mclkHoldL=oL; s->mclkHoldR=oR; s->mclkCnt=0; } oL=s->mclkHoldL; oR=s->mclkHoldR; }

    double _w=(double)s->mclkWet; *l=_dryL+(oL-_dryL)*_w; *r=_dryR+(oR-_dryR)*_w;

}

static inline void master_clockfilter(loopex_t *s, double *l, double *r){

    master_clock(s,l,r);

    master_filter(s,l,r);   /* filter sits after the clock */

}



/* ---- Perform K7/K8: rhythmic ducking pump, synced to the transport ---------
 * A sidechain-style volume duck on every beat division: gain drops hard on the
 * downbeat and breathes back up over the cycle. K7 = depth, K8 = rate (the beat
 * division). Locked to the Move tempo (punch_beat), so it sits with the loops. */
static const double PUMP_DIV[6] = { 1.0, 0.5, 1.0/3.0, 0.25, 1.0/6.0, 0.125 };  /* beats per cycle */
static inline void perf_pump(loopex_t *s, double *l, double *r){
    if(s->perfTrem<0.002f) return;
    int di=(int)((double)s->perfTremRate*5.99); if(di<0)di=0; if(di>5)di=5;
    double period=punch_beat()*PUMP_DIV[di]; if(period<64.0)period=64.0;
    s->tremPh += 1.0/period; while(s->tremPh>=1.0)s->tremPh-=1.0;
    double d=(double)s->perfTrem;
    /* Duck deepest at the downbeat (phase 0), recover over the cycle, then a short
     * smoothstep attack ducks back down just before the wrap. Both ends of the cycle
     * meet at the same gain (1-d), so there is no step and no click at the wrap. */
    double up=s->tremPh, env, a=0.10;                        /* attack = last 10% of the cycle */
    if(up < 1.0-a){ double t=up/(1.0-a); env=(1.0-d)+d*(t*t*(3.0-2.0*t)); }   /* release: (1-d) -> 1 */
    else          { double t=(up-(1.0-a))/a; env=1.0-d*(t*t*(3.0-2.0*t)); }   /* attack:  1 -> (1-d) */
    *l*=env; *r*=env;
}
/* ---- Master character / colour EQ (hardware console + sampler models) ------

 * Each preset = optional bit/SR crush -> saturation -> low shelf + mid peak +

 * high shelf, with a unity-ish makeup. Coefficients are clean-room voicings in

 * the spirit of the named units, not measured curves. */

enum { MEQ_OFF=0, MEQ_STUDER, MEQ_NEVE, MEQ_SSL, MEQ_API, MEQ_SP12, MEQ_MPC, MEQ_EMU, MEQ_AMPEX, MEQ_JUNO, MEQ_CONSOLE, MEQ_N };

static const char *meq_opts[MEQ_N] = { "Off","Studer","Neve","SSL","API","SP12","MPC","Emu","Ampex","Juno","Console" };

/* {loHz,loDb, midHz,midDb,midQ, hiHz,hiDb, sat, crushHz(0=none), makeupDb} */

static const double MEQ_DEF[MEQ_N][10] = {

    {   0,0,     0,0,0,        0,0,      0.00,     0, 0.0 },   /* Off */

    { 120, 1.5, 3500, 1.0,0.8, 12000, 2.5, 0.18,     0, -1.5 },   /* Studer A800 — tape head bump + airy HF, soft sat */

    {  90, 2.5,  500,-1.0,0.7, 14000, 3.5, 0.14,     0, -1.8 },   /* Neve 1073 — warm lows, silky top */

    { 200,-1.0, 1500, 1.5,0.9,  9000, 1.5, 0.10,     0, -0.8 },   /* SSL bus — tight lows, present mids */

    { 100, 1.0,  900, 2.5,1.1,  6000, 1.0, 0.20,     0, -1.6 },   /* API — punchy mids, fast */

    {  80, 2.0, 1800,-1.5,0.8,  7000,-3.0, 0.30, 26000, -1.0 },   /* SP-1200 — 12-bit crunch, rolled top */

    { 110, 3.0,  700, 0.5,0.7,  8000,-2.0, 0.24, 30000, -1.2 },   /* MPC60 — fat 12-bit, warm */

    {  70, 1.5, 2200,-1.0,0.9,  5500,-4.5, 0.34, 22000, -0.6 },   /* Emu SP — dark, gritty converter */

    { 100, 3.5,  400, 0.5,0.6, 10000, 1.0, 0.40,     0, -2.4 },   /* Ampex ATR — fat, saturated */

    { 160,-0.5, 2500, 2.0,1.0, 13000, 2.0, 0.08,     0, -0.8 },   /* Juno chorus-console — bright, glassy */

    { 130, 1.0, 1200, 0.8,0.8, 11000, 1.8, 0.16,     0, -1.2 },   /* Console — neutral glue colour */

};

static void master_eq_update(loopex_t *s){

    int p=s->masterEQ; if(p<0)p=0; if(p>=MEQ_N)p=MEQ_N-1;

    const double *d=MEQ_DEF[p];

    if(fabs(d[1])>0.05) bq_set_lowshelf(&s->eqLoSh,d[0],d[1],0.7); else bq_reset(&s->eqLoSh);

    if(fabs(d[3])>0.05) bq_set_peak(&s->eqMidPk,d[2],d[3],d[4]); else bq_reset(&s->eqMidPk);

    if(fabs(d[6])>0.05) bq_set_highshelf(&s->eqHiSh,d[5],d[6],0.7); else bq_reset(&s->eqHiSh);

    s->eqSat=(float)d[7];

    s->eqCrush=(d[8]>1.0)? (int)(SR/d[8]+0.5) : 0; if(s->eqCrush<1)s->eqCrush=0;

    s->eqMakeup=(float)pow(10.0,d[9]/20.0);

}

static inline void master_character(loopex_t *s, double *l, double *r){

    if(s->masterEQ<=MEQ_OFF) return;

    double L=*l,R=*r;

    if(s->eqCrush>1){ if(++s->eqCrushCnt>=s->eqCrush){ s->eqCrushHoldL=L; s->eqCrushHoldR=R; s->eqCrushCnt=0; } L=s->eqCrushHoldL; R=s->eqCrushHoldR; }

    if(s->eqSat>0.001f){ double dr=1.0+(double)s->eqSat*3.0; L=lb_tanh(L*dr)/lb_tanh(dr); R=lb_tanh(R*dr)/lb_tanh(dr); }

    L=bq_L(&s->eqLoSh,L); L=bq_L(&s->eqMidPk,L); L=bq_L(&s->eqHiSh,L);

    R=bq_R(&s->eqLoSh,R); R=bq_R(&s->eqMidPk,R); R=bq_R(&s->eqHiSh,R);

    *l=L*(double)s->eqMakeup; *r=R*(double)s->eqMakeup;

}



/* ---- Master glue compressor (slow bus comp, program-dependent) ------------- */

static inline void master_glue(loopex_t *s, double *l, double *r){

    if(s->masterGlue<0.01f) return;

    double amt=(double)s->masterGlue, det=fmax(fabs(*l),fabs(*r));

    double atk=exp(-1.0/(SR*0.010)), rel=exp(-1.0/(SR*0.25));   /* 10ms / 250ms glue */

    double env=fmax(s->glueEnvL,s->glueEnvR);

    env = (det>env)? atk*env+(1.0-atk)*det : rel*env+(1.0-rel)*det;

    s->glueEnvL=s->glueEnvR=env;

    double thDb=-12.0*amt, db=20.0*log10(env+1e-9), ratio=1.5+amt*2.5;

    double g=1.0; if(db>thDb){ double gr=(db-thDb)*(1.0-1.0/ratio); g=pow(10.0,-gr/20.0); }

    double mk=pow(10.0,(-thDb)*(1.0-1.0/ratio)*0.45/20.0*amt);

    *l*=g*mk; *r*=g*mk;

}



/* ---- Analog tape limiter (replaces the plain master limiter; drive = colour) */

static inline void tape_limiter(loopex_t *s, double *l, double *r){
    if(s->tapeLimit<=0.001f) return;                          /* true bypass */
    double drv=1.0+(double)s->tapeLimit*2.5;
    double det=fmax(fabs(*l),fabs(*r))*drv;
    const double atk=0.9285, rel=0.99977;
    s->tapeLimEnv = (det>s->tapeLimEnv)? atk*s->tapeLimEnv+(1.0-atk)*det : rel*s->tapeLimEnv+(1.0-rel)*det;
    const double ceil=0.90; double gr=(s->tapeLimEnv>ceil)? ceil/s->tapeLimEnv:1.0;
    double nrm=1.0/fmax(1.0, lb_tanh(drv)*1.287);             /* no upward makeup; output stays <= 1 */
    *l=lb_tanh(*l*drv*gr)*nrm; *r=lb_tanh(*r*drv*gr)*nrm;
}



/* Flush-to-zero: on aarch64 the FPCR FZ bit is per-thread and -ffast-math's
 * crtfastmath only sets it on the loading thread, not the SPI audio callback.
 * Set it explicitly each block so decaying filter/envelope states can't fall
 * into the denormal range and spike CPU. */
static inline void lb_enable_ftz(void){
#if defined(__aarch64__)
    uint64_t v; __asm__ __volatile__("mrs %0, fpcr" : "=r"(v));
    v |= (1ull<<24);   /* FZ */
    __asm__ __volatile__("msr fpcr, %0" :: "r"(v));
#endif
}

/* ---- Render Block ---- */
/* ---- Drift: a global drifting-delay memory, after Soma COSMOS -------------
 * Four delay lines of coprime length, each read at a slowly drifting tap, with
 * a feedback matrix that morphs from self-feedback (Blur 0) to a normalized
 * Hadamard cross-mix (Blur 1). The loop mix feeds it (Drift = feed level); the
 * memory recirculates (Feedback = sustain, tanh-limited); loud new input erases
 * old content (Suppress); a one-pole damps the tail (Damp); Mix blends the wet
 * ambient layer back into the master. The coprime lengths plus per-line async
 * LFOs mean the recombination never lands on an exact repeat. */
static inline void drift_sample(loopex_t *s, double *l, double *r){
    if(!s->drL[0]) return;
    float *sm=s->driftSm;
    const float tgt[8]={s->driftAmt,s->driftRate,s->driftSize,s->driftFb,s->driftSupr,s->driftBlur,s->driftDamp,s->driftMix};
    for(int i=0;i<8;i++) sm[i]+=(tgt[i]-sm[i])*0.002f;
    float amt=sm[0],rate=sm[1],size=sm[2],fb=sm[3],supr=sm[4],blur=sm[5],damp=sm[6],mix=sm[7];
    if(mix<0.0008f && amt<0.0008f) return;                 /* fully idle: leave the memory frozen */
    double inL=*l, inR=*r;
    double ie=fabs(inL)+fabs(inR); s->drInEnv += (ie - s->drInEnv)*0.0016;   /* input level for the suppressor */
    double sg = 1.0 - (double)supr*fmin(1.0, s->drInEnv*3.0);
    /* Silence bleed: once nothing has fed the memory for ~8 s, ramp the feedback
     * down so an abandoned tail always dies instead of droning forever. */
    if(s->drInEnv > 0.0008) s->drSilent = 0; else if(s->drSilent < (1<<30)) s->drSilent++;
    float bleedTgt = (s->drSilent > (int)(8.0*SR)) ? 0.0f : 1.0f;
    s->drBleed += (bleedTgt - s->drBleed) * 2.6e-5f;   /* ~4 s ramp to silence */
    double yL[DRIFT_N], yR[DRIFT_N];
    double tapFrac = 0.12 + 0.80*(double)size;
    double baseRate = (0.05 + 0.9*(double)rate)/(double)SR;
    for(int i=0;i<DRIFT_N;i++){
        int len=s->drLen[i];
        s->drPh[i]+= baseRate*(1.0+0.137*i); if(s->drPh[i]>=1.0)s->drPh[i]-=1.0;
        double lfo=sin(6.283185307179586*s->drPh[i]);
        double modS = lfo*(double)amt*(double)len*0.03;
        double rp = (double)s->drW[i] - tapFrac*(double)len + modS;
        while(rp<0)rp+=len; while(rp>=len)rp-=len;
        int i0=(int)rp; double fr=rp-i0; int i1=i0+1; if(i1>=len)i1=0;
        yL[i]=s->drL[i][i0]+(s->drL[i][i1]-s->drL[i][i0])*fr;
        yR[i]=s->drR[i][i0]+(s->drR[i][i1]-s->drR[i][i0])*fr;
    }
    double hL0=0.5*( yL[0]+yL[1]+yL[2]+yL[3]), hL1=0.5*( yL[0]-yL[1]+yL[2]-yL[3]);
    double hL2=0.5*( yL[0]+yL[1]-yL[2]-yL[3]), hL3=0.5*( yL[0]-yL[1]-yL[2]+yL[3]);
    double hR0=0.5*( yR[0]+yR[1]+yR[2]+yR[3]), hR1=0.5*( yR[0]-yR[1]+yR[2]-yR[3]);
    double hR2=0.5*( yR[0]+yR[1]-yR[2]-yR[3]), hR3=0.5*( yR[0]-yR[1]-yR[2]+yR[3]);
    double hL[4]={hL0,hL1,hL2,hL3}, hR[4]={hR0,hR1,hR2,hR3};
    double b=(double)blur, fbg=(double)fb*0.97*(double)s->drBleed;   /* capped < 1 so the tail always fades */
    double dco=0.05+0.9*(1.0-(double)damp);                 /* Damp 1 -> heavy low-pass in the tail */
    for(int i=0;i<DRIFT_N;i++){
        int len=s->drLen[i];
        double fL=(1.0-b)*yL[i]+b*hL[i], fR=(1.0-b)*yR[i]+b*hR[i];
        s->drDL[i]+= (float)((fL - s->drDL[i])*dco); s->drDR[i]+= (float)((fR - s->drDR[i])*dco);
        fL=s->drDL[i]; fR=s->drDR[i];
        double wL=(double)mf_tanh((float)(inL*(double)amt + fL*fbg*sg));
        double wR=(double)mf_tanh((float)(inR*(double)amt + fR*fbg*sg));
        s->drL[i][s->drW[i]]=(float)(wL+1e-25); s->drR[i][s->drW[i]]=(float)(wR+1e-25);
        s->drW[i]++; if(s->drW[i]>=len)s->drW[i]=0;
    }
    double wetL=0.5*(yL[0]+yL[1]+yL[2]+yL[3]), wetR=0.5*(yR[0]+yR[1]+yR[2]+yR[3]);
    *l = inL + wetL*(double)mix;
    *r = inR + wetR*(double)mix;
}
/* LaunchControl XL LED feedback: mirror each loop's transport state to its Track
 * Focus button (row 1) and its mute to the Track Control button (row 2), for both
 * templates at once (the controller ignores notes not on its active template).
 * Sent through the host's external MIDI out; capped at 8 packets/block. */
/* LED feedback goes out on this channel (2 = index 1). The Move reflects only its
 * own channel-1 note-ons to the pad LEDs, so a non-1 channel keeps the Track Focus
 * notes (68-83) off the Move pads. Set the LaunchControl templates to this channel. */
#define LCXL_CH 0x01
static inline void lcxl_leds(loopex_t *s){
    if(!g_host||!g_host->midi_send_external) return;
    const uint8_t ST = 0x90 | LCXL_CH;
    if(s->midiOut!=s->midiOutPrev){ for(int i=0;i<16;i++){ s->ledFocusCache[i]=0xFF; s->ledMuteCache[i]=0xFF; } s->midiOutPrev=s->midiOut; }
    int sent=0;
    for(int i=0;i<NUM_VOICES && sent<8;i++){
        uint8_t fc=0, mc=0;
        if(s->midiOut){ int st=(int)s->voice[i].state;
            fc = (st==VS_RECORDING)?15 : (st==VS_PLAYING||st==VS_OVERDUBBING)?60 : (st==VS_PAUSED)?62 : 0;   /* red/green/amber */
            mc = s->voice[i].muted ? 15 : 0; }                                                              /* red = muted */
        int fn=(i<8)?(68+i):(76+(i-8)), mn=(i<8)?(36+i):(44+(i-8));
        if(fc!=s->ledFocusCache[i]){ uint8_t m[4]={0x09,ST,(uint8_t)fn,fc}; g_host->midi_send_external(m,4); s->ledFocusCache[i]=fc; sent++; }
        if(mc!=s->ledMuteCache[i]){ uint8_t m[4]={0x09,ST,(uint8_t)mn,mc}; g_host->midi_send_external(m,4); s->ledMuteCache[i]=mc; sent++; }
    }
}
static void render_block(void *inst, int16_t *out_interleaved_lr, int frames) {
    loopex_t *s=(loopex_t*)inst;
    if(frames>128)frames=128; if(frames<0)frames=0;   /* host is fixed at 128; guard the per-block arrays */
    lb_enable_ftz();   /* denormal flush-to-zero on the audio thread */
    struct timespec _t0; clock_gettime(CLOCK_MONOTONIC,&_t0);
    /* A finished session load hands back a settings blob — apply it here (bounded
     * string parse, no I/O), once, at block start. */
    if(atomic_load(&s->sio.applyState)){ atomic_store(&s->sio.applyState,0); set_param(s,"state",s->sio.stateBuf); }
    int16_t *micBuf=NULL,*mixBuf=NULL;
    if(g_host&&g_host->mapped_memory){ micBuf=(int16_t*)(g_host->mapped_memory+g_host->audio_in_offset);
        mixBuf=(int16_t*)(g_host->mapped_memory+g_host->audio_out_offset); }
    int useMaster=(s->inSource==1)&&mixBuf;   /* Settings p2: record the Move mix bus instead of line-in */
    int laSlot=(s->inSource>=6&&s->inSource<=9)?(s->inSource-6):-1; uint32_t laBase=0; int laOn=0;
    if(laSlot>=0 && s->laShm && s->laShm->magic==LA_IN_MAGIC){
        la_in_slot_t *LS=&s->laShm->slots[laSlot];
        if(LS->active){ uint32_t wp=__atomic_load_n(&LS->write_pos,__ATOMIC_ACQUIRE); uint32_t need=(uint32_t)(frames*2);
            if(s->laSlotCur!=laSlot){ s->laRead=wp-need; s->laSlotCur=laSlot; }
            uint32_t avail=wp - s->laRead;
            if(avail > (LA_IN_RING_SAMPLES-LA_IN_RING_SAMPLES/4)){ s->laRead=wp-need; avail=need; }   /* catch up if the producer runs away */
            if(avail>=need){ laBase=s->laRead; s->laRead+=need; laOn=1; }   /* else starve -> block reads silence */
        }
    }
    /* Schwung published stems (InSrc Sch1-4 / SchM): read one pub-audio slot with a
     * private cursor. We gate purely on write_pos advancing (the master stem carries
     * active=0 yet valid), and never advance past wp, so a stem that stops just goes
     * silent instead of looping stale samples. */
    int bpaSlot=(s->inSource>=2&&s->inSource<=5)?(s->inSource-2):-1; uint32_t bpaBase=0; int bpaOn=0;
    if(bpaSlot>=0 && s->bpaShm && s->bpaShm->magic==BPA_MAGIC){
        bpa_slot_t *PS=&s->bpaShm->slots[bpaSlot];
        uint32_t wp=__atomic_load_n(&PS->write_pos,__ATOMIC_ACQUIRE); uint32_t need=(uint32_t)(frames*2);
        if(s->bpaSlotCur!=bpaSlot){ s->bpaRead=wp-need; s->bpaSlotCur=bpaSlot; }
        uint32_t avail=wp - s->bpaRead;
        if(avail > (BPA_RING_SAMPLES-BPA_RING_SAMPLES/4)){ s->bpaRead=wp-need; avail=need; }   /* fell behind -> resync */
        if(avail>=need){ bpaBase=s->bpaRead; s->bpaRead+=need; bpaOn=1; }
    }
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
    fxseq_tick(s,frames);   /* FX sequencer: step clock, chance, gate */

    for(int n=0;n<frames;n++){

        /* Tape transport gesture: Left brakes to a stop (~3 s), Right winds up to a tone (~2.5 s),

         * release eases back to 1x. Linear in log2(speed) = a constant glide in semitones. */

        { const double LS_MIN=-9.0, LS_MAX=5.0;

          if(s->tapeHold<0){ s->tapeLs-=3.0/SR; if(s->tapeLs<LS_MIN)s->tapeLs=LS_MIN; }

          else if(s->tapeHold>0){ s->tapeLs+=2.0/SR; if(s->tapeLs>LS_MAX)s->tapeLs=LS_MAX; }

          else if(s->tapeLs!=0.0){ s->tapeLs+=(0.0-s->tapeLs)*(1.0/(SR*0.35)); if(fabs(s->tapeLs)<1e-4)s->tapeLs=0.0; }

          s->tapeSpd=(s->tapeLs==0.0)?1.0:exp2(s->tapeLs);

          double g=(s->tapeLs+9.0)/3.0; s->tapeGain=(g<0.0)?0.0:(g>1.0)?1.0:g; }   /* the last three octaves fade out */

        double inL=0.0,inR=0.0;
        if(bpaOn){ double ig=(double)s->inputGain; const int16_t *rg=s->bpaShm->slots[bpaSlot].ring;
            uint32_t idx=(bpaBase+(uint32_t)(n*2))&BPA_RING_MASK;
            inL=(double)rg[idx]/32768.0*ig; inR=(double)rg[(idx+1)&BPA_RING_MASK]/32768.0*ig;
            double aL=fabs(inL),aR=fabs(inR);if(aL>s->inputPeakL)s->inputPeakL=aL;if(aR>s->inputPeakR)s->inputPeakR=aR; }
        else if(laOn){ double ig=(double)s->inputGain; const int16_t *rg=s->laShm->slots[laSlot].ring;
            uint32_t idx=(laBase+(uint32_t)(n*2))&LA_IN_RING_MASK;
            inL=(double)rg[idx]/32768.0*ig; inR=(double)rg[(idx+1)&LA_IN_RING_MASK]/32768.0*ig;
            double aL=fabs(inL),aR=fabs(inR);if(aL>s->inputPeakL)s->inputPeakL=aL;if(aR>s->inputPeakR)s->inputPeakR=aR; }
        else if(useMaster){ double ig=(double)s->inputGain;
            /* Master mix bus minus our own previous-block output, so recording the
             * master captures the other tracks and monitor, not our own loops (no runaway). */
            inL=((double)mixBuf[n*2]/32768.0 - (double)s->selfPrevL[n])*ig;
            inR=((double)mixBuf[n*2+1]/32768.0 - (double)s->selfPrevR[n])*ig;
            double aL=fabs(inL),aR=fabs(inR);if(aL>s->inputPeakL)s->inputPeakL=aL;if(aR>s->inputPeakR)s->inputPeakR=aR; }
        else if(micBuf){double ig=(double)s->inputGain;inL=(double)micBuf[n*2]/32768.0*ig;inR=(double)micBuf[n*2+1]/32768.0*ig;
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
            else if(v->state==VS_OVERDUBBING&&v->playHead<v->loopLen){
                if(s->undoTrack==vi&&s->undoLen==v->loopLen){ int ui=v->playHead;   /* save the original before it is overwritten (once per sample) */
                    if(s->undoGen[ui]!=s->undoCur){ s->undoL[ui]=v->bufferL[ui]; s->undoR[ui]=v->bufferR[ui]; s->undoGen[ui]=s->undoCur; s->undoCount++; } }
                switch(odMode){
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
            voice_render(&s->voice[vi],s,n,&vL,&vR,&aL,&aR,&bL,&bR);mixL+=vL;mixR+=vR;sAL+=aL;sAR+=aR;sBL+=bL;sBR+=bR;}
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
        if(s->mClockSpot==0) master_clockfilter(s,&mixL,&mixR);   /* Clock+filter pre-punch (default) */
        /* Punch-in FX: up to 4 slots in series, each with its own capture ring */
        { double xl=mixL,xr=mixR;
          for(int si=0;si<NUM_PSLOTS;si++){ PunchSlot *ps=&s->pslot[si];
              if(ps->idx<0){ ps->ringL[ps->w]=(float)xl; ps->ringR[ps->w]=(float)xr; }   /* idle: cache the chain signal at this slot (no engage boundary) */
              else { int hold=punch_holds_ring(s,ps);
                  if(!hold){ ps->ringL[ps->w]=(float)xl; ps->ringR[ps->w]=(float)xr; }         /* active: capture chain input */
                  double wl,wr; punch_slot_process(s,ps,n,&wl,&wr);
                  /* Loudness match: windowed grains, gated hits and slice fades all lose level
                   * against the dry signal. Track both powers (~100 ms) and make the wet up,
                   * never down, capped so gated effects do not get pumped into hits. */
                  ps->dPow+=(xl*xl+xr*xr-ps->dPow)*2.2e-4; ps->wPow+=(wl*wl+wr*wr-ps->wPow)*2.2e-4;
                  { double tg=1.0; if(ps->wPow>1e-7&&ps->dPow>1e-7){ tg=sqrt(ps->dPow/ps->wPow);
                        double cap=(PUNCH_DEFS[ps->idx].mech==PM_CHOP)?1.4:2.5; if(tg>cap)tg=cap; if(tg<1.0)tg=1.0; }
                    ps->mkGain+=(tg-ps->mkGain)*0.002; wl*=ps->mkGain; wr*=ps->mkGain; }
                  if(hold){ ps->w--; if(ps->w<0)ps->w=PUNCH_BUF-1; }                          /* net: w stays put while holding */
                  if(ps->restartPend&&!ps->releasing){ ps->env+=(0.0-ps->env)*0.06; if(ps->env<0.01) punch_slot_start(s,ps,ps->idx); }   /* queued retrigger */
                  else { int mm=PUNCH_DEFS[ps->idx].mech;
                         int slice=(mm==PM_REPEAT||mm==PM_REVERSE||mm==PM_GLIDE||mm==PM_CHOP);
                         double ak=ps->releasing?0.003:(slice?0.02:0.0013);   /* slice mechs stay punchy; shifters/grains fade in ~18ms so engage never clicks */
                         ps->env += ((ps->releasing?0.0:1.0)-ps->env)*ak; }
                  { double wf=(double)s->punchWidth*2.0, wm=0.5*(wl+wr), wsd=0.5*(wl-wr)*wf; wl=wm+wsd; wr=wm-wsd; }   /* effect stereo width: 0 mono .. 0.5 normal .. 1 wide (wet only) */
                  float *P=ps->pSm; double em=(PUNCH_DEFS[ps->idx].mech==PM_PALETTE)?ps->env:((double)P[3]+(1.0-(double)P[3])*ps->pressSm)*ps->env; if(em>1.0)em=1.0;
                  xl=xl+(wl-xl)*em; xr=xr+(wr-xr)*em;
                  if(ps->releasing && ps->env<0.004) ps->idx=-1; }
              ps->w++; if(ps->w>=PUNCH_BUF)ps->w=0; }
          mixL=xl; mixR=xr; }
        if(s->mClockSpot==1) master_clockfilter(s,&mixL,&mixR);   /* Clock+filter post-punch */
        drift_sample(s,&mixL,&mixR);         /* Drift: global COSMOS memory layer (Sample menu) */
        perf_pump(s,&mixL,&mixR);            /* Perform K7/K8: rhythmic ducking pump */
        master_character(s,&mixL,&mixR);     /* Settings: console/sampler colour */
        master_glue(s,&mixL,&mixR);          /* Settings: bus glue comp */
        mixL*=(double)s->masterVol; mixR*=(double)s->masterVol;   /* master output level */
        tape_limiter(s,&mixL,&mixR);         /* Settings: analog tape limiter (final) */
        s->selfPrevL[n]=(float)mixL; s->selfPrevR[n]=(float)mixR;   /* feed the master-record feedback guard next block */
        out_interleaved_lr[n*2]=(int16_t)lb_clampd(mixL*32767.0,-32767.0,32767.0);
        out_interleaved_lr[n*2+1]=(int16_t)lb_clampd(mixR*32767.0,-32767.0,32767.0);
    }
    lcxl_leds(s);   /* mirror loop state to the LaunchControl XL LEDs (when MIDI Out is on) */
    /* Process the two Palette send buses over the whole block (result feeds the next block) */
    if(s->busA&&!atomic_load(&s->fxBusy[0])){ pfx_process(s->busA,s->sbufAL,s->sbufAR,frames,s->sendAM1,s->sendAM2,s->sendADrift);
        memcpy(s->sretAL,s->sbufAL,(size_t)frames*sizeof(float)); memcpy(s->sretAR,s->sbufAR,(size_t)frames*sizeof(float)); }
    else { memset(s->sretAL,0,(size_t)frames*sizeof(float)); memset(s->sretAR,0,(size_t)frames*sizeof(float)); }
    if(s->busB&&!atomic_load(&s->fxBusy[1])){ pfx_process(s->busB,s->sbufBL,s->sbufBR,frames,s->sendBM1,s->sendBM2,s->sendBDrift);
        memcpy(s->sretBL,s->sbufBL,(size_t)frames*sizeof(float)); memcpy(s->sretBR,s->sbufBR,(size_t)frames*sizeof(float)); }
    else { memset(s->sretBL,0,(size_t)frames*sizeof(float)); memset(s->sretBR,0,(size_t)frames*sizeof(float)); }
    /* Per-loop pitch shifters: this block's input becomes next block's output (one block late) */
    for(int vi=0;vi<NUM_VOICES;vi++){ Voice *v=&s->voice[vi]; if(!v->psActive||!v->ps)continue;
        memcpy(v->psOutL,v->psInL,sizeof v->psOutL); memcpy(v->psOutR,v->psInR,sizeof v->psOutR);
        ps_process(v->ps,v->psOutL,v->psOutR,frames); }
    /* Palette punch slot: process the block it captured, ready for the next one */
    for(int si=0;si<NUM_PSLOTS;si++){ PunchSlot *ps=&s->pslot[si]; if(ps->idx<0||PUNCH_DEFS[ps->idx].mech!=PM_PALETTE)continue;
        memcpy(ps->poutL,ps->pinL,sizeof ps->poutL); memcpy(ps->poutR,ps->pinR,sizeof ps->poutR);
        float *P=ps->pSm; float amt=P[1]+(1.0f-P[1])*(float)ps->pressSm;   /* pressure pushes Amount */
        if(s->punchFx&&!atomic_load(&s->fxBusy[2])) pfx_process(s->punchFx,ps->poutL,ps->poutR,frames,amt,P[2],P[3]);
        else { memset(ps->poutL,0,sizeof ps->poutL); memset(ps->poutR,0,sizeof ps->poutR); } }

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
static const char *insrc_opts[]={"Line","Master","S1","S2","S3","S4","M1","M2","M3","M4"};
static const char *reverse_opts[]={"Normal","Reverse"};
static const char *stkind_opts[]={"Tumble","Stutter","Reverse","Tape","Gate","Crush"};
static int match_enum(const char *value, const char **opts, int count){for(int i=0;i<count;i++)if(strcmp(value,opts[i])==0)return i;return -1;}

/* ---- Overtake control (driven from ui.js via set_param("cmd", ...)) ---- */
/* Single-tap gesture: cycle Empty->Rec->Play<->Pause; from Odub -> Play. */
static void voice_tap(loopex_t *s, int vi) {
    if(vi<0||vi>=NUM_VOICES)return; Voice *v=&s->voice[vi]; s->selTrack=vi+1;
    switch(v->state){
    case VS_EMPTY: v->state=VS_RECORDING; v->recHead=0; v->loopLen=0; break;
    case VS_RECORDING: v->loopLen=v->recHead; v->playHead=0; v->playPhase=0.0; v->state=VS_PLAYING; break;
    case VS_PLAYING: v->state=VS_PAUSED; break;
    case VS_PAUSED: v->state=VS_PLAYING; break;
    case VS_OVERDUBBING: if((int)s->overdubMode==OD_DISINTEGRATION)atomic_store(&s->disintReq,vi+1); v->state=VS_PLAYING; break;
    }
}
/* Overdub gesture (double-tap): Play/Pause -> Overdub; Odub -> Play; Rec -> Play. */
static void voice_odub(loopex_t *s, int vi) {
    if(vi<0||vi>=NUM_VOICES)return; Voice *v=&s->voice[vi]; s->selTrack=vi+1;
    switch(v->state){
    case VS_PLAYING: case VS_PAUSED: v->state=VS_OVERDUBBING; v->recHead=v->playHead;
        s->undoCur++; if(!s->undoCur)s->undoCur=1; s->undoTrack=vi; s->undoCount=0; s->undoLen=v->loopLen; break;   /* fresh undo point */
    case VS_OVERDUBBING: if((int)s->overdubMode==OD_DISINTEGRATION)atomic_store(&s->disintReq,vi+1); v->state=VS_PLAYING; break;
    case VS_RECORDING: v->loopLen=v->recHead; v->playHead=0; v->playPhase=0.0; v->state=VS_PLAYING; break;
    default: break;
    }
}

static void set_param(void *inst, const char *key, const char *val) {
    loopex_t *s=(loopex_t*)inst;if(!key||!val)return;
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
        else if(strncmp(val,"undo",4)==0){ int t=s->undoTrack;   /* revert the last overdub */
            if(t>=0&&t<NUM_VOICES&&s->undoCount>0){ Voice *v=&s->voice[t]; if(v->state==VS_OVERDUBBING)v->state=VS_PLAYING; atomic_store(&s->undoReq,1); } }
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
        const char *c=strchr(val,':'); int n=c?atoi(c+1):1; if(n<1)n=1; if(n>NUM_SLOTS)n=NUM_SLOTS;
        if(atomic_load(&s->sio.busy)||!s->sio.active) return;
        atomic_store(&s->sio.slot,n);
        if(strncmp(val,"save",4)==0){
            get_param(s,"state",s->sio.stateBuf,(int)sizeof(s->sio.stateBuf));  /* snapshot settings */
            atomic_store(&s->sio.request,1);
        } else if(strncmp(val,"load",4)==0) atomic_store(&s->sio.request,2);
        else if(strncmp(val,"delete",6)==0) atomic_store(&s->sio.request,5);
        return; }
    if(strcmp(key,"punch")==0){ const char *c=strchr(val,':'); int n=c?atoi(c+1):-1;
        if(n<0||n>=NUM_PUNCH)return;
        if(strncmp(val,"on",2)==0){ s->fx.userHeld[n]=1; punch_on(s,n); }
        else if(strncmp(val,"off",3)==0){ s->fx.userHeld[n]=0; if(!s->fx.seqHeld[n]) punch_off(s,n); }   /* the sequencer may still own it */
        return; }
    if(strcmp(key,"pfx")==0){ int idx=atoi(val); const char *c1=strchr(val,':'); if(!c1)return; int p=atoi(c1+1);
        const char *c2=strchr(c1+1,':'); if(!c2)return; float v=lb_clampf((float)atof(c2+1),0.0f,1.0f);
        if(idx>=0&&idx<NUM_PUNCH&&p>=0&&p<4){ s->punchParams[idx][p]=v;
            if(p==0){ if(PUNCH_DEFS[idx].mech==PM_PALETTE){ int id=(int)(v*(PFX_NUM-1)+0.5f); if(id<0)id=0; if(id>=PFX_NUM)id=PFX_NUM-1; if(id!=s->punchFxId){ s->punchFxId=id; atomic_store(&s->fxSel[2],id); } }
                      else for(int i=0;i<NUM_PSLOTS;i++) if(s->pslot[i].idx==idx) punch_slot_retune(s,&s->pslot[i],idx); } }
        return; }
    /* State restore for punch params: "pfx0" .. "pfx15" = "rate,pitch,tone,mix" */
    if(strcmp(key,"pflfo")==0){ int idx=atoi(val); const char *c1=strchr(val,':'); if(!c1)return; int p=atoi(c1+1);
        const char *c2=strchr(c1+1,':'); if(!c2)return; float v=lb_clampf((float)atof(c2+1),0.0f,1.0f);
        if(idx>=0&&idx<NUM_PUNCH&&p>=0&&p<4) s->punchLfo[idx][p]=v; return; }
    if(strncmp(key,"pfl",3)==0&&key[3]>='0'&&key[3]<='9'){   /* state: pflN = dest,shape,speed,depth */
        int idx=atoi(key+3); if(idx>=0&&idx<NUM_PUNCH){ float a=0,b=0,c=0,d=0;
            if(sscanf(val,"%f,%f,%f,%f",&a,&b,&c,&d)==4){ s->punchLfo[idx][0]=lb_clampf(a,0,1); s->punchLfo[idx][1]=lb_clampf(b,0,1);
                s->punchLfo[idx][2]=lb_clampf(c,0,1); s->punchLfo[idx][3]=lb_clampf(d,0,1); } } return; }
    SETFR("mfCut",mfCut,0.0,1.0) SETFR("mfReso",mfReso,0.0,1.0)
    if(strcmp(key,"mfMode")==0){ int i=match_enum(val,mfmode_opts,MF_NVOICE); s->mfMode=(i>=0)?i:(int)lb_clampf((float)atof(val),0,MF_NVOICE-1); return; }
    SETFR("mClock",mClock,0.0,1.0)
    if(strcmp(key,"mClockMode")==0){ static const char*o[]={"Music","Free"}; int i=match_enum(val,o,2); s->mClockMode=(i>=0)?i:(atof(val)>0.5?1:0); return; }
    if(strcmp(key,"mClockSpot")==0){ static const char*o[]={"Pre","Post"}; int i=match_enum(val,o,2); s->mClockSpot=(i>=0)?i:(atof(val)>0.5?1:0); return; }
    if(strcmp(key,"inSource")==0){ int i=match_enum(val,insrc_opts,10); s->inSource=(i>=0)?i:(int)lb_clampf((float)atof(val),0,9); return; }
    SETFR("perfTrem",perfTrem,0.0,1.0) SETFR("perfTremRate",perfTremRate,0.0,1.0) SETFR("punchWidth",punchWidth,0.0,1.0)
    if(strcmp(key,"masterEQ")==0){ int i=match_enum(val,meq_opts,MEQ_N); s->masterEQ=(i>=0)?i:(int)lb_clampf((float)atof(val),0,MEQ_N-1); master_eq_update(s); return; }
    if(strcmp(key,"loopFiltMode")==0){ int i=match_enum(val,mfmode_opts,MF_NVOICE); s->loopFilterMode=(i>=0)?i:(int)lb_clampf((float)atof(val),0,MF_NVOICE-1); return; }
    SETFR("masterGlue",masterGlue,0.0,1.0) SETFR("tapeLimit",tapeLimit,0.0,1.0)
    SETFR("driftAmt",driftAmt,0.0,1.0) SETFR("driftRate",driftRate,0.0,1.0)
    SETFR("driftSize",driftSize,0.0,1.0) SETFR("driftFb",driftFb,0.0,1.0)
    SETFR("driftSupr",driftSupr,0.0,1.0) SETFR("driftBlur",driftBlur,0.0,1.0)
    SETFR("driftDamp",driftDamp,0.0,1.0) SETFR("driftMix",driftMix,0.0,1.0)
    if(strncmp(key,"pfx",3)==0&&key[3]>='0'&&key[3]<='9'){
        int idx=atoi(key+3);
        if(idx>=0&&idx<NUM_PUNCH){ float a=0,b=0,c=0,d=0;
            if(sscanf(val,"%f,%f,%f,%f",&a,&b,&c,&d)==4){
                s->punchParams[idx][0]=lb_clampf(a,0.0f,1.0f); s->punchParams[idx][1]=lb_clampf(b,0.0f,1.0f);
                s->punchParams[idx][2]=lb_clampf(c,0.0f,1.0f); s->punchParams[idx][3]=lb_clampf(d,0.0f,1.0f); } }
        return; }
    /* FX sequencer */
    if(strcmp(key,"fxseqRun")==0){ int on=(strcmp(val,"On")==0||atof(val)>0.5); if(on&&!s->fx.run)s->fx.restart=1; s->fx.run=on; return; }
    if(strcmp(key,"fxseqSpeed")==0){ int i=match_enum(val,fxspeed_opts,7); if(i<0)i=(int)lb_clampf((float)atof(val),0.0f,6.0f); s->fx.speed=i; return; }
    if(strcmp(key,"fxseqLen")==0){ int n=atoi(val); if(n<1)n=1; if(n>FXSEQ_STEPS)n=FXSEQ_STEPS; s->fx.len=n; return; }
    if(strcmp(key,"fxseqGate")==0){ s->fx.gate=lb_clampf((float)atof(val),0.1f,1.0f); return; }
    if(strcmp(key,"fxseqSwing")==0){ s->fx.swing=lb_clampf((float)atof(val),0.5f,0.75f); return; }
    if(strcmp(key,"fxseqDir")==0){ int i=match_enum(val,fxdir_opts,4); if(i<0)i=(int)lb_clampf((float)atof(val),0.0f,3.0f); s->fx.dir=i; return; }
    if(strcmp(key,"fxseqClear")==0){ if(atof(val)>0.5){ fxseq_off_all(s); for(int i=0;i<FXSEQ_STEPS;i++) memset(&s->fx.st[i],0,sizeof(FxStep)); } return; }
    if(strcmp(key,"fxseq")==0){   /* "restart" (Play pressed) or the state line "run,spd,len,gate,swing,dir" */
        if(strncmp(val,"restart",7)==0){ s->fx.restart=1; return; }
        int run=0,spd=1,len=16,dir=0; float gate=1,sw=0.5f;
        if(sscanf(val,"%d,%d,%d,%f,%f,%d",&run,&spd,&len,&gate,&sw,&dir)==6){
            s->fx.speed=(spd<0||spd>6)?1:spd; s->fx.len=(len<1||len>FXSEQ_STEPS)?16:len; s->fx.gate=lb_clampf(gate,0.1f,1.0f);
            s->fx.swing=lb_clampf(sw,0.5f,0.75f); s->fx.dir=(dir<0||dir>3)?0:dir; if(run&&!s->fx.run)s->fx.restart=1; s->fx.run=run?1:0; }
        return; }
    if(strcmp(key,"fxstep")==0){   /* "IDX:K:PAD:L0:L1:L2:L3:PRESS" — one pad entry of a step */
        int i=0,k=0,pad=0; float l0=0,l1=0,l2=0,l3=0,pr=0;
        if(sscanf(val,"%d:%d:%d:%f:%f:%f:%f:%f",&i,&k,&pad,&l0,&l1,&l2,&l3,&pr)==8&&i>=0&&i<FXSEQ_STEPS&&k>=0&&k<FXSEQ_MAXPADS&&pad>=0&&pad<NUM_PUNCH){
            FxStep *st=&s->fx.st[i]; st->pad[k]=(uint8_t)pad;
            st->lock[k][0]=lb_clampf(l0,0,1); st->lock[k][1]=lb_clampf(l1,0,1); st->lock[k][2]=lb_clampf(l2,0,1); st->lock[k][3]=lb_clampf(l3,0,1); st->press[k]=lb_clampf(pr,0,1); }
        return; }
    if(strcmp(key,"fxstepn")==0){ int i=atoi(val); const char *c=strchr(val,':'); int n=c?atoi(c+1):0;   /* pad count; 0 clears the step */
        if(i>=0&&i<FXSEQ_STEPS){ if(n<0)n=0; if(n>FXSEQ_MAXPADS)n=FXSEQ_MAXPADS; s->fx.st[i].n=(uint8_t)n; if(n==0){ s->fx.st[i].ext=0; s->fx.st[i].cycles=0; } } return; }
    if(strcmp(key,"fxchance")==0){ int i=atoi(val); const char *c=strchr(val,':'); int ch=c?atoi(c+1):0; if(i>=0&&i<FXSEQ_STEPS){ if(ch<0)ch=0; if(ch>13)ch=13; s->fx.st[i].chance=(uint8_t)ch; } return; }
    if(strcmp(key,"fxext")==0){ int i=atoi(val); const char *c=strchr(val,':'); int e=c?atoi(c+1):0; if(i>=0&&i<FXSEQ_STEPS){ s->fx.st[i].ext=e?1:0; if(e)s->fx.st[i].n=0; } return; }
    if(strncmp(key,"fxs",3)==0&&key[3]>='0'&&key[3]<='9'){   /* state: fxsN = n,ext,chance,pad:l0,l1,l2,l3,pr;... */
        int i=atoi(key+3); if(i<0||i>=FXSEQ_STEPS)return; FxStep *st=&s->fx.st[i]; memset(st,0,sizeof *st);
        int n=0,ext=0,ch=0,used=0; if(sscanf(val,"%d,%d,%d%n",&n,&ext,&ch,&used)<3)return;
        st->ext=ext?1:0; st->chance=(uint8_t)((ch<0||ch>13)?0:ch); const char *q=val+used; int k=0;
        while(*q&&k<FXSEQ_MAXPADS&&k<n){ if(*q==','||*q==';')q++; int pad=0; float l0=0,l1=0,l2=0,l3=0,pr=0; int u2=0;
            if(sscanf(q,"%d:%f,%f,%f,%f,%f%n",&pad,&l0,&l1,&l2,&l3,&pr,&u2)<6)break;
            st->pad[k]=(uint8_t)(pad&15); st->lock[k][0]=lb_clampf(l0,0,1); st->lock[k][1]=lb_clampf(l1,0,1); st->lock[k][2]=lb_clampf(l2,0,1); st->lock[k][3]=lb_clampf(l3,0,1); st->press[k]=pr; k++; q+=u2; }
        st->n=(uint8_t)k; return; }
    if(strcmp(key,"punchPress")==0){ int idx=atoi(val); const char *c=strchr(val,':'); float v=c?lb_clampf((float)atof(c+1),0.0f,1.0f):0.0f; if(idx>=0&&idx<NUM_PUNCH)s->punchPress[idx]=v; return; }
    SETFR("globalSat",globalSat,0.0,2.0) SETFR("masterComp",masterComp,0.0,1.0)
    SETFR("masterLoCut",masterLoCut,20.0,1000.0) SETFR("masterHiCut",masterHiCut,1000.0,20000.0)
    SETFR("masterVol",masterVol,0.0,1.5)
    if(strcmp(key,"preamp")==0){int idx=match_enum(val,preamp_opts,NUM_PREAMP);if(idx>=0)s->preamp=(float)idx;else s->preamp=lb_clampf((float)atof(val),0.0f,(float)(NUM_PREAMP-1));return;}
    if(strcmp(key,"overdubMode")==0){int idx=match_enum(val,odmode_opts,3);if(idx>=0)s->overdubMode=(float)idx;else s->overdubMode=lb_clampf((float)atof(val),0.0f,2.0f);return;}
    SETFR("stability",stability,0.0,1.0) SETFR("globalWowFlut",globalWowFlut,0.0,1.0) SETFR("inputMonitor",inputMonitor,0.0,1.0) SETFR("inputGain",inputGain,0.0,2.0)
    SETFR("inLow",inLow,-1.0,1.0) SETFR("inMid",inMid,-1.0,1.0) SETFR("inMidFreq",inMidFreq,0.0,1.0)
    SETFR("inHigh",inHigh,-1.0,1.0) SETFR("inHighFreq",inHighFreq,0.0,1.0)
    SETFR("tapeNoise",tapeNoise,0.0,1.0) SETFR("tapeDrive",tapeDrive,0.0,1.0) SETFR("tapeHF",tapeHF,0.0,1.0)
    if(strcmp(key,"midiIn")==0){ s->midiIn=(strcmp(val,"On")==0||atof(val)>0.5)?1:0; return; }
    if(strcmp(key,"midiOut")==0){ s->midiOut=(strcmp(val,"On")==0||atof(val)>0.5)?1:0; return; }
    SETFR("armThresh",armThresh,0.0,1.0)
    if(strcmp(key,"tapeHold")==0){ int h=atoi(val); s->tapeHold=(h<0)?-1:(h>0)?1:0; return; }
    SETFR("tapeLoCut",tapeLoCut,0.0,1.0) SETFR("tapeWow",tapeWow,0.0,1.0) SETFR("tapeFlut",tapeFlut,0.0,1.0) SETFR("tapeGen",tapeGen,0.0,1.0)
    if(strcmp(key,"sendAType")==0){int id=-1; for(int i=0;i<PFX_NUM;i++) if(strcmp(val,pfx_name(i))==0){id=i;break;} if(id<0)id=(int)lb_clampf((float)atof(val),0.0f,(float)(PFX_NUM-1)); s->sendAType=id; atomic_store(&s->fxSel[0],id); return;}   /* worker swaps it (may alloc) */
    if(strcmp(key,"sendBType")==0){int id=-1; for(int i=0;i<PFX_NUM;i++) if(strcmp(val,pfx_name(i))==0){id=i;break;} if(id<0)id=(int)lb_clampf((float)atof(val),0.0f,(float)(PFX_NUM-1)); s->sendBType=id; atomic_store(&s->fxSel[1],id); return;}
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
        double _sv=atof(val); if(_sv>100.0)_sv=100.0; if(_sv<-100.0)_sv=-100.0;
        double dist=_sv*(double)v->loopLen*0.01, win=SR*0.12;
        v->scrubRate=dist/win; v->scrubTimer=(int)win;
        return; }
    if(strcmp(key,"headpos")==0){ int si=s->selTrack-1; if(si<0||si>=NUM_VOICES)return; Voice *v=&s->voice[si];
        const char *c=strchr(val,':'); int hi=atoi(val); double d=c?atof(c+1):0.0; if(d>100.0)d=100.0; if(d<-100.0)d=-100.0;
        if(hi<0||hi>3||v->loopLen<=0)return;
        double mv=d*(double)v->loopLen*0.01;
        if(hi==0){ v->scatXfadePhase=v->playPhase; v->scatXfade=64; v->playPhase+=mv;
            while(v->playPhase>=(double)v->loopLen)v->playPhase-=(double)v->loopLen;
            while(v->playPhase<0.0)v->playPhase+=(double)v->loopLen; }
        else head_jump(v,hi,mv);
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
    SETVFR("v_sendA",send,0.0,1.0) SETVFR("v_sendB",sendB,0.0,1.0)
    SETVFR("v_scatter",scatter,0.0,1.0) SETVFR("v_glitch",glitch,0.0,1.0) SETVFR("v_tilt",tiltEQ,-1.0,1.0)
    SETVFR("v_eqBass",eqBass,-1.0,1.0) SETVFR("v_eqPresFrq",eqPresFreq,0.0,1.0)
    SETVFR("v_eqPresAmt",eqPresAmt,-1.0,1.0) SETVFR("v_eqTreble",eqTreble,-1.0,1.0)
    SETVFR("v_pitch",pitch,-2.0,2.0) SETVFR("v_filter",filter,0.0,1.0)
    SETVFR("v_pan",pan,-1.0,1.0) SETVFR("v_volume",volume,0.0,1.0) SETVFR("v_decay",decay,0.0,1.0)
    SETVFR("v_hvol2",hVol[1],0.0,1.0) SETVFR("v_hvol3",hVol[2],0.0,1.0) SETVFR("v_hvol4",hVol[3],0.0,1.0)
    SETVFR("v_hpan2",hPan[1],-1.0,1.0) SETVFR("v_hpan3",hPan[2],-1.0,1.0) SETVFR("v_hpan4",hPan[3],-1.0,1.0)
    SETVFR("v_atk",ampAtk,0.0,1.0) SETVFR("v_rel",ampRel,0.0,1.0)
    if(strncmp(key,"v_ph",4)==0&&key[4]>='1'&&key[4]<='4'){
        int hi=key[4]-'1'; const char *sub=key+5;
        if(strcmp(sub,"mode")==0){
            static const char *mo[]={"Off","Fwd","Bwd","Ping","Jump"};
            int m=match_enum(val,mo,5); if(m<0)m=(int)lb_clampf((float)atof(val),0.0f,4.0f);
            int was=v->ph[hi].mode; v->ph[hi].mode=m;
            if(m>0&&was==0){   /* turning a head on always restarts it at the loop start */
                int es=(int)(v->loopStart*(float)v->loopLen); if(es<0)es=0;
                v->ph[hi].phase=(double)es; v->ph[hi].dir=1;
                if(hi==0){ v->playPhase=(double)es; v->scatXfade=0; } }
            return; }
        if(strcmp(sub,"spd")==0){ v->ph[hi].spd=lb_clampf((float)atof(val),0.0f,1.0f); return; }
        return; }
    if(strcmp(key,"v_djReso")==0){v->djReso=lb_clampf((float)atof(val),0.0f,1.0f);dj_filter_update(v);return;}
    SETVFR("v_comp",comp,0.0,1.0) SETVFR("v_clock",clock,-2.0,2.0)
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
            else if(strcmp(sub,"sdB")==0)vr->sendB=lb_clampf(fv,0,1);
            else if(strcmp(sub,"sct")==0)vr->scatter=lb_clampf(fv,0,1);
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
            else if(strcmp(sub,"pit2")==0)vr->clock=lb_clampf(fv,-2,2);   /* old "clk" (a rate) is ignored: not the same parameter any more */
            else if(strcmp(sub,"ph")==0){ int m[4]; float sp[4];
                if(sscanf(val,"%d,%f,%d,%f,%d,%f,%d,%f",&m[0],&sp[0],&m[1],&sp[1],&m[2],&sp[2],&m[3],&sp[3])==8)
                    for(int k=0;k<4;k++){ vr->ph[k].mode=(m[k]<0||m[k]>4)?0:m[k];
                        vr->ph[k].spd=lb_clampf(sp[k],0,1); vr->ph[k].env=(vr->ph[k].mode>0)?1.0:0.0; } }
            else if(strcmp(sub,"hvp")==0){ float hv[3],hp[3];
                if(sscanf(val,"%f,%f,%f,%f,%f,%f",&hv[0],&hp[0],&hv[1],&hp[1],&hv[2],&hp[2])==6)
                    for(int k=0;k<3;k++){ vr->hVol[k+1]=lb_clampf(hv[k],0,1); vr->hPan[k+1]=lb_clampf(hp[k],-1,1); } }
            studer_eq_update(vr);tilt_eq_update(vr);}return;}
}

#define GETP(k,f) if(strcmp(key,k)==0)return snprintf(buf,buf_len,"%.4f",(double)s->f);
#define GETVP(k,f) if(strcmp(key,k)==0)return snprintf(buf,buf_len,"%.4f",(double)v->f);
#define GETE(k,f,opts,cnt) do{if(strcmp(key,k)==0){int _i=(int)roundf(s->f);if(_i<0)_i=0;if(_i>=(cnt))_i=(cnt)-1;return snprintf(buf,buf_len,"%s",(opts)[_i]);}}while(0)

/* ui_hierarchy JSON - MUST be returned from get_param for sound generators */
static const char *UI_HIERARCHY_JSON =
    "{\"modes\":null,\"levels\":{"
    "\"root\":{\"name\":\"Loopex\","
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
    "[{\"key\":\"globalSat\",\"name\":\"Sat\",\"type\":\"float\",\"min\":0,\"max\":2,\"step\":0.01},"
    "{\"key\":\"masterComp\",\"name\":\"Comp\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"masterLoCut\",\"name\":\"LoCut\",\"type\":\"int\",\"min\":20,\"max\":1000,\"step\":1},"
    "{\"key\":\"masterHiCut\",\"name\":\"HiCut\",\"type\":\"int\",\"min\":1000,\"max\":20000,\"step\":200},"
    "{\"key\":\"stability\",\"name\":\"Stabil\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"globalWowFlut\",\"name\":\"W/Flut\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"inputMonitor\",\"name\":\"InMon\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"masterVol\",\"name\":\"Out\",\"type\":\"float\",\"min\":0,\"max\":1.5,\"step\":0.01},"
    "{\"key\":\"sendAType\",\"name\":\"A Fx\",\"type\":\"enum\",\"options\":[\"Off\",\"Drive\",\"Sweeten\",\"Fuzz\",\"Howl\",\"Fold\",\"Swell\",\"Doubler\",\"Vibrato\",\"Phaser\",\"Tremolo\",\"Pitch\",\"Shift\",\"Cascade\",\"Reels\",\"Collage\",\"Reverse\",\"Space\",\"Bloom\",\"Filter\",\"Squash\",\"Cassette\",\"Broken\",\"Interference\",\"Halo\",\"Plate\",\"Quartz\",\"Prism\",\"Veil\"]},"
    "{\"key\":\"sendAM1\",\"name\":\"A Amt\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"sendAM2\",\"name\":\"A Mac\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"sendADrift\",\"name\":\"A Drf\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"sendBType\",\"name\":\"B Fx\",\"type\":\"enum\",\"options\":[\"Off\",\"Drive\",\"Sweeten\",\"Fuzz\",\"Howl\",\"Fold\",\"Swell\",\"Doubler\",\"Vibrato\",\"Phaser\",\"Tremolo\",\"Pitch\",\"Shift\",\"Cascade\",\"Reels\",\"Collage\",\"Reverse\",\"Space\",\"Bloom\",\"Filter\",\"Squash\",\"Cassette\",\"Broken\",\"Interference\",\"Halo\",\"Plate\",\"Quartz\",\"Prism\",\"Veil\"]},"
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
    "{\"key\":\"v_pitch\",\"name\":\"Speed\",\"type\":\"float\",\"min\":-2,\"max\":2,\"step\":0.01},"
    "{\"key\":\"v_filter\",\"name\":\"Filter\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"v_pan\",\"name\":\"Pan\",\"type\":\"float\",\"min\":-1,\"max\":1,\"step\":0.01},"
    "{\"key\":\"v_volume\",\"name\":\"Vol\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"v_decay\",\"name\":\"Decay\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"v_djReso\",\"name\":\"Reso\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"v_atk\",\"name\":\"Atk\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"v_rel\",\"name\":\"Rel\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"v_comp\",\"name\":\"Comp\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"v_clock\",\"name\":\"Pitch\",\"type\":\"float\",\"min\":-2,\"max\":2,\"step\":0.01}]";

#define APP(...) do{ if(p<buf_len){ int _n=snprintf(buf+p,(size_t)(buf_len-p),__VA_ARGS__); if(_n>0)p+=_n; if(p>buf_len)p=buf_len; } }while(0)
static int get_param(void *inst, const char *key, char *buf, int buf_len) {
    loopex_t *s=(loopex_t*)inst;if(!key)return -1;
    if(strncmp(key,"pfxq",4)==0){ int i=atoi(key+4); if(i<0||i>=NUM_PUNCH)return -1;
        return snprintf(buf,buf_len,"%.4f,%.4f,%.4f,%.4f",(double)s->punchParams[i][0],(double)s->punchParams[i][1],(double)s->punchParams[i][2],(double)s->punchParams[i][3]); }
    if(strncmp(key,"pflq",4)==0){ int i=atoi(key+4); if(i<0||i>=NUM_PUNCH)return -1;
        return snprintf(buf,buf_len,"%.4f,%.4f,%.4f,%.4f",(double)s->punchLfo[i][0],(double)s->punchLfo[i][1],(double)s->punchLfo[i][2],(double)s->punchLfo[i][3]); }

    /* Overtake: Manager discovery + UI feedback */
    if(strcmp(key,"module_id")==0)return snprintf(buf,buf_len,"loopex");
    if(strcmp(key,"cpu")==0)return snprintf(buf,buf_len,"%.1f",s->cpuPct);
    if(strcmp(key,"sessStatus")==0){ static const char *st[]={"","Saving..","Loading..","OK","Empty"};
        int i=atomic_load(&s->sio.status); if(i<0||i>4)i=0; return snprintf(buf,buf_len,"%s",st[i]); }
    if(strcmp(key,"sessSlot")==0)return snprintf(buf,buf_len,"%d",atomic_load(&s->sio.slot));
    if(strcmp(key,"sessName")==0){ int n=atomic_load(&s->sio.slot); if(n<1||n>NUM_SLOTS)n=1; return snprintf(buf,buf_len,"%s",s->sio.names[n]); }
    if(strcmp(key,"sessNames")==0){ int p=0; for(int n=1;n<=NUM_SLOTS&&p<buf_len-2;n++) APP("%s%s",s->sio.names[n],(n<NUM_SLOTS)?";":""); return p; }
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
            APP("%d,%d%s",v->ph[k].mode,pos,(k<3)?";":""); }
        return p; }
    if(strcmp(key,"fxseqRun")==0)return snprintf(buf,buf_len,"%s",s->fx.run?"On":"Off");
    if(strcmp(key,"fxseqSpeed")==0)return snprintf(buf,buf_len,"%s",fxspeed_opts[s->fx.speed]);
    if(strcmp(key,"fxseqLen")==0)return snprintf(buf,buf_len,"%d",s->fx.len);
    if(strcmp(key,"fxseqGate")==0)return snprintf(buf,buf_len,"%.4f",(double)s->fx.gate);
    if(strcmp(key,"fxseqSwing")==0)return snprintf(buf,buf_len,"%.4f",(double)s->fx.swing);
    if(strcmp(key,"fxseqDir")==0)return snprintf(buf,buf_len,"%s",fxdir_opts[s->fx.dir]);
    if(strcmp(key,"fxseqClear")==0)return snprintf(buf,buf_len,"0");
    if(strcmp(key,"fxpat")==0){   /* "pos;................" . empty  o pads  - extension */
        int p=snprintf(buf,buf_len,"%d;",s->fx.run?s->fx.pos:-1);
        for(int i=0;i<FXSEQ_STEPS&&p<buf_len-1;i++) buf[p++]=s->fx.st[i].ext?'-':(s->fx.st[i].n?'o':'.');
        buf[p]=0; return p; }
    if(strncmp(key,"fxstep",6)==0&&key[6]>='0'&&key[6]<='9'){ int i=atoi(key+6); if(i<0||i>=FXSEQ_STEPS)return -1; const FxStep *st=&s->fx.st[i];
        int p=snprintf(buf,buf_len,"%d,%d,%d",st->n,st->ext,st->chance);
        for(int k=0;k<st->n&&k<FXSEQ_MAXPADS;k++) APP(";%d:%.4f,%.4f,%.4f,%.4f,%.4f",st->pad[k],(double)st->lock[k][0],(double)st->lock[k][1],(double)st->lock[k][2],(double)st->lock[k][3],(double)st->press[k]);
        return p; }
    GETP("mfCut",mfCut) GETP("mfReso",mfReso)
    if(strcmp(key,"mfMode")==0)return snprintf(buf,buf_len,"%s",mfmode_opts[s->mfMode]);
    GETP("mClock",mClock)
    if(strcmp(key,"mClockMode")==0)return snprintf(buf,buf_len,"%s",s->mClockMode?"Free":"Music");
    if(strcmp(key,"mClockSpot")==0)return snprintf(buf,buf_len,"%s",s->mClockSpot?"Post":"Pre");
    GETP("perfTrem",perfTrem) GETP("perfTremRate",perfTremRate) GETP("punchWidth",punchWidth)
    if(strcmp(key,"masterEQ")==0)return snprintf(buf,buf_len,"%s",meq_opts[s->masterEQ]);
    GETP("masterGlue",masterGlue) GETP("tapeLimit",tapeLimit)
    GETP("driftAmt",driftAmt) GETP("driftRate",driftRate) GETP("driftSize",driftSize) GETP("driftFb",driftFb)
    GETP("driftSupr",driftSupr) GETP("driftBlur",driftBlur) GETP("driftDamp",driftDamp) GETP("driftMix",driftMix)
    if(strcmp(key,"undoAvail")==0)return snprintf(buf,buf_len,"%d",(s->undoTrack>=0&&s->undoCount>0&&!atomic_load(&s->undoReq))?s->undoTrack+1:0);
    if(strcmp(key,"armed")==0){ int p=0;
        for(int i=0;i<NUM_VOICES&&p<buf_len-1;i++) buf[p++]=(char)('0'+(s->voice[i].armed?1:0));
        buf[p]='\0'; return p; }
    if(strcmp(key,"states")==0){ int p=0;
        for(int i=0;i<NUM_VOICES&&p<buf_len-1;i++) buf[p++]=(char)('0'+(int)s->voice[i].state);
        buf[p]='\0'; return p; }
    if(strcmp(key,"mutes")==0){ int p=0;
        for(int i=0;i<NUM_VOICES&&p<buf_len-1;i++) buf[p++]=(char)('0'+(s->voice[i].muted?1:0));
        buf[p]='\0'; return p; }

    /* Sound generators MUST return ui_hierarchy from get_param */
    if(strcmp(key,"ui_hierarchy")==0){int len=(int)strlen(UI_HIERARCHY_JSON);if(len>=buf_len)return -1;
        memcpy(buf,UI_HIERARCHY_JSON,len+1);return len;}
    if(strcmp(key,"chain_params")==0){int len=(int)strlen(CHAIN_PARAMS_JSON);if(len>=buf_len)return -1;
        memcpy(buf,CHAIN_PARAMS_JSON,len+1);return len;}
    if(strcmp(key,"name")==0)return snprintf(buf,buf_len,"Loopex");

    GETP("globalSat",globalSat) GETP("masterComp",masterComp)
    if(strcmp(key,"masterLoCut")==0)return snprintf(buf,buf_len,"%d",(int)s->masterLoCut);
    if(strcmp(key,"masterHiCut")==0)return snprintf(buf,buf_len,"%d",(int)s->masterHiCut);
    GETP("masterVol",masterVol)
    GETE("preamp",preamp,preamp_opts,NUM_PREAMP); GETE("overdubMode",overdubMode,odmode_opts,3); GETE("inSource",inSource,insrc_opts,10); GETE("loopFiltMode",loopFilterMode,mfmode_opts,MF_NVOICE);
    GETP("stability",stability) GETP("globalWowFlut",globalWowFlut) GETP("inputMonitor",inputMonitor) GETP("inputGain",inputGain)
    GETP("inLow",inLow) GETP("inMid",inMid) GETP("inMidFreq",inMidFreq) GETP("inHigh",inHigh) GETP("inHighFreq",inHighFreq)
    GETP("tapeNoise",tapeNoise) GETP("tapeDrive",tapeDrive) GETP("tapeHF",tapeHF)
    if(strcmp(key,"midiIn")==0)return snprintf(buf,buf_len,"%s",s->midiIn?"On":"Off");
    if(strcmp(key,"midiOut")==0)return snprintf(buf,buf_len,"%s",s->midiOut?"On":"Off");
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
    GETVP("v_sat",saturation) GETVP("v_wowflut",wowFlutter)
    GETVP("v_sendA",send) GETVP("v_sendB",sendB) GETVP("v_scatter",scatter)
    GETVP("v_glitch",glitch) GETVP("v_tilt",tiltEQ)
    GETVP("v_eqBass",eqBass) GETVP("v_eqPresFrq",eqPresFreq) GETVP("v_eqPresAmt",eqPresAmt)
    GETVP("v_eqTreble",eqTreble) GETVP("v_pitch",pitch) GETVP("v_filter",filter)
    GETVP("v_pan",pan) GETVP("v_volume",volume) GETVP("v_decay",decay)
    GETVP("v_hvol2",hVol[1]) GETVP("v_hvol3",hVol[2]) GETVP("v_hvol4",hVol[3])
    GETVP("v_hpan2",hPan[1]) GETVP("v_hpan3",hPan[2]) GETVP("v_hpan4",hPan[3])
    GETVP("v_djReso",djReso) GETVP("v_atk",ampAtk) GETVP("v_rel",ampRel) GETVP("v_comp",comp) GETVP("v_clock",clock)
    if(strncmp(key,"v_ph",4)==0&&key[4]>='1'&&key[4]<='4'){
        int hi=key[4]-'1'; const char *sub=key+5;
        static const char *mo[]={"Off","Fwd","Bwd","Ping","Jump"};
        if(strcmp(sub,"mode")==0){ int m=v->ph[hi].mode; if(m<0||m>4)m=0; return snprintf(buf,buf_len,"%s",mo[m]); }
        if(strcmp(sub,"spd")==0) return snprintf(buf,buf_len,"%.4f",(double)v->ph[hi].spd); }

    if(strcmp(key,"v_state")==0){static const char *sts[]={"Empty","Rec","Play","Pause","Odub"};
        int st=(int)v->state;if(st<0||st>4)st=0;return snprintf(buf,buf_len,"%s",sts[st]);}
    if(strcmp(key,"v_loopLen")==0)return snprintf(buf,buf_len,"%.1f",(double)v->loopLen/SR);

    /* State serialization: dump all global + all 16 voices' params */
    if(strcmp(key,"state")==0){int p=0; int trunc=0;
        /* Truncation-aware APP for the state blob: a state that doesn't fit the
         * caller's buffer must fail with -1 (host stores nothing) rather than
         * hand back a truncated blob that restores half the voices. */
        #undef APP
        #define APP(...) do{ int _r=buf_len-p; if(_r>0){ int _n=snprintf(buf+p,(size_t)_r,__VA_ARGS__); if(_n<0){trunc=1;} else if(_n>=_r){p=buf_len;trunc=1;} else p+=_n; } else trunc=1; }while(0)
        #define WF(k,val) APP("%s=%.4f\n",k,(double)(val))
        #define WI(k,val) APP("%s=%d\n",k,(int)(val))
        WF("globalSat",s->globalSat);WF("masterComp",s->masterComp);
        WI("masterLoCut",(int)s->masterLoCut);WI("masterHiCut",(int)s->masterHiCut);
        WI("preamp",(int)s->preamp);WI("overdubMode",(int)s->overdubMode);
        WF("stability",s->stability);WF("globalWowFlut",s->globalWowFlut);WF("inputMonitor",s->inputMonitor);WF("inputGain",s->inputGain);
        WI("selTrack",s->selTrack);
        /* Global FX send buses (by effect NAME so ids stay stable across versions) */
        APP("sendAType=%s\nsendBType=%s\n",pfx_name(s->sendAType),pfx_name(s->sendBType));
        WF("sendAM1",s->sendAM1);WF("sendAM2",s->sendAM2);WF("sendADrift",s->sendADrift);
        WF("sendBM1",s->sendBM1);WF("sendBM2",s->sendBM2);WF("sendBDrift",s->sendBDrift);
        /* Perform */
        WF("stMix",s->stMix);WF("stStep",s->stStep);WF("stOdds",s->stOdds);
        WF("stSize",s->stSize);WF("stReach",s->stReach);WI("stKind",s->stKind);WF("dropAmt",s->dropAmt);
        WF("mfCut",s->mfCut);WF("mfReso",s->mfReso);WI("mfMode",s->mfMode);
        WF("mClock",s->mClock);WI("mClockMode",s->mClockMode);WI("mClockSpot",s->mClockSpot);
        WF("perfTrem",s->perfTrem);WF("perfTremRate",s->perfTremRate);WF("punchWidth",s->punchWidth);
        WI("masterEQ",s->masterEQ);WF("masterGlue",s->masterGlue);WF("tapeLimit",s->tapeLimit);
        for(int pi=0;pi<NUM_PUNCH;pi++) APP("pfl%d=%.4f,%.4f,%.4f,%.4f\n",pi,
            (double)s->punchLfo[pi][0],(double)s->punchLfo[pi][1],(double)s->punchLfo[pi][2],(double)s->punchLfo[pi][3]);
        /* Input / Tape chain */
        WF("inLow",s->inLow);WF("inMid",s->inMid);WF("inMidFreq",s->inMidFreq);
        WF("inHigh",s->inHigh);WF("inHighFreq",s->inHighFreq);
        WF("tapeNoise",s->tapeNoise);WF("tapeDrive",s->tapeDrive);WF("tapeHF",s->tapeHF);
        WI("midiIn",s->midiIn);WI("midiOut",s->midiOut);WF("armThresh",s->armThresh);WF("tapeLoCut",s->tapeLoCut);WF("tapeWow",s->tapeWow);WF("tapeFlut",s->tapeFlut);WF("tapeGen",s->tapeGen);
        /* Master + keyboard */
        WF("masterVol",s->masterVol);WI("rootNote",s->rootNote);WI("inSource",s->inSource);WI("loopFiltMode",s->loopFilterMode);
        WF("driftAmt",s->driftAmt);WF("driftRate",s->driftRate);WF("driftSize",s->driftSize);WF("driftFb",s->driftFb);
        WF("driftSupr",s->driftSupr);WF("driftBlur",s->driftBlur);WF("driftDamp",s->driftDamp);WF("driftMix",s->driftMix);
        /* FX sequencer */
        APP("fxseq=%d,%d,%d,%.3f,%.3f,%d\n",s->fx.run,s->fx.speed,s->fx.len,(double)s->fx.gate,(double)s->fx.swing,s->fx.dir);
        for(int i=0;i<FXSEQ_STEPS;i++){ const FxStep *st=&s->fx.st[i]; if(!st->n&&!st->ext)continue;
            APP("fxs%d=%d,%d,%d",i,st->n,st->ext,st->chance);
            for(int k=0;k<st->n&&k<FXSEQ_MAXPADS;k++) APP(";%d:%.3f,%.3f,%.3f,%.3f,%.3f",st->pad[k],(double)st->lock[k][0],(double)st->lock[k][1],(double)st->lock[k][2],(double)st->lock[k][3],(double)st->press[k]);
            APP("\n"); }
        /* Punch-in FX per-effect params */
        for(int pi=0;pi<NUM_PUNCH;pi++)
            APP("pfx%d=%.4f,%.4f,%.4f,%.4f\n",pi,
                (double)s->punchParams[pi][0],(double)s->punchParams[pi][1],
                (double)s->punchParams[pi][2],(double)s->punchParams[pi][3]);
        for(int i=0;i<NUM_VOICES;i++){Voice *vi=&s->voice[i];
            APP("v%d.srt=%.4f\nv%d.end=%.4f\nv%d.rev=%.4f\n",i,(double)vi->loopStart,i,(double)vi->loopEnd,i,(double)vi->reverse);
            APP("v%d.sat=%.4f\nv%d.wf=%.4f\nv%d.snd=%.4f\n",i,(double)vi->saturation,i,(double)vi->wowFlutter,i,(double)vi->send);
            APP("v%d.gli=%.4f\nv%d.tlt=%.4f\n",i,(double)vi->glitch,i,(double)vi->tiltEQ);
            APP("v%d.eB=%.4f\nv%d.ePF=%.4f\nv%d.ePA=%.4f\nv%d.eT=%.4f\n",i,(double)vi->eqBass,i,(double)vi->eqPresFreq,i,(double)vi->eqPresAmt,i,(double)vi->eqTreble);
            APP("v%d.pit=%.4f\nv%d.fil=%.4f\nv%d.pan=%.4f\nv%d.vol=%.4f\nv%d.dec=%.4f\n",
                i,(double)vi->pitch,i,(double)vi->filter,i,(double)vi->pan,i,(double)vi->volume,i,(double)vi->decay);
            APP("v%d.rso=%.4f\nv%d.atk=%.4f\nv%d.rel=%.4f\n",i,(double)vi->djReso,i,(double)vi->ampAtk,i,(double)vi->ampRel);
            APP("v%d.cmp=%.4f\nv%d.pit2=%.4f\n",i,(double)vi->comp,i,(double)vi->clock);
            APP("v%d.sdB=%.4f\nv%d.sct=%.4f\n",i,(double)vi->sendB,i,(double)vi->scatter);
            APP("v%d.ph=%d,%.4f,%d,%.4f,%d,%.4f,%d,%.4f\n",i,vi->ph[0].mode,(double)vi->ph[0].spd,vi->ph[1].mode,(double)vi->ph[1].spd,vi->ph[2].mode,(double)vi->ph[2].spd,vi->ph[3].mode,(double)vi->ph[3].spd);
            APP("v%d.hvp=%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n",i,(double)vi->hVol[1],(double)vi->hPan[1],(double)vi->hVol[2],(double)vi->hPan[2],(double)vi->hVol[3],(double)vi->hPan[3]);}
        #undef WF
        #undef WI
        return trunc?-1:p;}

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
