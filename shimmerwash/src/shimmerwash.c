/* shimmerwash.c v0.3.0 — shoegaze shimmer reverb, Schwung audio FX (API v2).
 *
 *   INPUT      gain, HP/LP filter (freq, Q), pre-delay, bloom swell
 *   SHIFT A/B  pitch (semitones), fine (cents), delay, mode, amount, pan
 *   REVERB     length, size, color, mod rate, mod amount, spread, freeze
 *   DUCK       threshold, attack, release, depth (ducks the wet signal)
 *   MIX        dry/wet
 *
 * Shifter modes:
 *   PRE   shifter eats the reverb INPUT; pitched copy is sent into the reverb
 *   FDBK  shifter eats the reverb OUTPUT and feeds it back into the reverb
 *         input (classic shimmer: each pass climbs another interval)
 *   POST  shifter eats the reverb OUTPUT; pitched copy is added to the output
 *         only (parallel; never re-enters the loop)
 *
 * Reverb core: 8-line feedback delay network, Hadamard mixing, per-line
 * damping, slow modulated delay reads, 4 allpass diffusers per side.
 * Pitch shifting: two-tap crossfaded granular delay line (Hann windows).
 *
 * Host conventions followed (see the vendored host headers and the 4K EQ
 * module this layout is modelled on):
 *   - enums are read back as option NAMES and set by name or by number
 *   - keys may arrive as "<component>:<key>"; the prefix is ignored
 *   - ui_hierarchy is served by the plugin (named pages + preset browser)
 *   - state is one JSON object, restored literally
 *   - process_block never allocates, never logs, never touches the filesystem
 *
 * Original implementation from standard published techniques.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <stdint.h>

#include "host/audio_fx_api_v2.h"

#define SW_VERSION "0.3.0"

static const host_api_v1_t *g_host = NULL;

/* ---- parameters -------------------------------------------------------- */

enum {
    /* reverb */
    P_MIX, P_DECAY, P_SIZE, P_COLOR, P_MODRATE, P_MODAMT, P_PREDELAY, P_BLOOM,
    /* shifter A */
    P_A_PITCH, P_A_FINE, P_A_DELAY, P_A_MODE, P_A_AMT, P_A_PAN,
    /* shifter B */
    P_B_PITCH, P_B_FINE, P_B_DELAY, P_B_MODE, P_B_AMT, P_B_PAN,
    /* wash */
    P_SH_TONE, P_SH_LOW, P_SPREAD, P_FREEZE,
    /* input */
    P_IN_GAIN, P_FILT_TYPE, P_FILT_FREQ, P_FILT_Q,
    /* ducker */
    P_DUCK_AMT, P_DUCK_THR, P_DUCK_ATT, P_DUCK_REL,
    NPARAMS
};

typedef struct {
    const char *key;
    const char *name;                /* knob label; the PAGE names the context */
    int type;                        /* 0 int, 1 enum                          */
    int mn, mx, def;
    const char *unit;                /* NULL if none                           */
    const char *opts;                /* enum option names, comma-separated     */
    int viz_switch;                  /* on/off enum: draw as a switch          */
} pdef_t;

static const pdef_t kP[NPARAMS] = {
    { "mix",       "MIX",    0,    0,  100,  40, "%",  NULL, 0 },
    { "decay",     "LENGTH", 0,    0,  100,  55, NULL, NULL, 0 },
    { "size",      "SIZE",   0,    0,  100,  80, NULL, NULL, 0 },
    { "color",     "COLOR",  0,    0,  100,  40, NULL, NULL, 0 },
    { "modrate",   "M.RATE", 0,    0,  100,  50, NULL, NULL, 0 },
    { "modamt",    "M.AMT",  0,    0,  100,  50, NULL, NULL, 0 },
    { "predelay",  "PREDLY", 0,    0,  250,  40, "ms", NULL, 0 },
    { "bloom",     "BLOOM",  0,    0,  100,  35, NULL, NULL, 0 },

    { "a_pitch",   "PITCH",  0,  -24,   24,  12, "st", NULL, 0 },
    { "a_fine",    "FINE",   0, -100,  100,   6, "ct", NULL, 0 },
    { "a_delay",   "DELAY",  0,    0, 1000,   0, "ms", NULL, 0 },
    { "a_mode",    "MODE",   1,    0,    2,   1, NULL, "PRE,FDBK,POST", 0 },
    { "a_amt",     "AMT",    0,    0,  100,  40, NULL, NULL, 0 },
    { "a_pan",     "PAN",    0, -100,  100,   0, NULL, NULL, 0 },

    { "b_pitch",   "PITCH",  0,  -24,   24,   7, "st", NULL, 0 },
    { "b_fine",    "FINE",   0, -100,  100,  -6, "ct", NULL, 0 },
    { "b_delay",   "DELAY",  0,    0, 1000,   0, "ms", NULL, 0 },
    { "b_mode",    "MODE",   1,    0,    2,   1, NULL, "PRE,FDBK,POST", 0 },
    { "b_amt",     "AMT",    0,    0,  100,  40, NULL, NULL, 0 },
    { "b_pan",     "PAN",    0, -100,  100,   0, NULL, NULL, 0 },

    { "sh_tone",   "TONE",   0,    0,  100,  50, NULL, NULL, 0 },
    { "sh_low",    "LOCUT",  0,    0,  100,  54, NULL, NULL, 0 },
    { "spread",    "SPREAD", 0,    0,  100,  70, NULL, NULL, 0 },
    { "freeze",    "FREEZE", 1,    0,    1,   0, NULL, "Off,On", 1 },

    { "in_gain",   "GAIN",   0,  -12,   12,   0, "dB", NULL, 0 },
    { "filt_type", "TYPE",   1,    0,    1,   0, NULL, "HP,LP", 0 },
    { "filt_freq", "FREQ",   0,    0,  100,   0, NULL, NULL, 0 },
    { "filt_q",    "Q",      0,    0,  100,   7, NULL, NULL, 0 },

    { "duck_amt",  "DEPTH",  0,    0,  100,   0, NULL, NULL, 0 },
    { "duck_thr",  "THRESH", 0,  -60,    0, -24, "dB", NULL, 0 },
    { "duck_att",  "ATTACK", 0,    1,  200,  20, "ms", NULL, 0 },
    { "duck_rel",  "REL",    0,   10, 1000, 150, "ms", NULL, 0 },
};

enum { MODE_PRE = 0, MODE_FB = 1, MODE_POST = 2 };

/* ---- pages (ui_hierarchy) ---------------------------------------------- */
/* The root level is always shown by the host as "Main"; the others are named
 * pages reached from it. Every parameter appears on exactly one page. */

typedef struct { const char *id, *label; int n; int keys[8]; } level_t;

static const int kRootKeys[8] = { P_MIX, P_DECAY, P_SIZE, P_COLOR, P_MODRATE, P_MODAMT, P_PREDELAY, P_BLOOM };
static const level_t kLevels[] = {
    { "sha",   "SHFT A", 6, { P_A_PITCH, P_A_FINE, P_A_DELAY, P_A_MODE, P_A_AMT, P_A_PAN } },
    { "shb",   "SHFT B", 6, { P_B_PITCH, P_B_FINE, P_B_DELAY, P_B_MODE, P_B_AMT, P_B_PAN } },
    { "wash",  "WASH",   4, { P_SH_TONE, P_SH_LOW, P_SPREAD, P_FREEZE } },
    { "input", "INPUT",  4, { P_IN_GAIN, P_FILT_TYPE, P_FILT_FREQ, P_FILT_Q } },
    { "duck",  "DUCK",   4, { P_DUCK_AMT, P_DUCK_THR, P_DUCK_ATT, P_DUCK_REL } },
};
#define NLEVELS ((int)(sizeof(kLevels) / sizeof(kLevels[0])))

/* ---- factory presets ---------------------------------------------------- */
/* Each preset starts from INIT (every default) and overrides what it lists.
 * Freeze is a performance control and is never touched by a recall. */

typedef struct { int k, v; } kv_t;
typedef struct { const char *name; kv_t kv[16]; } preset_t;

#define END { -1, 0 }
static const preset_t kPresets[] = {
    { "INIT", { END } },
    { "Slow Bloom",     { {P_MIX,45},{P_DECAY,75},{P_SIZE,90},{P_COLOR,35},{P_BLOOM,80},{P_PREDELAY,80},{P_A_AMT,35},{P_B_AMT,25}, END } },
    { "Cathedral",      { {P_MIX,40},{P_DECAY,85},{P_SIZE,100},{P_COLOR,50},{P_B_PITCH,19},{P_A_AMT,45},{P_B_AMT,20},{P_MODAMT,60}, END } },
    { "Octave Down",    { {P_A_PITCH,-12},{P_A_AMT,50},{P_B_PITCH,12},{P_B_AMT,30},{P_COLOR,30},{P_DECAY,70}, END } },
    { "Angel Choir",    { {P_A_MODE,MODE_PRE},{P_A_PITCH,12},{P_A_AMT,60},{P_B_MODE,MODE_PRE},{P_B_PITCH,7},{P_B_AMT,45},{P_DECAY,60},{P_SIZE,85},{P_BLOOM,50}, END } },
    { "Fifths Tower",   { {P_A_PITCH,7},{P_A_AMT,45},{P_B_PITCH,19},{P_B_AMT,35},{P_DECAY,80},{P_SIZE,95},{P_COLOR,45}, END } },
    { "Dark Drift",     { {P_DECAY,90},{P_COLOR,15},{P_A_PITCH,-12},{P_A_AMT,40},{P_B_AMT,25},{P_MODAMT,70},{P_MODRATE,35},{P_MIX,50}, END } },
    { "Echo Climb",     { {P_A_AMT,50},{P_A_DELAY,315},{P_B_AMT,40},{P_B_DELAY,626},{P_DECAY,65},{P_SIZE,85}, END } },
    { "Endless Pad",    { {P_DECAY,100},{P_SIZE,100},{P_A_AMT,55},{P_A_FINE,12},{P_B_PITCH,12},{P_B_AMT,45},{P_B_FINE,-12},{P_BLOOM,60},{P_MIX,55}, END } },
    { "Parallel Sheen", { {P_A_MODE,MODE_POST},{P_A_AMT,60},{P_B_MODE,MODE_POST},{P_B_PITCH,19},{P_B_AMT,30},{P_DECAY,55},{P_MIX,40}, END } },
    { "Tight Sparkle",  { {P_DECAY,30},{P_SIZE,40},{P_COLOR,70},{P_A_MODE,MODE_POST},{P_A_AMT,50},{P_B_MODE,MODE_POST},{P_B_PITCH,24},{P_B_AMT,25},{P_MIX,30},{P_PREDELAY,10},{P_BLOOM,0}, END } },
    { "Ducked Halo",    { {P_DECAY,70},{P_DUCK_AMT,70},{P_DUCK_THR,-30},{P_DUCK_REL,300},{P_MIX,50}, END } },
    { "Swell Pad",      { {P_BLOOM,100},{P_PREDELAY,120},{P_DECAY,80},{P_A_MODE,MODE_PRE},{P_A_AMT,55},{P_B_MODE,MODE_PRE},{P_B_PITCH,7},{P_B_AMT,40},{P_MIX,55}, END } },
    { "Wide Open",      { {P_SPREAD,100},{P_MODAMT,80},{P_MODRATE,35},{P_SIZE,100},{P_DECAY,80},{P_A_PAN,-50},{P_A_AMT,45},{P_B_PAN,50},{P_B_AMT,35}, END } },
};
#undef END
#define NPRESETS ((int)(sizeof(kPresets) / sizeof(kPresets[0])))

/* ---- constants --------------------------------------------------------- */

#define TWO_PI      6.2831853f
#define PI_F        3.14159265f

#define NLINES      8
#define LINE_SZ     4096
#define LINE_MASK   (LINE_SZ - 1)
#define MOD_MAX     30.0f

#define PD_SZ       16384
#define PD_MASK     (PD_SZ - 1)

#define NAP         4
#define AP_MAX      512

#define SH_SZ       4096
#define SH_MASK     (SH_SZ - 1)
#define SH_W        2048.0f

#define DL_SZ       65536
#define DL_MASK     (DL_SZ - 1)

#define INJ_GAIN    0.35f      /* shimmer-feedback injection per FDN line   */
#define IN_GAIN     0.12f      /* dry-input injection per FDN line          */
#define OUT_GAIN    0.50f      /* FDN tap sum (feeds shifters)              */
#define OUT_MAKEUP  2.5f       /* wet output level, outside the loop        */
#define SH_MAX      0.45f      /* feedback gain at Amount = 100 (per shifter) */
#define AP_G        0.55f

static const int   kLineLen[NLINES] = { 1409, 1693, 1931, 2273, 2579, 2927, 3307, 3677 };
static const float kLfoHz[NLINES]   = { 0.13f, 0.19f, 0.23f, 0.29f, 0.31f, 0.37f, 0.41f, 0.43f };
static const float kInjSign[NLINES] = { 1, 1, -1, 1, -1, -1, 1, -1 };
static const int   kApLenL[NAP]     = { 142, 107, 379, 277 };
static const int   kApLenR[NAP]     = { 151, 113, 397, 293 };

typedef struct { float buf[AP_MAX]; int idx, len; } ap_t;

typedef struct {
    float shb[2][SH_SZ];            /* grain history, L/R                     */
    int   wp;
    float d;                        /* grain read phase [0, SH_W)             */
    float dl[2][DL_SZ];             /* post-shift delay line                  */
    int   dwp;
    float sm_delay;                 /* smoothed delay, samples                */
    float lp[2][2], hp[2];          /* tone filters                           */
    float ratio, amt, gl, gr, tgt_delay;
    int   mode, swap;
} shifter_t;

typedef struct {
    int v[NPARAMS];
    int preset;                     /* browser position, not a DSP parameter  */
    float sr;

    /* FDN */
    float line[NLINES][LINE_SZ];
    int   lwp;
    float lp[NLINES], lfo_ph[NLINES];

    /* input / pre-delay / diffusion */
    float pd[2][PD_SZ];
    int   pdwp;
    ap_t  apL[NAP], apR[NAP];
    float svf_ic1[2], svf_ic2[2];

    shifter_t sh[2];

    /* derived targets */
    float g[NLINES];
    float damp_a, sh_lp_a, sh_hp_a;
    float sh_comp, in_comp;
    float tgt_size, lfo_mult;
    float svf_a1, svf_a2, svf_a3, svf_k;

    /* smoothed controls */
    float sm_wet, sm_dry, sm_fz, sm_spread, sm_depth, sm_pd, sm_size, sm_ing, sm_duck;

    float env_f, env_s;             /* bloom swell followers                  */
    float env_d;                    /* ducker detector                        */
    float dcx[2], dcy[2];
} sw_t;

/* ---- helpers ----------------------------------------------------------- */

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static inline float fast_sin(float p)
{
    float x = (p < 0.5f ? p : p - 1.0f) * TWO_PI;
    float y = 1.27323954f * x - 0.40528473f * x * fabsf(x);
    return 0.225f * (y * fabsf(y) - y) + y;
}

static inline float wrap01(float p) { return p - floorf(p); }

static inline float rd(const float *b, int mask, int wp, float d)
{
    int   di = (int)d;
    float fr = d - (float)di;
    float a  = b[(wp - di) & mask];
    float c  = b[(wp - di - 1) & mask];
    return a + fr * (c - a);
}

static inline float ap_proc(ap_t *a, float x)
{
    float d = a->buf[a->idx];
    float w = x + AP_G * d;
    float y = d - AP_G * w;
    a->buf[a->idx] = w;
    if (++a->idx >= a->len) a->idx = 0;
    return y;
}

static void mix_gains(int mix, float *wet, float *dry)
{
    float m = clampi(mix, 0, 100) * 0.01f * 1.5707963f;
    *wet = sinf(m);
    *dry = cosf(m);
}

/* Control-thread only: derives every coefficient from the parameter values. */
static void recompute(sw_t *p)
{
    const int *v = p->v;
    const float sr = p->sr;

    p->tgt_size = 0.35f + 0.65f * v[P_SIZE] * 0.01f;
    float rt60  = 0.8f * powf(75.0f, v[P_DECAY] * 0.01f);          /* 0.8..60 s */
    for (int i = 0; i < NLINES; i++)
        p->g[i] = powf(10.0f, -3.0f * (float)kLineLen[i] * p->tgt_size / (sr * rt60));

    float fc = 1200.0f * powf(10.0f, v[P_COLOR] * 0.01f);          /* 1.2..12 kHz */
    p->damp_a = 1.0f - expf(-TWO_PI * fc / sr);

    float fsh = 2500.0f * powf(3.6f, v[P_SH_TONE] * 0.01f);        /* 2.5..9 kHz */
    p->sh_lp_a = 1.0f - expf(-TWO_PI * fsh / sr);
    float fhp = 40.0f * powf(10.0f, v[P_SH_LOW] * 0.01f);          /* 40..400 Hz */
    p->sh_hp_a = 1.0f - expf(-TWO_PI * fhp / sr);

    p->lfo_mult = 0.25f * powf(16.0f, v[P_MODRATE] * 0.01f);       /* 0.25x..4x */

    /* Loop-gain safety: a long decay accumulates ~1/(1-g^2). Shimmer-feedback
     * injection is scaled by the inverse square root, and the input send
     * partially, so level and stability hold from short to near-infinite. */
    {
        const float g0 = powf(10.0f, -3.0f * 2273.0f / (sr * 8.0f));
        float gm = p->g[3];
        float c  = sqrtf((1.0f - gm * gm) / (1.0f - g0 * g0));
        p->sh_comp = c > 1.3f ? 1.3f : c;
        float ci = powf((1.0f - gm) / (1.0f - g0), 0.6f);
        p->in_comp = ci < 0.15f ? 0.15f : (ci > 2.0f ? 2.0f : ci);
    }

    /* Input filter (TPT state-variable, 12 dB/oct). */
    {
        float f = 20.0f * powf(1000.0f, v[P_FILT_FREQ] * 0.01f);
        float fmax = 0.44f * sr;
        if (f > fmax) f = fmax;
        float g = tanf(PI_F * f / sr);
        float q = 0.5f + v[P_FILT_Q] * 0.03f;
        p->svf_k  = 1.0f / q;
        p->svf_a1 = 1.0f / (1.0f + g * (g + p->svf_k));
        p->svf_a2 = g * p->svf_a1;
        p->svf_a3 = g * p->svf_a2;
    }

    const int ip[2] = { P_A_PITCH, P_B_PITCH }, iff[2] = { P_A_FINE, P_B_FINE };
    const int id[2] = { P_A_DELAY, P_B_DELAY }, im[2] = { P_A_MODE, P_B_MODE };
    const int ia[2] = { P_A_AMT, P_B_AMT },     in_[2] = { P_A_PAN, P_B_PAN };
    for (int k = 0; k < 2; k++) {
        shifter_t *s = &p->sh[k];
        s->ratio     = powf(2.0f, ((float)v[ip[k]] + v[iff[k]] * 0.01f) / 12.0f);
        s->amt       = v[ia[k]] * 0.01f;
        s->mode      = v[im[k]];
        float dly    = v[id[k]] * 0.001f * sr;
        s->tgt_delay = dly > (float)(DL_SZ - 4) ? (float)(DL_SZ - 4) : dly;
        float pan    = v[in_[k]] * 0.01f;
        s->gl = pan > 0.0f ? 1.0f - pan : 1.0f;
        s->gr = pan < 0.0f ? 1.0f + pan : 1.0f;
        s->swap = (k == 1);                 /* B is cross-coupled for width */
    }
}

static void apply_preset(sw_t *p, int idx)
{
    idx = clampi(idx, 0, NPRESETS - 1);
    for (int i = 0; i < NPARAMS; i++)
        if (i != P_FREEZE) p->v[i] = kP[i].def;
    for (const kv_t *e = kPresets[idx].kv; e->k >= 0; e++)
        p->v[e->k] = clampi(e->v, kP[e->k].mn, kP[e->k].mx);
    p->preset = idx;
    recompute(p);
}

/* ---- one shifter, one sample ------------------------------------------- */

static inline void shifter_run(shifter_t *s, float srcL, float srcR,
                               float lp_a, float hp_a, float *outL, float *outR)
{
    s->shb[0][s->wp] = srcL;
    s->shb[1][s->wp] = srcR;
    const int wp = s->wp;

    float ph = s->d * (1.0f / SH_W);
    float w1 = 0.5f - 0.5f * fast_sin(wrap01(ph + 0.25f));
    float d2 = s->d + SH_W * 0.5f; if (d2 >= SH_W) d2 -= SH_W;

    float yL = w1 * rd(s->shb[0], SH_MASK, wp, s->d) + (1.0f - w1) * rd(s->shb[0], SH_MASK, wp, d2);
    float yR = w1 * rd(s->shb[1], SH_MASK, wp, s->d) + (1.0f - w1) * rd(s->shb[1], SH_MASK, wp, d2);

    s->wp = (s->wp + 1) & SH_MASK;
    s->d += 1.0f - s->ratio;
    if (s->d < 0.0f)   s->d += SH_W;
    if (s->d >= SH_W)  s->d -= SH_W;

    s->sm_delay += (s->tgt_delay - s->sm_delay) * 0.0003f;
    s->dl[0][s->dwp] = yL;
    s->dl[1][s->dwp] = yR;
    yL = rd(s->dl[0], DL_MASK, s->dwp, s->sm_delay);
    yR = rd(s->dl[1], DL_MASK, s->dwp, s->sm_delay);
    s->dwp = (s->dwp + 1) & DL_MASK;

    float in2[2] = { yL, yR }, o[2];
    for (int c = 0; c < 2; c++) {
        s->lp[c][0] += lp_a * (in2[c]      - s->lp[c][0]);
        s->lp[c][1] += lp_a * (s->lp[c][0] - s->lp[c][1]);
        s->hp[c]    += hp_a * (s->lp[c][1] - s->hp[c]);
        o[c] = s->lp[c][1] - s->hp[c];
    }
    if (s->swap) { float t = o[0]; o[0] = o[1]; o[1] = t; }
    *outL = o[0] * s->gl;
    *outR = o[1] * s->gr;
}

/* ---- lifecycle --------------------------------------------------------- */

static void *v2_create_instance(const char *module_dir, const char *config_json)
{
    (void)module_dir; (void)config_json;
    sw_t *p = (sw_t *)calloc(1, sizeof(sw_t));
    if (!p) return NULL;

    p->sr = (g_host && g_host->sample_rate > 0) ? (float)g_host->sample_rate : 44100.0f;

    for (int i = 0; i < NPARAMS; i++) p->v[i] = kP[i].def;
    for (int i = 0; i < NAP; i++) { p->apL[i].len = kApLenL[i]; p->apR[i].len = kApLenR[i]; }
    for (int i = 0; i < NLINES; i++) p->lfo_ph[i] = (float)i / (float)NLINES;
    p->sh[0].d = 0.0f;
    p->sh[1].d = SH_W * 0.37f;

    recompute(p);
    mix_gains(p->v[P_MIX], &p->sm_wet, &p->sm_dry);
    p->sm_size   = p->tgt_size;
    p->sm_spread = 0.5f + 1.1f * p->v[P_SPREAD] * 0.01f;
    p->sm_depth  = MOD_MAX * p->v[P_MODAMT] * 0.01f;
    p->sm_pd     = p->v[P_PREDELAY] * 0.001f * p->sr;
    p->sm_ing    = powf(10.0f, p->v[P_IN_GAIN] / 20.0f);
    p->sm_duck   = 1.0f;
    p->sh[0].sm_delay = p->sh[0].tgt_delay;
    p->sh[1].sm_delay = p->sh[1].tgt_delay;
    return p;
}

static void v2_destroy_instance(void *instance) { free(instance); }

/* ---- audio ------------------------------------------------------------- */

static void v2_process_block(void *instance, int16_t *io, int frames)
{
    sw_t *p = (sw_t *)instance;
    if (!p || !io || frames <= 0) return;

#if defined(__aarch64__)
    /* Flush denormals to zero: long reverb tails decay into them. */
    uint64_t fpcr_saved;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr_saved));
    __asm__ volatile("msr fpcr, %0" : : "r"(fpcr_saved | (1ULL << 24)));
#endif

    const int *v = p->v;
    const float sr = p->sr;
    float tgt_wet, tgt_dry;
    mix_gains(v[P_MIX], &tgt_wet, &tgt_dry);
    const float tgt_fz     = v[P_FREEZE] ? 1.0f : 0.0f;
    const float tgt_spread = 0.5f + 1.1f * v[P_SPREAD] * 0.01f;
    const float tgt_depth  = MOD_MAX * v[P_MODAMT] * 0.01f;
    float tgt_pd           = v[P_PREDELAY] * 0.001f * sr;
    if (tgt_pd > (float)(PD_SZ - 4)) tgt_pd = (float)(PD_SZ - 4);
    const float tgt_ing    = powf(10.0f, v[P_IN_GAIN] / 20.0f);
    const int   filt_lp    = v[P_FILT_TYPE] == 1;

    const int   swell_on = v[P_BLOOM] > 3;
    const float swell_n  = v[P_BLOOM] * 0.004f * sr + 1.0f;
    const float a_att    = 1.0f - expf(-1.0f / swell_n);
    const float rel      = expf(-1.0f / (0.06f * sr));

    const int   duck_on    = v[P_DUCK_AMT] > 0;
    const float duck_depth = v[P_DUCK_AMT] * 0.3f;                 /* dB */
    const float duck_thr   = (float)v[P_DUCK_THR];
    const float d_att = 1.0f - expf(-1.0f / (v[P_DUCK_ATT] * 0.001f * sr));
    const float d_rel = 1.0f - expf(-1.0f / (v[P_DUCK_REL] * 0.001f * sr));

    const float sm = 0.002f, smf = 0.0005f, smp = 0.0004f, sms = 0.0003f;

    for (int n = 0; n < frames; n++) {
        const float inL = io[n * 2 + 0] * (1.0f / 32768.0f);
        const float inR = io[n * 2 + 1] * (1.0f / 32768.0f);

        p->sm_wet    += (tgt_wet    - p->sm_wet)    * sm;
        p->sm_dry    += (tgt_dry    - p->sm_dry)    * sm;
        p->sm_spread += (tgt_spread - p->sm_spread) * sm;
        p->sm_depth  += (tgt_depth  - p->sm_depth)  * sm;
        p->sm_ing    += (tgt_ing    - p->sm_ing)    * sm;
        p->sm_pd     += (tgt_pd     - p->sm_pd)     * smp;
        p->sm_fz     += (tgt_fz     - p->sm_fz)     * smf;
        p->sm_size   += (p->tgt_size - p->sm_size)  * sms;
        const float fz   = p->sm_fz;
        const float live = 1.0f - fz;

        /* ---- 1. FDN read + stereo taps ---- */
        float vv[NLINES], t[NLINES];
        const float damp = p->damp_a + (1.0f - p->damp_a) * fz;
        for (int i = 0; i < NLINES; i++) {
            p->lfo_ph[i] += kLfoHz[i] * p->lfo_mult / sr;
            if (p->lfo_ph[i] >= 1.0f) p->lfo_ph[i] -= 1.0f;
            float D   = (float)kLineLen[i] * p->sm_size + p->sm_depth * fast_sin(p->lfo_ph[i]);
            float raw = rd(p->line[i], LINE_MASK, p->lwp, D);
            p->lp[i] += damp * (raw - p->lp[i]);
            t[i]  = p->lp[i];
            float ge = p->g[i] + (0.99999f - p->g[i]) * fz;
            vv[i] = p->lp[i] * ge;
        }
        const float tapL = OUT_GAIN * (t[0] - t[2] + t[4] - t[6]);
        const float tapR = OUT_GAIN * (t[1] - t[3] - t[5] + t[7]);

        /* ---- 2. input chain: gain -> filter -> bloom swell -> pre-delay ---- */
        float sL = inL * p->sm_ing, sR = inR * p->sm_ing;
        {
            float xs[2] = { sL, sR };
            for (int c = 0; c < 2; c++) {
                float v3 = xs[c] - p->svf_ic2[c];
                float v1 = p->svf_a1 * p->svf_ic1[c] + p->svf_a2 * v3;
                float v2 = p->svf_ic2[c] + p->svf_a2 * p->svf_ic1[c] + p->svf_a3 * v3;
                p->svf_ic1[c] = 2.0f * v1 - p->svf_ic1[c];
                p->svf_ic2[c] = 2.0f * v2 - p->svf_ic2[c];
                xs[c] = filt_lp ? v2 : (xs[c] - p->svf_k * v1 - v2);
            }
            sL = xs[0]; sR = xs[1];
        }
        float swell = 1.0f;
        {
            float lvl = fabsf(sL) > fabsf(sR) ? fabsf(sL) : fabsf(sR);
            p->env_f = lvl > p->env_f ? lvl : p->env_f * rel;
            if (p->env_f > p->env_s) p->env_s += (p->env_f - p->env_s) * a_att;
            else                     p->env_s  = p->env_f;
            if (swell_on) {
                swell = p->env_s / (p->env_f + 1e-5f);
                if (swell > 1.0f) swell = 1.0f;
            }
        }
        p->pd[0][p->pdwp] = sL * swell;
        p->pd[1][p->pdwp] = sR * swell;
        float pdd = p->sm_pd < 1.0f ? 1.0f : p->sm_pd;
        float xL = rd(p->pd[0], PD_MASK, p->pdwp + 1, pdd);
        float xR = rd(p->pd[1], PD_MASK, p->pdwp + 1, pdd);
        p->pdwp = (p->pdwp + 1) & PD_MASK;

        /* ---- 3. pitch shifters: route by mode ---- */
        float pre[2] = { 0, 0 }, post[2] = { 0, 0 }, fbs[2] = { 0, 0 };
        for (int k = 0; k < 2; k++) {
            shifter_t *s = &p->sh[k];
            float srcL = (s->mode == MODE_PRE) ? xL : tapL;
            float srcR = (s->mode == MODE_PRE) ? xR : tapR;
            float yL, yR;
            shifter_run(s, srcL, srcR, p->sh_lp_a, p->sh_hp_a, &yL, &yR);
            if (s->mode == MODE_PRE) {
                pre[0] += yL * s->amt;  pre[1] += yR * s->amt;
            } else if (s->mode == MODE_FB) {
                float g = s->amt * SH_MAX * live * p->sh_comp;
                fbs[0] += tanhf(yL * g); fbs[1] += tanhf(yR * g);
            } else {
                post[0] += yL * s->amt; post[1] += yR * s->amt;
            }
        }

        /* ---- 4. diffusion + injection ---- */
        xL += pre[0]; xR += pre[1];
        for (int i = 0; i < NAP; i++) { xL = ap_proc(&p->apL[i], xL); xR = ap_proc(&p->apR[i], xR); }
        const float inScale = live * IN_GAIN * p->in_comp;
        const float uL = xL * inScale, uR = xR * inScale;

        /* ---- 5. Hadamard mix + write ---- */
        for (int h = 1; h < NLINES; h <<= 1)
            for (int i = 0; i < NLINES; i += h << 1)
                for (int j = i; j < i + h; j++) {
                    float a = vv[j], b = vv[j + h];
                    vv[j] = a + b; vv[j + h] = a - b;
                }
        for (int i = 0; i < NLINES; i++) {
            float inj = kInjSign[i] * (((i & 1) ? uR : uL) + ((i & 1) ? fbs[1] : fbs[0]) * INJ_GAIN);
            p->line[i][p->lwp] = vv[i] * 0.35355339f + inj;
        }
        p->lwp = (p->lwp + 1) & LINE_MASK;

        /* ---- 6. wet output: POST shifters, ducker, spread, DC, mix ---- */
        float wetL = tapL + post[0], wetR = tapR + post[1];

        float gd = 1.0f;
        if (duck_on) {
            float lvl = fabsf(inL) > fabsf(inR) ? fabsf(inL) : fabsf(inR);
            p->env_d += (lvl - p->env_d) * (lvl > p->env_d ? d_att : d_rel);
            float db   = 20.0f * log10f(p->env_d + 1e-6f);
            float over = db - duck_thr;
            float red  = over > 0.0f ? over * 2.0f : 0.0f;
            if (red > duck_depth) red = duck_depth;
            gd = expf(-red * 0.11512925f);
        }
        p->sm_duck += (gd - p->sm_duck) * 0.01f;
        wetL *= p->sm_duck; wetR *= p->sm_duck;

        float M = 0.5f * (wetL + wetR), S = 0.5f * (wetL - wetR) * p->sm_spread;
        float oWL = tanhf((M + S) * OUT_MAKEUP), oWR = tanhf((M - S) * OUT_MAKEUP);
        float yL = oWL - p->dcx[0] + 0.999f * p->dcy[0]; p->dcx[0] = oWL; p->dcy[0] = yL;
        float yR = oWR - p->dcx[1] + 0.999f * p->dcy[1]; p->dcx[1] = oWR; p->dcy[1] = yR;

        float oL = (yL * p->sm_wet + inL * p->sm_dry) * 32768.0f;
        float oR = (yR * p->sm_wet + inR * p->sm_dry) * 32768.0f;
        if (oL >  32767.0f) oL =  32767.0f;
        if (oL < -32768.0f) oL = -32768.0f;
        if (oR >  32767.0f) oR =  32767.0f;
        if (oR < -32768.0f) oR = -32768.0f;
        io[n * 2 + 0] = (int16_t)oL;
        io[n * 2 + 1] = (int16_t)oR;
    }

#if defined(__aarch64__)
    __asm__ volatile("msr fpcr, %0" : : "r"(fpcr_saved));
#endif
}

/* ---- param plumbing ----------------------------------------------------- */

static int pindex(const char *key)
{
    for (int i = 0; i < NPARAMS; i++)
        if (!strcmp(key, kP[i].key)) return i;
    return -1;
}

/* Enum option lookup: exact name, then case-insensitive. */
static int enum_find(const pdef_t *d, const char *val)
{
    const size_t vl = strlen(val);
    const char *s = d->opts;
    int idx = 0;
    while (*s) {
        const char *e = strchr(s, ',');
        size_t l = e ? (size_t)(e - s) : strlen(s);
        if (l == vl && !strncasecmp(s, val, l)) return idx;
        idx++;
        s += l;
        if (*s == ',') s++;
    }
    return -1;
}

static int enum_name(const pdef_t *d, int idx, char *out, int cap)
{
    const char *s = d->opts;
    int i = 0;
    while (*s) {
        const char *e = strchr(s, ',');
        int l = e ? (int)(e - s) : (int)strlen(s);
        if (i == idx) return snprintf(out, (size_t)cap, "%.*s", l, s);
        i++;
        s += l;
        if (*s == ',') s++;
    }
    return -1;
}

static int json_int(const char *json, const char *key, int *out)
{
    char pat[48];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(json, pat);
    if (!p) return -1;
    p += strlen(pat);
    while (*p == ' ') p++;
    *out = atoi(p);
    return 0;
}

static void v2_set_param(void *instance, const char *key, const char *val)
{
    sw_t *p = (sw_t *)instance;
    if (!p || !key || !val) return;

    const char *colon = strrchr(key, ':');
    if (colon) key = colon + 1;

    if (!strcmp(key, "state")) {
        int x;
        if (json_int(val, "sw", &x) != 0) return;          /* not our blob */
        for (int i = 0; i < NPARAMS; i++)
            if (json_int(val, kP[i].key, &x) == 0)
                p->v[i] = clampi(x, kP[i].mn, kP[i].mx);
        /* Browser position is restored WITHOUT recalling: the values in the
         * blob are the truth, and re-applying would discard later edits. */
        if (json_int(val, "preset", &x) == 0) p->preset = clampi(x, 0, NPRESETS - 1);
        recompute(p);
        return;
    }
    if (!strcmp(key, "preset")) { apply_preset(p, atoi(val)); return; }

    int i = pindex(key);
    if (i < 0) return;
    int x;
    if (kP[i].type == 1) {
        x = enum_find(&kP[i], val);
        if (x < 0) x = atoi(val);
    } else {
        x = atoi(val);
    }
    p->v[i] = clampi(x, kP[i].mn, kP[i].mx);
    recompute(p);
}

#define APP(...)                                                              \
    do {                                                                      \
        int r_ = snprintf(buf + n, (n < len ? (size_t)(len - n) : 0), __VA_ARGS__); \
        if (r_ < 0) return -1;                                                \
        n += r_;                                                              \
        if (n >= len) return -1;                                              \
    } while (0)

static int build_chain_params(char *buf, int len)
{
    int n = 0;
    APP("[");
    for (int i = 0; i < NPARAMS; i++) {
        const pdef_t *d = &kP[i];
        if (i) APP(",");
        APP("{\"key\":\"%s\",\"name\":\"%s\",", d->key, d->name);
        if (d->type == 0) {
            APP("\"type\":\"int\",\"min\":%d,\"max\":%d,\"default\":%d", d->mn, d->mx, d->def);
        } else {
            APP("\"type\":\"enum\",\"options\":[");
            const char *s = d->opts;
            int first = 1;
            while (*s) {
                const char *e = strchr(s, ',');
                int l = e ? (int)(e - s) : (int)strlen(s);
                APP("%s\"%.*s\"", first ? "" : ",", l, s);
                first = 0;
                s += l;
                if (*s == ',') s++;
            }
            APP("],\"default\":%d", d->def);
        }
        if (d->unit) APP(",\"unit\":\"%s\"", d->unit);
        if (d->viz_switch) APP(",\"viz\":{\"kind\":\"switch\"}");
        APP("}");
    }
    APP("]");
    return n;
}

static int build_ui_hierarchy(char *buf, int len)
{
    int n = 0;
    APP("{\"modes\":null,\"levels\":{");

    /* root: shown by the host as "Main"; links to every other page */
    APP("\"root\":{\"name\":\"MAIN\",\"children\":null,\"knobs\":[");
    for (int i = 0; i < 8; i++) APP("%s\"%s\"", i ? "," : "", kP[kRootKeys[i]].key);
    APP("],\"params\":[");
    for (int i = 0; i < 8; i++)
        APP("%s{\"key\":\"%s\",\"label\":\"%s\"}", i ? "," : "", kP[kRootKeys[i]].key, kP[kRootKeys[i]].name);
    for (int l = 0; l < NLEVELS; l++)
        APP(",{\"level\":\"%s\",\"label\":\"%s\"}", kLevels[l].id, kLevels[l].label);
    APP(",{\"level\":\"presets\",\"label\":\"PRESET\"}]},");

    for (int l = 0; l < NLEVELS; l++) {
        const level_t *lv = &kLevels[l];
        APP("\"%s\":{\"name\":\"%s\",\"children\":null,\"knobs\":[", lv->id, lv->label);
        for (int i = 0; i < lv->n; i++) APP("%s\"%s\"", i ? "," : "", kP[lv->keys[i]].key);
        APP("],\"params\":[");
        for (int i = 0; i < lv->n; i++)
            APP("%s{\"key\":\"%s\",\"label\":\"%s\"}", i ? "," : "", kP[lv->keys[i]].key, kP[lv->keys[i]].name);
        APP("]},");
    }
    /* Preset browser: its own page kind, deliberately no "knobs". */
    APP("\"presets\":{\"name\":\"PRESET\",\"children\":null,"
        "\"list_param\":\"preset\",\"count_param\":\"preset_count\",\"name_param\":\"preset_name\"}");
    APP("}}");
    return n;
}

static int build_state(const sw_t *p, char *buf, int len)
{
    int n = 0;
    APP("{\"sw\":1,\"build\":\"" SW_VERSION "\"");
    for (int i = 0; i < NPARAMS; i++) APP(",\"%s\":%d", kP[i].key, p->v[i]);
    APP(",\"preset\":%d}", p->preset);
    return n;
}

static int v2_get_param(void *instance, const char *key, char *buf, int buf_len)
{
    sw_t *p = (sw_t *)instance;
    if (!p || !key || !buf || buf_len <= 1) return -1;

    const char *colon = strrchr(key, ':');
    if (colon) key = colon + 1;

    if (!strcmp(key, "name"))          return snprintf(buf, (size_t)buf_len, "Shimwash");
    if (!strcmp(key, "chain_params"))  return build_chain_params(buf, buf_len);
    if (!strcmp(key, "ui_hierarchy"))  return build_ui_hierarchy(buf, buf_len);
    if (!strcmp(key, "state"))         return build_state(p, buf, buf_len);

    if (!strcmp(key, "preset"))        return snprintf(buf, (size_t)buf_len, "%d", p->preset);
    if (!strcmp(key, "preset_count"))  return snprintf(buf, (size_t)buf_len, "%d", NPRESETS);
    if (!strcmp(key, "preset_name"))   return snprintf(buf, (size_t)buf_len, "%s", kPresets[clampi(p->preset, 0, NPRESETS - 1)].name);

    int i = pindex(key);
    if (i < 0) return -1;
    if (kP[i].type == 1) {
        int r = enum_name(&kP[i], clampi(p->v[i], kP[i].mn, kP[i].mx), buf, buf_len);
        return r;
    }
    return snprintf(buf, (size_t)buf_len, "%d", p->v[i]);
}

/* ---- entry point ------------------------------------------------------- */

#ifndef SHIMMERWASH_TEST
static audio_fx_api_v2_t g_fx_api_v2;

audio_fx_api_v2_t *move_audio_fx_init_v2(const host_api_v1_t *host)
{
    g_host = host;
    memset(&g_fx_api_v2, 0, sizeof(g_fx_api_v2));
    g_fx_api_v2.api_version      = AUDIO_FX_API_VERSION_2;
    g_fx_api_v2.create_instance  = v2_create_instance;
    g_fx_api_v2.destroy_instance = v2_destroy_instance;
    g_fx_api_v2.process_block    = v2_process_block;
    g_fx_api_v2.set_param        = v2_set_param;
    g_fx_api_v2.get_param        = v2_get_param;
    if (host && host->log) host->log("shimmerwash " SW_VERSION ": loaded");
    return &g_fx_api_v2;
}
#endif
