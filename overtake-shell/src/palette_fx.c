/**
 * palette_fx.c — self-contained, reusable Palette DSP engine (24 effects + Off).
 * Extracted from palette-move/src/dsp/palette.c (host half removed). MIT.
 *
 * The effect half below (helper kernels, slot_dsp_t, all fx_* functions, FX_TABLE,
 * FX_NAMES, is_clouds_fx, warps/clouds externs) is copied VERBATIM from palette.c;
 * every effect-side symbol stays `static`. The public pfx_* API is implemented at
 * the bottom against an opaque pfx_slot that wraps one slot_dsp_t + the two
 * pre-allocated Clouds pools (SPACE/BLOOM), replicating palette.c's create_instance,
 * slot_reset, slot_apply_select clouds dispatch, and process_block per-slot call.
 */
#include "palette_fx.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#define SR        44100.0f
#define MAX_DELAY (44100 * 2)   /* 2 s stereo delay line per slot */

static inline float clampf(float x, float lo, float hi){ return x<lo?lo:(x>hi?hi:x); }
static inline int   clampi(int x, int lo, int hi){ return x<lo?lo:(x>hi?hi:x); }

static inline float frand(uint32_t *s){               /* xorshift32 → [0,1) */
    uint32_t x=*s; x^=x<<13; x^=x>>17; x^=x<<5; *s=x;
    return (x>>8)*(1.0f/16777216.0f);
}

enum {
    PFX_OFF = 0,
    /* Character (group 0) */ PFX_DRIVE, PFX_SWEETEN, PFX_FUZZ, PFX_HOWL, PFX_FOLD, PFX_SWELL,
    /* Movement  (group 1) */ PFX_DOUBLER, PFX_VIBRATO, PFX_PHASER, PFX_TREMOLO, PFX_PITCH, PFX_SHIFT,
    /* Diffusion (group 2) */ PFX_CASCADE, PFX_REELS, PFX_COLLAGE, PFX_REVERSE, PFX_SPACE, PFX_BLOOM,
    /* Texture   (group 3) */ PFX_FILTER, PFX_SQUASH, PFX_CASSETTE, PFX_BROKEN, PFX_INTERFERENCE, PFX_HALO,
    PFX_PLATE,              /* Dattorro plate reverb (own state pool, appended) */
    PFX_QUARTZ, PFX_PRISM,  /* dual-mode Hadamard 8-line FDN (Res / Essaim port, shared pool) */
    PFX_VEIL,               /* modulated Householder FDN (Phasma port, own pool) */
    PFX_COUNT               /* = 29 (Off + 24 + Plate + Quartz + Prism + Veil) */
};

static const char *FX_NAMES[PFX_COUNT] = {
    "Off",
    "Drive","Sweeten","Fuzz","Howl","Fold","Swell",
    "Doubler","Vibrato","Phaser","Tremolo","Pitch","Shift",
    "Cascade","Reels","Collage","Reverse","Space","Bloom",
    "Filter","Squash","Cassette","Broken","Interference","Halo",
    "Plate","Quartz","Prism","Veil"
};

extern void *pfx_clouds_alloc(int fx_id, float sr);
extern void  pfx_clouds_free(void *heavy);
extern void  pfx_clouds_reset(void *heavy);
extern void  pfx_clouds_process(int fx_id, void *heavy, float *l, float *r, int n,
                                float amount, float macro, float drift);

typedef struct {
    /* delay line (Cascade/Reels/Reverse/Collage/Doubler/Vibrato/Pitch/Broken) */
    float *dl_l, *dl_r;       /* MAX_DELAY each */
    int    wp;                /* write pointer */
    /* generic one-pole / cascaded filter states (stereo) */
    float  z1l,z1r, z2l,z2r, z3l,z3r, z4l,z4r;
    /* state-variable filter (Filter / Howl) stereo: lp & bp integrators */
    float  lp_l,bp_l, lp_r,bp_r;
    /* biquad (peaking/shelf) stereo, direct-form I */
    float  bx1l,bx2l,by1l,by2l, bx1r,bx2r,by1r,by2r;
    /* envelope followers */
    float  env, env2;
    /* parameter smoothing companions */
    float  sm1,sm2,sm3,sm4;
    /* LFO phases (0..1) */
    float  lfo,lfo2,lfo3;
    /* allpass chains: Phaser (≤12 stages, 1 state each) stereo */
    float  ap_l[16], ap_r[16], ap2_l[16], ap2_r[16];
    /* SHIFT: Warps QuadratureTransform Hilbert — 17 allpass filters × 2 states,
     * per channel: hil[ch*34 + filt*2 + (0=x,1=y)] */
    float  hil[68];
    /* misc per-effect scratch */
    float  f1,f2,f3,f4,f5,f6;
    int    i1,i2,i3,i4;
    /* HALO resonator bank: 6 voices × per-channel damping one-pole */
    float  halo_lp_l[6], halo_lp_r[6];
    uint32_t seed;            /* per-slot deterministic RNG (drift) */
    /* tempo sync (set per block by the host): >0 = use instead of Macro mapping */
    float  sync_time;         /* synced delay time in samples (0 = free) */
    float  sync_rate;         /* synced LFO rate in Hz (0 = free) */
    /* heavy C++ (Clouds) effect state — lazily alloc'd on select change */
    void  *heavy;
    int    heavy_kind;        /* PFX_* id the heavy ptr currently serves (0 = none) */
    void  *plate;             /* Dattorro plate state (pfx_plate_t*), alloc'd at create */
    void  *fdn;               /* Quartz/Prism FDN state (pfx_fdn_t*), alloc'd at create — one pool, both modes */
    void  *veil;              /* Veil Householder FDN state (pfx_veil_t*), alloc'd at create */
} slot_dsp_t;

typedef void (*fx_process_fn)(slot_dsp_t*, float*, float*, int,
                              float amount, float macro, float drift);
typedef void (*fx_reset_fn)(slot_dsp_t*);

typedef struct {
    fx_process_fn process;
    fx_reset_fn   reset;      /* may be NULL */
} palette_effect_t;

static inline int is_clouds_fx(int id){
    return id==PFX_SPACE || id==PFX_BLOOM;
}

#ifndef TWO_PI
#define TWO_PI 6.28318530718f
#endif
#define DENORM 1e-20f
static inline float lerpf(float a,float b,float t){ return a+(b-a)*t; }
/* fractional delay read, `d` samples behind the next write index `wp` */
static inline float dlr(const float *b,int wp,float d){
    float rp=(float)wp-d;
    while(rp<0.0f) rp+=(float)MAX_DELAY;
    while(rp>=(float)MAX_DELAY) rp-=(float)MAX_DELAY;
    int i0=(int)rp; float fr=rp-(float)i0; int i1=i0+1; if(i1>=MAX_DELAY) i1=0;
    return b[i0]+(b[i1]-b[i0])*fr;
}
/* slow musical random-walk in [-1,1] stored in *st (drift LFO) */
static inline float wander(float *st,uint32_t *seed,float speed){
    *st += speed*((frand(seed)-0.5f)*2.0f - *st);
    return *st;
}

/* ── Sibling kernels (Filliformes, MIT) — extracted verbatim from shipped plugins ──
 * sb_tanh: super-boom-move Padé tanh.  delay_saturate: krautdrums-move feedback
 * saturator w/ DC removal.  tape_cubic/tape_asym: mello-move apply_tape_stage. */
static inline float sb_tanh(float x){                       /* super-boom-move */
    if(x>3.0f) return 1.0f; if(x<-3.0f) return -1.0f;
    float x2=x*x; return x*(27.0f+x2)/(27.0f+9.0f*x2);
}
static inline float sb_apply_dist(float x,int mode){        /* super-boom apply_dist */
    switch(mode){
        case 0: return sinf(clampf(x,-1.57f,1.57f));            /* Boost */
        case 1: return (x>0.0f)? sb_tanh(x) : (x*0.55f);       /* Tube  */
        case 2: return clampf(x*1.4f,-0.85f,0.85f);            /* Fuzz  */
        default: return x;
    }
}
static inline float delay_saturate(float x,float drive,float asym){ /* krautdrums-move */
    float clipped=sb_tanh(x*drive+asym);
    return (clipped - sb_tanh(asym))/drive;
}
static inline float tape_cubic(float x,float drive){        /* mello-move */
    float y=x*drive; if(y>1.5f) return 1.0f; if(y<-1.5f) return -1.0f;
    return y - y*y*y*(1.0f/6.75f);
}
static inline float tape_asym(float x,float drive,float asym){ /* mello-move */
    return sb_tanh(x*drive+asym)-sb_tanh(asym);
}

static void fx_passthrough(slot_dsp_t *s, float *l, float *r, int n,
                           float amount, float macro, float drift){
    (void)s;(void)l;(void)r;(void)n;(void)amount;(void)macro;(void)drift;
}

/* ── CHARACTER ─────────────────────────────────────────────────────────────── */

/* DRIVE — tube-ish overdrive that KEEPS the low end. The bass band (<~200 Hz) is
 * split off and passed clean; only the harmonic band is driven through the
 * Airwindows Spiral shaper sin(x·|x|)/|x| (Chris Johnson, MIT — soft musical fold).
 * macro = pre-emphasis tilt (brightens INTO the shaper, never high-passes the
 * output), drift = slow bias wander. State: z1=bass one-pole. */
static void fx_drive(slot_dsp_t *s, float *l, float *r, int n,
                     float amount, float macro, float drift){
    float drv  = 1.0f + amount*6.0f + amount*amount*30.0f;
    float tilt = (macro-0.5f)*2.0f;                  /* −1..+1 brightness tilt */
    const float aBass=0.030f;                        /* ~200 Hz low/high split */
    float himk = 0.7f + amount*0.5f;                 /* harmonic-band makeup */
    for(int i=0;i<n;i++){
        float bias = drift>0.0f ? wander(&s->f1,&s->seed,0.0006f)*drift*0.10f : 0.0f;
        for(int ch=0;ch<2;ch++){
            float x=(ch?r:l)[i];
            float *lp=ch?&s->z1r:&s->z1l;
            *lp += aBass*(x-*lp)+DENORM;
            float bass=*lp, high=x-*lp;              /* clean sub + harmonic band */
            float pre=high*(1.0f+0.6f*tilt)+bias;    /* tilt = pre-emphasis, not HPF */
            float d=pre*drv, ad=fabsf(d);
            float sh=(ad>1e-6f)? sinf(d*ad)/ad : d;  /* Spiral soft fold */
            float wet=bass + sh*himk;                /* bass FULL → low end intact */
            (ch?r:l)[i]=lerpf(x, wet, amount);       /* amount=0 → dry */
        }
    }
}

/* SWEETEN — console preamp. Gentle comp + the Airwindows Density sin-fold
 * saturator (Chris Johnson, MIT) for body, + an Air-style HF tilt shelf.
 * amount=comp/sat, macro=tone (dark↔air), drift=subtle level drift. */
static void fx_sweeten(slot_dsp_t *s, float *l, float *r, int n,
                       float amount, float macro, float drift){
    float tilt=(macro-0.5f)*2.0f;
    float thr=0.5f, ratio=0.35f+amount*0.4f;         /* gentle comp */
    float density=amount*3.0f, dwet=clampf(density,0.0f,1.0f);  /* Density sat amount */
    float comp=1.0f/(1.0f+density*0.5f);             /* compensate sin-fold gain */
    for(int i=0;i<n;i++){
        float mono=0.5f*(fabsf(l[i])+fabsf(r[i]));
        s->env += (mono>s->env?0.02f:0.004f)*(mono-s->env)+DENORM;  /* fast atk slow rel */
        float gr=1.0f; if(s->env>thr) gr=1.0f-(1.0f-thr/s->env)*ratio;
        float dl=(drift>0.0f)? 1.0f+wander(&s->f1,&s->seed,0.0004f)*drift*0.03f : 1.0f;
        float mdrive=1.0f+amount*2.2f;                /* density drive */
        float mcomp=1.0f/(0.55f+mdrive*0.45f);
        for(int ch=0;ch<2;ch++){
            float dry=(ch?r:l)[i];
            float x=dry*gr*dl;
            /* Airwindows Mojo density fold (Chris Johnson, MIT): pow(|x|,0.25)
             * thickens low-level detail before a very soft sin-fold → perceived
             * density/RMS ("louder without clipping"), the console-glue voice. */
            float xx=x*mdrive;
            float mojo=sqrtf(sqrtf(fabsf(xx)+1e-9f));  /* |x|^0.25 (two sqrts) */
            float sat=(sinf(xx*mojo*1.57079633f)/mojo)*0.987654f*mcomp;
            (void)dwet;(void)comp;(void)density;
            /* Air-style HF tilt shelf (macro) */
            float *z=ch?&s->z1r:&s->z1l;
            *z += 0.10f*(sat-*z)+DENORM;              /* LP component */
            float wet=(tilt>=0.0f)? sat+(sat-*z)*tilt*0.8f      /* brighten = air */
                                  : lerpf(sat,*z,-tilt);        /* darken */
            (ch?r:l)[i]=lerpf(dry,wet,amount);        /* amount=0 → dry */
        }
    }
}

/* FUZZ — Big-Muff-style TWO cascaded soft-clip stages. The cascade gives the
 * compressed, sustaining, "singing" voice at MODERATE output (not just loud):
 * input band-limited ~1.2 kHz → starved bias → two soft clips in series →
 * DC-block → post tone. amount=sustain/gain, macro=tone/bias, drift=bias drift.
 * State: z3=input LP, z4/bp=DC-block x/y, z2=tone LP. */
static void fx_fuzz(slot_dsp_t *s, float *l, float *r, int n,
                    float amount, float macro, float drift){
    float sustain=0.5f+amount*amount*2.6f;           /* pre-gain into the cascade */
    float bias=(macro-0.5f)*0.4f;                    /* starved bias → asymmetry */
    float tone=0.12f+macro*0.5f;                     /* post tone LP */
    const float aIn=0.157f;                          /* input LP ~1.2 kHz */
    for(int i=0;i<n;i++){
        float b=bias + (drift>0.0f? wander(&s->f1,&s->seed,0.002f)*drift*0.3f:0.0f);
        for(int ch=0;ch<2;ch++){
            float x=(ch?r:l)[i];
            float *ilp=ch?&s->z3r:&s->z3l;
            *ilp += aIn*(x-*ilp)+DENORM;
            float xi=*ilp + b;
            float g=28.0f*sustain;
            float s1=-sb_tanh(g*xi);                  /* stage 1 (inverts) */
            float s2=-sb_tanh(g*0.5f*s1);             /* stage 2 cascade → sustain */
            float *dcx=ch?&s->z4r:&s->z4l, *dcy=ch?&s->bp_r:&s->bp_l;
            float y=s2 - *dcx + 0.9995f*(*dcy);       /* DC blocker (bias offset) */
            *dcx=s2; *dcy=y;
            float *tz=ch?&s->z2r:&s->z2l;
            *tz += tone*(y-*tz)+DENORM;
            (ch?r:l)[i]=lerpf(x, *tz*0.6f, amount);   /* moderate output; amount=0 → dry */
        }
    }
}

/* HOWL — resonant filter-fuzz / synth stab. Cytomic/TPT state-variable filter
 * (Andrew Simper, topology-preserving — unconditionally stable at high freq/Q,
 * unlike the Chamberlin SVF). Bandpass voiced into soft clip. amount=intensity
 * (drive + resonance), macro=resonant freq, drift=res-freq wander.
 * State per ch: lp_*=ic1eq, bp_*=ic2eq. */
static void fx_howl(slot_dsp_t *s, float *l, float *r, int n,
                    float amount, float macro, float drift){
    float base=120.0f*powf(40.0f,macro);             /* resonant freq 120..4800 Hz (TILT) */
    float fz=base*(drift>0.0f?(1.0f+wander(&s->f1,&s->seed,0.0008f)*drift*0.5f):1.0f);
    float g=tanf(3.14159265f*clampf(fz,40.0f,9000.0f)/SR);
    float k=1.0f-amount*0.985f; if(k<0.015f)k=0.015f; /* damping → near self-oscillation */
    float a1=1.0f/(1.0f+g*(g+k)), a2=g*a1, a3=g*a2;
    float drive=1.0f+amount*amount*20.0f;            /* fuzz drive feeding the resonator */
    float voice=0.6f+amount*0.9f;
    for(int ch=0;ch<2;ch++){
        float *ic1=ch?&s->lp_r:&s->lp_l, *ic2=ch?&s->bp_r:&s->bp_l;
        float *src=ch?r:l;
        for(int i=0;i<n;i++){
            /* Fuzz FRONT-END: harmonic-rich excitation so the high-Q filter sings,
             * blooms and stabs (manual: "resonant filter fuzz"), not a clean BP. */
            float fuzz=sb_apply_dist(src[i]*drive,2);
            float v3=fuzz-*ic2;
            float v1=a1*(*ic1)+a2*v3;
            float v2=*ic2+a2*(*ic1)+a3*v3;
            *ic1=2.0f*v1-*ic1+DENORM; *ic2=2.0f*v2-*ic2+DENORM;
            float bp=sb_tanh(v1*voice);              /* soft-clip resonance → self-limits, blooms */
            src[i]=lerpf(src[i],bp*0.9f,amount);     /* amount=0 → dry */
        }
    }
}

/* SWELL — auto volume-swell. Hysteresis gate + Zeno-pole envelopes from Airwindows
 * Swell (Chris Johnson, MIT): separate on/off thresholds (no chatter) and separate
 * attack/release "Zeno arrow" poles. amount=swell time, macro=sensitivity,
 * drift=threshold jitter. State: i1/i2=louder L/R, sm1/sm2=swell gain L/R. */
static void fx_swell(slot_dsp_t *s, float *l, float *r, int n,
                     float amount, float macro, float drift){
    float sens=0.02f+(1.0f-macro)*0.28f;             /* on-threshold */
    float speedOn =0.00015f+(1.0f-amount)*0.0028f;   /* longer swell = slower rise */
    float speedOff=0.0008f;                           /* gentle fade-out */
    for(int i=0;i<n;i++){
        float thOn=sens*(drift>0.0f?(1.0f+wander(&s->f1,&s->seed,0.003f)*drift*0.5f):1.0f);
        float thOff=thOn*0.5f;                        /* hysteresis: off < on */
        for(int ch=0;ch<2;ch++){
            float x=(ch?r:l)[i]; float a=fabsf(x);
            int *louder=ch?&s->i2:&s->i1; float *sw=ch?&s->sm2:&s->sm1;
            if(a>thOn && !*louder) *louder=1;
            if(a<thOff && *louder) *louder=0;
            if(*louder) *sw = *sw*(1.0f-speedOn)+speedOn;   /* Zeno → 1 */
            else      { *sw = *sw*(1.0f-speedOff); if(*sw<1e-15f)*sw=0.0f; } /* Zeno → 0, denormal floor */
            (ch?r:l)[i]=lerpf(x,x*(*sw),amount); /* amount=0 → dry */
        }
    }
}

/* Authentic Mutable Warps tables (warps_data.c, MIT) — see SOURCES. */
extern const float lut_bipolar_fold[];   /* 4096-pt, centred at +2048, range ±0.87 */
extern const float lut_ap_poles[];       /* 17 Hilbert allpass poles (SHIFT) */

/* FOLD — West-Coast wavefolder using the REAL Warps lut_bipolar_fold curve via
 * the ALGORITHM_FOLD interpolation. amount=fold drive, macro=offset/symmetry,
 * drift=fold-point wobble. */
static void fx_fold(slot_dsp_t *s, float *l, float *r, int n,
                    float amount, float macro, float drift){
    const float kScale=2048.0f/2.295f;               /* Warps: 2048/((2+0.25)*1.02) */
    float drive=0.5f+amount*amount*4.0f;
    float off=(macro-0.5f)*1.6f;
    float comp=0.55f/(0.6f+0.4f*drive);              /* level compensation — folding is loud */
    for(int i=0;i<n;i++){
        float w=(drift>0.0f? wander(&s->f1,&s->seed,0.0009f)*drift*0.25f:0.0f);
        for(int ch=0;ch<2;ch++){
            float dry=(ch?r:l)[i];
            float x=dry*drive + off + w;
            float idx=x*kScale + 2048.0f;             /* index into lut_bipolar_fold */
            int i0=(int)idx; if(i0<1)i0=1; else if(i0>4094)i0=4094;
            float fr=idx-(float)i0; if(fr<0.0f)fr=0.0f; else if(fr>1.0f)fr=1.0f;
            float y=lut_bipolar_fold[i0]+(lut_bipolar_fold[i0+1]-lut_bipolar_fold[i0])*fr;
            float *z=ch?&s->z1r:&s->z1l;
            *z += 0.5f*(y*comp-*z)+DENORM;             /* mild LP smooth, level-matched */
            (ch?r:l)[i]=lerpf(dry,*z,amount);          /* amount=0 → dry */
        }
    }
}

/* ── MOVEMENT ──────────────────────────────────────────────────────────────── */

/* DOUBLER — ADT stereo double-track / slapback. amount=mix, macro=time,
 * drift=random momentary detune. */
static void fx_doubler(slot_dsp_t *s, float *l, float *r, int n,
                       float amount, float macro, float drift){
    float baseL=(s->sync_time>0.0f)? s->sync_time : SR*(0.012f+macro*0.045f); /* 12..57ms / synced */
    float baseR=baseL*1.28f;
    for(int i=0;i<n;i++){
        s->lfo+=0.6f/SR; if(s->lfo>=1.0f)s->lfo-=1.0f;
        s->lfo2+=0.43f/SR; if(s->lfo2>=1.0f)s->lfo2-=1.0f;
        float wob=(drift>0.0f? wander(&s->f1,&s->seed,0.004f)*drift:0.0f);
        float dL=baseL*(1.0f+0.004f*sinf(s->lfo*TWO_PI)+0.01f*wob);
        float dR=baseR*(1.0f+0.004f*sinf(s->lfo2*TWO_PI)-0.01f*wob);
        s->dl_l[s->wp]=l[i]; s->dl_r[s->wp]=r[i];
        float eL=dlr(s->dl_l,s->wp,dL), eR=dlr(s->dl_r,s->wp,dR);
        s->wp=(s->wp+1)%MAX_DELAY;
        l[i]=lerpf(l[i],l[i]*0.7f+eL,amount);
        r[i]=lerpf(r[i],r[i]*0.7f+eR,amount);
    }
}

/* VIBRATO — fully-stereo modulated delay. L and R get the SAME LFO 90° apart
 * (true stereo vibrato/widening). 2nd LFO through-zero-FMs the sweep (Airwindows
 * character) + HF restore for interpolation dulling. amount=depth, macro=rate,
 * drift=sine→random waveshape + FM depth. lfo=main phase, lfo2=FM phase. */
static void fx_vibrato(slot_dsp_t *s, float *l, float *r, int n,
                       float amount, float macro, float drift){
    float rate=(s->sync_rate>0.0f)? s->sync_rate : 0.5f+macro*7.5f;  /* 0.5..8 Hz / synced */
    float depth=SR*0.0005f*(0.3f+amount*4.0f);       /* mod depth in samples */
    float base=SR*0.006f;
    float fmRate=rate*1.61f;                          /* incommensurate 2nd LFO */
    float fmDepth=0.2f+drift*0.35f;                   /* through-zero FM amount (tamed) */
    for(int i=0;i<n;i++){
        s->lfo2+=fmRate/SR; if(s->lfo2>=1.0f)s->lfo2-=1.0f;
        s->lfo+=(rate*(1.0f+fmDepth*0.5f*sinf(s->lfo2*TWO_PI)))/SR;  /* through-zero FM */
        if(s->lfo>=1.0f)s->lfo-=1.0f; if(s->lfo<0.0f)s->lfo+=1.0f;
        float phR=s->lfo+0.25f; if(phR>=1.0f)phR-=1.0f;   /* R 90° offset = stereo */
        float rndL=(drift>0.0f? wander(&s->f1,&s->seed,0.008f):0.0f);
        float rndR=(drift>0.0f? wander(&s->f3,&s->seed,0.008f):0.0f);
        float modL=lerpf(sinf(s->lfo*TWO_PI),rndL,clampf(drift,0.0f,0.9f));
        float modR=lerpf(sinf(phR*TWO_PI),    rndR,clampf(drift,0.0f,0.9f));
        float dL=base+depth*(1.0f+modL), dR=base+depth*(1.0f+modR);
        s->dl_l[s->wp]=l[i]; s->dl_r[s->wp]=r[i];
        float oL=dlr(s->dl_l,s->wp,dL), oR=dlr(s->dl_r,s->wp,dR);
        /* HF restore: high-shelf the interpolation loss back in */
        s->z1l+=0.4f*(oL-s->z1l)+DENORM; oL+=(oL-s->z1l)*0.10f;
        s->z1r+=0.4f*(oR-s->z1r)+DENORM; oR+=(oR-s->z1r)*0.10f;
        l[i]=oL; r[i]=oR;
        s->wp=(s->wp+1)%MAX_DELAY;
    }
}

/* PHASER — N-stage allpass cascade + feedback. Frequency-accurate: the LFO sweeps
 * the notch frequency in Hz and the first-order allpass coefficient is derived as
 * a=(g-1)/(g+1), g=tan(π·fc/SR) — so notches sit at musical frequencies (Surge-style)
 * instead of a raw 0–1 coefficient. amount=intensity/stages, macro=rate,
 * drift=sweep randomness. */
static void fx_phaser(slot_dsp_t *s, float *l, float *r, int n,
                      float amount, float macro, float drift){
    int stages=2+(int)(amount*10.0f); if(stages>12)stages=12;  /* 2..12 */
    float rate=(s->sync_rate>0.0f)? s->sync_rate : 0.05f+macro*macro*4.0f;  /* synced */
    float fb=amount*0.6f;
    for(int i=0;i<n;i++){
        s->lfo+=rate/SR; if(s->lfo>=1.0f)s->lfo-=1.0f;
        float rnd=(drift>0.0f? wander(&s->f2,&s->seed,0.01f)*drift*0.4f:0.0f);
        for(int ch=0;ch<2;ch++){
            /* quadrature LFO: R is 90° ahead of L → fully stereo sweep */
            float ph=s->lfo + (ch?0.25f:0.0f); if(ph>=1.0f)ph-=1.0f;
            float sweep=0.5f+0.5f*sinf(ph*TWO_PI)+rnd;
            float fc=200.0f*exp2f(4.0f*clampf(sweep,0.0f,1.0f)); /* 16^x = 2^(4x), exact + cheaper */
            float g=tanf(3.14159265f*clampf(fc,30.0f,12000.0f)/SR);
            float coef=(g-1.0f)/(g+1.0f);                       /* allpass coefficient */
            float *ap=ch?s->ap_r:s->ap_l; float *fbz=ch?&s->z2r:&s->z2l;
            float x=(ch?r:l)[i]+ *fbz*fb;
            for(int k=0;k<stages;k++){
                float y=coef*x+ap[k];                 /* 1st-order allpass */
                ap[k]=x-coef*y; x=y;
            }
            *fbz=x;
            (ch?r:l)[i]=lerpf((ch?r:l)[i],0.5f*((ch?r:l)[i]+x),amount); /* amount=0 → dry */
        }
    }
}

/* TREMOLO — Airwindows Tremolo (Chris Johnson, MIT): chase-smoothed skew/density
 * waveshaping. amount=depth, macro=rate. DRIFT = AutoPan: morphs L/R from in-phase
 * (mono tremolo) to anti-phase (the channels duck oppositely → the modulation pans
 * across the stereo field). State: sm1/sm2=chased speed/depth, f4/f5=last targets. */
static void fx_tremolo(slot_dsp_t *s, float *l, float *r, int n,
                       float amount, float macro, float drift){
    float speedChase=macro*macro*macro*macro;          /* A^4 (Airwindows) */
    float depthChase=clampf(amount,0.0f,1.0f);
    float speedSpeed=300.0f/(fabsf(s->f4-speedChase)+1.0f);
    float depthSpeed=300.0f/(fabsf(s->f5-depthChase)+1.0f);
    s->f4=speedChase; s->f5=depthChase;
    float pan=clampf(drift,0.0f,1.0f);
    for(int i=0;i<n;i++){
        s->sm1=((s->sm1*speedSpeed)+speedChase)/(speedSpeed+1.0f);
        s->sm2=((s->sm2*depthSpeed)+depthChase)/(depthSpeed+1.0f);
        float speed=(s->sync_rate>0.0f)? s->sync_rate*TWO_PI/SR : 0.0001f+(s->sm1/1000.0f);
        float s2=s->sm2*s->sm2, s4=s2*s2; float skew=1.0f+s->sm2*s4*s4; /* sm2^9 via mults */
        float density=((1.0f-s->sm2)*2.0f)-1.0f;
        float offset=sinf(s->lfo);
        s->lfo+=speed; if(s->lfo>TWO_PI)s->lfo-=TWO_PI;
        float control=fabsf(offset);
        if(density>0.0f) control=control*(1.0f-density)+sinf(control)*density;
        else             control=control*(1.0f+density)+(1.0f-cosf(control))*(-density);
        float ctl[2]={control, lerpf(control,1.0f-control,pan)};  /* R anti-phase at drift=1 */
        for(int ch=0;ch<2;ch++){
            float thickness=((ctl[ch]*2.0f)-1.0f)*skew;
            float out=fabsf(thickness);
            float x=(ch?r:l)[i];
            float br=fabsf(x); if(br>1.57079633f)br=1.57079633f;
            br=(thickness>0.0f)? sinf(br) : (1.0f-cosf(br));
            (ch?r:l)[i]=(x>0.0f)? x*(1.0f-out)+br*out : x*(1.0f-out)-br*out;
        }
    }
}

/* PITCH — delay-line pitch shifter ±1 oct + lo-fi grit. amount=wet mix,
 * macro=pitch, drift=resolution drop. Dual-tap crossfade window. */
static void fx_pitch(slot_dsp_t *s, float *l, float *r, int n,
                     float amount, float macro, float drift){
    float ratio=powf(2.0f,(macro-0.5f)*2.0f);        /* 0.5 .. 2.0 */
    float win=SR*0.040f;                              /* 40 ms window */
    float drate=(1.0f-ratio);                         /* read-ptr drift / sample */
    /* drift = gentle resolution drop: 9 bits (subtle) → 4 bits (gritty), blended in */
    float q=(drift>0.0f)? powf(2.0f, 9.0f-drift*5.0f) : 0.0f;
    for(int i=0;i<n;i++){
        s->dl_l[s->wp]=l[i]; s->dl_r[s->wp]=r[i];
        s->f1+=drate; if(s->f1<0)s->f1+=win; if(s->f1>=win)s->f1-=win;
        float p2=s->f1+win*0.5f; if(p2>=win)p2-=win;
        float w1=0.5f-0.5f*cosf(TWO_PI*s->f1/win);    /* triangular/raised-cos xfade */
        float w2=1.0f-w1;
        float oL=dlr(s->dl_l,s->wp,win-s->f1)*w1 + dlr(s->dl_l,s->wp,win-p2)*w2;
        float oR=dlr(s->dl_r,s->wp,win-s->f1)*w1 + dlr(s->dl_r,s->wp,win-p2)*w2;
        s->wp=(s->wp+1)%MAX_DELAY;
        if(q>0.0f){ /* round (no DC bias), blend by drift so low drift stays subtle */
            oL=lerpf(oL, floorf(oL*q+0.5f)/q, drift);
            oR=lerpf(oR, floorf(oR*q+0.5f)/q, drift);
        }
        l[i]=lerpf(l[i],oL,amount); r[i]=lerpf(r[i],oR,amount);
    }
}

/* SHIFT — Bode single-sideband frequency shifter using the REAL Mutable Warps
 * QuadratureTransform: a 17-stage first-order allpass network (lut_ap_poles) that
 * produces a broadband 90° quadrature pair, multiplied by a complex carrier.
 * amount=wet mix, macro=shift Hz (±), drift=shift wander.
 * State per channel: s->hil[ch*34 + filt*2 + {0:x,1:y}] (Warps AllPassFilter). */
static inline float warps_ap(float *st, float in, float coef){ /* y=coef*(in-y_)+x_ */
    float y = coef*(in - st[1]) + st[0];
    st[0]=in; st[1]=y; return y;
}
static void fx_shift(slot_dsp_t *s, float *l, float *r, int n,
                     float amount, float macro, float drift){
    float shift=(macro-0.5f)*1000.0f;                /* −500..+500 Hz */
    for(int i=0;i<n;i++){
        float w=(drift>0.0f? wander(&s->f1,&s->seed,0.0006f)*drift*40.0f:0.0f);
        s->lfo2 += (shift+w)/SR; if(s->lfo2>=1.0f)s->lfo2-=1.0f; if(s->lfo2<0.0f)s->lfo2+=1.0f;
        float cw=cosf(s->lfo2*TWO_PI), sw=sinf(s->lfo2*TWO_PI);
        for(int ch=0;ch<2;ch++){
            float *H=&s->hil[ch*34];                  /* 17 filters × 2 states */
            float x=(ch?r:l)[i];
            float iv=0.0f, qv=0.0f;                   /* i_out / q_out */
            for(int k=0;k<17;k++){                     /* QuadratureTransform.Process */
                float coef=-lut_ap_poles[k];
                float *dst=(k&1)?&qv:&iv;
                float src=(k<=1)?x:*dst;               /* first two read input, then cascade */
                *dst=warps_ap(&H[k*2], src, coef);
            }
            float sh=iv*cw - qv*sw;                    /* single-sideband shift */
            (ch?r:l)[i]=lerpf(x,sh,amount);
        }
    }
}

/* ── DIFFUSION ─────────────────────────────────────────────────────────────── */

/* delay-core helper. Differentiates BBD vs tape via: feedback cap, HF damping,
 * wow rate/depth, flutter, feedback saturation drive, and tape hiss. The wet tap
 * scales with amount so amount=0 → dry. */
static void delay_core(slot_dsp_t *s, float *l, float *r, int n,
                       float amount, float macro, float drift,
                       float fb_cap, float lp_amt, float wow_hz, float wow_depth,
                       float sat_drive, float flutter_depth, float hiss){
    float t=(s->sync_time>0.0f)? s->sync_time : SR*(0.03f*powf(40.0f,macro)); /* 30ms..1.2s / synced */
    if(s->f6<1.0f || s->f6>SR*2.5f) s->f6=t;          /* init/repair smoothed time */
    float fb=amount*fb_cap;
    float wet=clampf(amount*1.4f,0.0f,1.0f);          /* amount=0 → dry */
    for(int i=0;i<n;i++){
        s->f6 += 0.0004f*(t-s->f6);                   /* glide delay time (no zipper/click) */
        s->lfo+=wow_hz/SR;  if(s->lfo>=1.0f)s->lfo-=1.0f;     /* wow */
        s->lfo2+=6.3f/SR;   if(s->lfo2>=1.0f)s->lfo2-=1.0f;   /* flutter */
        float wob=wow_depth*sinf(s->lfo*TWO_PI)
                 + flutter_depth*sinf(s->lfo2*TWO_PI)
                 + (drift>0.0f? wander(&s->f1,&s->seed,0.001f)*drift*wow_depth*3.0f:0.0f);
        float d=s->f6*(1.0f+wob);
        float tapL=dlr(s->dl_l,s->wp,d), tapR=dlr(s->dl_r,s->wp,d);
        /* 2-pole LP darkening in the feedback (more = warmer/tape) */
        s->z1l+=lp_amt*(tapL-s->z1l)+DENORM; s->z2l+=lp_amt*(s->z1l-s->z2l)+DENORM;
        s->z1r+=lp_amt*(tapR-s->z1r)+DENORM; s->z2r+=lp_amt*(s->z1r-s->z2r)+DENORM;
        float fl=delay_saturate(s->z2l*fb, sat_drive, 0.02f);
        float fr=delay_saturate(s->z2r*fb, sat_drive, 0.02f);
        s->dl_l[s->wp]=l[i]+fl; s->dl_r[s->wp]=r[i]+fr;   /* hiss stays OUT of the feedback path */
        s->wp=(s->wp+1)%MAX_DELAY;
        float hsL=0.0f,hsR=0.0f;
        if(hiss>0.0f){ float sig=fabsf(tapL)+fabsf(tapR);   /* gated tape hiss, on the wet output only */
            if(sig>0.001f){ hsL=hiss*(frand(&s->seed)-0.5f); hsR=hiss*(frand(&s->seed)-0.5f); } }
        l[i]+=(tapL*0.9f+hsL)*wet; r[i]+=(tapR*0.9f+hsR)*wet;
    }
}
/* CASCADE — BBD bucket-brigade: brighter-but-bandlimited repeats, fast subtle
 * clock warble, moderate saturation, no hiss. Clean-ish, metallic, can self-osc. */
static void fx_cascade(slot_dsp_t *s, float *l, float *r, int n,
                       float amount, float macro, float drift){
    delay_core(s,l,r,n,amount,macro,drift,0.95f,0.22f,6.0f,0.00035f,1.3f,0.0f,0.0f);
}
/* REELS — worn tape echo (RE-201): warmer/darker, slow wow + flutter, heavier
 * tape saturation, gated hiss, higher feedback cap (self-oscillates). */
static void fx_reels(slot_dsp_t *s, float *l, float *r, int n,
                     float amount, float macro, float drift){
    delay_core(s,l,r,n,amount,macro,drift,1.05f,0.42f,0.7f,0.0016f,1.9f,0.0006f,0.0022f);
}

/* REVERSE — reverse delay. amount=wet mix, macro=segment time, drift=pitch mod.
 * Records forward; plays windowed segments backward. */
static void fx_reverse(slot_dsp_t *s, float *l, float *r, int n,
                       float amount, float macro, float drift){
    float seg=(s->sync_time>0.0f)? s->sync_time : SR*(0.08f+macro*0.6f); /* 80..680ms / synced */
    float spd=1.0f+(drift>0.0f? wander(&s->f1,&s->seed,0.0008f)*drift*0.05f:0.0f);
    for(int i=0;i<n;i++){
        s->dl_l[s->wp]=l[i]; s->dl_r[s->wp]=r[i];
        s->f2-=spd; if(s->f2<=0.0f) s->f2+=seg;       /* reverse read offset grows */
        float d=s->f2;                                 /* read this far behind */
        float env=0.5f-0.5f*cosf(TWO_PI*(seg-s->f2)/seg);  /* window the segment */
        float oL=dlr(s->dl_l,s->wp,d)*env, oR=dlr(s->dl_r,s->wp,d)*env;
        s->wp=(s->wp+1)%MAX_DELAY;
        l[i]=lerpf(l[i],oL,amount); r[i]=lerpf(r[i],oR,amount);
    }
}

/* COLLAGE — glitch/granular looping delay. amount=feedback, macro=loop time,
 * drift=random double-speed/reverse grains. */
static void fx_collage(slot_dsp_t *s, float *l, float *r, int n,
                       float amount, float macro, float drift){
    float loop=(s->sync_time>0.0f)? s->sync_time : SR*(0.05f+macro*1.2f);
    if(s->f5<1.0f || s->f5>SR*1.3f) s->f5=loop;       /* init/repair smoothed loop time */
    float fb=amount*0.9f;
    for(int i=0;i<n;i++){
        s->f5 += 0.0004f*(loop-s->f5);                /* glide loop time (no distortion on knob turn) */
        float lps=s->f5;
        /* grain scheduler: f3=grain pos, f4=grain speed, i1=samples left */
        if(s->i1<=0){
            s->i1=(int)(SR*(0.04f+frand(&s->seed)*0.12f));
            s->f4=1.0f;
            if(drift>0.0f && frand(&s->seed)<drift*0.5f) s->f4=(frand(&s->seed)<0.5f?2.0f:-1.0f);
            s->f3=frand(&s->seed)*lps;
        }
        s->i1--;
        s->f3+=s->f4; if(s->f3<0)s->f3+=lps; if(s->f3>=lps)s->f3-=lps;
        float gL=dlr(s->dl_l,s->wp,lps-s->f3), gR=dlr(s->dl_r,s->wp,lps-s->f3);
        s->dl_l[s->wp]=l[i]+gL*fb; s->dl_r[s->wp]=r[i]+gR*fb;
        s->wp=(s->wp+1)%MAX_DELAY;
        l[i]=lerpf(l[i],gL,amount*0.9f); r[i]=lerpf(r[i],gR,amount*0.9f);
    }
}

/* ── TEXTURE ───────────────────────────────────────────────────────────────── */

/* FILTER — multimode Tilt/LP/HP (SVF) with resonance. amount=cutoff,
 * macro=mode (0 LP · 0.5 tilt · 1 HP), drift=cutoff wander. */
/* FILTER — single-knob DJ filter on a resonant Cytomic/TPT SVF (stable). amount
 * sweeps LP→through→HP (center = open/neutral, like a DJ isolator); macro = resonance;
 * drift = cutoff wander. State per ch: lp_*=ic1eq, bp_*=ic2eq. */
static void fx_filter(slot_dsp_t *s, float *l, float *r, int n,
                      float amount, float macro, float drift){
    float amt=clampf(amount,0.0f,1.0f);
    int hp; float fc;
    if(amt<0.5f){ hp=0; fc=200.0f*powf(90.0f, amt*2.0f); }        /* LP 200 Hz .. 18 kHz */
    else        { hp=1; fc=20.0f *powf(20.0f,(amt-0.5f)*2.0f); }  /* HP 20 Hz .. 400 Hz */
    if(drift>0.0f) fc*=1.0f+wander(&s->f1,&s->seed,0.0007f)*drift*0.3f;
    fc=clampf(fc,20.0f,18000.0f);
    float wetx=clampf(fabsf(amt-0.5f)/0.05f,0.0f,1.0f);           /* dry exactly at center */
    float g=tanf(3.14159265f*fc/SR);
    float k=1.0f-clampf(macro,0.0f,1.0f)*0.9f;                    /* resonance (1/Q) */
    float a1=1.0f/(1.0f+g*(g+k)), a2=g*a1, a3=g*a2;
    for(int ch=0;ch<2;ch++){
        float *ic1=ch?&s->lp_r:&s->lp_l, *ic2=ch?&s->bp_r:&s->bp_l;
        float *src=ch?r:l;
        for(int i=0;i<n;i++){
            float x=src[i];
            float v3=x-*ic2, v1=a1*(*ic1)+a2*v3, v2=*ic2+a2*(*ic1)+a3*v3;
            *ic1=2.0f*v1-*ic1+DENORM; *ic2=2.0f*v2-*ic2+DENORM;
            float filt=hp? (x-k*v1-v2) : v2;          /* HP or LP tap */
            src[i]=lerpf(x,filt,wetx);
        }
    }
}

/* SQUASH — vari-mu compressor + overdrive. PORTED from Airwindows Pressure4
 * (Chris Johnson, MIT). amount=compression, macro=mu character (punchy↔smooth),
 * drift=threshold jitter. State: f1/f2=muSpeed A/B, f3/f4=muCoeff A/B, i1=flip.
 * Faithful to the Pressure4 per-sample algorithm; VST/dither scaffolding dropped. */
static void fx_squash(slot_dsp_t *s, float *l, float *r, int n,
                      float amount, float macro, float drift){
    if(s->f1<1.0f){ s->f1=s->f2=10000.0f; s->f3=s->f4=1.0f; } /* Pressure4 ctor inits */
    float A=clampf(amount,0.0f,1.0f), B=0.4f;          /* B = release speed (fixed, musical) */
    float threshold=1.0f-(A*0.95f);
    float muMakeupGain=1.0f/threshold;
    float release=powf(1.28f-B,5.0f)*32768.0f;          /* overallscale = 1 @ 44.1k */
    float fastest=sqrtf(release);
    float mewiness=(macro*2.0f)-1.0f, unmew;
    int positivemu; if(mewiness>=0){positivemu=1;unmew=1.0f-mewiness;}
                    else{positivemu=0;mewiness=-mewiness;unmew=1.0f-mewiness;}
    float outGain=1.0f;
    float comp=0.5f+0.5f*threshold;                      /* gain-comp: cancels the 1/threshold makeup */
    for(int i=0;i<n;i++){
        float th=threshold*(drift>0.0f?(1.0f+wander(&s->f5,&s->seed,0.004f)*drift*0.2f):1.0f);
        float xl=l[i]*muMakeupGain, xr=r[i]*muMakeupGain;
        float sense=fabsf(xl); if(fabsf(xr)>sense) sense=fabsf(xr);
        float *spd = s->i1?&s->f1:&s->f2;                /* muSpeedA/B */
        float *cof = s->i1?&s->f3:&s->f4;                /* muCoefficientA/B */
        if(sense>th){
            float muVary=th/sense, muAttack=sqrtf(fabsf(*spd));
            *cof = *cof*(muAttack-1.0f);
            *cof += (muVary<th)? th : muVary;
            *cof /= muAttack;
        } else {
            *cof = *cof*((*spd)*(*spd)-1.0f) + 1.0f;
            *cof /= (*spd)*(*spd);
        }
        float ns=(*spd)*((*spd)-1.0f) + fabsf(sense*release)+fastest;
        *spd = ns/(*spd);
        float coeff = positivemu? (*cof)*(*cof) : sqrtf(fabsf(*cof));
        coeff = coeff*mewiness + (*cof)*unmew;
        xl*=coeff*outGain; xr*=coeff*outGain;
        /* Pressure4 second-stage sin() overdrive */
        float br=fabsf(xl); br=(br>1.57079633f)?1.0f:sinf(br); xl=(xl>0)?br:-br;
        br=fabsf(xr);      br=(br>1.57079633f)?1.0f:sinf(br); xr=(xr>0)?br:-br;
        l[i]=lerpf(l[i],xl*comp,A); r[i]=lerpf(r[i],xr*comp,A); /* level-matched; amount=0 → dry */
        s->i1^=1;
    }
}

/* CASSETTE — worn cassette: WOW (slow sine) + FLUTTER (Airwindows ToTape6-style
 * random-walk pitch wobble) + tape COMPRESSION + mello tape saturation + HF roll.
 * Minimal hiss. amount=degrade intensity, macro=tone, drift=warble depth.
 * State: lfo=wow phase; f2=flutter rate, f3=flutter sweep, f4=flutter target;
 * z1=tone LP; env=compressor follower; base delay. */
static void fx_cassette(slot_dsp_t *s, float *l, float *r, int n,
                        float amount, float macro, float drift){
    float wow_depth =(0.4f+amount*1.2f)*(1.0f+drift*1.2f); /* samples of wow */
    float flut_depth=(0.3f+amount*0.9f)*(1.0f+drift*1.6f); /* samples of flutter */
    float roll=0.06f+(1.0f-macro)*0.5f;              /* tone: darker at low macro */
    float hiss=amount*0.0010f;                        /* much less than before */
    float base=SR*0.006f;
    float sat_d=1.0f+amount*0.7f, asym=0.10f*amount;
    float comp_amt=amount*0.6f;                       /* tape compression depth */
    for(int i=0;i<n;i++){
        s->lfo+=0.55f/SR; if(s->lfo>=1.0f)s->lfo-=1.0f;            /* wow ~0.55 Hz */
        /* ToTape6 random-walk flutter: rate eases toward a new random target */
        s->f2 = s->f2*0.9985f + s->f4*0.0015f;
        s->f3 += (5.0f + s->f2*8.0f)/SR;
        if(s->f3>=1.0f){ s->f3-=1.0f; s->f4=0.24f+frand(&s->seed)*0.74f; }
        float d=base + wow_depth*sinf(s->lfo*TWO_PI) + flut_depth*sinf(s->f3*TWO_PI);
        s->dl_l[s->wp]=l[i]; s->dl_r[s->wp]=r[i];
        float xL=dlr(s->dl_l,s->wp,d), xR=dlr(s->dl_r,s->wp,d);
        s->wp=(s->wp+1)%MAX_DELAY;
        /* tape compression (gentle, program-dependent gain reduction) */
        float mono=0.5f*(fabsf(xL)+fabsf(xR));
        s->env+=(mono>s->env?0.05f:0.002f)*(mono-s->env)+DENORM;
        float gr=1.0f/(1.0f+s->env*comp_amt*2.0f);
        xL*=gr; xR*=gr;
        /* mello tape sat + HF rolloff */
        xL=tape_asym(tape_cubic(xL,sat_d),1.0f,asym);
        xR=tape_asym(tape_cubic(xR,sat_d),1.0f,asym);
        s->z1l+=roll*(xL-s->z1l)+DENORM; s->z1r+=roll*(xR-s->z1r)+DENORM;
        float nz=(mono>0.0008f)? hiss*((frand(&s->seed)-0.5f)*2.0f):0.0f;  /* gated */
        l[i]=lerpf(l[i],s->z1l+nz,amount); r[i]=lerpf(r[i],s->z1r+nz,amount);
    }
}

/* BROKEN — motor-failure pitch drops + AM/FM wobble + dropouts. amount=breakdown,
 * macro=rate of failures, drift=dropout randomness. */
static void fx_broken(slot_dsp_t *s, float *l, float *r, int n,
                      float amount, float macro, float drift){
    float rate=0.1f+macro*macro*3.0f;                /* motor failure LFO Hz */
    for(int i=0;i<n;i++){
        s->lfo+=rate/SR; if(s->lfo>=1.0f)s->lfo-=1.0f;
        /* periodic motor stall: read offset is ~0 at rest (so wet≈dry, NO doubling)
         * and grows during the dip (pitch drops, then recovers each cycle). */
        float dip=amount*(0.5f-0.5f*cosf(s->lfo*TWO_PI));
        s->dl_l[s->wp]=l[i]; s->dl_r[s->wp]=r[i];
        float rd=3.0f + dip*SR*0.03f                  /* baseline ~0 + up to ~30 ms drop */
               + (drift>0.0f? (0.5f+0.5f*sinf(s->lfo*TWO_PI*1.7f))*drift*SR*0.003f : 0.0f);
        float xL=dlr(s->dl_l,s->wp,rd), xR=dlr(s->dl_r,s->wp,rd);
        s->wp=(s->wp+1)%MAX_DELAY;
        /* DRIFT = occasional SMOOTHED dropouts (gate ramps → no clicks) + the warble above */
        if(drift>0.0f && frand(&s->seed)<drift*0.0004f) s->i1=(int)(SR*0.05f*frand(&s->seed));
        float gtarget=1.0f; if(s->i1>0){ s->i1--; gtarget=0.0f; }
        s->f3 += 0.012f*(gtarget - s->f3) + DENORM;   /* ~3 ms gate ramp */
        float am=1.0f-amount*0.3f*(0.5f-0.5f*cosf(s->lfo*TWO_PI*2.0f));   /* AM wobble */
        l[i]=lerpf(l[i],xL*s->f3*am,amount); r[i]=lerpf(r[i],xR*s->f3*am,amount);
    }
}

/* INTERFERENCE — lo-fi telecom/radio destruction. Core PORTED from Airwindows
 * DeRez2 (Chris Johnson, MIT): sample-rate reduction + soften + µ-law encode +
 * bit-depth quantize. Ring-mod carrier (macro) + static bursts (drift) on top.
 * amount=crush intensity, macro=carrier/tone, drift=static. State: z1=lastSample,
 * z2=heldSample, z3=lastDry, z4=lastOut (per ch); f1=position, f2/f3=incrA/incrB. */
static void fx_interference(slot_dsp_t *s, float *l, float *r, int n,
                            float amount, float macro, float drift){
    float A=clampf(amount,0.0f,1.0f);
    /* crush scales WITH amount (was inverted → heavy crush + noise at low amounts).
     * targetA = sample-rate increment: ~1 (clean) at A=0 → 0.03 (heavy SR reduction) at A=1.
     * targetB = bit-quant step: 0 (no quantize) at A=0 → coarse at A=1. */
    float targetA=1.0f - A*0.92f; if(targetA<0.08f)targetA=0.08f;
    float soften=(1.0f+targetA)/2.0f;
    float targetB=A*A*0.22f;                          /* bit-depth derez (scales up with amount) */
    float hard=0.6f;                                  /* more dry blend = less harsh */
    float carr=200.0f+macro*macro*3000.0f;
    const float L256=logf(256.0f);
    for(int i=0;i<n;i++){
        s->f2=((s->f2*999.0f)+targetA)/1000.0f;        /* incrementA (rate) */
        s->f3=((s->f3*999.0f)+targetB)/1000.0f;        /* incrementB (bits) */
        s->f1+=s->f2;                                  /* position */
        float in[2]={l[i],r[i]};
        float out[2]={s->z2l,s->z2r};                  /* outputSample = heldSample */
        if(s->f1>1.0f){
            s->f1-=1.0f;
            float last[2]={s->z1l,s->z1r};
            float h0=last[0]*s->f1+in[0]*(1.0f-s->f1);
            float h1=last[1]*s->f1+in[1]*(1.0f-s->f1);
            out[0]=out[0]*(1.0f-soften)+h0*soften;
            out[1]=out[1]*(1.0f-soften)+h1*soften;
            s->z2l=h0; s->z2r=h1;                      /* heldSample */
        }
        s->z1l=in[0]; s->z1r=in[1];                    /* lastSample */
        float lastOut[2]={s->z4l,s->z4r};
        float lastDry[2]={s->z3l,s->z3r};
        for(int c=0;c<2;c++){
            float x=out[c];
            if(x!=lastOut[c]){ float t=x; x=x*hard+lastDry[c]*(1.0f-hard); lastOut[c]=t; }
            else lastOut[c]=x;
            float temp=x;
            if(x>1.0f)x=1.0f; else if(x<-1.0f)x=-1.0f;  /* µ-law encode */
            if(x>0.0f) x= logf(1.0f+255.0f*fabsf(x))/L256;
            else if(x<0.0f) x=-logf(1.0f+255.0f*fabsf(x))/L256;
            x=temp*hard + x*(1.0f-hard);
            if(s->f3>0.0005f){                          /* bit-depth quantize (O(1)) */
                if(x>0.0f)      x=ceilf (x/s->f3)*s->f3;
                else if(x<0.0f) x=floorf(x/s->f3)*s->f3;
            }
            x=lerpf(x, x*sinf(s->lfo*TWO_PI), macro*0.3f);  /* ring-mod radio */
            if(drift>0.0f && frand(&s->seed)<drift*0.04f) x+=(frand(&s->seed)-0.5f)*drift;
            out[c]=x;
        }
        s->z3l=in[0]; s->z3r=in[1];                    /* lastDry = dry input */
        s->z4l=lastOut[0]; s->z4r=lastOut[1];
        s->lfo+=carr/SR; if(s->lfo>=1.0f)s->lfo-=1.0f;
        l[i]=lerpf(in[0], out[0]*0.6f, amount);        /* amount=0 → dry; trim µ-law boost */
        r[i]=lerpf(in[1], out[1]*0.6f, amount);
    }
}

/* HALO — ethereal harmonic resonator pad (Walrus Qi / OBNE Dark-Star vibe).
 * A 6-voice Karplus-Strong / tuned-comb bank: the chain audio sympathetically
 * excites delay lines tuned to a chord (root, 5th, oct, +oct-3rd, +oct-5th, 2-oct),
 * each with a damping one-pole and soft-clipped feedback so they ring/bloom into a
 * frozen harmonic drone. Pure C (replaces the FFT FREEZE). amount = resonance/
 * sustain (feedback), macro = root pitch + brightness, drift = per-voice detune.
 * Comb buffers reuse dl_l/dl_r (6 regions × 1024); shared region index = i1;
 * per-voice damping = halo_lp_l/r; drift walk = f1. */
#define HALO_VLEN 1024
static void fx_halo(slot_dsp_t *s, float *l, float *r, int n,
                    float amount, float macro, float drift){
    static const float ratio[6]={1.0f,1.49831f,2.0f,2.51984f,2.99661f,4.0f}; /* R 5 8ve +M3 +5 +2-8ve */
    float root=55.0f*powf(2.0f,macro*2.0f);          /* 55..220 Hz root (A1..A3) */
    float fb=0.90f+amount*0.099f;                     /* 0.90..0.999 ring time */
    float damp=0.12f+macro*0.55f;                     /* brighter as macro rises */
    float exc=amount*0.5f;                            /* excitation into the bank */
    for(int i=0;i<n;i++){
        float drf=(drift>0.0f)? wander(&s->f1,&s->seed,0.0006f)*drift*0.012f : 0.0f;
        for(int ch=0;ch<2;ch++){
            float *buf=ch?s->dl_r:s->dl_l;
            float *lp =ch?s->halo_lp_r:s->halo_lp_l;
            float in=(ch?r:l)[i];
            float sum=0.0f;
            for(int v=0;v<6;v++){
                float dl=(SR/(root*ratio[v]))*(1.0f+drf*(float)v);
                if(dl>HALO_VLEN-2) dl=HALO_VLEN-2; if(dl<2.0f) dl=2.0f;
                float *vb=buf+v*HALO_VLEN;
                float rp=(float)s->i1-dl; while(rp<0.0f) rp+=HALO_VLEN;
                int i0=(int)rp; float fr=rp-(float)i0; int i1n=i0+1; if(i1n>=HALO_VLEN)i1n=0;
                float y=vb[i0]+(vb[i1n]-vb[i0])*fr;
                lp[v]+=damp*(y-lp[v])+DENORM;          /* loop damping */
                vb[s->i1]=in*exc + sb_tanh(lp[v]*fb);  /* excite + soft-clipped regen */
                sum+=y;
            }
            (ch?r:l)[i]=lerpf(in, sum*0.32f, amount);  /* amount=0 → dry */
        }
        s->i1++; if(s->i1>=HALO_VLEN) s->i1=0;
    }
}

/* ── Vtable: index by PFX_* id. OFF has no entry (host skips). ───────────────── */
/* ---- Plate reverb: Jon Dattorro, "Effect Design Part 1" (JAES 1997), figure 1.
 * The published topology at its published delay lengths (29761 Hz, scaled to
 * 44.1 kHz): bandwidth LP -> 4 input diffusers -> a figure-8 tank of two halves,
 * each = modulated all-pass, delay, damping LP, all-pass, delay, cross-fed with
 * `decay`. Seven output taps per side straight from the paper. 100% wet (send).
 * amount = decay, macro = pre-delay + bandwidth, drift = damping + tank modulation. */
#define DS(x) (((x)*148181)/100000+1)     /* 29761 -> 44100, integer so the struct is fixed-size */
#define PL_MODX 40                          /* headroom for the modulated all-passes */
typedef struct {
    float in1[DS(142)], in2[DS(107)], in3[DS(379)], in4[DS(277)]; int p1,p2,p3,p4;
    float apL[DS(672)+PL_MODX], dL1[DS(4453)], ap2L[DS(1800)], dL2[DS(3720)]; int pAL,pDL1,pA2L,pDL2;
    float apR[DS(908)+PL_MODX], dR1[DS(4217)], ap2R[DS(2656)], dR2[DS(3163)]; int pAR,pDR1,pA2R,pDR2;
    float pre[DS(4096)]; int pPre;
    float bwZ, dampL, dampR, lfo;
} pfx_plate_t;
/* write x into a ring of size n at *p and return the sample that was there (delay of n) */
static inline float pl_dl(float *b, int n, int *p, float x){ float z=b[*p]; b[*p]=x; if(++*p>=n)*p=0; return z; }
/* tap: the sample written t samples ago (t < n) */
static inline float pl_tap(const float *b, int n, int p, int t){ int i=p-t; if(i<0)i+=n; return b[i]; }
/* fractional tap for the modulated all-passes */
static inline float pl_tapf(const float *b, int n, int p, float t){
    int ti=(int)t; float f=t-(float)ti; int i0=p-ti; if(i0<0)i0+=n; int i1=i0-1; if(i1<0)i1+=n;
    return b[i0]*(1.0f-f)+b[i1]*f; }
/* all-pass of nominal length n in a ring of size n (Dattorro sign convention) */
static inline float pl_ap(float *b, int n, int *p, float x, float g){
    float z=b[*p]; float v=x+g*z; float y=z-g*v; b[*p]=v; if(++*p>=n)*p=0; return y; }
/* modulated all-pass: nominal delay n, ring n+PL_MODX, read at n+exc */
static inline float pl_apm(float *b, int size, int n, int *p, float x, float g, float exc){
    float d=(float)n+exc; if(d<1.0f)d=1.0f; if(d>(float)(size-2))d=(float)(size-2);
    float z=pl_tapf(b,size,*p,d); float v=x+g*z; float y=z-g*v; b[*p]=v; if(++*p>=size)*p=0; return y; }

static void pfx_plate_step(pfx_plate_t *r, float inL, float inR, float *outL, float *outR,
                           float amount, float macro, float drift){
    const float decay=0.30f+amount*0.67f;                       /* 0.30 .. 0.97 */
    const float bw=0.9995f-macro*0.4f;                           /* macro darkens the input a little */
    const float damp=0.0005f+drift*0.5f;
    const float modDepth=(2.0f+drift*10.0f)*1.48181f;
    int preN=(int)(macro*macro*0.09f*SR); if(preN>DS(4096)-2)preN=DS(4096)-2;
    float x=(inL+inR)*0.5f;
    /* pre-delay (macro), then Dattorro's input bandwidth one-pole */
    r->pre[r->pPre]=x; { int i=r->pPre-preN; if(i<0)i+=DS(4096); x=r->pre[i]; } if(++r->pPre>=DS(4096))r->pPre=0;
    r->bwZ=x*bw+r->bwZ*(1.0f-bw); x=r->bwZ;
    x=pl_ap(r->in1,DS(142),&r->p1,x,0.75f);
    x=pl_ap(r->in2,DS(107),&r->p2,x,0.75f);
    x=pl_ap(r->in3,DS(379),&r->p3,x,0.625f);
    x=pl_ap(r->in4,DS(277),&r->p4,x,0.625f);
    /* tank: the two halves cross-feed each other's last delay */
    r->lfo+=1.0f/SR; if(r->lfo>=1.0f)r->lfo-=1.0f;
    float excL=modDepth*sinf(r->lfo*TWO_PI), excR=modDepth*cosf(r->lfo*TWO_PI);
    float fbL=pl_tap(r->dR2,DS(3163),r->pDR2,DS(3163)-1)*decay;   /* right half output -> left half */
    float fbR=pl_tap(r->dL2,DS(3720),r->pDL2,DS(3720)-1)*decay;
    float tL=x+fbL, tR=x+fbR;
    tL=pl_apm(r->apL,DS(672)+PL_MODX,DS(672),&r->pAL,tL,-0.70f,excL);
    tL=pl_dl(r->dL1,DS(4453),&r->pDL1,tL);
    r->dampL=tL*(1.0f-damp)+r->dampL*damp; tL=r->dampL*decay;
    tL=pl_ap(r->ap2L,DS(1800),&r->pA2L,tL,0.50f);
    pl_dl(r->dL2,DS(3720),&r->pDL2,tL);
    tR=pl_apm(r->apR,DS(908)+PL_MODX,DS(908),&r->pAR,tR,-0.70f,excR);
    tR=pl_dl(r->dR1,DS(4217),&r->pDR1,tR);
    r->dampR=tR*(1.0f-damp)+r->dampR*damp; tR=r->dampR*decay;
    tR=pl_ap(r->ap2R,DS(2656),&r->pA2R,tR,0.50f);
    pl_dl(r->dR2,DS(3163),&r->pDR2,tR);
    /* output taps (paper, table 2) */
    float yl= pl_tap(r->dR1,DS(4217),r->pDR1,DS(266))  + pl_tap(r->dR1,DS(4217),r->pDR1,DS(2974))
            - pl_tap(r->ap2R,DS(2656),r->pA2R,DS(1913)) + pl_tap(r->dR2,DS(3163),r->pDR2,DS(1996))
            - pl_tap(r->dL1,DS(4453),r->pDL1,DS(1990))  - pl_tap(r->ap2L,DS(1800),r->pA2L,DS(187))
            - pl_tap(r->dL2,DS(3720),r->pDL2,DS(1066));
    float yr= pl_tap(r->dL1,DS(4453),r->pDL1,DS(353))  + pl_tap(r->dL1,DS(4453),r->pDL1,DS(3627))
            - pl_tap(r->ap2L,DS(1800),r->pA2L,DS(1228)) + pl_tap(r->dL2,DS(3720),r->pDL2,DS(2673))
            - pl_tap(r->dR1,DS(4217),r->pDR1,DS(2111))  - pl_tap(r->ap2R,DS(2656),r->pA2R,DS(335))
            - pl_tap(r->dR2,DS(3163),r->pDR2,DS(121));
    *outL=yl*0.6f; *outR=yr*0.6f;
}
static void fx_plate(slot_dsp_t *d, float *l, float *r, int n, float amount, float macro, float drift){
    pfx_plate_t *p=(pfx_plate_t*)d->plate; if(!p)return;   /* NULL pool -> passthrough */
    for(int i=0;i<n;i++){ float wl,wr; pfx_plate_step(p,l[i],r[i],&wl,&wr,amount,macro,drift); l[i]=wl; r[i]=wr; }
}

/* ---- QUARTZ / PRISM — dual-mode modulated 8-line FDN reverb ---------------
 * Ported from the reverb shared by Res, Essaim and Phasma (filliformes), which
 * reproduces the two Ableton Max reverbs those instruments were built around:
 *
 *   QUARTZ — modulated 8x8 FDN with 4 series input-diffusion allpasses and
 *            dual-band damping (HF one-pole + LF shelf). Dattorro/Lexicon
 *            lineage; the classic room-to-hall voice (abl.dsp.quartz~ role).
 *   PRISM  — the same tank with FREQUENCY-DEPENDENT DECAY: each line's feedback
 *            splits into low/mid/high and every band decays on its own RT60,
 *            so the tail changes colour as it rings (abl.dsp.prism~ role).
 *
 * Topology follows the Ghidra-read structure of those objects; the matrix
 * (normalised Hadamard), delay lengths and coefficients are clean-room.
 * One pool serves both — only one can be selected on a slot at a time.
 * 100% wet, like Plate: Palette sits on a send bus.
 */
#define FDN_LINES 8
#define FDN_LMAX  4096           /* per-line ring: max line is 1617*2 = 3234 @44.1k */
#define FDN_NAP   4              /* input-diffusion allpasses */
#define FDN_APMAX 1024
#define FDN_PDMAX 8192           /* predelay ring (~186 ms) */

static const int   FDN_BASE[FDN_LINES] = { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 };
static const int   FDN_APLEN[FDN_NAP]  = { 225, 341, 441, 556 };
static const float FDN_APG[FDN_NAP]    = { 0.72f, 0.70f, 0.68f, 0.66f };

typedef struct {
    int    mode;                              /* 0 = Quartz, 1 = Prism */
    /* smoothed control values (one pole per block) */
    float  p_amt, p_mac, p_drf; int primed;
    /* derived params */
    double size, decay, damp, low_damp, diffusion, predelay_ms;
    double mod_depth, mod_rate, crossover, lowmult, highmult, locut_hz;
    /* tank */
    float  line[FDN_LINES][FDN_LMAX];
    int    linelen[FDN_LINES]; float linelen_s[FDN_LINES];   /* target + glided length */
    int    lw[FDN_LINES];
    float  damp_z[FDN_LINES], lo_z[FDN_LINES];               /* Quartz HF one-pole + LF shelf */
    float  ls_z[FDN_LINES], hs_z[FDN_LINES];                 /* Prism band splits */
    float  gain[FDN_LINES], glow[FDN_LINES], ghigh[FDN_LINES];
    double modph[FDN_LINES];
    float  ls_a, hs_a;
    /* input diffusion + predelay + wet low cut */
    float  ap[FDN_NAP][FDN_APMAX]; int apw[FDN_NAP];
    float  pd[2][FDN_PDMAX]; int pdw;
    float  hp_x[2], hp_y[2];
} pfx_fdn_t;

/* delay lengths and per-band decay gains from size / decay / crossover */
static void fdn_recompute(pfx_fdn_t *f){
    double scale = 0.35 + 1.65 * f->size;
    double rt60  = 0.25 + f->decay * f->decay * 11.75;          /* 0..1 -> 0.25..12 s */
    double lowsplit  = clampf((float)(400.0  * pow(4.0, f->crossover - 0.3)),   60.0f, 3000.0f);
    double highsplit = clampf((float)(5500.0 * pow(4.0, f->crossover - 0.3)), 1500.0f, 14000.0f);
    f->ls_a = (float)(1.0 - exp(-(double)TWO_PI * lowsplit  / SR));
    f->hs_a = (float)(1.0 - exp(-(double)TWO_PI * highsplit / SR));
    for(int i=0;i<FDN_LINES;i++){
        int L = (int)(FDN_BASE[i] * scale);
        if(L < 8) L = 8; if(L > FDN_LMAX - 64) L = FDN_LMAX - 64;
        f->linelen[i] = L;
        double t = (double)L / SR;
        double lm = f->lowmult  > 0.02 ? f->lowmult  : 0.02;
        double hm = f->highmult > 0.02 ? f->highmult : 0.02;
        f->gain[i]  = (float)pow(10.0, -3.0 * t / rt60);
        f->glow[i]  = (float)pow(10.0, -3.0 * t / (rt60 * lm));
        f->ghigh[i] = (float)pow(10.0, -3.0 * t / (rt60 * hm));
    }
}

static void fdn_init(pfx_fdn_t *f, int mode){
    f->mode = mode;
    f->size = 0.6; f->decay = 0.6; f->damp = 0.3; f->low_damp = 0.35;
    f->diffusion = 0.85; f->predelay_ms = 20.0;
    f->mod_depth = 6.0; f->mod_rate = 0.5;
    f->crossover = 0.3; f->lowmult = 1.0; f->highmult = 1.0; f->locut_hz = 200.0;
    f->primed = 0;
    for(int i=0;i<FDN_LINES;i++) f->modph[i] = (double)i / FDN_LINES;   /* staggered */
    fdn_recompute(f);
    for(int i=0;i<FDN_LINES;i++) f->linelen_s[i] = (float)f->linelen[i];
}

/* fractional read `delay` samples back. The ring is a FIXED FDN_LMAX, so the
 * effective length can glide — Size morphs tape-style instead of clicking. */
static inline float fdn_read(pfx_fdn_t *f, int i, double delay){
    double rp = (double)f->lw[i] - delay;
    while(rp < 0) rp += FDN_LMAX;
    int i0 = (int)rp; double frac = rp - i0;
    if(i0 >= FDN_LMAX) i0 -= FDN_LMAX;
    int i1 = i0 + 1; if(i1 >= FDN_LMAX) i1 = 0;
    return f->line[i][i0] * (float)(1.0 - frac) + f->line[i][i1] * (float)frac;
}
static inline float fdn_ap(pfx_fdn_t *f, int i, float x, float dscale){
    int L = FDN_APLEN[i], r = f->apw[i];
    float g = FDN_APG[i] * dscale, buf = f->ap[i][r];
    float y = -g * x + buf;
    f->ap[i][r] = x + g * y;
    f->apw[i] = (r + 1) % L;
    return y;
}

/* Block process, 100% wet. The per-line LFO is evaluated once per block and
 * ramped across it — 8 sin() per block instead of 8 per sample, and at <=2 Hz
 * the ramp is indistinguishable from the curve. */
static void fdn_process(pfx_fdn_t *f, float *l, float *r, int n){
    float damp_a  = clampf((float)(1.0 - f->damp * 0.85), 0.05f, 1.0f);
    float dscale  = (float)(0.15 + 0.85 * f->diffusion);
    float lowkeep = clampf((float)(1.0 - f->low_damp * 0.9), 0.05f, 1.0f);
    float locut_a = (float)((double)TWO_PI * f->locut_hz / SR); if(locut_a > 0.5f) locut_a = 0.5f;
    float hp_a    = 1.0f - locut_a; if(hp_a < 0.0f) hp_a = 0.0f;
    int pdlen = (int)(f->predelay_ms * 0.001 * SR);
    if(pdlen < 1) pdlen = 1; if(pdlen > FDN_PDMAX-1) pdlen = FDN_PDMAX-1;

    float mod0[FDN_LINES], modInc[FDN_LINES];
    for(int i=0;i<FDN_LINES;i++){
        double step = f->mod_rate * (0.7 + 0.09 * i) / SR;
        mod0[i] = (float)(f->mod_depth * sin((double)TWO_PI * f->modph[i]));
        double end = f->modph[i] + step * n;
        modInc[i] = (float)((f->mod_depth * sin((double)TWO_PI * end) - mod0[i]) / (n > 0 ? n : 1));
        f->modph[i] = end; while(f->modph[i] >= 1.0) f->modph[i] -= 1.0;
    }

    for(int s=0;s<n;s++){
        float d[FDN_LINES], fb[FDN_LINES];
        /* predelay */
        int rp = f->pdw - pdlen; while(rp < 0) rp += FDN_PDMAX;
        float pdL = f->pd[0][rp], pdR = f->pd[1][rp];
        f->pd[0][f->pdw] = l[s]; f->pd[1][f->pdw] = r[s];
        f->pdw = (f->pdw + 1) % FDN_PDMAX;
        /* input diffusion */
        float x = 0.5f * (pdL + pdR);
        for(int i=0;i<FDN_NAP;i++) x = fdn_ap(f, i, x, dscale);
        /* modulated fractional taps */
        for(int i=0;i<FDN_LINES;i++){
            f->linelen_s[i] += ((float)f->linelen[i] - f->linelen_s[i]) * 0.0006f;   /* ~35 ms glide */
            d[i] = fdn_read(f, i, (double)f->linelen_s[i] - 2.0 - (mod0[i] + modInc[i]*s));
        }
        /* 8-point normalised Hadamard (fast Walsh-Hadamard, 3 stages) */
        { float a[8];
          for(int i=0;i<8;i++) a[i] = d[i];
          for(int st=1; st<8; st<<=1)
              for(int j=0; j<8; j += (st<<1))
                  for(int k=0;k<st;k++){ float u=a[j+k], v=a[j+k+st]; a[j+k]=u+v; a[j+k+st]=u-v; }
          for(int i=0;i<8;i++) fb[i] = a[i] * 0.35355339f;   /* 1/sqrt(8) */
        }
        /* write back, coloured by the mode's absorption */
        for(int i=0;i<FDN_LINES;i++){
            float v = x + fb[i];
            if(f->mode){   /* PRISM: per-band decay */
                float low  = (f->ls_z[i] += f->ls_a * (v - f->ls_z[i]));
                float rem  = v - low;
                float mid  = (f->hs_z[i] += f->hs_a * (rem - f->hs_z[i]));
                float high = rem - mid;
                v = low * f->glow[i] + mid * f->gain[i] + high * f->ghigh[i];
            } else {       /* QUARTZ: HF damping -> decay -> LF shelf */
                f->damp_z[i] += damp_a * (v - f->damp_z[i]);
                v = f->damp_z[i] * f->gain[i];
                f->lo_z[i] += locut_a * (v - f->lo_z[i]);
                v = (v - f->lo_z[i]) + f->lo_z[i] * lowkeep;
            }
            if(v > 4.0f) v = 4.0f; else if(v < -4.0f) v = -4.0f;
            f->line[i][f->lw[i]] = v;
            f->lw[i] = (f->lw[i] + 1) % FDN_LMAX;
        }
        /* signed output taps (decorrelated), then a one-pole low cut on the wet */
        float oL = (d[0] - d[1] + d[2] - d[3] + d[6] - d[7]) * 0.4082f;   /* 1/sqrt(6) */
        float oR = (d[4] - d[5] + d[6] - d[7] + d[0] - d[2]) * 0.4082f;
        float hL = hp_a * (f->hp_y[0] + oL - f->hp_x[0]); f->hp_x[0] = oL; f->hp_y[0] = hL;
        float hR = hp_a * (f->hp_y[1] + oR - f->hp_x[1]); f->hp_x[1] = oR; f->hp_y[1] = hR;
        l[s] = hL; r[s] = hR;
    }
}

/* Shared control mapping. Params are smoothed per block, so a knob turn glides
 * the tank (size glides by construction, the decay gains follow the smoother). */
static void fdn_run(slot_dsp_t *dsp, float *l, float *r, int n, int mode,
                    float amount, float macro, float drift){
    pfx_fdn_t *f = (pfx_fdn_t*)dsp->fdn; if(!f) return;      /* NULL pool -> passthrough */
    if(f->mode != mode || !f->primed){ fdn_init(f, mode); }   /* fresh pool or mode switch */
    if(!f->primed){ f->p_amt = amount; f->p_mac = macro; f->p_drf = drift; f->primed = 1; }
    else { f->p_amt += (amount - f->p_amt) * 0.25f; f->p_mac += (macro - f->p_mac) * 0.25f;
           f->p_drf += (drift  - f->p_drf) * 0.25f; }
    float A = f->p_amt, M = f->p_mac, D = f->p_drf;
    if(mode == 0){
        /* QUARTZ — Amount: room to hall. Macro: dark to bright. Drift: still to swimming. */
        f->size  = 0.20 + 0.80 * A;
        f->decay = 0.15 + 0.85 * A;
        f->damp      = 0.90 * (1.0 - M);           /* HF absorption */
        f->low_damp  = 0.15 + 0.45 * (1.0 - M);    /* LF shelf */
        f->locut_hz  = 90.0 + 220.0 * M;
        f->diffusion = 0.55 + 0.45 * A;
        f->lowmult = f->highmult = 1.0;
        f->mod_depth   = 1.5 + 20.0 * D;
        f->mod_rate    = 0.20 + 1.60 * D;
        f->predelay_ms = 5.0 + 70.0 * D;
    } else {
        /* PRISM — Amount: decay. Macro: spectral tilt, lows ring <-> highs shimmer.
         * Drift: crossover placement + modulation. */
        f->size  = 0.45 + 0.55 * A;
        f->decay = 0.30 + 0.70 * A;
        f->damp = 0.10; f->low_damp = 0.20;
        f->diffusion = 0.85;
        { double t = (double)M * 2.0 - 1.0;        /* -1 dark/boomy .. +1 airy/shimmer */
          f->lowmult  = (t < 0.0) ? (1.0 - t * 2.0)   : (1.0 - t * 0.72);   /* 3.0 .. 0.28 */
          f->highmult = (t < 0.0) ? (1.0 + t * 0.72)  : (1.0 + t * 1.2); }  /* 0.28 .. 2.2 */
        f->crossover   = 0.12 + 0.55 * D;
        f->locut_hz    = 110.0 + 150.0 * M;
        f->mod_depth   = 3.0 + 14.0 * D;
        f->mod_rate    = 0.15 + 0.90 * D;
        f->predelay_ms = 10.0 + 50.0 * D;
    }
    fdn_recompute(f);
    fdn_process(f, l, r, n);
}
static void fx_quartz(slot_dsp_t *d, float *l, float *r, int n, float amount, float macro, float drift){
    fdn_run(d, l, r, n, 0, amount, macro, drift);
}
static void fx_prism(slot_dsp_t *d, float *l, float *r, int n, float amount, float macro, float drift){
    fdn_run(d, l, r, n, 1, amount, macro, drift);
}

/* ---- VEIL — modulated Householder FDN (Phasma's tank) ---------------------
 * Ported from Phasma (filliformes, objects/fx/fx.c). A different animal from
 * Quartz/Prism: the feedback is a HOUSEHOLDER reflection (m = x - (2/N)*sum)
 * rather than a Hadamard mix, the four input diffusers are themselves
 * LFO-MODULATED (the CloudSeed trick, with all LFOs refreshed every 8 samples),
 * the lines are long (23-83 ms) and damped in-loop. That combination is what
 * makes it lush and non-metallic where an 8-line tank usually rings.
 * Phasma's Prism band-split colouring rides along on Drift.
 * Mono tank (as in the source), stereo taps for width. 100% wet.
 */
#define VEIL_LINE 8
#define VEIL_DIFF 4
#define VEIL_LPOOL 50432         /* sum of per-line rings @44.1k (see VEIL_LMS) */
#define VEIL_DPOOL 5248          /* sum of per-diffuser rings */

static const float VEIL_LMS[VEIL_LINE]  = { 23.1f, 29.3f, 37.7f, 43.9f, 53.3f, 61.1f, 71.9f, 83.1f };
static const float VEIL_DMS[VEIL_DIFF]  = { 6.2f, 9.7f, 13.3f, 17.1f };
static const float VEIL_LMODR[VEIL_LINE]= { 0.071f,0.093f,0.111f,0.130f,0.147f,0.163f,0.181f,0.194f }; /* Hz */
static const float VEIL_DMODR[VEIL_DIFF]= { 0.91f, 1.13f, 1.37f, 1.61f };

typedef struct {
    int    ready;
    float  p_amt, p_mac, p_drf; int primed;
    /* flat pools + per-slot offsets (one alloc, no per-line buffers) */
    float  lbuf[VEIL_LPOOL]; int loff[VEIL_LINE], lmax[VEIL_LINE], llen[VEIL_LINE], lw[VEIL_LINE]; float llen_s[VEIL_LINE];
    float  dbuf[VEIL_DPOOL]; int doff[VEIL_DIFF], dmax[VEIL_DIFF], dlen[VEIL_DIFF], dw[VEIL_DIFF];
    double lph[VEIL_LINE], dph[VEIL_DIFF];
    float  llfo[VEIL_LINE], dlfo[VEIL_DIFF], lmod[VEIL_LINE], llp[VEIL_LINE];
    int    modctr;
    float  g, damp, modscale;
    float  pr_ls_z[VEIL_LINE], pr_hs_z[VEIL_LINE], pr_ls_a, pr_hs_a;
    float  glow, gmid, ghigh;
} pfx_veil_t;

static void veil_init(pfx_veil_t *v){
    int off = 0;
    for(int i=0;i<VEIL_LINE;i++){
        int m = (int)((VEIL_LMS[i] * 2.6f + 10.0f) * SR / 1000.0f);
        if(off + m > VEIL_LPOOL) m = VEIL_LPOOL - off;        /* pool guard */
        if(m < 8) m = 8;
        v->loff[i] = off; v->lmax[i] = m; off += m; v->llen[i] = (int)(VEIL_LMS[i] * SR / 1000.0f);
        v->lph[i] = 0.09 * i;
        v->llen_s[i] = (float)v->llen[i];
        v->lmod[i] = (float)((1.5 + 0.28 * i) * SR / 1000.0);  /* 1.5..3.5 ms */
    }
    off = 0;
    for(int i=0;i<VEIL_DIFF;i++){
        int m = (int)((VEIL_DMS[i] * 2.0f + 5.0f) * SR / 1000.0f);
        if(off + m > VEIL_DPOOL) m = VEIL_DPOOL - off;
        if(m < 8) m = 8;
        v->doff[i] = off; v->dmax[i] = m; off += m;
        v->dph[i] = 0.13 * i;
        v->dlen[i] = (int)(VEIL_DMS[i] * SR / 1000.0f);
        if(v->dlen[i] > v->dmax[i] - 2) v->dlen[i] = v->dmax[i] - 2;
    }
    v->g = 0.80f; v->damp = 0.35f; v->modscale = 1.0f;
    v->glow = v->gmid = v->ghigh = v->g;
    v->pr_ls_a = (float)(1.0 - exp(-(double)TWO_PI * 300.0 / SR));
    v->pr_hs_a = (float)(1.0 - exp(-(double)TWO_PI * 4200.0 / SR));
    v->ready = 1;
}

static inline float veil_read(const float *buf, int max, int w, float delay){
    if(delay < 1.0f) delay = 1.0f;
    float rp = (float)w - delay;
    while(rp < 0.0f) rp += max;
    int i0 = (int)rp; float fr = rp - i0; int i1 = i0 + 1; if(i1 >= max) i1 -= max;
    return buf[i0] * (1.0f - fr) + buf[i1] * fr;
}

static void fx_veil(slot_dsp_t *dsp, float *l, float *r, int n,
                    float amount, float macro, float drift){
    pfx_veil_t *v = (pfx_veil_t*)dsp->veil; if(!v) return;    /* NULL pool -> passthrough */
    if(!v->ready) veil_init(v);
    if(!v->primed){ v->p_amt = amount; v->p_mac = macro; v->p_drf = drift; v->primed = 1; }
    else { v->p_amt += (amount - v->p_amt) * 0.25f; v->p_mac += (macro - v->p_mac) * 0.25f;
           v->p_drf += (drift  - v->p_drf) * 0.25f; }
    float A = v->p_amt, M = v->p_mac, D = v->p_drf;

    /* Amount: room size + tail length. Macro: dark -> bright (in-loop damping).
     * Drift: movement + Phasma's Prism colouring (lows tighten, highs shimmer). */
    float sscale = 0.5f + (0.1f + 0.9f * A) * 1.4f;
    for(int i=0;i<VEIL_LINE;i++){
        int len = (int)(VEIL_LMS[i] * sscale * SR / 1000.0f);
        if(len < 2) len = 2; if(len > v->lmax[i] - 2) len = v->lmax[i] - 2;
        v->llen[i] = len;
    }
    v->g    = 0.55f + 0.37f * A;                     /* feedback, capped below 1 */
    v->damp = 0.06f + 0.86f * M;                     /* one-pole coeff: higher = brighter */
    v->modscale = 0.35f + 1.30f * D;
    { float lowmult  = 1.0f + (0.50f - 1.0f) * D;    /* lows decay up to 2x faster */
      float highmult = 1.0f + (1.18f - 1.0f) * D;    /* highs ring ~18% longer     */
      v->gmid  = v->g;
      v->glow  = v->g * lowmult;
      v->ghigh = v->g * highmult;
      if(v->ghigh > 0.94f) v->ghigh = 0.94f; }       /* keep feedback < 1 (stable) */

    for(int s=0;s<n;s++){
        /* all LFOs refreshed every 8 samples (cheap, still smooth) */
        if(++v->modctr >= 8){
            v->modctr = 0;
            for(int i=0;i<VEIL_DIFF;i++){
                v->dph[i] += VEIL_DMODR[i] * 8.0 / SR; if(v->dph[i] >= 1.0) v->dph[i] -= 1.0;
                v->dlfo[i] = (float)sin(v->dph[i] * (double)TWO_PI);
            }
            for(int i=0;i<VEIL_LINE;i++){
                v->lph[i] += VEIL_LMODR[i] * 8.0 / SR; if(v->lph[i] >= 1.0) v->lph[i] -= 1.0;
                v->llfo[i] = (float)sin(v->lph[i] * (double)TWO_PI);
            }
        }
        /* input diffusion: four series MODULATED allpasses */
        float x = 0.5f * (l[s] + r[s]);
        for(int d=0; d<VEIL_DIFF; d++){
            float *buf = v->dbuf + v->doff[d];
            float delay = v->dlen[d] + v->dlfo[d] * (0.3f * SR / 1000.0f) * v->modscale;
            float bufout = veil_read(buf, v->dmax[d], v->dw[d], delay);
            const float ag = 0.7f;
            float inv = x + bufout * ag;
            buf[v->dw[d]] = inv;
            v->dw[d] = (v->dw[d] + 1) % v->dmax[d];
            x = bufout - inv * ag;
        }
        float inj = x * 0.5f;

        /* FDN: modulated reads + in-loop damping, Householder mix, write back */
        float lo[VEIL_LINE], sum = 0.0f;
        for(int i=0;i<VEIL_LINE;i++){
            v->llen_s[i] += ((float)v->llen[i] - v->llen_s[i]) * 0.0006f;   /* ~35 ms glide: size morphs, no scratch */
            float delay = v->llen_s[i] + v->llfo[i] * v->lmod[i] * v->modscale;
            float rd = veil_read(v->lbuf + v->loff[i], v->lmax[i], v->lw[i], delay);
            v->llp[i] += v->damp * (rd - v->llp[i]);
            if(!(v->llp[i] > 1e-20f || v->llp[i] < -1e-20f)) v->llp[i] = 0.0f;   /* denormal flush */
            lo[i] = v->llp[i]; sum += lo[i];
        }
        float hf = (2.0f / VEIL_LINE) * sum;                  /* Householder reflection */
        for(int i=0;i<VEIL_LINE;i++){
            float m = lo[i] - hf;
            /* Prism band split: low+mid+high == m, so at Drift=0 this is exactly g*m */
            float low  = (v->pr_ls_z[i] += v->pr_ls_a * (m - v->pr_ls_z[i]));
            float rem  = m - low;
            float mid  = (v->pr_hs_z[i] += v->pr_hs_a * (rem - v->pr_hs_z[i]));
            float high = rem - mid;
            if(!(v->pr_ls_z[i] > 1e-20f || v->pr_ls_z[i] < -1e-20f)) v->pr_ls_z[i] = 0.0f;
            if(!(v->pr_hs_z[i] > 1e-20f || v->pr_hs_z[i] < -1e-20f)) v->pr_hs_z[i] = 0.0f;
            float w = inj + low * v->glow + mid * v->gmid + high * v->ghigh;
            if(!(w * w < 1e18f)) w = 0.0f;                    /* self-heal: never store inf/NaN */
            v->lbuf[v->loff[i] + v->lw[i]] = w;
            v->lw[i] = (v->lw[i] + 1) % v->lmax[i];
        }
        /* signed stereo taps off the damped line outputs (the source sums to mono) */
        float oL = (lo[0] - lo[1] + lo[2] - lo[3] + lo[6] - lo[7]) * 0.4082f;
        float oR = (lo[4] - lo[5] + lo[6] - lo[7] + lo[0] - lo[2]) * 0.4082f;
        if(!(oL * oL < 1e18f)) oL = 0.0f;
        if(!(oR * oR < 1e18f)) oR = 0.0f;
        l[s] = oL; r[s] = oR;
    }
}

static const palette_effect_t FX_TABLE[PFX_COUNT] = {
    [PFX_OFF]          = { fx_passthrough,   NULL },
    [PFX_DRIVE]        = { fx_drive,         NULL },
    [PFX_SWEETEN]      = { fx_sweeten,       NULL },
    [PFX_FUZZ]         = { fx_fuzz,          NULL },
    [PFX_HOWL]         = { fx_howl,          NULL },
    [PFX_SWELL]        = { fx_swell,         NULL },
    [PFX_FOLD]         = { fx_fold,          NULL },
    [PFX_DOUBLER]      = { fx_doubler,       NULL },
    [PFX_VIBRATO]      = { fx_vibrato,       NULL },
    [PFX_PHASER]       = { fx_phaser,        NULL },
    [PFX_TREMOLO]      = { fx_tremolo,       NULL },
    [PFX_PITCH]        = { fx_pitch,         NULL },
    [PFX_SHIFT]        = { fx_shift,         NULL },
    [PFX_CASCADE]      = { fx_cascade,       NULL },
    [PFX_REELS]        = { fx_reels,         NULL },
    [PFX_SPACE]        = { fx_passthrough,   NULL },  /* Clouds (heavy) — dispatched specially */
    [PFX_COLLAGE]      = { fx_collage,       NULL },
    [PFX_REVERSE]      = { fx_reverse,       NULL },
    [PFX_BLOOM]        = { fx_passthrough,   NULL },  /* Clouds (heavy) — dispatched specially */
    [PFX_FILTER]       = { fx_filter,        NULL },
    [PFX_SQUASH]       = { fx_squash,        NULL },
    [PFX_CASSETTE]     = { fx_cassette,      NULL },
    [PFX_BROKEN]       = { fx_broken,        NULL },
    [PFX_INTERFERENCE] = { fx_interference,  NULL },
    [PFX_HALO]         = { fx_halo,          NULL },  /* pure-C resonator pad (replaces FFT Freeze) */
    [PFX_PLATE]        = { fx_plate,         NULL },  /* Dattorro plate reverb (own state pool) */
    [PFX_QUARTZ]       = { fx_quartz,        NULL },  /* modulated 8-line FDN, dual-band damping */
    [PFX_PRISM]        = { fx_prism,         NULL },  /* same tank, frequency-dependent decay   */
    [PFX_VEIL]         = { fx_veil,          NULL },  /* Householder tank, modulated diffusers  */
};


/* ═══════════════════════════════════════════════════════════════════════════
 *  PUBLIC API (palette_fx.h) — opaque per-bus state, no slot_dsp_t leakage.
 * ═══════════════════════════════════════════════════════════════════════════ */
struct pfx_slot {
    slot_dsp_t dsp;
    int   select;                 /* PFX_* id (0 = Off) */
    void *space_pool, *bloom_pool;/* pre-allocated Clouds engines (SPACE / BLOOM) */
};

const char *pfx_name(int id){
    if(id < 0 || id >= PFX_COUNT) return "Off";
    return FX_NAMES[id];
}

/* Clear all per-effect scratch (replicates palette.c slot_reset): preserves the
 * delay-line allocation, the heavy (Clouds) pointer/kind and the RNG seed; zeroes
 * everything else and both delay buffers. Never frees the delay pointers. */
static void pfx_slot_reset(slot_dsp_t *d){
    float   *dl_l = d->dl_l, *dl_r = d->dl_r;
    void    *heavy = d->heavy; int hk = d->heavy_kind;
    void    *plate = d->plate;
    void    *fdn = d->fdn;
    void    *veil = d->veil;
    uint32_t seed = d->seed;
    memset(d, 0, sizeof(slot_dsp_t));
    d->dl_l = dl_l; d->dl_r = dl_r;
    d->heavy = heavy; d->heavy_kind = hk; d->seed = seed;
    d->plate = plate; d->fdn = fdn; d->veil = veil;
    if(dl_l) memset(dl_l, 0, MAX_DELAY*sizeof(float));
    if(dl_r) memset(dl_r, 0, MAX_DELAY*sizeof(float));
    if(plate) memset(plate, 0, sizeof(pfx_plate_t));   /* clear reverb tail on select change */
    if(fdn)   memset(fdn,   0, sizeof(pfx_fdn_t));     /* same for the FDN tank (re-inits on next block) */
    if(veil)  memset(veil,  0, sizeof(pfx_veil_t));    /* and the Veil tank */
}

pfx_slot *pfx_create(float sr){
    (void)sr;                                   /* engine is fixed at SR internally */
    pfx_slot *s = (pfx_slot*)calloc(1, sizeof(pfx_slot));
    if(!s) return NULL;
    s->dsp.dl_l = (float*)calloc(MAX_DELAY, sizeof(float));
    s->dsp.dl_r = (float*)calloc(MAX_DELAY, sizeof(float));
    if(!s->dsp.dl_l || !s->dsp.dl_r){
        free(s->dsp.dl_l); free(s->dsp.dl_r); free(s);
        return NULL;
    }
    s->dsp.seed = 0x9e3779b9u;
    s->dsp.plate = calloc(1, sizeof(pfx_plate_t));   /* NULL-safe: plate passes through on OOM */
    s->dsp.fdn   = calloc(1, sizeof(pfx_fdn_t));     /* NULL-safe: Quartz/Prism pass through on OOM */
    s->dsp.veil  = calloc(1, sizeof(pfx_veil_t));    /* NULL-safe: Veil passes through on OOM */
    s->select   = PFX_OFF;
    /* pre-allocate the heavy Clouds engines once, off the audio thread (NULL-safe:
     * a NULL pool makes the corresponding effect passthrough). */
    s->space_pool = pfx_clouds_alloc(PFX_SPACE, sr);
    s->bloom_pool = pfx_clouds_alloc(PFX_BLOOM, sr);
    return s;
}

void pfx_destroy(pfx_slot *s){
    if(!s) return;
    free(s->dsp.dl_l); free(s->dsp.dl_r);       /* heavy pools are separate */
    free(s->dsp.plate); free(s->dsp.fdn); free(s->dsp.veil);
    if(s->space_pool) pfx_clouds_free(s->space_pool);
    if(s->bloom_pool) pfx_clouds_free(s->bloom_pool);
    free(s);
}

void pfx_select(pfx_slot *s, int id){
    if(!s) return;
    if(id < PFX_OFF) id = PFX_OFF;
    if(id > PFX_COUNT-1) id = PFX_COUNT-1;
    /* reference the pre-allocated pool for Clouds effects (no runtime alloc); reset
     * it so no reverb tail carries into the new selection. */
    void *want = (id==PFX_SPACE) ? s->space_pool
               : (id==PFX_BLOOM) ? s->bloom_pool : NULL;
    if(want) pfx_clouds_reset(want);
    s->dsp.heavy      = want;
    s->dsp.heavy_kind = want ? id : 0;
    pfx_slot_reset(&s->dsp);                     /* clear scratch (heavy preserved) */
    s->select = id;
}

void pfx_process(pfx_slot *s, float *l, float *r, int n,
                 float amount, float macro, float drift){
    if(!s || s->select == PFX_OFF) return;       /* Off = passthrough */
    if(is_clouds_fx(s->select)){
        pfx_clouds_process(s->select, s->dsp.heavy, l, r, n, amount, macro, drift);
    } else {
        FX_TABLE[s->select].process(&s->dsp, l, r, n, amount, macro, drift);
    }
}
