/*
 * LBX Shell — minimal Overtake DSP (generator/jack role) + RAM probe.
 *
 * Validates the Overtake SDK plumbing AND the LoopBox memory risk:
 *   - exports move_plugin_init_v2 (generator role)
 *   - answers get_param("module_id") = "lbxshell"  (Manager discovery)
 *   - RAM PROBE: create_instance runs the "shrinking-capacity fallback ladder" —
 *     tries 16 tracks x 45s stereo int16 (~121 MB), stepping the seconds down
 *     until every track's calloc succeeds. Then render_block INCREMENTALLY
 *     touches the pages (a little per block, on the audio thread — cheap and
 *     safe) to force real commit, so we learn whether the RAM actually holds
 *     under the Move, not just whether a lazy overcommit calloc returned.
 *   - get_param("ram") reports the achieved config + commit progress for the UI.
 *
 * render_block outputs silence; audio isn't what this validates.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "plugin_api_v1.h"

static const host_api_v1_t *g_host = NULL;

#define SR            44100
#define TARGET_TRACKS 16
#define COMMIT_PER_BLOCK 262144   /* int16 elems touched per render block (~512KB) */

/* seconds ladder: most ambitious first */
static const int SECS_LADDER[] = { 45, 40, 35, 30, 25, 20, 15, 10, 5 };
#define N_LADDER ((int)(sizeof(SECS_LADDER)/sizeof(SECS_LADDER[0])))

typedef struct {
    char   hello[32];

    /* RAM probe */
    int      tracks;              /* achieved track count (0 = alloc failed) */
    int      secs;                /* achieved seconds/track */
    size_t   samples;             /* frames per channel = secs*SR */
    int16_t *flat[TARGET_TRACKS * 2];   /* L,R,L,R... one calloc each */
    int      n_bufs;              /* = tracks*2 */
    size_t   total_elems;         /* n_bufs * samples (commit target) */
    size_t   cur_b, cur_off;      /* incremental commit cursor */
    size_t   committed_elems;
    int      committed;
} shell_t;

static void free_bufs(shell_t *s) {
    for (int i = 0; i < TARGET_TRACKS * 2; i++) { free(s->flat[i]); s->flat[i] = NULL; }
    s->n_bufs = 0;
}

/* Try to allocate `tracks*2` buffers of `samples` int16 each. 1 = all ok. */
static int try_alloc(shell_t *s, int tracks, size_t samples) {
    for (int i = 0; i < tracks * 2; i++) {
        s->flat[i] = (int16_t *)calloc(samples, sizeof(int16_t));
        if (!s->flat[i]) { for (int j = 0; j < i; j++) { free(s->flat[j]); s->flat[j] = NULL; } return 0; }
    }
    s->n_bufs = tracks * 2;
    return 1;
}

static void *shell_create(const char *module_dir, const char *json_defaults) {
    (void)module_dir; (void)json_defaults;
    shell_t *s = (shell_t *)calloc(1, sizeof(shell_t));
    if (!s) return NULL;
    snprintf(s->hello, sizeof(s->hello), "%s", "idle");

    /* fallback ladder: fixed 16 tracks, step seconds down until it fits */
    s->tracks = 0;
    for (int k = 0; k < N_LADDER; k++) {
        size_t samples = (size_t)SECS_LADDER[k] * SR;
        if (try_alloc(s, TARGET_TRACKS, samples)) {
            s->tracks  = TARGET_TRACKS;
            s->secs    = SECS_LADDER[k];
            s->samples = samples;
            break;
        }
    }
    if (s->tracks) {
        s->total_elems = (size_t)s->n_bufs * s->samples;
        s->cur_b = 0; s->cur_off = 0; s->committed_elems = 0; s->committed = 0;
    }
    return s;
}

static void shell_destroy(void *inst) {
    if (inst) free_bufs((shell_t *)inst);
    free(inst);
}

static void shell_on_midi(void *inst, const uint8_t *msg, int len, int source) {
    (void)inst; (void)msg; (void)len; (void)source;
}

static void shell_set_param(void *inst, const char *key, const char *val) {
    shell_t *s = (shell_t *)inst;
    if (!s || !key || !val) return;
    if (strcmp(key, "hello") == 0) snprintf(s->hello, sizeof(s->hello), "%s", val);
}

static int shell_get_param(void *inst, const char *key, char *buf, int buf_len) {
    shell_t *s = (shell_t *)inst;
    if (!key || !buf || buf_len <= 0) return -1;
    if (strcmp(key, "module_id") == 0)
        return snprintf(buf, buf_len, "%s", "lbxshell");
    if (s && strcmp(key, "ram") == 0) {
        if (s->tracks == 0)
            return snprintf(buf, buf_len, "ALLOC FAILED");
        double mb = (double)s->total_elems * sizeof(int16_t) / 1.0e6;
        if (s->committed)
            return snprintf(buf, buf_len, "%dx%ds OK %.0fMB", s->tracks, s->secs, mb);
        int pct = s->total_elems ? (int)(100 * s->committed_elems / s->total_elems) : 0;
        return snprintf(buf, buf_len, "%dx%ds commit %d%%", s->tracks, s->secs, pct);
    }
    if (s && strcmp(key, "hello") == 0)
        return snprintf(buf, buf_len, "%s", s->hello);
    return -1;
}

static int shell_get_error(void *inst, char *buf, int buf_len) {
    (void)inst; (void)buf; (void)buf_len; return 0;
}

static void shell_render_block(void *inst, int16_t *out_interleaved_lr, int frames) {
    shell_t *s = (shell_t *)inst;
    if (out_interleaved_lr) memset(out_interleaved_lr, 0, (size_t)frames * 2 * sizeof(int16_t));

    /* Incrementally commit the probe buffers: touch up to COMMIT_PER_BLOCK
     * int16s, walking (buffer, offset) without any per-element division. */
    if (s && s->tracks && !s->committed) {
        size_t n = COMMIT_PER_BLOCK;
        while (n-- && !s->committed) {
            s->flat[s->cur_b][s->cur_off] = 0;
            s->committed_elems++;
            if (++s->cur_off >= s->samples) {
                s->cur_off = 0;
                if (++s->cur_b >= (size_t)s->n_bufs) s->committed = 1;
            }
        }
    }
}

static plugin_api_v2_t g_api = {
    .api_version      = MOVE_PLUGIN_API_VERSION_2,
    .create_instance  = shell_create,
    .destroy_instance = shell_destroy,
    .on_midi          = shell_on_midi,
    .set_param        = shell_set_param,
    .get_param        = shell_get_param,
    .get_error        = shell_get_error,
    .render_block     = shell_render_block,
};

plugin_api_v2_t *move_plugin_init_v2(const host_api_v1_t *host) {
    g_host = host;
    return &g_api;
}
