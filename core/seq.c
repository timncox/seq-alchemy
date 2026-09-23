/* seq -- see seq.h for the rules this file keeps. */
#include "seq.h"

#include <math.h>
#include <string.h>

#define START_EPS 1e-6    /* the transport starts this far before tick 0 so
                             that step one falls inside the first window */
#define RETRIG_US 2000    /* gate dip between two separate notes */
#define MAX_SEMIS 60      /* 5 V: notes above fold down an octave */

/* ---------------------------------------------------------------- scales */

static const int8_t kScales[SEQ_SCALE_COUNT][7] = {
    {0, 2, 4, 5, 7, 9, 11}, /* major */
    {0, 2, 3, 5, 7, 8, 10}, /* natural minor */
    {0, 2, 3, 5, 7, 9, 10}, /* dorian */
    {0, 1, 3, 5, 7, 8, 10}, /* phrygian */
    {0, 2, 4, 6, 7, 9, 11}, /* lydian */
    {0, 2, 4, 5, 7, 9, 10}, /* mixolydian */
    {0, 2, 3, 5, 7, 8, 11}, /* harmonic minor */
    {0, 3, 5, 7, 10},       /* minor pentatonic */
    {0, 2, 4, 7, 9},        /* major pentatonic */
};
static const uint8_t kScaleLen[SEQ_SCALE_COUNT] = {7, 7, 7, 7, 7, 7, 7, 5, 5};

int seq_scale_len(int scale)
{
    if(scale < 0 || scale >= SEQ_SCALE_COUNT)
        scale = 0;
    return kScaleLen[scale];
}

int seq_degree_semis(int scale, int degree)
{
    if(scale < 0 || scale >= SEQ_SCALE_COUNT)
        scale = 0;
    int n   = kScaleLen[scale];
    int oct = degree >= 0 ? degree / n : -((-degree + n - 1) / n);
    int i   = degree - oct * n;
    return oct * 12 + kScales[scale][i];
}

/* ---------------------------------------------------------------- hashing */

enum { F_HIT = 1, F_LEN, F_TONE, F_IDX, F_SLIDE, F_JUMP, F_JSIGN };

static uint32_t mix32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

/* A fixed number in [0, 1) for one property of one step. */
static float unit(uint32_t seed, uint32_t field)
{
    return (float)(mix32(seed ^ (field * 0x9e3779b9U)) >> 8) * (1.0f / 16777216.0f);
}

static uint32_t rng_next(seq_t *s)
{
    uint32_t x = s->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return s->rng = x;
}

static float clamp01(float x) { return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); }

/* ---------------------------------------------------------------- voices */

static seq_pattern_t *cur_pat(seq_t *s, int v) { return &s->pat[v][s->v[v].slot]; }
static const seq_pattern_t *cur_pat_c(const seq_t *s, int v)
{
    return &s->pat[v][s->v[v].slot];
}

/* Downbeats come in first as DENSITY rises: a step's hit number is scaled
 * down on the beat and the half-beat, so it crosses the threshold sooner. */
static float beat_weight(int k)
{
    return (k % 4 == 0) ? 0.55f : (k % 2 == 0) ? 0.8f : 1.0f;
}

static seq_step_t realize(const seq_t *s, int v, const seq_pattern_t *p, int k)
{
    const seq_knobs_t *kn = &p->knobs;
    uint32_t           sd = p->seed[k];
    seq_step_t         st;
    memset(&st, 0, sizeof st);

    st.on = unit(sd, F_HIT) * beat_weight(k) < clamp01(kn->density);

    /* LENGTH bends a fixed number through a power curve: the exponent falls
     * as the knob rises, so every note only gets longer. */
    float g = exp2f((0.5f - clamp01(kn->length)) * 4.0f);
    float d = powf(unit(sd, F_LEN), g);
    int   len = (int)(2.0f + d * 30.0f + 0.5f);
    st.len  = (uint8_t)(len > 32 ? 32 : len);

    st.scale = unit(sd, F_TONE) < clamp01(kn->tone_bias);
    int range = kn->range < 1 ? 1 : (kn->range > 3 ? 3 : kn->range);
    int n = (st.scale ? seq_scale_len(s->prog.scale) : s->prog.tones) * range;
    float u = unit(sd, F_IDX);
    if(v == SEQ_BASS)
        u = u * u; /* the bass leans on the root */
    int idx = (int)(u * (float)n);
    st.index = (uint8_t)(idx >= n ? n - 1 : idx);

    st.slide = unit(sd, F_SLIDE) < clamp01(kn->slide);
    if(unit(sd, F_JUMP) < clamp01(kn->jump))
        st.jump = (v == SEQ_BASS || unit(sd, F_JSIGN) < 0.5f) ? 1 : -1;
    return st;
}

seq_knobs_t *seq_knobs(seq_t *s, int v) { return &cur_pat(s, v)->knobs; }

seq_step_t seq_step(const seq_t *s, int v, int k)
{
    const seq_pattern_t *p = cur_pat_c(s, v);
    k %= SEQ_MAX_STEPS;
    if(k < 0)
        k += SEQ_MAX_STEPS;
    return p->locked ? p->lock[k] : realize(s, v, p, k);
}

static void save_undo(seq_pattern_t *p)
{
    memcpy(p->undo, p->seed, sizeof p->seed);
    p->has_undo = true;
}

void seq_roll(seq_t *s, int v)
{
    seq_pattern_t *p = cur_pat(s, v);
    if(p->locked)
        return; /* LOCK protects the keeper */
    save_undo(p);
    for(int k = 0; k < SEQ_MAX_STEPS; k++)
        p->seed[k] = rng_next(s);
}

void seq_mutate(seq_t *s, int v, float amount)
{
    seq_pattern_t *p = cur_pat(s, v);
    if(p->locked)
        return;
    save_undo(p);
    uint32_t th = (uint32_t)(clamp01(amount) * 4294967295.0);
    for(int k = 0; k < SEQ_MAX_STEPS; k++)
    {
        uint32_t r = rng_next(s);
        if(amount >= 1.0f || r < th)
            p->seed[k] = rng_next(s);
    }
}

void seq_undo(seq_t *s, int v)
{
    seq_pattern_t *p = cur_pat(s, v);
    if(!p->has_undo || p->locked)
        return;
    uint32_t tmp[SEQ_MAX_STEPS];
    memcpy(tmp, p->seed, sizeof tmp);
    memcpy(p->seed, p->undo, sizeof tmp);
    memcpy(p->undo, tmp, sizeof tmp);
}

void seq_lock(seq_t *s, int v, bool lock)
{
    seq_pattern_t *p = cur_pat(s, v);
    if(lock && !p->locked)
        for(int k = 0; k < SEQ_MAX_STEPS; k++)
            p->lock[k] = realize(s, v, p, k);
    p->locked = lock;
}

/* ---------------------------------------------------------------- chords */

static double prog_ticks(const seq_prog_t *pr)
{
    double t = 0;
    for(int i = 0; i < pr->count; i++)
        t += (double)(pr->chord[i].bars ? pr->chord[i].bars : 1) * SEQ_BAR;
    return t;
}

static int chord_at(const seq_prog_t *pr, double tick)
{
    double total = prog_ticks(pr);
    if(total <= 0 || tick < 0)
        return 0;
    double t = fmod(tick, total);
    for(int i = 0; i < pr->count; i++)
    {
        double len = (double)(pr->chord[i].bars ? pr->chord[i].bars : 1) * SEQ_BAR;
        if(t < len)
            return i;
        t -= len;
    }
    return pr->count - 1;
}

int seq_chord_index(const seq_t *s) { return chord_at(&s->prog, s->pos); }

/* Semitones above the voice's 0 V for step st under chord ci. */
static float note_for(const seq_t *s, int v, seq_step_t st, int ci)
{
    const seq_prog_t *pr = &s->prog;
    int n   = seq_scale_len(pr->scale);
    int deg = pr->chord[ci].degree;
    if(v == SEQ_BASS && deg > n / 2)
        deg -= n; /* keep the bass near the key's root: V goes below, not above */
    int tones = pr->tones < 1 ? 3 : pr->tones;
    if(st.scale)
        deg += st.index;
    else
        deg += 2 * (st.index % tones) + n * (st.index / tones);
    int semis = pr->root + seq_degree_semis(pr->scale, deg)
                + 12 * (s->base_octave[v] + cur_pat_c(s, v)->knobs.octave + st.jump);
    while(semis > MAX_SEMIS)
        semis -= 12;
    while(semis < 0)
        semis += 12;
    return (float)semis;
}

float seq_step_note(const seq_t *s, int v, int k)
{
    return note_for(s, v, seq_step(s, v, k), seq_chord_index(s));
}

/* ---------------------------------------------------------------- patterns */

void seq_voice_queue(seq_t *s, int v, int slot)
{
    if(slot >= 0 && slot < SEQ_VOICE_SLOTS)
        s->v[v].queued = (uint8_t)slot;
}

void seq_voice_copy(seq_t *s, int v, int from, int to)
{
    if(from < 0 || to < 0 || from >= SEQ_VOICE_SLOTS || to >= SEQ_VOICE_SLOTS)
        return;
    s->pat[v][to] = s->pat[v][from];
}

void seq_drum_queue(seq_t *s, int slot)
{
    if(slot >= 0 && slot < SEQ_DRUM_SLOTS)
        s->drum_queued = (uint8_t)slot;
}

void seq_voice_mute(seq_t *s, int v, bool mute) { s->v[v].mute = mute; }
void seq_drum_mute(seq_t *s, int d, bool mute) { s->drum_mute[d] = mute; }

void seq_drum_set(seq_t *s, int d, int k, int prob)
{
    if(d < 0 || d >= SEQ_DRUMS || k < 0 || k >= SEQ_MAX_STEPS)
        return;
    s->drums[s->drum_slot].prob[d][k] = (uint8_t)(prob < 0 ? 0 : (prob > 4 ? 4 : prob));
}

/* ---------------------------------------------------------------- init */

static void default_drums(seq_drums_t *dr)
{
    memset(dr, 0, sizeof *dr);
    for(int d = 0; d < SEQ_DRUMS; d++)
        dr->steps[d] = 16;
    for(int k = 0; k < 16; k += 4)
        dr->prob[SEQ_KICK][k] = 4;
    dr->prob[SEQ_SNARE][4]  = 4;
    dr->prob[SEQ_SNARE][12] = 4;
    for(int k = 2; k < 16; k += 4)
        dr->prob[SEQ_HAT][k] = 4;
}

void seq_init(seq_t *s, uint32_t seed)
{
    memset(s, 0, sizeof *s);
    s->rng = mix32(seed) | 1u;

    /* A minor, i - VI - III - VII, a bar each. */
    s->prog.root  = 9;
    s->prog.scale = SEQ_SCALE_MINOR;
    s->prog.tones = 3;
    s->prog.count = 4;
    const int8_t degs[4] = {0, 5, 2, 6};
    for(int i = 0; i < 4; i++)
    {
        s->prog.chord[i].degree = degs[i];
        s->prog.chord[i].bars   = 1;
    }

    s->base_octave[SEQ_BASS] = 1;
    s->base_octave[SEQ_LEAD] = 2;

    const seq_knobs_t kb = {.density = 0.6f, .length = 0.4f, .slide = 0.2f,
                            .jump = 0.15f, .tone_bias = 0.1f, .swing = 1.0f,
                            .octave = 0, .range = 1, .steps = 16, .div = 1};
    const seq_knobs_t kl = {.density = 0.35f, .length = 0.6f, .slide = 0.1f,
                            .jump = 0.1f, .tone_bias = 0.3f, .swing = 1.0f,
                            .octave = 0, .range = 2, .steps = 16, .div = 1};
    for(int v = 0; v < SEQ_VOICES; v++)
        for(int sl = 0; sl < SEQ_VOICE_SLOTS; sl++)
        {
            seq_pattern_t *p = &s->pat[v][sl];
            p->knobs = v == SEQ_BASS ? kb : kl;
            for(int k = 0; k < SEQ_MAX_STEPS; k++)
                p->seed[k] = rng_next(s);
        }
    for(int sl = 0; sl < SEQ_DRUM_SLOTS; sl++)
        default_drums(&s->drums[sl]);

    s->bpm        = 120.0f;
    s->swing      = 0.5f;
    s->glide_ms   = 60;
    s->clock_ppqn = 4;
    for(int v = 0; v < SEQ_VOICES; v++)
        s->v[v].cur_step = -1;
    for(int d = 0; d < SEQ_DRUMS; d++)
        s->drum_step[d] = -1;
}

/* ---------------------------------------------------------------- transport */

static void all_gates_off(seq_t *s)
{
    for(int v = 0; v < SEQ_VOICES; v++)
        s->v[v].gate = false;
}

static void rewind(seq_t *s)
{
    s->pos = -START_EPS;
    for(int v = 0; v < SEQ_VOICES; v++)
        s->v[v].cur_step = -1;
    for(int d = 0; d < SEQ_DRUMS; d++)
        s->drum_step[d] = -1;
}

void seq_play(seq_t *s)
{
    rewind(s);
    s->running = true;
}

void seq_stop(seq_t *s)
{
    s->running = false;
    all_gates_off(s);
}

static uint32_t ticks_per_edge(const seq_t *s)
{
    uint32_t p = s->clock_ppqn ? s->clock_ppqn : 4;
    return SEQ_PPQN / p;
}

void seq_clock_edge(seq_t *s, uint32_t now_us)
{
    if(!s->ext)
    {
        /* First edge: the external clock takes over and step one plays now. */
        s->ext       = true;
        s->running   = true;
        s->edges     = 0;
        s->period_us = 0;
        s->edge_us   = now_us;
        s->reset_pending = false;
        rewind(s);
        return;
    }
    uint32_t p = now_us - s->edge_us;
    s->period_us = s->period_us ? (s->period_us + p) / 2 : p;
    s->edge_us   = now_us;
    s->running   = true;
    if(s->reset_pending)
    {
        s->reset_pending = false;
        s->edges         = 0;
        rewind(s);
        return;
    }
    s->edges++;
}

void seq_reset_edge(seq_t *s, uint32_t now_us)
{
    (void)now_us;
    if(s->ext)
        s->reset_pending = true;
    else
        rewind(s);
}

/* ---------------------------------------------------------------- playback */

static void start_note(seq_t *s, int v, long g, double start, uint32_t now_us)
{
    seq_vstate_t        *vs = &s->v[v];
    const seq_pattern_t *p  = cur_pat_c(s, v);
    int steps = p->knobs.steps ? p->knobs.steps : 16;
    int k     = (int)(g % steps);
    vs->cur_step = k;

    seq_step_t st = seq_step(s, v, k);
    if(vs->mute || !st.on)
        return; /* a tied note before keeps sounding until its gate_off */

    int    div  = p->knobs.div ? p->knobs.div : 1;
    double tpsv = (double)SEQ_TPS * div;
    float  note = note_for(s, v, st, chord_at(&s->prog, start));
    /* Legato only when the note before was a real tie (longer than its step);
     * a full-step note whose gate merely overlaps because of swing is not. */
    bool   holding = vs->gate && vs->tied && vs->gate_off > start;

    if(st.slide && vs->gate)
    {
        /* 303 slide: the gate carries over and the pitch glides. */
        vs->target = note;
        float dist = fabsf(note - vs->pitch);
        vs->glide_rate = s->glide_ms ? dist / (float)s->glide_ms : 0.f;
        if(vs->glide_rate == 0.f)
            vs->pitch = note;
    }
    else
    {
        vs->pitch = vs->target = note;
        vs->glide_rate = 0.f;
        if(vs->gate && !holding)
        {
            /* The last note only just ended: dip so this one is heard as new.
             * A note still held (a tie) goes on legato with no new attack. */
            vs->dip       = true;
            vs->dip_until = now_us + RETRIG_US;
        }
    }
    vs->gate     = true;
    vs->tied     = st.len > 16;
    vs->gate_off = start + tpsv * st.len / 16.0;

    /* A slide on the NEXT step holds this gate into it, as on a 303: the
     * next note then finds the gate still up and glides legato. 1.5 steps
     * covers the next step's swing delay. */
    seq_step_t nx = seq_step(s, v, (k + 1) % steps);
    if(nx.on && nx.slide && !vs->mute && vs->gate_off < start + tpsv * 1.5)
        vs->gate_off = start + tpsv * 1.5;
}

static void fire_drum(seq_t *s, int d, long g, uint32_t now_us)
{
    const seq_drums_t *dr = &s->drums[s->drum_slot];
    int steps = dr->steps[d] ? dr->steps[d] : 16;
    int k     = (int)(g % steps);
    s->drum_step[d] = k;
    int p = dr->prob[d][k];
    if(p == 0 || s->drum_mute[d])
        return;
    if(p < 4 && (int)(rng_next(s) % 4) >= p)
        return;
    s->trig_on[d]    = true;
    s->trig_until[d] = now_us + SEQ_TRIG_MS * 1000u;
}

static double swing_off(const seq_t *s, long g, double tps, float amount)
{
    if((g & 1) == 0)
        return 0.0;
    double sw = (double)s->swing - 0.5;
    if(sw < 0)
        sw = 0;
    if(sw > 0.25)
        sw = 0.25;
    return sw * 2.0 * tps * clamp01(amount);
}

/* Fire everything that starts in [a, b). */
static void run_window(seq_t *s, double a, double b, uint32_t now_us)
{
    /* Bar lines first: queued patterns change there. */
    for(long bar = (long)floor(a / SEQ_BAR); (double)bar * SEQ_BAR < b; bar++)
    {
        double t = (double)bar * SEQ_BAR;
        if(bar < 0 || t < a)
            continue;
        for(int v = 0; v < SEQ_VOICES; v++)
            s->v[v].slot = s->v[v].queued;
        s->drum_slot = s->drum_queued;
    }

    for(int v = 0; v < SEQ_VOICES; v++)
    {
        const seq_pattern_t *p = cur_pat_c(s, v);
        int    div = p->knobs.div ? p->knobs.div : 1;
        double tps = (double)SEQ_TPS * div;
        for(long g = (long)floor(a / tps) - 1; (double)g * tps < b; g++)
        {
            if(g < 0)
                continue;
            double start = (double)g * tps + swing_off(s, g, tps, p->knobs.swing);
            if(start >= a && start < b)
                start_note(s, v, g, start, now_us);
        }
    }

    for(long g = (long)floor(a / SEQ_TPS) - 1; (double)g * SEQ_TPS < b; g++)
    {
        if(g < 0)
            continue;
        double start = (double)g * SEQ_TPS + swing_off(s, g, SEQ_TPS, 1.0f);
        if(start >= a && start < b)
            for(int d = 0; d < SEQ_DRUMS; d++)
                fire_drum(s, d, g, now_us);
    }
}

void seq_process(seq_t *s, uint32_t now_us, seq_out_t *out)
{
    uint32_t dt_us = s->have_last ? now_us - s->last_us : 0;
    s->last_us   = now_us;
    s->have_last = true;

    if(s->ext && now_us - s->edge_us > SEQ_CLOCK_LOST_MS * 1000u)
    {
        s->ext     = false;
        seq_stop(s);
    }

    if(s->running)
    {
        double next;
        if(s->ext)
        {
            /* Extrapolate from the last edge at the measured period, but
             * never past where the next edge will land; the tick of the
             * latest edge itself is always reached. */
            double tpe  = (double)ticks_per_edge(s);
            double base = (double)s->edges * tpe;
            next = base + START_EPS;
            if(s->period_us)
            {
                uint32_t since = now_us - s->edge_us;
                if(since > s->period_us)
                    since = s->period_us;
                double x = base + tpe * (double)since / (double)s->period_us;
                if(x > next)
                    next = x;
            }
            if(next > base + tpe - START_EPS)
                next = base + tpe - START_EPS;
        }
        else
            next = s->pos + (double)dt_us * s->bpm * SEQ_PPQN / 60e6;

        if(next > s->pos)
        {
            run_window(s, s->pos, next, now_us);
            s->pos = next;
        }

        for(int v = 0; v < SEQ_VOICES; v++)
            if(s->v[v].gate && s->pos >= s->v[v].gate_off)
                s->v[v].gate = false;
    }

    /* Glide and the retrigger dip run in real time. */
    float dt_ms = (float)dt_us / 1000.0f;
    for(int v = 0; v < SEQ_VOICES; v++)
    {
        seq_vstate_t *vs = &s->v[v];
        if(vs->dip && (int32_t)(now_us - vs->dip_until) >= 0)
            vs->dip = false;
        if(vs->glide_rate > 0.f)
        {
            float step = vs->glide_rate * dt_ms;
            float diff = vs->target - vs->pitch;
            if(fabsf(diff) <= step)
            {
                vs->pitch      = vs->target;
                vs->glide_rate = 0.f;
            }
            else
                vs->pitch += diff > 0 ? step : -step;
        }
    }

    for(int d = 0; d < SEQ_DRUMS; d++)
        if(s->trig_on[d] && (int32_t)(now_us - s->trig_until[d]) >= 0)
            s->trig_on[d] = false;

    if(out)
    {
        for(int v = 0; v < SEQ_VOICES; v++)
        {
            const seq_vstate_t *vs = &s->v[v];
            out->pitch[v] = vs->pitch / 12.0f;
            out->gate[v]  = vs->gate && !vs->dip;
        }
        for(int d = 0; d < SEQ_DRUMS; d++)
            out->trig[d] = s->trig_on[d];
    }
}
