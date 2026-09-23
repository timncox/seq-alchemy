/* Native tests for core/seq.c. Every check prints on failure; exit 1 if any. */
#include "seq.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_checks, g_fails;
#define CHECK(cond, ...)                                              \
    do                                                                \
    {                                                                 \
        g_checks++;                                                   \
        if(!(cond))                                                   \
        {                                                             \
            g_fails++;                                                \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);               \
            printf(__VA_ARGS__);                                      \
            printf("\n");                                             \
        }                                                             \
    } while(0)

static seq_t S; /* big: keep it off the stack */

/* ------------------------------------------------------------ recording */

#define MAX_EV 512
typedef struct {
    int      n;
    uint32_t t[MAX_EV]; /* ms */
} evlist_t;

typedef struct {
    evlist_t gate_up[SEQ_VOICES];
    evlist_t gate_down[SEQ_VOICES];
    evlist_t trig[SEQ_DRUMS];
    bool     last_gate[SEQ_VOICES];
    bool     last_trig[SEQ_DRUMS];
} rec_t;

static rec_t R;

static void push(evlist_t *e, uint32_t ms)
{
    if(e->n < MAX_EV)
        e->t[e->n++] = ms;
}

static void rec_clear(void) { memset(&R, 0, sizeof R); }

/* Process from ms a to ms b (exclusive) at 1 ms, recording edges. Optional
 * clock edges every clock_ms starting at clock_from (0 = none). */
static seq_out_t run(seq_t *s, uint32_t a, uint32_t b, uint32_t clock_ms,
                     uint32_t clock_from, uint32_t clock_until)
{
    seq_out_t o;
    memset(&o, 0, sizeof o);
    for(uint32_t ms = a; ms < b; ms++)
    {
        if(clock_ms && ms >= clock_from && ms < clock_until
           && (ms - clock_from) % clock_ms == 0)
            seq_clock_edge(s, ms * 1000u);
        seq_process(s, ms * 1000u, &o);
        for(int v = 0; v < SEQ_VOICES; v++)
        {
            if(o.gate[v] && !R.last_gate[v])
                push(&R.gate_up[v], ms);
            if(!o.gate[v] && R.last_gate[v])
                push(&R.gate_down[v], ms);
            R.last_gate[v] = o.gate[v];
        }
        for(int d = 0; d < SEQ_DRUMS; d++)
        {
            if(o.trig[d] && !R.last_trig[d])
                push(&R.trig[d], ms);
            R.last_trig[d] = o.trig[d];
        }
    }
    return o;
}

static bool has_near(const evlist_t *e, double ms, double tol)
{
    for(int i = 0; i < e->n; i++)
        if(fabs((double)e->t[i] - ms) <= tol)
            return true;
    return false;
}

/* Silence the generated voices so drum tests aren't cluttered. */
static void quiet_voices(seq_t *s)
{
    for(int v = 0; v < SEQ_VOICES; v++)
        seq_knobs(s, v)->density = 0.f;
}

static void clear_drums(seq_t *s)
{
    for(int d = 0; d < SEQ_DRUMS; d++)
        for(int k = 0; k < SEQ_MAX_STEPS; k++)
            seq_drum_set(s, d, k, 0);
}

/* ------------------------------------------------------------ scales */

static void test_scales(void)
{
    CHECK(seq_degree_semis(SEQ_SCALE_MINOR, 0) == 0, "minor deg 0");
    CHECK(seq_degree_semis(SEQ_SCALE_MINOR, 2) == 3, "minor deg 2 = b3");
    CHECK(seq_degree_semis(SEQ_SCALE_MINOR, 7) == 12, "minor deg 7 = octave");
    CHECK(seq_degree_semis(SEQ_SCALE_MINOR, -1) == -2, "minor deg -1 = b7 below");
    CHECK(seq_degree_semis(SEQ_SCALE_MINOR, -7) == -12, "minor deg -7");
    CHECK(seq_degree_semis(SEQ_SCALE_MINOR_PENT, 5) == 12, "pent deg 5 = octave");
    CHECK(seq_scale_len(SEQ_SCALE_MAJOR_PENT) == 5, "pent len");
}

/* ------------------------------------------------------------ generation */

static void test_determinism(void)
{
    static seq_t a, b;
    seq_init(&a, 42);
    seq_init(&b, 42);
    bool same = true;
    for(int v = 0; v < SEQ_VOICES; v++)
        for(int k = 0; k < SEQ_MAX_STEPS; k++)
            same &= seq_step(&a, v, k).on == seq_step(&b, v, k).on
                    && seq_step(&a, v, k).index == seq_step(&b, v, k).index
                    && seq_step(&a, v, k).len == seq_step(&b, v, k).len;
    CHECK(same, "same seed -> same line");
    seq_init(&b, 43);
    int diff = 0;
    for(int k = 0; k < 16; k++)
        diff += seq_step(&a, SEQ_BASS, k).index != seq_step(&b, SEQ_BASS, k).index
                || seq_step(&a, SEQ_BASS, k).on != seq_step(&b, SEQ_BASS, k).on;
    CHECK(diff > 0, "another seed -> another line");
}

static void test_density_monotone(void)
{
    int violations = 0;
    for(uint32_t seed = 1; seed <= 50; seed++)
    {
        seq_init(&S, seed);
        for(int v = 0; v < SEQ_VOICES; v++)
        {
            bool prev[SEQ_MAX_STEPS] = {0};
            for(int i = 0; i <= 20; i++)
            {
                seq_knobs(&S, v)->density = (float)i / 20.f;
                for(int k = 0; k < SEQ_MAX_STEPS; k++)
                {
                    bool on = seq_step(&S, v, k).on;
                    if(prev[k] && !on)
                        violations++;
                    prev[k] = on;
                }
            }
        }
    }
    CHECK(violations == 0, "raising density removed %d notes", violations);

    seq_init(&S, 7);
    seq_knobs(&S, SEQ_BASS)->density = 0.f;
    int n = 0;
    for(int k = 0; k < SEQ_MAX_STEPS; k++)
        n += seq_step(&S, SEQ_BASS, k).on;
    CHECK(n == 0, "density 0 -> silence (%d)", n);
    seq_knobs(&S, SEQ_BASS)->density = 1.f;
    n = 0;
    for(int k = 0; k < SEQ_MAX_STEPS; k++)
        n += seq_step(&S, SEQ_BASS, k).on;
    CHECK(n == SEQ_MAX_STEPS, "density 1 -> every step (%d)", n);

    /* Round trip: down then back up restores the line exactly. */
    seq_knobs(&S, SEQ_BASS)->density = 0.5f;
    bool before[SEQ_MAX_STEPS];
    for(int k = 0; k < SEQ_MAX_STEPS; k++)
        before[k] = seq_step(&S, SEQ_BASS, k).on;
    seq_knobs(&S, SEQ_BASS)->density = 0.1f;
    seq_knobs(&S, SEQ_BASS)->density = 0.5f;
    bool same = true;
    for(int k = 0; k < SEQ_MAX_STEPS; k++)
        same &= before[k] == seq_step(&S, SEQ_BASS, k).on;
    CHECK(same, "density round trip restores the line");

    /* Downbeats arrive first: at low density, beats outnumber off-beats. */
    int beats = 0, offs = 0;
    for(uint32_t seed = 1; seed <= 200; seed++)
    {
        seq_init(&S, seed);
        seq_knobs(&S, SEQ_BASS)->density = 0.3f;
        for(int k = 0; k < 16; k++)
        {
            if(!seq_step(&S, SEQ_BASS, k).on)
                continue;
            if(k % 4 == 0)
                beats++;
            else if(k % 2 == 1)
                offs++;
        }
    }
    /* Per step: a beat (4 per bar) plays far more often than an odd 16th
     * (8 per bar). Expected ~0.55 vs ~0.30 at density 0.3. */
    double beat_rate = beats / (200.0 * 4), off_rate = offs / (200.0 * 8);
    CHECK(beat_rate > 1.5 * off_rate, "downbeats first: per-step %.2f vs %.2f",
          beat_rate, off_rate);
}

static void test_length_monotone(void)
{
    int violations = 0, tied_lo = 0, tied_hi = 0;
    for(uint32_t seed = 1; seed <= 50; seed++)
    {
        seq_init(&S, seed);
        uint8_t prev[SEQ_MAX_STEPS] = {0};
        for(int i = 0; i <= 20; i++)
        {
            seq_knobs(&S, SEQ_LEAD)->length = (float)i / 20.f;
            for(int k = 0; k < SEQ_MAX_STEPS; k++)
            {
                uint8_t len = seq_step(&S, SEQ_LEAD, k).len;
                if(len < prev[k])
                    violations++;
                prev[k] = len;
                if(i == 0 && len > 16)
                    tied_lo++;
                if(i == 20 && len > 16)
                    tied_hi++;
                CHECK(len >= 2 && len <= 32, "len %d out of range", len);
            }
        }
    }
    CHECK(violations == 0, "raising length shortened %d notes", violations);
    CHECK(tied_hi > 4 * tied_lo, "long knob ties more: %d vs %d", tied_hi, tied_lo);
}

static bool in_set(int pc, const int *set, int n)
{
    for(int i = 0; i < n; i++)
        if(((set[i] % 12) + 12) % 12 == pc)
            return true;
    return false;
}

static void test_chord_following(void)
{
    seq_init(&S, 99);
    seq_prog_t *pr = &S.prog;
    for(int v = 0; v < SEQ_VOICES; v++)
    {
        seq_knobs(&S, v)->density   = 1.f;
        seq_knobs(&S, v)->tone_bias = 0.f;
        seq_knobs(&S, v)->jump      = 0.f;
    }
    int bad = 0;
    for(int ci = 0; ci < pr->count; ci++)
    {
        S.pos = (double)ci * SEQ_BAR + 1.0;
        CHECK(seq_chord_index(&S) == ci, "chord index at bar %d", ci);
        int pcs[3];
        for(int t = 0; t < 3; t++)
            pcs[t] = pr->root + seq_degree_semis(pr->scale, pr->chord[ci].degree + 2 * t);
        for(int v = 0; v < SEQ_VOICES; v++)
            for(int k = 0; k < 16; k++)
            {
                int note = (int)seq_step_note(&S, v, k);
                if(!in_set(note % 12, pcs, 3))
                    bad++;
            }
    }
    CHECK(bad == 0, "tone bias 0 -> only chord tones (%d strays)", bad);

    /* The same step re-voices: rhythm unchanged, pitch moves with the chord. */
    S.pos = 1.0;
    float n0[16];
    bool  on0[16];
    for(int k = 0; k < 16; k++)
    {
        n0[k]  = seq_step_note(&S, SEQ_LEAD, k);
        on0[k] = seq_step(&S, SEQ_LEAD, k).on;
    }
    S.pos = SEQ_BAR + 1.0;
    int moved = 0;
    bool rhythm = true;
    for(int k = 0; k < 16; k++)
    {
        moved += seq_step_note(&S, SEQ_LEAD, k) != n0[k];
        rhythm &= seq_step(&S, SEQ_LEAD, k).on == on0[k];
    }
    CHECK(rhythm, "chord change keeps the rhythm");
    CHECK(moved > 8, "chord change re-voices the pitches (%d moved)", moved);

    /* Tone bias 1: always in the scale, and all notes 0..5 V. */
    int strays = 0, out = 0;
    for(uint32_t seed = 1; seed <= 30; seed++)
    {
        seq_init(&S, seed);
        for(int v = 0; v < SEQ_VOICES; v++)
        {
            seq_knobs(&S, v)->density   = 1.f;
            seq_knobs(&S, v)->tone_bias = 1.f;
            seq_knobs(&S, v)->range     = 3;
            seq_knobs(&S, v)->jump      = 0.5f;
            seq_knobs(&S, v)->octave    = 3;
        }
        int scale[7], n = seq_scale_len(S.prog.scale);
        for(int i = 0; i < n; i++)
            scale[i] = S.prog.root + seq_degree_semis(S.prog.scale, i);
        for(int ci = 0; ci < S.prog.count; ci++)
        {
            S.pos = (double)ci * SEQ_BAR;
            for(int v = 0; v < SEQ_VOICES; v++)
                for(int k = 0; k < SEQ_MAX_STEPS; k++)
                {
                    float note = seq_step_note(&S, v, k);
                    if(!in_set((int)note % 12, scale, n))
                        strays++;
                    if(note < 0.f || note > 60.f)
                        out++;
                }
        }
    }
    CHECK(strays == 0, "tone bias 1 -> only scale tones (%d strays)", strays);
    CHECK(out == 0, "%d notes outside 0..5 V", out);
}

static void test_lock_mutate_undo(void)
{
    seq_init(&S, 5);
    seq_knobs(&S, SEQ_BASS)->density = 0.5f;
    seq_step_t before[16];
    float      notes[16];
    S.pos = 1.0;
    for(int k = 0; k < 16; k++)
    {
        before[k] = seq_step(&S, SEQ_BASS, k);
        notes[k]  = seq_step_note(&S, SEQ_BASS, k);
    }
    seq_lock(&S, SEQ_BASS, true);
    seq_knobs(&S, SEQ_BASS)->density = 1.f;
    seq_knobs(&S, SEQ_BASS)->length  = 1.f;
    seq_roll(&S, SEQ_BASS);
    seq_mutate(&S, SEQ_BASS, 1.f);
    bool same = true;
    for(int k = 0; k < 16; k++)
        same &= seq_step(&S, SEQ_BASS, k).on == before[k].on
                && seq_step(&S, SEQ_BASS, k).len == before[k].len
                && seq_step(&S, SEQ_BASS, k).index == before[k].index;
    CHECK(same, "locked line ignores knobs, roll and mutate");

    seq_knobs(&S, SEQ_BASS)->octave = 1;
    int up = 0, on = 0;
    for(int k = 0; k < 16; k++)
        if(before[k].on)
        {
            on++;
            float n = seq_step_note(&S, SEQ_BASS, k);
            up += (n == notes[k] + 12.f) || (notes[k] + 12.f > 60.f);
        }
    CHECK(up == on, "octave transposes a locked line (%d of %d)", up, on);
    seq_knobs(&S, SEQ_BASS)->octave = 0;

    S.pos = SEQ_BAR + 1.0; /* next chord */
    int moved = 0;
    for(int k = 0; k < 16; k++)
        moved += before[k].on && seq_step_note(&S, SEQ_BASS, k) != notes[k];
    CHECK(moved > 0, "a locked line still follows the chords");

    seq_lock(&S, SEQ_BASS, false);
    seq_knobs(&S, SEQ_BASS)->density = 0.5f;
    seq_knobs(&S, SEQ_BASS)->length  = 0.4f;
    uint32_t seeds[SEQ_MAX_STEPS];
    memcpy(seeds, S.pat[SEQ_BASS][0].seed, sizeof seeds);
    seq_mutate(&S, SEQ_BASS, 0.f);
    CHECK(memcmp(seeds, S.pat[SEQ_BASS][0].seed, sizeof seeds) == 0, "mutate 0 changes nothing");
    seq_mutate(&S, SEQ_BASS, 0.25f);
    int changed = 0;
    for(int k = 0; k < SEQ_MAX_STEPS; k++)
        changed += seeds[k] != S.pat[SEQ_BASS][0].seed[k];
    CHECK(changed > 4 && changed < 30, "mutate 25%% changed %d of 64", changed);
    seq_undo(&S, SEQ_BASS);
    CHECK(memcmp(seeds, S.pat[SEQ_BASS][0].seed, sizeof seeds) == 0, "undo restores");
    seq_undo(&S, SEQ_BASS);
    changed = 0;
    for(int k = 0; k < SEQ_MAX_STEPS; k++)
        changed += seeds[k] != S.pat[SEQ_BASS][0].seed[k];
    CHECK(changed > 4, "undo again is redo");
    seq_roll(&S, SEQ_BASS);
    changed = 0;
    for(int k = 0; k < SEQ_MAX_STEPS; k++)
        changed += seeds[k] != S.pat[SEQ_BASS][0].seed[k];
    CHECK(changed == SEQ_MAX_STEPS, "roll replaces every step (%d)", changed);
}

/* ------------------------------------------------------------ timing */

static void test_internal_clock(void)
{
    seq_init(&S, 1);
    quiet_voices(&S);
    rec_clear();
    seq_process(&S, 0, NULL);
    seq_play(&S);
    run(&S, 0, 2000, 0, 0, 0);
    /* 120 BPM: a quarter = 500 ms; the default kick is four on the floor. */
    CHECK(R.trig[SEQ_KICK].n == 4, "4 kicks in 2 s (%d)", R.trig[SEQ_KICK].n);
    for(int i = 0; i < 4; i++)
        CHECK(has_near(&R.trig[SEQ_KICK], 500.0 * i, 1.5), "kick at %d ms", 500 * i);
    CHECK(has_near(&R.trig[SEQ_SNARE], 500.0, 1.5), "snare on 2");
    CHECK(has_near(&R.trig[SEQ_SNARE], 1500.0, 1.5), "snare on 4");
    CHECK(R.trig[SEQ_PERC].n == 0, "perc empty by default");

    /* Swing 0.75 delays odd 16ths by half a step. */
    seq_init(&S, 1);
    quiet_voices(&S);
    clear_drums(&S);
    seq_drum_set(&S, SEQ_PERC, 0, 4);
    seq_drum_set(&S, SEQ_PERC, 1, 4);
    S.swing = 0.75f;
    rec_clear();
    seq_process(&S, 0, NULL);
    seq_play(&S);
    run(&S, 0, 300, 0, 0, 0);
    CHECK(has_near(&R.trig[SEQ_PERC], 0.0, 1.5), "unswung step 1 on time");
    CHECK(has_near(&R.trig[SEQ_PERC], 187.5, 1.5), "swung step 2 at 187.5 ms");
    CHECK(!has_near(&R.trig[SEQ_PERC], 125.0, 5), "swung step 2 not at 125 ms");

    /* Stop: no more triggers, gates down. */
    seq_stop(&S);
    rec_clear();
    run(&S, 300, 1000, 0, 0, 0);
    CHECK(R.trig[SEQ_PERC].n == 0, "stopped transport is silent");
}

static void test_external_clock(void)
{
    /* 16th clock (4 ppqn) every 100 ms: steps land on the edges. */
    seq_init(&S, 1);
    quiet_voices(&S);
    clear_drums(&S);
    for(int k = 0; k < 16; k++)
        seq_drum_set(&S, SEQ_HAT, k, 4);
    rec_clear();
    run(&S, 0, 950, 100, 0, 500); /* edges at 0..400 ms, then the clock stops */
    CHECK(S.ext, "external clock took over");
    CHECK(R.trig[SEQ_HAT].n == 5, "5 edges -> 5 steps, no run-on (%d)", R.trig[SEQ_HAT].n);
    for(int i = 0; i < 5; i++)
        CHECK(has_near(&R.trig[SEQ_HAT], 100.0 * i, 1.5), "hat on edge at %d ms", 100 * i);

    rec_clear();
    run(&S, 950, 2600, 0, 0, 0);
    CHECK(!S.ext && !S.running, "clock lost after 2 s -> stopped, back to internal");

    /* Reset: the NEXT edge is step one. Kick on step 1 only. */
    seq_init(&S, 1);
    quiet_voices(&S);
    clear_drums(&S);
    seq_drum_set(&S, SEQ_KICK, 0, 4);
    rec_clear();
    run(&S, 0, 650, 100, 0, 10000);      /* steps 0..6 at 0..600 */
    seq_reset_edge(&S, 650000);
    run(&S, 650, 800, 100, 0, 10000);    /* edge at 700 = step one */
    CHECK(R.trig[SEQ_KICK].n == 2, "kick at start and after reset (%d)", R.trig[SEQ_KICK].n);
    CHECK(has_near(&R.trig[SEQ_KICK], 700.0, 1.5), "reset -> next edge is step one");

    /* 24 ppqn: 16ths every 6 edges. */
    seq_init(&S, 1);
    quiet_voices(&S);
    clear_drums(&S);
    for(int k = 0; k < 16; k++)
        seq_drum_set(&S, SEQ_HAT, k, 4);
    S.clock_ppqn = 24;
    rec_clear();
    run(&S, 0, 600, 20, 0, 10000); /* edge 20 ms -> 16th = 120 ms */
    CHECK(R.trig[SEQ_HAT].n == 5, "24 ppqn: 5 steps in 600 ms (%d)", R.trig[SEQ_HAT].n);
    CHECK(has_near(&R.trig[SEQ_HAT], 120.0, 1.5) && has_near(&R.trig[SEQ_HAT], 480.0, 1.5),
          "24 ppqn: steps on every 6th edge");
}

static void test_drums(void)
{
    seq_init(&S, 3);
    quiet_voices(&S);
    seq_drum_mute(&S, SEQ_KICK, true);
    rec_clear();
    seq_process(&S, 0, NULL);
    seq_play(&S);
    run(&S, 0, 2000, 0, 0, 0);
    CHECK(R.trig[SEQ_KICK].n == 0, "muted kick is silent");
    CHECK(R.trig[SEQ_SNARE].n == 2, "snare unaffected by kick mute");

    /* 50 % probability over 200 bars. */
    seq_init(&S, 3);
    quiet_voices(&S);
    clear_drums(&S);
    seq_drum_set(&S, SEQ_PERC, 0, 2);
    S.bpm = 300.f; /* bar = 800 ms */
    rec_clear();
    seq_process(&S, 0, NULL);
    seq_play(&S);
    run(&S, 0, 160000, 0, 0, 0);
    CHECK(R.trig[SEQ_PERC].n > 70 && R.trig[SEQ_PERC].n < 130,
          "50%% step fired %d of 200", R.trig[SEQ_PERC].n);

    /* A queued pattern waits for the bar line. Slot 1: kick on 1 only. */
    seq_init(&S, 3);
    quiet_voices(&S);
    for(int k = 0; k < SEQ_MAX_STEPS; k++)
        for(int d = 0; d < SEQ_DRUMS; d++)
            S.drums[1].prob[d][k] = 0;
    S.drums[1].prob[SEQ_KICK][0] = 4;
    rec_clear();
    seq_process(&S, 0, NULL);
    seq_play(&S);
    run(&S, 0, 700, 0, 0, 0);
    seq_drum_queue(&S, 1);
    run(&S, 700, 4000, 0, 0, 0);
    /* Bar 1 (0..2 s): 4 kicks; from bar 2 only the downbeat. */
    CHECK(has_near(&R.trig[SEQ_KICK], 1000.0, 1.5) && has_near(&R.trig[SEQ_KICK], 1500.0, 1.5),
          "old pattern plays out the bar");
    CHECK(has_near(&R.trig[SEQ_KICK], 2000.0, 1.5), "new pattern from the bar line");
    CHECK(!has_near(&R.trig[SEQ_KICK], 2500.0, 5), "new pattern has no kick on 2");
    CHECK(S.drum_slot == 1, "slot switched");

    /* Polymeter: a 3-step perc against a 16-step bar. */
    seq_init(&S, 3);
    quiet_voices(&S);
    clear_drums(&S);
    seq_drum_set(&S, SEQ_PERC, 0, 4);
    S.drums[0].steps[SEQ_PERC] = 3;
    rec_clear();
    seq_process(&S, 0, NULL);
    seq_play(&S);
    run(&S, 0, 1000, 0, 0, 0);
    CHECK(R.trig[SEQ_PERC].n == 3 && has_near(&R.trig[SEQ_PERC], 375.0, 1.5),
          "3-step perc every 3 16ths (%d)", R.trig[SEQ_PERC].n);
}

/* Hand-build a locked bass line to test gates and glide. */
static void set_lock(seq_t *s, int k, bool on, int len, bool slide, int index)
{
    seq_pattern_t *p = &s->pat[SEQ_BASS][s->v[SEQ_BASS].slot];
    p->locked         = true;
    p->lock[k].on     = on;
    p->lock[k].len    = (uint8_t)len;
    p->lock[k].slide  = slide;
    p->lock[k].index  = (uint8_t)index;
    p->lock[k].scale  = false;
    p->lock[k].jump   = 0;
}

static void locked_bass(seq_t *s)
{
    seq_knobs(s, SEQ_LEAD)->density = 0.f;
    for(int k = 0; k < SEQ_MAX_STEPS; k++)
        set_lock(s, k, false, 8, false, 0);
    clear_drums(s);
    s->prog.count         = 1; /* one chord, so pitches are easy to predict */
    s->prog.chord[0].degree = 0;
}

static void test_gates_and_glide(void)
{
    /* Half-length notes: gate up at the step, down half a step later. */
    seq_init(&S, 8);
    locked_bass(&S);
    set_lock(&S, 0, true, 8, false, 0);
    set_lock(&S, 1, true, 8, false, 1);
    rec_clear();
    seq_process(&S, 0, NULL);
    seq_play(&S);
    run(&S, 0, 300, 0, 0, 0);
    CHECK(has_near(&R.gate_up[SEQ_BASS], 0, 1.5) && has_near(&R.gate_down[SEQ_BASS], 62.5, 1.5),
          "note 1: 0..62.5 ms");
    CHECK(has_near(&R.gate_up[SEQ_BASS], 125, 1.5) && has_near(&R.gate_down[SEQ_BASS], 187.5, 1.5),
          "note 2: 125..187.5 ms");

    /* Full-step notes are retriggered with a short dip. */
    seq_init(&S, 8);
    locked_bass(&S);
    set_lock(&S, 0, true, 16, false, 0);
    set_lock(&S, 1, true, 8, false, 1);
    rec_clear();
    seq_process(&S, 0, NULL);
    seq_play(&S);
    run(&S, 0, 300, 0, 0, 0);
    CHECK(R.gate_up[SEQ_BASS].n == 2 && has_near(&R.gate_up[SEQ_BASS], 127, 2.5),
          "full-step notes retrigger (%d ups)", R.gate_up[SEQ_BASS].n);

    /* Tie: a 2-step note into the next -> gate held, pitch jumps, no glide. */
    seq_init(&S, 8);
    locked_bass(&S);
    set_lock(&S, 0, true, 32, false, 0);
    set_lock(&S, 1, true, 8, false, 1);
    rec_clear();
    seq_process(&S, 0, NULL);
    seq_play(&S);
    seq_out_t o = run(&S, 0, 126, 0, 0, 0);
    float p1 = o.pitch[SEQ_BASS];
    o = run(&S, 126, 130, 0, 0, 0);
    CHECK(R.gate_up[SEQ_BASS].n == 1, "tie: one attack only (%d)", R.gate_up[SEQ_BASS].n);
    CHECK(o.gate[SEQ_BASS], "tie: gate still up");
    CHECK(o.pitch[SEQ_BASS] != p1 && S.v[SEQ_BASS].glide_rate == 0.f, "tie: pitch jumps");

    /* Slide: a short note followed by a slide step -> the gate is held and the
     * pitch glides over glide_ms. */
    seq_init(&S, 8);
    locked_bass(&S);
    S.glide_ms = 60;
    set_lock(&S, 0, true, 8, false, 0);
    set_lock(&S, 1, true, 8, true, 2); /* root -> fifth, a slide */
    rec_clear();
    seq_process(&S, 0, NULL);
    seq_play(&S);
    o = run(&S, 0, 124, 0, 0, 0);
    float from = o.pitch[SEQ_BASS];
    CHECK(o.gate[SEQ_BASS], "slide: previous gate held up to the slide");
    o = run(&S, 124, 155, 0, 0, 0);
    float mid = o.pitch[SEQ_BASS];
    o = run(&S, 155, 200, 0, 0, 0);
    float to = o.pitch[SEQ_BASS];
    CHECK(R.gate_up[SEQ_BASS].n == 1, "slide: no new attack (%d)", R.gate_up[SEQ_BASS].n);
    CHECK(fabsf((to - from) * 12.f - 7.f) < 0.01f, "slide lands a fifth up (%.3f st)",
          (to - from) * 12.f);
    CHECK(mid > from && mid < to, "slide passes between (%.3f %.3f %.3f)", from, mid, to);

    /* Voice mute: no gates at all. */
    seq_init(&S, 8);
    seq_knobs(&S, SEQ_BASS)->density = 1.f;
    seq_voice_mute(&S, SEQ_BASS, true);
    rec_clear();
    seq_process(&S, 0, NULL);
    seq_play(&S);
    run(&S, 0, 1000, 0, 0, 0);
    CHECK(R.gate_up[SEQ_BASS].n == 0, "muted voice never gates");

    /* Voice pattern queue: slot 1 silent, switches on the bar. */
    seq_init(&S, 8);
    seq_knobs(&S, SEQ_BASS)->density = 1.f;
    S.pat[SEQ_BASS][1].knobs.density = 0.f;
    rec_clear();
    seq_process(&S, 0, NULL);
    seq_play(&S);
    run(&S, 0, 500, 0, 0, 0);
    seq_voice_queue(&S, SEQ_BASS, 1);
    run(&S, 500, 3000, 0, 0, 0);
    int late = 0;
    for(int i = 0; i < R.gate_up[SEQ_BASS].n; i++)
        late += R.gate_up[SEQ_BASS].t[i] >= 2000;
    CHECK(has_near(&R.gate_up[SEQ_BASS], 1875, 3) && late == 0,
          "bass pattern switch waits for the bar (late %d)", late);
}

int main(void)
{
    test_scales();
    test_determinism();
    test_density_monotone();
    test_length_monotone();
    test_chord_following();
    test_lock_mutate_undo();
    test_internal_clock();
    test_external_clock();
    test_drums();
    test_gates_and_glide();
    printf("%d checks, %d failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
