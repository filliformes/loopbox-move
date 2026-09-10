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
#include "plugin_api_v1.h"

static const host_api_v1_t *g_host = NULL;

#define SR              44100.0
#define TWOPI           (2.0 * M_PI)
#define NUM_VOICES      16
#define LOOP_SECONDS    45
#define LOOP_SAMPLES    ((int)(SR * LOOP_SECONDS))
#define FLUTTER_BUF     1024
#define DELAY_BUF       88200
#define GLITCH_BUF      44100
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
static inline double clock_to_speed(float clock) { return 0.25*pow(8.0,(double)clock); }
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
    float saturation, wowFlutter, send, glitch, tiltEQ;
    float eqBass, eqPresFreq, eqPresAmt, eqTreble;
    float pitch, filter, pan, volume, decay;
    Biquad djLpA, djLpB, djLpC, djHpA, djHpB, djHpC;
    Biquad eqLow, eqMid, eqHigh, tiltLo, tiltHi;
    double flutBufL[FLUTTER_BUF], flutBufR[FLUTTER_BUF];
    int flutWr; double flutSweep, flutNextMax;
    double glitchBufL[GLITCH_BUF], glitchBufR[GLITCH_BUF];
    int glitchPos, glitchSliceLen, glitchActive;
    double playPhase, stabLpStateL, stabLpStateR;
    uint32_t rng, lastPadFrame;
    int smpDir, smpFile;
} Voice;

typedef struct {
    double eA[PRV_EA+5],eB[PRV_EB+5],eC[PRV_EC+5],eD[PRV_ED+5],eE[PRV_EE+5],eF[PRV_EF+5];
    int ceA,ceB,ceC,ceD,ceE,ceF;
    double aA[PRV_A+5],aB[PRV_B+5],aC[PRV_C+5],aD[PRV_D+5],aE[PRV_E+5],aF[PRV_F+5];
    double aG[PRV_G+5],aH[PRV_H+5],aI[PRV_I+5],aJ[PRV_J+5],aK[PRV_K+5],aL[PRV_L+5];
    int cA,cB,cC,cD,cE,cF,cG,cH,cI,cJ,cK,cL;
    double pre[PRV_PRE+5];int cPre;double fbA,fbB,fbC,fbD,prevA,prevB,prevC,prevD,iirA,iirB;
} PlateReverb;

typedef struct {
    double bufL[DELAY_BUF],bufR[DELAY_BUF];
    double delayL,delayR,prevSampleL,prevSampleR,sweepL,sweepR;
    Biquad regenFilter;
} TapeDelay;

typedef struct {
    float globalSat,masterComp,masterLoCut,masterHiCut,clock;
    float delayRate,delayFeedback,delayTone,delayMix;
    float rvbTime,rvbSize,rvbDamp,rvbMix;
    float preamp,overdubMode,stability;int selTrack;
    float globalWowFlut,inputMonitor,inputGain;
    double inputPeakL,inputPeakR;
    Voice voice[NUM_VOICES];
    TapeDelay delay;PlateReverb rvb;Biquad masterLo,masterHi;
    double compEnvL,compEnvR,casLpL,casLpR,clockHoldL,clockHoldR;int clockCounter;uint32_t rng;
    double gFlutBufL[FLUTTER_BUF],gFlutBufR[FLUTTER_BUF];int gFlutWr;double gFlutSweep,gFlutNextMax;
    uint32_t frameClock;
    struct { char dirs[SMP_MAX_DIRS][SMP_NAME_LEN];char dirPaths[SMP_MAX_DIRS][SMP_PATH_LEN];int numDirs;
             char files[SMP_MAX_FILES][SMP_NAME_LEN];char filePaths[SMP_MAX_FILES][SMP_PATH_LEN];
             int numFiles,curDirIdx; } browser;
} loopbox_t;

/* ---- DJ Filter (Isolator3 cascaded biquads) ---- */
static void dj_filter_update(Voice *v) {
    double f=v->filter;
    double lpF=(f<=0.5)?500.0+(f*2.0)*19500.0:20000.0;
    double hpF=(f>=0.5)?20.0+((f-0.5)*2.0)*7980.0:20.0;
    if(lpF>20000.0)lpF=20000.0;if(hpF<20.0)hpF=20.0;
    bq_set_lp(&v->djLpA,lpF,4.46570214);bq_set_lp(&v->djLpB,lpF,0.70710678);bq_set_lp(&v->djLpC,lpF,0.50316379);
    bq_set_hp(&v->djHpA,hpF,4.46570214);bq_set_hp(&v->djHpB,hpF,0.70710678);bq_set_hp(&v->djHpC,hpF,0.50316379);
}
static inline void dj_filter_stereo(Voice *v, double *l, double *r) {
    *l=bq_L(&v->djLpA,*l);*l=bq_L(&v->djLpB,*l);*l=bq_L(&v->djLpC,*l);
    *l=bq_L(&v->djHpA,*l);*l=bq_L(&v->djHpB,*l);*l=bq_L(&v->djHpC,*l);
    *r=bq_R(&v->djLpA,*r);*r=bq_R(&v->djLpB,*r);*r=bq_R(&v->djLpC,*r);
    *r=bq_R(&v->djHpA,*r);*r=bq_R(&v->djHpB,*r);*r=bq_R(&v->djHpC,*r);
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
    v->state=VS_EMPTY;v->loopLen=0;v->recHead=0;v->playHead=0;v->playPhase=0.0;
    v->glitchActive=0;v->glitchPos=0;v->stabLpStateL=0.0;v->stabLpStateR=0.0;
}

/* ---- Stutter/Beat-Repeat (stereo) ---- */
static inline void voice_stutter_stereo(Voice *v, double *l, double *r, double amt) {
    if(amt<0.05){v->glitchActive=0;return;}
    int baseLen=(v->loopLen>0)?v->loopLen:(int)(SR*2.0);
    int div;
    if(amt<0.20)div=1;else if(amt<0.35)div=2;else if(amt<0.50)div=4;
    else if(amt<0.65)div=8;else if(amt<0.80)div=16;else div=32;
    int sliceLen=baseLen/div;
    if(sliceLen<256)sliceLen=256;if(sliceLen>GLITCH_BUF)sliceLen=GLITCH_BUF;
    /* Detect slice change: re-capture */
    if(!v->glitchActive||v->glitchSliceLen!=sliceLen){v->glitchSliceLen=sliceLen;v->glitchActive=2;v->glitchPos=0;}
    if(v->glitchActive==2){
        /* Capturing phase: record into buffer, pass through */
        v->glitchBufL[v->glitchPos]=*l;v->glitchBufR[v->glitchPos]=*r;
        v->glitchPos++;if(v->glitchPos>=sliceLen){v->glitchActive=1;v->glitchPos=0;}
        return;}
    /* Repeating phase: output captured slice */
    *l=v->glitchBufL[v->glitchPos];*r=v->glitchBufR[v->glitchPos];
    v->glitchPos++;if(v->glitchPos>=sliceLen)v->glitchPos=0;
}

/* ---- Tape Delay (TapeDelay2-inspired, 100% wet) ---- */
static void delay_init(TapeDelay *d){memset(d,0,sizeof(TapeDelay));d->delayL=d->delayR=DELAY_BUF/2.0;d->sweepR=2.1;bq_reset(&d->regenFilter);}
static void delay_process(TapeDelay *d, double inL, double inR, double *outL, double *outR, double rate, double feedback, double tone) {
    double baseSpeed=(rate*rate*24.0)+1.0;
    double bpFreq=lb_clampd(0.05+tone*0.45,0.001,0.499)*SR*0.5;
    bq_set_bp(&d->regenFilter,bpFreq,0.707+tone*0.5);
    double vibSpeed=rate*rate*0.005;
    double speedL=baseSpeed+baseSpeed*0.1*sin(d->sweepL),speedR=baseSpeed+baseSpeed*0.1*sin(d->sweepR);
    d->sweepL+=vibSpeed*inL*inL*0.5+0.001;if(d->sweepL>TWOPI)d->sweepL-=TWOPI;
    d->sweepR+=vibSpeed*inR*inR*0.5+0.001;if(d->sweepR>TWOPI)d->sweepR-=TWOPI;
    int posL=(int)floor(d->delayL);double newL=inL+d->bufL[posL%DELAY_BUF]*feedback;newL=bq_L(&d->regenFilter,newL);
    d->delayL-=speedL;if(d->delayL<0)d->delayL+=DELAY_BUF;
    double incL=(speedL>0.001)?(newL-d->prevSampleL)/speedL:0.0;int p=posL;
    while(p!=(int)floor(d->delayL)){d->bufL[p%DELAY_BUF]=d->prevSampleL;d->prevSampleL+=incL;p--;if(p<0)p+=DELAY_BUF;}
    d->prevSampleL=newL;*outL=d->bufL[(int)floor(d->delayL)%DELAY_BUF];
    int posR=(int)floor(d->delayR);double newR=inR+d->bufR[posR%DELAY_BUF]*feedback;newR=bq_R(&d->regenFilter,newR);
    d->delayR-=speedR;if(d->delayR<0)d->delayR+=DELAY_BUF;
    double incR=(speedR>0.001)?(newR-d->prevSampleR)/speedR:0.0;p=posR;
    while(p!=(int)floor(d->delayR)){d->bufR[p%DELAY_BUF]=d->prevSampleR;d->prevSampleR+=incR;p--;if(p<0)p+=DELAY_BUF;}
    d->prevSampleR=newR;*outR=d->bufR[(int)floor(d->delayR)%DELAY_BUF];
}

/* ---- Plate Reverb (Dattorro kPlateA-inspired) ---- */
static void reverb_init(PlateReverb *r){memset(r,0,sizeof(PlateReverb));}
static void reverb_process(PlateReverb *r, double inL, double inR, double *outL, double *outR, double rvTime, double rvSize, double rvDamp) {
    double input=(inL+inR)*0.5;
    double feedback=0.35+rvTime*0.55;double predelay=rvSize*(double)PRV_PRE*0.8;
    r->pre[r->cPre]=input;r->cPre++;if(r->cPre>=PRV_PRE)r->cPre=0;
    int preRead=r->cPre-(int)predelay;if(preRead<0)preRead+=PRV_PRE;input=r->pre[preRead];
    r->iirA=r->iirA*(0.5+rvSize*0.4)+input*(0.5-rvSize*0.4);input=r->iirA;
    r->eA[r->ceA]=input;r->ceA++;if(r->ceA>=PRV_EA)r->ceA=0;
    r->eB[r->ceB]=input;r->ceB++;if(r->ceB>=PRV_EB)r->ceB=0;
    r->eC[r->ceC]=input;r->ceC++;if(r->ceC>=PRV_EC)r->ceC=0;
    r->eD[r->ceD]=input;r->ceD++;if(r->ceD>=PRV_ED)r->ceD=0;
    r->eE[r->ceE]=input;r->ceE++;if(r->ceE>=PRV_EE)r->ceE=0;
    r->eF[r->ceF]=input;r->ceF++;if(r->ceF>=PRV_EF)r->ceF=0;
    double earlyL=r->eA[(r->ceA-PRV_EA+PRV_EA*2)%PRV_EA]*0.17+r->eC[(r->ceC-PRV_EC+PRV_EC*2)%PRV_EC]*0.13+r->eE[(r->ceE-PRV_EE+PRV_EE*2)%PRV_EE]*0.11;
    double earlyR=r->eB[(r->ceB-PRV_EB+PRV_EB*2)%PRV_EB]*0.15+r->eD[(r->ceD-PRV_ED+PRV_ED*2)%PRV_ED]*0.14+r->eF[(r->ceF-PRV_EF+PRV_EF*2)%PRV_EF]*0.12;
    double tankIn=(earlyL+earlyR)*0.5+r->fbA*feedback;
    r->aA[r->cA]=tankIn+r->fbD*feedback*0.3;r->cA++;if(r->cA>=PRV_A)r->cA=0;r->prevA=r->aA[r->cA%PRV_A];
    r->aB[r->cB]=r->prevA;r->cB++;if(r->cB>=PRV_B)r->cB=0;
    r->aC[r->cC]=r->aB[r->cC%PRV_B]+r->fbB*feedback*0.2;r->cC++;if(r->cC>=PRV_C)r->cC=0;
    r->aD[r->cD]=r->aC[r->cC%PRV_C];r->cD++;if(r->cD>=PRV_D)r->cD=0;r->prevB=r->aD[r->cD%PRV_D];
    r->aE[r->cE]=r->prevB+r->fbC*feedback*0.2;r->cE++;if(r->cE>=PRV_E)r->cE=0;
    r->aF[r->cF]=r->aE[r->cE%PRV_E];r->cF++;if(r->cF>=PRV_F)r->cF=0;
    r->aG[r->cG]=r->aF[r->cF%PRV_F];r->cG++;if(r->cG>=PRV_G)r->cG=0;r->prevC=r->aG[r->cG%PRV_G];
    r->aH[r->cH]=r->prevC;r->cH++;if(r->cH>=PRV_H)r->cH=0;
    r->aI[r->cI]=r->aH[r->cH%PRV_H]+r->fbD*feedback*0.15;r->cI++;if(r->cI>=PRV_I)r->cI=0;
    r->aJ[r->cJ]=r->aI[r->cI%PRV_I];r->cJ++;if(r->cJ>=PRV_J)r->cJ=0;r->prevD=r->aJ[r->cJ%PRV_J];
    r->aK[r->cK]=r->prevD;r->cK++;if(r->cK>=PRV_K)r->cK=0;
    r->aL[r->cL]=r->aK[r->cK%PRV_K];r->cL++;if(r->cL>=PRV_L)r->cL=0;
    r->fbA=r->prevA*0.5;r->fbB=r->prevB*0.5;r->fbC=r->prevC*0.5;r->fbD=r->prevD*0.5;
    double dampK=1.0-rvDamp*0.7;
    r->fbA*=dampK;r->fbB*=dampK;r->fbC*=dampK;r->fbD*=dampK;
    r->iirB=r->iirB*(0.6+rvTime*0.35)+r->fbA*(0.4-rvTime*0.35);r->fbA=r->iirB;
    *outL=earlyL*0.4+r->prevA*0.25+r->prevC*0.2+r->aL[r->cL%PRV_L]*0.15;
    *outR=earlyR*0.4+r->prevB*0.25+r->prevD*0.2+r->aK[r->cK%PRV_K]*0.15;
}

/* ---- Master Compressor (Logical4-inspired) ---- */
static inline void master_comp(double *l, double *r, double amt, double *envL, double *envR) {
    if(amt<0.005)return;double det=fmax(fabs(*l),fabs(*r));
    double atkC=exp(-1.0/(SR*0.003)),relC=exp(-1.0/(SR*0.15));
    double env=fmax(*envL,*envR);env=(det>env)?atkC*env+(1.0-atkC)*det:relC*env+(1.0-relC)*det;
    *envL=*envR=env;double thDb=-6.0-amt*18.0,db=20.0*log10(env+1e-12);
    double ratio=2.0+amt*6.0;
    if(db>thDb){double gr=(db-thDb)*(1.0-1.0/ratio);
        double gain=pow(10.0,-gr/20.0);*l*=gain;*r*=gain;}
    /* Makeup gain: compensate for gain reduction */
    double makeupDb=(0.0-thDb)*(1.0-1.0/ratio)*0.5;
    double makeup=pow(10.0,makeupDb/20.0);*l*=makeup;*r*=makeup;
}

/* ---- Voice Render (stereo) ---- */
static void voice_render(Voice *v, loopbox_t *s, double *outL, double *outR, double *sendL, double *sendR, double clockSpeed) {
    *outL=*outR=*sendL=*sendR=0.0;
    if(v->state!=VS_PLAYING&&v->state!=VS_OVERDUBBING)return;
    if(v->loopLen<=0||!v->bufferL||!v->bufferR)return;
    int effStart=(int)(v->loopStart*(float)v->loopLen);int effEnd=(int)(v->loopEnd*(float)v->loopLen);
    if(effEnd<=effStart)effEnd=effStart+1;if(effEnd>v->loopLen)effEnd=v->loopLen;
    int effLen=effEnd-effStart;if(effLen<1)effLen=1;
    double rate=pow(2.0,(double)v->pitch)*clockSpeed;if(v->reverse>0.5f)rate=-rate;
    double relPhase=v->playPhase-(double)effStart;
    while(relPhase<0)relPhase+=(double)effLen;while(relPhase>=(double)effLen)relPhase-=(double)effLen;
    double absPhase=(double)effStart+relPhase;
    int i0=(int)absPhase%v->loopLen;int i1=(i0+1)%v->loopLen;double frac=absPhase-floor(absPhase);
    double rawL=((double)v->bufferL[i0]*(1.0-frac)+(double)v->bufferL[i1]*frac)/32768.0;
    double rawR=((double)v->bufferR[i0]*(1.0-frac)+(double)v->bufferR[i1]*frac)/32768.0;
    v->playPhase+=rate;double dEnd=(double)effEnd,dStart=(double)effStart;
    while(v->playPhase>=dEnd)v->playPhase-=(double)effLen;while(v->playPhase<dStart)v->playPhase+=(double)effLen;
    v->playHead=(int)v->playPhase;
    double sL=rawL,sR=rawR;
    dj_filter_stereo(v,&sL,&sR);
    sL=voice_saturate(sL,(double)v->saturation);sR=voice_saturate(sR,(double)v->saturation);
    voice_wowflutter_stereo(v,&sL,&sR,(double)v->wowFlutter);
    voice_stutter_stereo(v,&sL,&sR,(double)v->glitch);
    tilt_eq_stereo(v,&sL,&sR);studer_eq_stereo(v,&sL,&sR);
    if(s->stability>0.005f){sL=apply_stability(sL,(double)s->stability,&v->rng);sR=apply_stability(sR,(double)s->stability,&v->rng);
        double stabK=0.2+(double)s->stability*0.6;
        v->stabLpStateL+=stabK*(sL-v->stabLpStateL);sL=v->stabLpStateL;
        v->stabLpStateR+=stabK*(sR-v->stabLpStateR);sR=v->stabLpStateR;}
    double vol=(double)v->volume,pn=-(double)v->pan;
    double panL=cos((pn+1.0)*0.25*M_PI),panR=sin((pn+1.0)*0.25*M_PI);
    *outL=sL*vol*panL;*outR=sR*vol*panR;double sendAmt=(double)v->send;
    *sendL=(sL+sR)*0.5*vol*sendAmt;*sendR=*sendL;
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
        v->flutNextMax=0.5;v->rng=12345+i*7919;v->smpDir=0;v->smpFile=-1;
        bq_reset(&v->djLpA);bq_reset(&v->djLpB);bq_reset(&v->djLpC);
        bq_reset(&v->djHpA);bq_reset(&v->djHpB);bq_reset(&v->djHpC);
        bq_reset(&v->eqLow);bq_reset(&v->eqMid);bq_reset(&v->eqHigh);
        bq_reset(&v->tiltLo);bq_reset(&v->tiltHi);
        dj_filter_update(v);studer_eq_update(v);tilt_eq_update(v);}
    s->globalSat=0.0f;s->masterComp=0.0f;s->masterLoCut=20.0f;s->masterHiCut=20000.0f;s->clock=0.5f;
    s->delayRate=0.3f;s->delayFeedback=0.35f;s->delayTone=0.5f;s->delayMix=0.5f;
    s->rvbTime=0.4f;s->rvbSize=0.5f;s->rvbDamp=0.3f;s->rvbMix=0.5f;
    s->preamp=0.0f;s->overdubMode=0.0f;s->stability=0.0f;s->selTrack=1;s->rng=42;
    s->globalWowFlut=0.0f;s->inputMonitor=0.0f;s->inputGain=1.0f;s->gFlutNextMax=0.5;s->frameClock=100000;
    bq_reset(&s->masterLo);bq_reset(&s->masterHi);delay_init(&s->delay);reverb_init(&s->rvb);
    smp_scan_dirs(s);if(s->browser.numDirs>0)smp_scan_files(s,0);return s;
}
static void destroy_instance(void *inst){loopbox_t *s=(loopbox_t*)inst;if(!s)return;
    for(int i=0;i<NUM_VOICES;i++){free(s->voice[i].bufferL);free(s->voice[i].bufferR);}free(s);}

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

    if(source==MOVE_MIDI_SOURCE_INTERNAL) {
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

/* ---- Render Block ---- */
static void render_block(void *inst, int16_t *out_interleaved_lr, int frames) {
    loopbox_t *s=(loopbox_t*)inst;s->frameClock+=(uint32_t)frames;
    int16_t *micBuf=NULL;if(g_host&&g_host->mapped_memory)micBuf=(int16_t*)(g_host->mapped_memory+g_host->audio_in_offset);
    int selIdx=s->selTrack-1;
    if(selIdx>=0&&selIdx<NUM_VOICES){Voice *sv=&s->voice[selIdx];dj_filter_update(sv);studer_eq_update(sv);tilt_eq_update(sv);}
    if(s->masterLoCut>21.0f)bq_set_hp(&s->masterLo,(double)s->masterLoCut,0.707);
    if(s->masterHiCut<19999.0f)bq_set_lp(&s->masterHi,(double)s->masterHiCut,0.707);
    double clockSpeed=clock_to_speed(s->clock);int decimFactor=(clockSpeed<0.99)?(int)(1.0/clockSpeed):1;
    if(decimFactor<1)decimFactor=1;if(decimFactor>4)decimFactor=4;
    OverdubMode odMode=(OverdubMode)(int)lb_clampf(s->overdubMode,0.0f,2.0f);

    for(int n=0;n<frames;n++){
        double inL=0.0,inR=0.0;
        if(micBuf){double ig=(double)s->inputGain;inL=(double)micBuf[n*2]/32768.0*ig;inR=(double)micBuf[n*2+1]/32768.0*ig;
            double aL=fabs(inL),aR=fabs(inR);if(aL>s->inputPeakL)s->inputPeakL=aL;if(aR>s->inputPeakR)s->inputPeakR=aR;}
        int preModel=(int)lb_clampf(s->preamp,0.0f,11.0f);
        apply_preamp_sample(&inL,&inR,preModel,&s->casLpL,&s->casLpR,&s->rng);
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

        double mixL=0.0,mixR=0.0,sendBusL=0.0,sendBusR=0.0;
        for(int vi=0;vi<NUM_VOICES;vi++){double vL,vR,sL,sR;
            voice_render(&s->voice[vi],s,&vL,&vR,&sL,&sR,clockSpeed);mixL+=vL;mixR+=vR;sendBusL+=sL;sendBusR+=sR;}
        if(s->inputMonitor>0.005f){double mg=(double)s->inputMonitor;mixL+=inL*mg;mixR+=inR*mg;}

        /* Clock SR decimation + aliasing noise (Mood mk2 style) */
        s->clockCounter++;if(s->clockCounter>=decimFactor){s->clockHoldL=mixL;s->clockHoldR=mixR;s->clockCounter=0;}
        mixL=s->clockHoldL;mixR=s->clockHoldR;
        if(decimFactor>1){double nAmt=(double)(decimFactor-1)*0.008;mixL+=lb_rand(&s->rng)*nAmt;mixR+=lb_rand(&s->rng)*nAmt;}

        double delL=0,delR=0,revL=0,revR=0;
        delay_process(&s->delay,sendBusL,sendBusR,&delL,&delR,(double)s->delayRate,(double)s->delayFeedback,(double)s->delayTone);
        reverb_process(&s->rvb,sendBusL,sendBusR,&revL,&revR,(double)s->rvbTime,(double)s->rvbSize,(double)s->rvbDamp);
        double dlyMx=(double)s->delayMix,rvbMx=(double)s->rvbMix;
        mixL+=delL*dlyMx+revL*rvbMx;mixR+=delR*dlyMx+revR*rvbMx;
        mixL=global_saturate(mixL,(double)s->globalSat);mixR=global_saturate(mixR,(double)s->globalSat);
        master_wowflutter_stereo(s,&mixL,&mixR,(double)s->globalWowFlut);
        master_comp(&mixL,&mixR,(double)s->masterComp,&s->compEnvL,&s->compEnvR);
        if(s->masterLoCut>21.0f){mixL=bq_L(&s->masterLo,mixL);mixR=bq_R(&s->masterLo,mixR);}
        if(s->masterHiCut<19999.0f){mixL=bq_L(&s->masterHi,mixL);mixR=bq_R(&s->masterHi,mixR);}
        mixL=lb_tanh(mixL);mixR=lb_tanh(mixR);
        out_interleaved_lr[n*2]=(int16_t)lb_clampd(mixL*32767.0,-32767.0,32767.0);
        out_interleaved_lr[n*2+1]=(int16_t)lb_clampd(mixR*32767.0,-32767.0,32767.0);
    }
    /* Decay input peak meters (~50ms decay) */
    s->inputPeakL*=0.95;s->inputPeakR*=0.95;
}

/* ---- Parameters ---- */
#define SETFR(k,field,lo,hi) if(strcmp(key,k)==0){s->field=lb_clampf((float)atof(val),(float)(lo),(float)(hi));return;}
#define SETVFR(k,field,lo,hi) if(strcmp(key,k)==0){v->field=lb_clampf((float)atof(val),(float)(lo),(float)(hi));return;}
static const char *preamp_opts[]={"Clean","Cass1","Cass2","VHS1","VHS2","Reel15","Reel7","Reel3","4trk","Porta","Dub","Warp"};
static const char *odmode_opts[]={"Replace","Multiply","Disint"};
static const char *reverse_opts[]={"Normal","Reverse"};
static int match_enum(const char *value, const char **opts, int count){for(int i=0;i<count;i++)if(strcmp(value,opts[i])==0)return i;return -1;}

static void set_param(void *inst, const char *key, const char *val) {
    loopbox_t *s=(loopbox_t*)inst;if(!key||!val)return;
    SETFR("globalSat",globalSat,0.0,1.0) SETFR("masterComp",masterComp,0.0,1.0)
    SETFR("masterLoCut",masterLoCut,20.0,500.0) SETFR("masterHiCut",masterHiCut,1000.0,20000.0)
    SETFR("clock",clock,0.0,1.0)
    SETFR("delayRate",delayRate,0.01,1.0) SETFR("delayFeedback",delayFeedback,0.0,0.95)
    SETFR("delayTone",delayTone,0.0,1.0) SETFR("delayMix",delayMix,0.0,1.0)
    SETFR("rvbTime",rvbTime,0.0,1.0) SETFR("rvbSize",rvbSize,0.0,1.0)
    SETFR("rvbDamp",rvbDamp,0.0,1.0) SETFR("rvbMix",rvbMix,0.0,1.0)
    if(strcmp(key,"preamp")==0){int idx=match_enum(val,preamp_opts,12);if(idx>=0)s->preamp=(float)idx;else s->preamp=lb_clampf((float)atof(val),0.0f,11.0f);return;}
    if(strcmp(key,"overdubMode")==0){int idx=match_enum(val,odmode_opts,3);if(idx>=0)s->overdubMode=(float)idx;else s->overdubMode=lb_clampf((float)atof(val),0.0f,2.0f);return;}
    SETFR("stability",stability,0.0,1.0) SETFR("globalWowFlut",globalWowFlut,0.0,1.0) SETFR("inputMonitor",inputMonitor,0.0,1.0) SETFR("inputGain",inputGain,0.0,2.0)
    if(strcmp(key,"selTrack")==0){s->selTrack=(int)lb_clampf((float)atof(val),1.0f,16.0f);
        int si=s->selTrack-1;if(si>=0&&si<NUM_VOICES)smp_scan_files(s,s->voice[si].smpDir);return;}
    if(strcmp(key,"clearSel")==0){float t=(float)atof(val);if(t>0.5f){int ci=s->selTrack-1;
        if(ci>=0&&ci<NUM_VOICES)voice_clear(&s->voice[ci]);}return;}
    if(strcmp(key,"clearAll")==0){float t=(float)atof(val);if(t>0.5f){for(int ci=0;ci<NUM_VOICES;ci++)voice_clear(&s->voice[ci]);}return;}
    int selIdx=s->selTrack-1;if(selIdx<0||selIdx>=NUM_VOICES)return;Voice *v=&s->voice[selIdx];
    SETVFR("v_start",loopStart,0.0,1.0) SETVFR("v_end",loopEnd,0.0,1.0)
    if(strcmp(key,"v_reverse")==0){int idx=match_enum(val,reverse_opts,2);if(idx>=0)v->reverse=(float)idx;else v->reverse=lb_clampf((float)atof(val),0.0f,1.0f);return;}
    SETVFR("v_sat",saturation,0.0,1.0) SETVFR("v_wowflut",wowFlutter,0.0,1.0)
    SETVFR("v_send",send,0.0,1.0) SETVFR("v_glitch",glitch,0.0,1.0) SETVFR("v_tilt",tiltEQ,-1.0,1.0)
    SETVFR("v_eqBass",eqBass,-1.0,1.0) SETVFR("v_eqPresFrq",eqPresFreq,0.0,1.0)
    SETVFR("v_eqPresAmt",eqPresAmt,-1.0,1.0) SETVFR("v_eqTreble",eqTreble,-1.0,1.0)
    SETVFR("v_pitch",pitch,-2.0,2.0) SETVFR("v_filter",filter,0.0,1.0)
    SETVFR("v_pan",pan,-1.0,1.0) SETVFR("v_volume",volume,0.0,1.0) SETVFR("v_decay",decay,0.0,1.0)
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
    "\"knobs\":[\"delayRate\",\"delayFeedback\",\"delayTone\",\"delayMix\",\"rvbTime\",\"rvbSize\",\"rvbDamp\",\"rvbMix\"],"
    "\"params\":[\"delayRate\",\"delayFeedback\",\"delayTone\",\"delayMix\",\"rvbTime\",\"rvbSize\",\"rvbDamp\",\"rvbMix\"]},"
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
    "{\"key\":\"delayRate\",\"name\":\"DlyRt\",\"type\":\"float\",\"min\":0.01,\"max\":1,\"step\":0.01},"
    "{\"key\":\"delayFeedback\",\"name\":\"DlyFb\",\"type\":\"float\",\"min\":0,\"max\":0.95,\"step\":0.01},"
    "{\"key\":\"delayTone\",\"name\":\"DlyTn\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"delayMix\",\"name\":\"DlyMx\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"rvbTime\",\"name\":\"RvTim\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"rvbSize\",\"name\":\"RvSiz\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"rvbDamp\",\"name\":\"RvDmp\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"rvbMix\",\"name\":\"RvMix\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
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
    "{\"key\":\"v_glitch\",\"name\":\"Stutter\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
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
    "{\"key\":\"v_smpDir\",\"name\":\"SmpDir\",\"type\":\"enum\",\"options\":[\"0\",\"1\",\"2\",\"3\",\"4\",\"5\",\"6\",\"7\",\"8\",\"9\",\"10\",\"11\",\"12\",\"13\",\"14\",\"15\",\"16\",\"17\",\"18\",\"19\",\"20\",\"21\",\"22\",\"23\",\"24\",\"25\",\"26\",\"27\",\"28\",\"29\",\"30\",\"31\"]},"
    "{\"key\":\"v_smpFile\",\"name\":\"Sample\",\"type\":\"enum\",\"options\":[\"-1\",\"0\",\"1\",\"2\",\"3\",\"4\",\"5\",\"6\",\"7\",\"8\",\"9\",\"10\",\"11\",\"12\",\"13\",\"14\",\"15\",\"16\",\"17\",\"18\",\"19\",\"20\",\"21\",\"22\",\"23\",\"24\",\"25\",\"26\",\"27\",\"28\",\"29\",\"30\",\"31\",\"32\",\"33\",\"34\",\"35\",\"36\",\"37\",\"38\",\"39\",\"40\",\"41\",\"42\",\"43\",\"44\",\"45\",\"46\",\"47\",\"48\",\"49\",\"50\",\"51\",\"52\",\"53\",\"54\",\"55\",\"56\",\"57\",\"58\",\"59\",\"60\",\"61\",\"62\",\"63\"]}]";

static int get_param(void *inst, const char *key, char *buf, int buf_len) {
    loopbox_t *s=(loopbox_t*)inst;if(!key)return -1;

    /* Sound generators MUST return ui_hierarchy from get_param */
    if(strcmp(key,"ui_hierarchy")==0){int len=(int)strlen(UI_HIERARCHY_JSON);if(len>=buf_len)return -1;
        memcpy(buf,UI_HIERARCHY_JSON,len+1);return len;}
    if(strcmp(key,"chain_params")==0){int len=(int)strlen(CHAIN_PARAMS_JSON);if(len>=buf_len)return -1;
        memcpy(buf,CHAIN_PARAMS_JSON,len+1);return len;}
    if(strcmp(key,"name")==0)return snprintf(buf,buf_len,"LoopBox");

    GETP("globalSat",globalSat) GETP("masterComp",masterComp)
    if(strcmp(key,"masterLoCut")==0)return snprintf(buf,buf_len,"%d",(int)s->masterLoCut);
    if(strcmp(key,"masterHiCut")==0)return snprintf(buf,buf_len,"%d",(int)s->masterHiCut);
    GETP("clock",clock) GETP("delayRate",delayRate) GETP("delayFeedback",delayFeedback)
    GETP("delayTone",delayTone) GETP("delayMix",delayMix)
    GETP("rvbTime",rvbTime) GETP("rvbSize",rvbSize) GETP("rvbDamp",rvbDamp) GETP("rvbMix",rvbMix)
    GETE("preamp",preamp,preamp_opts,12); GETE("overdubMode",overdubMode,odmode_opts,3);
    GETP("stability",stability) GETP("globalWowFlut",globalWowFlut) GETP("inputMonitor",inputMonitor) GETP("inputGain",inputGain)
    if(strcmp(key,"inputPeak")==0)return snprintf(buf,buf_len,"%.3f",fmax(s->inputPeakL,s->inputPeakR));
    if(strcmp(key,"selTrack")==0)return snprintf(buf,buf_len,"%d",s->selTrack);
    if(strcmp(key,"clearSel")==0||strcmp(key,"clearAll")==0)return snprintf(buf,buf_len,"0");

    int selIdx=s->selTrack-1;if(selIdx<0||selIdx>=NUM_VOICES)return -1;Voice *v=&s->voice[selIdx];
    GETVP("v_start",loopStart) GETVP("v_end",loopEnd)
    if(strcmp(key,"v_reverse")==0){int _i=(int)roundf(v->reverse);if(_i<0)_i=0;if(_i>1)_i=1;return snprintf(buf,buf_len,"%s",reverse_opts[_i]);}
    GETVP("v_sat",saturation) GETVP("v_wowflut",wowFlutter) GETVP("v_send",send)
    GETVP("v_glitch",glitch) GETVP("v_tilt",tiltEQ)
    GETVP("v_eqBass",eqBass) GETVP("v_eqPresFrq",eqPresFreq) GETVP("v_eqPresAmt",eqPresAmt)
    GETVP("v_eqTreble",eqTreble) GETVP("v_pitch",pitch) GETVP("v_filter",filter)
    GETVP("v_pan",pan) GETVP("v_volume",volume) GETVP("v_decay",decay)

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
        WF("clock",s->clock);WF("delayRate",s->delayRate);WF("delayFeedback",s->delayFeedback);
        WF("delayTone",s->delayTone);WF("delayMix",s->delayMix);
        WF("rvbTime",s->rvbTime);WF("rvbSize",s->rvbSize);WF("rvbDamp",s->rvbDamp);WF("rvbMix",s->rvbMix);
        WI("preamp",(int)s->preamp);WI("overdubMode",(int)s->overdubMode);
        WF("stability",s->stability);WF("globalWowFlut",s->globalWowFlut);WF("inputMonitor",s->inputMonitor);WF("inputGain",s->inputGain);
        WI("selTrack",s->selTrack);
        for(int i=0;i<NUM_VOICES;i++){Voice *vi=&s->voice[i];
            p+=snprintf(buf+p,buf_len-p,"v%d.srt=%.4f\nv%d.end=%.4f\nv%d.rev=%.4f\n",i,(double)vi->loopStart,i,(double)vi->loopEnd,i,(double)vi->reverse);
            p+=snprintf(buf+p,buf_len-p,"v%d.sat=%.4f\nv%d.wf=%.4f\nv%d.snd=%.4f\n",i,(double)vi->saturation,i,(double)vi->wowFlutter,i,(double)vi->send);
            p+=snprintf(buf+p,buf_len-p,"v%d.gli=%.4f\nv%d.tlt=%.4f\n",i,(double)vi->glitch,i,(double)vi->tiltEQ);
            p+=snprintf(buf+p,buf_len-p,"v%d.eB=%.4f\nv%d.ePF=%.4f\nv%d.ePA=%.4f\nv%d.eT=%.4f\n",i,(double)vi->eqBass,i,(double)vi->eqPresFreq,i,(double)vi->eqPresAmt,i,(double)vi->eqTreble);
            p+=snprintf(buf+p,buf_len-p,"v%d.pit=%.4f\nv%d.fil=%.4f\nv%d.pan=%.4f\nv%d.vol=%.4f\nv%d.dec=%.4f\n",
                i,(double)vi->pitch,i,(double)vi->filter,i,(double)vi->pan,i,(double)vi->volume,i,(double)vi->decay);}
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
