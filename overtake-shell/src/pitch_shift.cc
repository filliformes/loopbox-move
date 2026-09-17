/*
 * pitch_shift.cc — per-loop pitch shifter for Loopex.
 *
 * Wraps Signalsmith Stretch 1.1.0 (MIT, Geraint Luff) — a phase-vocoder
 * style shifter with real transient/tonality handling — behind a tiny C API,
 * the same way fx_clouds.cc isolates the Clouds engine. loopex.c only holds
 * a void*. Configured at block 2048 / interval 512 (46 ms / 11.6 ms): total
 * latency ~ one block, which loopex.c compensates by nudging the loop's
 * playheads forward when the shifter engages, so a shifted loop stays in
 * time with the others.
 */
#include <new>
#include <cstdlib>
#include <cstring>
#include "signalsmith-stretch.h"

struct ps_t {
    signalsmith::stretch::SignalsmithStretch<float> st;
    float *inL, *inR, *outL, *outR;
    int blockMax;
};

extern "C" {

void *ps_create(float sr, int blockMax, int staggerSamples) {
    ps_t *p = new (std::nothrow) ps_t();
    if (!p) return nullptr;
    (void)sr;
    p->st.configure(2, 2048, 512);
    p->blockMax = blockMax;
    p->inL = (float*)calloc((size_t)blockMax, sizeof(float));
    p->inR = (float*)calloc((size_t)blockMax, sizeof(float));
    p->outL = (float*)calloc((size_t)blockMax, sizeof(float));
    p->outR = (float*)calloc((size_t)blockMax, sizeof(float));
    if (!p->inL || !p->inR || !p->outL || !p->outR) { free(p->inL); free(p->inR); free(p->outL); free(p->outR); delete p; return nullptr; }
    p->st.setTransposeSemitones(0.0f);
    /* Stagger the STFT interval phase across instances so 16 voices do not all
     * run their FFT in the same audio callback. */
    if (staggerSamples > 0) {
        float *z[2] = { (float*)calloc((size_t)staggerSamples, sizeof(float)), (float*)calloc((size_t)staggerSamples, sizeof(float)) };
        if (z[0] && z[1]) p->st.process(z, staggerSamples, z, staggerSamples);
        free(z[0]); free(z[1]);
    }
    return p;
}

void ps_destroy(void *h) {
    ps_t *p = (ps_t*)h; if (!p) return;
    free(p->inL); free(p->inR); free(p->outL); free(p->outR);
    delete p;
}

void ps_reset(void *h) { ps_t *p = (ps_t*)h; if (p) p->st.reset(); }

int ps_latency(void *h) { ps_t *p = (ps_t*)h; return p ? p->st.inputLatency() + p->st.outputLatency() : 0; }

void ps_set_semitones(void *h, float semitones) { ps_t *p = (ps_t*)h; if (p) p->st.setTransposeSemitones(semitones, 0.0f); }

/* In-place, n <= blockMax. Audio-thread safe: no allocation after create. */
void ps_process(void *h, float *l, float *r, int n) {
    ps_t *p = (ps_t*)h; if (!p || n <= 0) return;
    if (n > p->blockMax) n = p->blockMax;
    memcpy(p->inL, l, (size_t)n * sizeof(float)); memcpy(p->inR, r, (size_t)n * sizeof(float));
    float *in[2] = { p->inL, p->inR }, *out[2] = { p->outL, p->outR };
    p->st.process(in, n, out, n);
    memcpy(l, p->outL, (size_t)n * sizeof(float)); memcpy(r, p->outR, (size_t)n * sizeof(float));
}

} /* extern "C" */
