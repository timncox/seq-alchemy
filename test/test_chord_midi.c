/* Native tests for core/chord_midi.c. Every check prints on failure; exit 1
 * if any. */
#include "chord_midi.h"

#include <stdio.h>
#include <stdlib.h>
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

static seq_t             S;
static chord_midi_t      M;
static chord_midi_ring_t R;

/* Everything that came out, drained as the UART would. */
static uint8_t g_out[1 << 16];
static size_t  g_n;

static void drain(void)
{
    uint8_t b;
    while(chord_midi_ring_pop(&R, &b))
        if(g_n < sizeof g_out)
            g_out[g_n++] = b;
}

/* Run the sequencer from ms a to b at 1 ms with the chord output after
 * each seq_process, draining every ms. */
static void run(uint32_t a, uint32_t b)
{
    seq_out_t o;
    for(uint32_t ms = a; ms < b; ms++)
    {
        seq_process(&S, ms * 1000u, &o);
        chord_midi_update(&M, &S, &R);
        drain();
    }
}

static void setup(int mode)
{
    seq_init(&S, 7);
    /* C major, I - vi - IV - V, a bar each. */
    S.prog.root  = 0;
    S.prog.scale = SEQ_SCALE_MAJOR;
    S.prog.tones = 3;
    S.prog.count = 4;
    const int8_t degs[4] = {0, 5, 3, 4};
    for(int i = 0; i < 4; i++)
        S.prog.chord[i].degree = degs[i];
    chord_midi_init(&M);
    M.mode = (uint8_t)mode;
    chord_midi_ring_init(&R);
    g_n = 0;
}

/* The bytes with clock / start / stop taken out. */
static size_t notes_only(uint8_t *dst)
{
    size_t n = 0;
    for(size_t i = 0; i < g_n; i++)
        if(g_out[i] < 0xF8)
            dst[n++] = g_out[i];
    return n;
}

static int count_byte(uint8_t x)
{
    int c = 0;
    for(size_t i = 0; i < g_n; i++)
        c += g_out[i] == x;
    return c;
}

static void dump(const uint8_t *b, size_t n)
{
    for(size_t i = 0; i < n; i++)
        printf("%02X%s", b[i], (i % 3 == 2) ? " | " : " ");
    printf("\n");
}

/* ------------------------------------------------------------ voicing */

static void test_voicing(void)
{
    setup(CHORD_MIDI_NOTES);
    uint8_t n[4];
    const uint8_t want[4][3] = {{60, 64, 67},  /* C:  C4 E4 G4 */
                                {57, 60, 64},  /* Am: A3 C4 E4 */
                                {65, 69, 72},  /* F:  F4 A4 C5 */
                                {55, 59, 62}}; /* G:  G3 B3 D4 */
    for(int c = 0; c < 4; c++)
    {
        int k = chord_midi_voicing(&S.prog, c, n);
        CHECK(k == 3, "chord %d: %d notes", c, k);
        CHECK(!memcmp(n, want[c], 3), "chord %d: %d %d %d", c, n[0], n[1], n[2]);
    }

    S.prog.tones = 4; /* sevenths: Cmaj7, Am7 */
    int k = chord_midi_voicing(&S.prog, 0, n);
    CHECK(k == 4 && n[0] == 60 && n[1] == 64 && n[2] == 67 && n[3] == 71,
          "Cmaj7 %d %d %d %d", n[0], n[1], n[2], n[3]);
    chord_midi_voicing(&S.prog, 1, n);
    CHECK(n[0] == 57 && n[1] == 60 && n[2] == 64 && n[3] == 67,
          "Am7 %d %d %d %d", n[0], n[1], n[2], n[3]);

    /* The default progression: A minor i - VI - III - VII. Root F#3..F4. */
    seq_init(&S, 1);
    for(int c = 0; c < 4; c++)
    {
        chord_midi_voicing(&S.prog, c, n);
        CHECK(n[0] >= CHORD_MIDI_ROOT_LO && n[0] <= CHORD_MIDI_ROOT_LO + 11,
              "default chord %d root %d", c, n[0]);
    }
    /* Every key, scale, degree and size: root F#3..F4, rising, within an
     * octave and a fifth (pentatonic sevenths span the most). */
    int bad = 0;
    for(int root = 0; root < 12; root++)
        for(int sc = 0; sc < SEQ_SCALE_COUNT; sc++)
            for(int deg = -7; deg <= 13; deg++)
                for(int t = 1; t <= 5; t++)
                {
                    S.prog.root = (uint8_t)root;
                    S.prog.scale = (uint8_t)sc;
                    S.prog.tones = (uint8_t)t;
                    S.prog.chord[0].degree = (int8_t)deg;
                    int c = chord_midi_voicing(&S.prog, 0, n);
                    if(c < 3 || c > 4 || n[0] < 54 || n[0] > 65 || n[c - 1] - n[0] > 19)
                        bad++;
                    for(int i = 1; i < c; i++)
                        if(n[i] <= n[i - 1])
                            bad++;
                }
    CHECK(bad == 0, "%d voicings out of range or not rising", bad);
}

/* ------------------------------------------------------------ changes */

static void test_progression_bytes(void)
{
    setup(CHORD_MIDI_NOTES);
    run(0, 50);
    CHECK(g_n == 0, "stopped: %zu bytes", g_n);

    seq_play(&S);
    run(50, 50 + 8 * 1000 + 100); /* 120 BPM: a bar is 2 s; C Am F G, C again */
    seq_stop(&S);
    run(8150, 8200);

    const uint8_t want[] = {
        0x90, 60, 100, 0x90, 64, 100, 0x90, 67, 100, /* C on */
        0x80, 67, 0,   0x90, 57, 100,                /* -> Am: G off, A on; C E held */
        0x80, 57, 0,   0x80, 60, 0,   0x80, 64, 0,   /* -> F */
        0x90, 65, 100, 0x90, 69, 100, 0x90, 72, 100,
        0x80, 65, 0,   0x80, 69, 0,   0x80, 72, 0,   /* -> G */
        0x90, 55, 100, 0x90, 59, 100, 0x90, 62, 100,
        0x80, 55, 0,   0x80, 59, 0,   0x80, 62, 0,   /* -> C (loop) */
        0x90, 60, 100, 0x90, 64, 100, 0x90, 67, 100,
        0x80, 60, 0,   0x80, 64, 0,   0x80, 67, 0,   /* stop: all off */
    };
    CHECK(g_n == sizeof want && !memcmp(g_out, want, sizeof want),
          "C-Am-F-G-C-stop bytes (%zu, want %zu)", g_n, sizeof want);
    if(g_n != sizeof want || memcmp(g_out, want, sizeof want))
        dump(g_out, g_n);
    CHECK(count_byte(0xF8) == 0 && count_byte(0xFA) == 0, "no clock in notes mode");
}

static void test_change_lands_on_bar(void)
{
    /* The Am arrives within a millisecond of the bar line. */
    setup(CHORD_MIDI_NOTES);
    seq_play(&S);
    seq_out_t o;
    uint32_t  at = 0;
    for(uint32_t ms = 0; ms < 2100 && !at; ms++)
    {
        seq_process(&S, ms * 1000u, &o);
        size_t before = chord_midi_ring_count(&R);
        chord_midi_update(&M, &S, &R);
        if(ms > 0 && chord_midi_ring_count(&R) > before)
            at = ms;
        drain();
    }
    CHECK(at >= 2000 && at <= 2001, "chord change at %u ms (bar at 2000)", at);
}

static void test_channel_and_off(void)
{
    setup(CHORD_MIDI_NOTES);
    seq_play(&S);
    run(0, 100);
    g_n = 0;
    M.channel = 4; /* channel 5 */
    run(100, 102);
    const uint8_t want[] = {0x80, 60, 0, 0x80, 64, 0, 0x80, 67, 0,
                            0x94, 60, 100, 0x94, 64, 100, 0x94, 67, 100};
    CHECK(g_n == sizeof want && !memcmp(g_out, want, sizeof want), "channel move");
    if(g_n != sizeof want || memcmp(g_out, want, sizeof want))
        dump(g_out, g_n);

    g_n = 0;
    M.mode = CHORD_MIDI_OFF;
    run(102, 200);
    const uint8_t off[] = {0x84, 60, 0, 0x84, 64, 0, 0x84, 67, 0};
    CHECK(g_n == sizeof off && !memcmp(g_out, off, sizeof off), "output off releases");
    g_n = 0;
    run(200, 4500); /* chord changes while off: nothing */
    CHECK(g_n == 0, "off: %zu bytes", g_n);

    /* Live edit: the key moves up a tone while C plays -> D, all three move. */
    setup(CHORD_MIDI_NOTES);
    seq_play(&S);
    run(0, 100);
    g_n = 0;
    S.prog.root = 2;
    run(100, 101);
    uint8_t n[64];
    size_t  k = notes_only(n);
    CHECK(k == 18 && n[0] == 0x80 && n[9] == 0x90 && n[10] == 62, "key change re-voices");
}

static void test_clock(void)
{
    setup(CHORD_MIDI_CLOCK);
    run(0, 100);
    CHECK(g_n == 0, "stopped: no clock (%zu)", g_n);
    seq_play(&S);
    run(100, 1100); /* 1 s at 120 BPM: two beats */
    CHECK(g_out[0] == 0xFA && g_out[1] == 0xF8 && g_out[2] == 0x90,
          "start, clock, then notes: %02X %02X %02X", g_out[0], g_out[1], g_out[2]);
    int f8 = count_byte(0xF8);
    CHECK(f8 >= 48 && f8 <= 49, "2 beats = 48 clocks (+ the one at 0): %d", f8);
    CHECK(count_byte(0xFA) == 1, "one start");

    /* A rewind while running (J2 reset on the internal clock): Start again. */
    seq_reset_edge(&S, 1100 * 1000u);
    run(1100, 1600);
    CHECK(count_byte(0xFA) == 2, "reset -> start again (%d)", count_byte(0xFA));
    f8 = count_byte(0xF8);
    CHECK(f8 >= 48 + 24 && f8 <= 49 + 25, "clock restarted from 0: %d", f8);

    seq_stop(&S);
    g_n = 0;
    run(1600, 2000);
    CHECK(g_n >= 10 && g_out[0] == 0xFC, "stop first, then the notes off");
    CHECK(count_byte(0xF8) == 0, "no clock while stopped");
    CHECK(count_byte(0x80) == 3, "three offs");

    /* Clock mode -> notes mode while running: a Stop, notes stay. */
    setup(CHORD_MIDI_CLOCK);
    seq_play(&S);
    run(0, 500);
    g_n = 0;
    M.mode = CHORD_MIDI_NOTES;
    run(500, 1000);
    CHECK(g_n == 1 && g_out[0] == 0xFC, "clock off -> 0xFC only (%zu)", g_n);
}

static void test_external_clock_lost(void)
{
    /* Following J1 at 16ths; the clock stops: transport stops, notes off. */
    setup(CHORD_MIDI_CLOCK);
    S.clock_ppqn = 4;
    seq_out_t o;
    for(uint32_t ms = 0; ms < 2000; ms++)
    {
        if(ms % 125 == 0)
            seq_clock_edge(&S, ms * 1000u);
        seq_process(&S, ms * 1000u, &o);
        chord_midi_update(&M, &S, &R);
        drain();
    }
    CHECK(count_byte(0xFA) == 1 && count_byte(0x90) == 3, "J1 starts it");
    int f8 = count_byte(0xF8);
    CHECK(f8 >= 15 * 6 && f8 <= 16 * 6 + 1, "6 clocks per J1 16th: %d", f8);
    g_n = 0;
    run(2000, 2000 + SEQ_CLOCK_LOST_MS + 100);
    CHECK(count_byte(0xFC) == 1 && count_byte(0x80) == 3, "clock lost -> stop + offs");
}

/* ------------------------------------------------------------ no stuck notes */

static uint32_t rng = 12345;
static uint32_t rnd(uint32_t n)
{
    rng = rng * 1664525u + 1013904223u;
    return (rng >> 8) % n;
}

/* A receiver: note on / off by channel. Counts the offences. */
static int8_t g_on[16][128];
static int    g_double_on, g_orphan_off, g_bad_bytes;

static void receive(void)
{
    static uint8_t msg[3];
    static int     have = 0;
    for(size_t i = 0; i < g_n; i++)
    {
        uint8_t b = g_out[i];
        if(b >= 0xF8)
            continue; /* real time, may be anywhere */
        if(b & 0x80)
        {
            if(have != 0)
                g_bad_bytes++;
            msg[0] = b;
            have   = 1;
            continue;
        }
        if(have == 0)
        {
            g_bad_bytes++;
            continue;
        }
        msg[have++] = b;
        if(have == 3)
        {
            int ch = msg[0] & 15, note = msg[1];
            if((msg[0] & 0xF0) == 0x90)
            {
                if(g_on[ch][note])
                    g_double_on++;
                g_on[ch][note] = 1;
            }
            else if((msg[0] & 0xF0) == 0x80)
            {
                if(!g_on[ch][note])
                    g_orphan_off++;
                g_on[ch][note] = 0;
            }
            else
                g_bad_bytes++;
            have = 0;
        }
    }
    g_n = 0;
}

/* Once everything sent has arrived, the receiver has on exactly what the
 * output thinks it holds. */
static int g_desync;
static bool in_sync(void)
{
    int on = 0;
    for(int c = 0; c < 16; c++)
        for(int n = 0; n < 128; n++)
            if(g_on[c][n])
            {
                on++;
                bool held = false;
                for(int i = 0; i < M.n_held; i++)
                    held |= c == M.sent_channel && n == M.held[i];
                if(!held)
                    return false;
            }
    return on == M.n_held;
}

static void test_no_stuck_notes(void)
{
    setup(CHORD_MIDI_CLOCK);
    memset(g_on, 0, sizeof g_on);
    g_double_on = g_orphan_off = g_bad_bytes = 0;
    seq_play(&S);
    seq_out_t o;
    uint32_t  ms = 0;
    int       changes = 0;
    for(int i = 0; i < 1000; i++)
    {
        switch(rnd(10))
        {
        case 0: S.prog.chord[rnd(SEQ_MAX_CHORDS)].degree = (int8_t)(rnd(21) - 7); break;
        case 1: S.prog.tones = (uint8_t)(3 + rnd(2)); break;
        case 2: S.prog.root = (uint8_t)rnd(12); break;
        case 3: S.prog.scale = (uint8_t)rnd(SEQ_SCALE_COUNT); break;
        case 4: S.prog.count = (uint8_t)(1 + rnd(SEQ_MAX_CHORDS)); break;
        case 5: if(S.running) seq_stop(&S); else seq_play(&S); break;
        case 6: M.mode = (uint8_t)rnd(CHORD_MIDI_MODES); break;
        case 7: M.channel = (uint8_t)rnd(16); break;
        case 8: seq_reset_edge(&S, ms * 1000u); break;
        default: S.bpm = 30.f + (float)rnd(271); break;
        }
        /* Run a while. Sometimes the UART falls behind: the ring is nearly
         * full of other bytes (0xFE, which the receiver ignores) and drains
         * slowly, so changes have to wait for room. */
        uint32_t span  = 1 + rnd(400);
        bool     stall = rnd(3) == 0;
        if(stall)
            for(size_t f = rnd(CHORD_MIDI_RING); chord_midi_ring_free(&R) > f;)
                R.buf[R.head++ % CHORD_MIDI_RING] = 0xFE;
        for(uint32_t k = 0; k < span; k++, ms++)
        {
            seq_process(&S, ms * 1000u, &o);
            size_t before = chord_midi_ring_count(&R);
            chord_midi_update(&M, &S, &R);
            if(chord_midi_ring_count(&R) > before)
                changes++;
            if(!stall || k % 16 == 0)
                for(int j = 0; j < 3; j++) /* ~31,250 baud: 3 bytes a ms */
                {
                    uint8_t b;
                    if(chord_midi_ring_pop(&R, &b))
                        g_out[g_n++] = b;
                }
            receive();
            if(chord_midi_ring_count(&R) == 0 && !in_sync())
                g_desync++;
        }
    }
    /* Flush: output off, run until the ring is empty. */
    M.mode = CHORD_MIDI_OFF;
    for(int k = 0; k < 2000; k++, ms++)
    {
        seq_process(&S, ms * 1000u, &o);
        chord_midi_update(&M, &S, &R);
        drain();
        receive();
    }
    int stuck = 0;
    for(int c = 0; c < 16; c++)
        for(int n = 0; n < 128; n++)
            stuck += g_on[c][n];
    CHECK(M.dropped > 20, "the ring never filled (%u)", M.dropped);
    CHECK(changes > 300, "only %d changes went out", changes);
    CHECK(stuck == 0, "%d stuck notes", stuck);
    CHECK(g_desync == 0, "%d times the receiver disagreed with what is held", g_desync);
    CHECK(g_double_on == 0, "%d notes switched on twice", g_double_on);
    CHECK(g_orphan_off == 0, "%d offs for notes not on", g_orphan_off);
    CHECK(g_bad_bytes == 0, "%d malformed bytes", g_bad_bytes);
    CHECK(M.n_held == 0 && !M.clock_started, "nothing held at the end");
    printf("  no-stuck run: %d changes, %u put off for ring room\n", changes, M.dropped);
}

static void test_full_ring_waits(void)
{
    /* A full ring: the change waits, whole, and goes out once there is room. */
    setup(CHORD_MIDI_NOTES);
    for(int i = 0; i < CHORD_MIDI_RING - 4; i++)
        R.buf[R.head++ % CHORD_MIDI_RING] = 0xFE; /* active sensing filler */
    seq_play(&S);
    seq_out_t o;
    seq_process(&S, 0, &o);
    chord_midi_update(&M, &S, &R);
    CHECK(chord_midi_ring_count(&R) == CHORD_MIDI_RING - 4 && M.n_held == 0 && M.dropped == 1,
          "no room: nothing half-sent");
    uint8_t b;
    for(int i = 0; i < 10; i++)
        chord_midi_ring_pop(&R, &b);
    chord_midi_update(&M, &S, &R);
    CHECK(M.n_held == 3 && chord_midi_ring_count(&R) == CHORD_MIDI_RING - 14 + 9,
          "room again: the chord goes out whole");
}

/* ------------------------------------------------------------ CV */

static chord_cv_t C;

/* Run from ms a to b; returns the volts at the end and how many times the
 * value changed on the way. */
static float run_cv(uint32_t a, uint32_t b, int mode, int *changes)
{
    seq_out_t o;
    float     v = C.volts, last = v;
    for(uint32_t ms = a; ms < b; ms++)
    {
        seq_process(&S, ms * 1000u, &o);
        v = chord_cv_update(&C, &S, mode);
        if(changes && v != last)
            (*changes)++;
        last = v;
    }
    return v;
}

static bool near(float a, float b) { return a > b - 1e-6f && a < b + 1e-6f; }

static void test_cv(void)
{
    /* C major I - vi - IV - V: roots C A F G = 0, 9, 5, 7 twelfths. */
    setup(CHORD_MIDI_OFF);
    chord_cv_init(&C, &S);
    CHECK(near(C.volts, 0.f), "boot: chord 1's root (%f)", C.volts);
    seq_play(&S);
    const float want[5] = {0.f, 9.f / 12, 5.f / 12, 7.f / 12, 0.f};
    for(int bar = 0; bar < 5; bar++)
    {
        int ch = 0;
        run_cv(bar * 2000u, bar * 2000u + 2, CHORD_CV_ROOT, NULL); /* the change */
        float v = run_cv(bar * 2000u + 2, bar * 2000u + 2000, CHORD_CV_ROOT, &ch);
        CHECK(near(v, want[bar]), "bar %d: %f V, want %f", bar + 1, v, want[bar]);
        CHECK(ch == 0, "bar %d: steady within the bar (%d changes)", bar + 1, ch);
    }
    /* The change lands on the bar line: 1999 ms is still C, 2001 is Am. */
    setup(CHORD_MIDI_OFF);
    chord_cv_init(&C, &S);
    seq_play(&S);
    CHECK(near(run_cv(0, 2000, CHORD_CV_ROOT, NULL), 0.f), "1999 ms: C");
    CHECK(near(run_cv(2000, 2002, CHORD_CV_ROOT, NULL), 0.75f), "2001 ms: A");

    /* Key change while running: up a tone, the vi of D is B. */
    S.prog.root = 2;
    CHECK(near(run_cv(2002, 2003, CHORD_CV_ROOT, NULL), 11.f / 12), "key D: B");

    /* A minor default progression: i VI III VII = A F C G. Key 9 wraps. */
    seq_init(&S, 1);
    for(int c = 0; c < 4; c++)
    {
        const float am[4] = {9.f / 12, 5.f / 12, 0.f, 7.f / 12};
        CHECK(near(chord_cv_root_volts(&S.prog, c), am[c]), "A minor chord %d", c);
    }
    /* Every key, scale, degree: inside 0 .. 11/12 and on a semitone. */
    int bad = 0;
    for(int root = 0; root < 12; root++)
        for(int sc = 0; sc < SEQ_SCALE_COUNT; sc++)
            for(int deg = -7; deg <= 13; deg++)
            {
                S.prog.root = (uint8_t)root;
                S.prog.scale = (uint8_t)sc;
                S.prog.chord[0].degree = (int8_t)deg;
                float v = chord_cv_root_volts(&S.prog, 0);
                float k = v * 12.f;
                if(v < 0.f || v > 11.f / 12 + 1e-6f || !near(k, (float)(int)(k + 0.5f)))
                    bad++;
            }
    CHECK(bad == 0, "%d roots off the 1/12 V grid", bad);

    /* Stop holds; key edits while stopped wait; play goes back to chord 1. */
    setup(CHORD_MIDI_OFF);
    chord_cv_init(&C, &S);
    seq_play(&S);
    run_cv(0, 4100, CHORD_CV_ROOT, NULL); /* in bar 3: F */
    seq_stop(&S);
    int ch = 0;
    float v = run_cv(4100, 6000, CHORD_CV_ROOT, &ch);
    CHECK(near(v, 5.f / 12) && ch == 0, "stopped: F held (%f, %d changes)", v, ch);
    S.prog.root = 7;
    CHECK(near(run_cv(6000, 6100, CHORD_CV_ROOT, NULL), 5.f / 12), "stopped: key edit waits");
    seq_play(&S);
    CHECK(near(run_cv(6100, 6102, CHORD_CV_ROOT, NULL), 7.f / 12), "play: G major's I = G");

    /* J1 clock lost: holds too. */
    setup(CHORD_MIDI_OFF);
    chord_cv_init(&C, &S);
    S.clock_ppqn = 4;
    seq_out_t o;
    for(uint32_t ms = 0; ms < 2600; ms++)
    {
        if(ms % 125 == 0)
            seq_clock_edge(&S, ms * 1000u);
        seq_process(&S, ms * 1000u, &o);
        chord_cv_update(&C, &S, CHORD_CV_ROOT);
    }
    CHECK(S.running && near(C.volts, 0.75f), "J1: bar 2, A");
    ch = 0;
    v  = run_cv(2600, 2600 + SEQ_CLOCK_LOST_MS + 500, CHORD_CV_ROOT, &ch);
    CHECK(!S.running && near(v, 0.75f) && ch == 0, "clock lost: A held");

    /* REF: 0 V, and the root comes back after. */
    CHECK(near(chord_cv_update(&C, &S, CHORD_CV_REF), 0.f), "ref = 0 V");
    CHECK(near(chord_cv_update(&C, &S, CHORD_CV_ROOT), 0.75f), "ref left: root again");

    /* The Chord out choices. */
    const uint8_t mm[CHORD_OUT_CHOICES] = {0, 1, 2, 0, 2, 0}, cm[CHORD_OUT_CHOICES] = {0, 0, 0, 1, 1, 2};
    int wrong = 0;
    for(int i = 0; i < CHORD_OUT_CHOICES; i++)
    {
        uint8_t m, c;
        chord_out_modes(i, &m, &c);
        wrong += m != mm[i] || c != cm[i];
    }
    uint8_t m = 9, c = 9;
    chord_out_modes(77, &m, &c);
    CHECK(wrong == 0 && m == CHORD_MIDI_OFF && c == CHORD_CV_OFF, "chord out choices");
}

int main(void)
{
    test_cv();
    test_voicing();
    test_progression_bytes();
    test_change_lands_on_bar();
    test_channel_and_off();
    test_clock();
    test_external_clock_lost();
    test_full_ring_waits();
    test_no_stuck_notes();
    printf("chord_midi: %d checks, %d failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
