/* PALETTE — Clouds-backed heavy effects (SPACE / BLOOM / FREEZE).
 * Author: Filliformes. License: MIT.
 *
 * Fully isolated C++ behind 3 extern "C" calls; palette.c holds only a void*.
 * Only the clouds_engine include root is on this TU's path (see scripts/build.sh
 * -Ivendor/clouds_engine) — never the Warps stmlib, so no ODR collision.
 *
 *   SPACE  — Mutable Clouds Reverb (Griesinger/Dattorro tank). The lush reverb.
 *   BLOOM  — Clouds Reverb + octave-up shimmer regeneration.
 *   FREEZE — granular magnitude-freeze: dense overlapping windowed grains from a
 *            frozen capture buffer (spectral-style smear without an FFT).
 *
 * Effect ids MUST match the PFX_* enum in palette.c.
 */

#include <new>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "clouds/dsp/frame.h"
#include "clouds/dsp/fx/reverb.h"

using namespace clouds;

enum { FX_SPACE = 17, FX_BLOOM = 18 };  /* MUST match palette.c PFX_* */

/* (FREEZE removed — replaced by the pure-C HALO resonator in palette.c.) */

#define CLD_SR     44100.0f
#define CLD_MAXBLK 128
#ifndef CLD_TWO_PI
#define CLD_TWO_PI 6.28318530718f
#endif

static inline float cclampf(float x,float lo,float hi){ return x<lo?lo:(x>hi?hi:x); }
static inline float clerp(float a,float b,float t){ return a+(b-a)*t; }
static inline uint32_t crng(uint32_t *s){ uint32_t x=*s; x^=x<<13; x^=x>>17; x^=x<<5; *s=x; return x; }
static inline float crandf(uint32_t *s){ return (crng(s)>>8)*(1.0f/16777216.0f); }

/* Every heavy state begins with `kind` so free() can dispatch from a bare void*. */
struct HeavyHdr { int kind; };

/* ── SPACE ─────────────────────────────────────────────────────────────────── */
struct SpaceState {
    int      kind;
    Reverb   verb;
    uint16_t buf[16384];
};
static SpaceState *space_new(){
    SpaceState *st = new(std::nothrow) SpaceState;
    if(!st) return NULL;
    st->kind = FX_SPACE;
    memset(st->buf, 0, sizeof(st->buf));
    st->verb.Init(st->buf);
    st->verb.set_input_gain(0.2f);
    st->verb.set_diffusion(0.625f);
    st->verb.set_lp(0.7f);
    return st;
}
static void space_process(SpaceState *st, float *l, float *r, int n,
                          float amount, float macro, float drift){
    st->verb.set_amount(1.0f);                              /* fully wet internally; mix outside */
    st->verb.set_time(0.25f + macro*0.70f);                 /* small at macro=0 → bigger */
    st->verb.set_diffusion(cclampf(0.5f + macro*0.3f + drift*0.1f,0.0f,0.9f));
    st->verb.set_lp(cclampf(0.7f - drift*0.25f,0.2f,0.95f));/* slow tone wander */
    FloatFrame fr[CLD_MAXBLK];
    if(n>CLD_MAXBLK) n=CLD_MAXBLK;
    float dryL[CLD_MAXBLK], dryR[CLD_MAXBLK];
    for(int i=0;i<n;i++){ dryL[i]=l[i]; dryR[i]=r[i]; fr[i].l=l[i]; fr[i].r=r[i]; }
    st->verb.Process(fr, (size_t)n);
    float wet=cclampf(amount,0.0f,1.0f);
    for(int i=0;i<n;i++){
        /* cross-bleed guarantees BOTH channels carry the tail (no single-side reverb) */
        float wl=fr[i].l*0.7f + fr[i].r*0.3f;
        float wr=fr[i].r*0.7f + fr[i].l*0.3f;
        l[i]=clerp(dryL[i], wl, wet);                       /* amount=0 → dry */
        r[i]=clerp(dryR[i], wr, wet);
    }
}

/* ── BLOOM ─────────────────────────────────────────────────────────────────── */
#define BLOOM_SH 8192                                       /* ~0.18 s shimmer buf */
struct BloomState {
    int      kind;
    Reverb   verb;
    uint16_t buf[16384];
    float    shl[BLOOM_SH], shr[BLOOM_SH];                  /* shimmer (octave-up) ring */
    int      shwp;
    float    grpos;                                         /* grain read phase */
    uint32_t seed;
};
static BloomState *bloom_new(){
    BloomState *st = new(std::nothrow) BloomState;
    if(!st) return NULL;
    st->kind = FX_BLOOM;
    memset(st->buf,0,sizeof(st->buf));
    memset(st->shl,0,sizeof(st->shl)); memset(st->shr,0,sizeof(st->shr));
    st->shwp=0; st->grpos=0.0f; st->seed=0x1234abcdu;
    st->verb.Init(st->buf);
    st->verb.set_input_gain(0.2f);
    st->verb.set_diffusion(0.7f);
    st->verb.set_lp(0.6f);
    return st;
}
static inline float bloom_read(const float *b,int wp,float d){
    float rp=(float)wp-d; while(rp<0)rp+=BLOOM_SH; while(rp>=BLOOM_SH)rp-=BLOOM_SH;
    int i0=(int)rp; float fr=rp-(float)i0; int i1=i0+1; if(i1>=BLOOM_SH)i1=0;
    return b[i0]+(b[i1]-b[i0])*fr;
}
static void bloom_process(BloomState *st, float *l, float *r, int n,
                          float amount, float macro, float drift){
    /* reverb first (lush, long) */
    st->verb.set_amount(1.0f);
    st->verb.set_time(0.30f + macro*0.65f);                 /* shorter at macro=0 → long bloom at max */
    st->verb.set_diffusion(0.7f);
    st->verb.set_lp(cclampf(0.55f - drift*0.2f,0.2f,0.9f));
    FloatFrame fr[CLD_MAXBLK];
    if(n>CLD_MAXBLK) n=CLD_MAXBLK;
    for(int i=0;i<n;i++){ fr[i].l=l[i]; fr[i].r=r[i]; }
    st->verb.Process(fr,(size_t)n);
    /* octave-up shimmer with regeneration; window = grain over half the buffer */
    float shfb = amount*0.85f;                               /* shimmer feedback */
    float win  = (float)BLOOM_SH*0.5f;
    for(int i=0;i<n;i++){
        st->grpos += 1.0f;  if(st->grpos>=win) st->grpos-=win;  /* +1 oct = 2x read */
        float p2=st->grpos+win*0.5f; if(p2>=win)p2-=win;
        float w1=0.5f-0.5f*cosf(CLD_TWO_PI*st->grpos/win), w2=1.0f-w1;
        float oL=bloom_read(st->shl,st->shwp,win-st->grpos)*w1+bloom_read(st->shl,st->shwp,win-p2)*w2;
        float oR=bloom_read(st->shr,st->shwp,win-st->grpos)*w1+bloom_read(st->shr,st->shwp,win-p2)*w2;
        /* write reverb tail + shimmer feedback into the shimmer ring */
        float wl=fr[i].l + oL*shfb, wr=fr[i].r + oR*shfb;
        wl=tanhf(wl); wr=tanhf(wr);
        st->shl[st->shwp]=wl; st->shr[st->shwp]=wr;
        st->shwp=(st->shwp+1)%BLOOM_SH;
        float wetL=fr[i].l + oL*amount, wetR=fr[i].r + oR*amount;
        float mxl=wetL*0.7f+wetR*0.3f, mxr=wetR*0.7f+wetL*0.3f;  /* stereo-safe */
        l[i]=clerp(l[i],mxl,amount);         /* amount=0 → dry */
        r[i]=clerp(r[i],mxr,amount);
    }
}

/* FREEZE is implemented in fx_spectral.cc (Signalsmith FFT) and reached through
 * the heavy dispatch below — its state also begins with `kind == FX_FREEZE`. */

/* ── extern "C" interface (called from palette.c) ────────────────────────────── */
extern "C" void *pfx_clouds_alloc(int fx_id, float sr){
    switch(fx_id){
        case FX_SPACE:  return (void*)space_new();
        case FX_BLOOM:  return (void*)bloom_new();
        default:        return NULL;
    }
}
/* Reset a pre-allocated heavy engine to a fresh state (called when a slot newly
 * takes it — no runtime alloc; kills any carried reverb tail). */
extern "C" void pfx_clouds_reset(void *heavy){
    if(!heavy) return;
    HeavyHdr *h=(HeavyHdr*)heavy;
    if(h->kind==FX_SPACE){
        SpaceState *st=(SpaceState*)heavy;
        memset(st->buf,0,sizeof(st->buf));
        st->verb.Init(st->buf);
        st->verb.set_input_gain(0.2f); st->verb.set_diffusion(0.625f); st->verb.set_lp(0.7f);
    } else if(h->kind==FX_BLOOM){
        BloomState *st=(BloomState*)heavy;
        memset(st->buf,0,sizeof(st->buf));
        memset(st->shl,0,sizeof(st->shl)); memset(st->shr,0,sizeof(st->shr));
        st->shwp=0; st->grpos=0.0f;
        st->verb.Init(st->buf);
        st->verb.set_input_gain(0.2f); st->verb.set_diffusion(0.7f); st->verb.set_lp(0.6f);
    }
}
extern "C" void pfx_clouds_free(void *heavy){
    if(!heavy) return;
    HeavyHdr *h=(HeavyHdr*)heavy;
    switch(h->kind){
        case FX_SPACE:  delete (SpaceState*)heavy; break;
        case FX_BLOOM:  delete (BloomState*)heavy; break;
        default: break;
    }
}
extern "C" void pfx_clouds_process(int fx_id, void *heavy, float *l, float *r, int n,
                                   float amount, float macro, float drift){
    if(!heavy) return;
    switch(fx_id){
        case FX_SPACE:  space_process ((SpaceState*)heavy, l,r,n,amount,macro,drift); break;
        case FX_BLOOM:  bloom_process ((BloomState*)heavy, l,r,n,amount,macro,drift); break;
        default: break;
    }
}
