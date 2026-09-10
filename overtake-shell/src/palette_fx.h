#ifndef PALETTE_FX_H
#define PALETTE_FX_H
#define PFX_NUM 26                       /* Off + 24 + Plate */
const char *pfx_name(int id);            /* id 0..25; "Off","Drive",...,"Plate" */
typedef struct pfx_slot pfx_slot;        /* opaque per-bus state */
pfx_slot *pfx_create(float sr);          /* allocs slot state incl delay lines + clouds pools; NULL on OOM. NOT audio-thread. */
void pfx_destroy(pfx_slot *s);
void pfx_select(pfx_slot *s, int id);    /* switch effect: reset state, lazy alloc/free clouds. NOT audio-thread (may alloc). */
void pfx_process(pfx_slot *s, float *l, float *r, int n, float amount, float macro, float drift); /* audio-thread safe, in-place stereo float +-1; handles OFF(passthrough)/clouds/C effects */
#endif
