/*
 * render -- hear the sequencer core without hardware.
 *
 * Runs core/seq.c exactly as the Lab would (seq_process every millisecond)
 * and plays its outputs through stand-ins for the patch: pitch + gate into a
 * saw -> resonant low-pass -> VCA for the bass, a pulse lead with a little
 * delay, and four synthesized drums on the triggers. The synths are only
 * there to make the CV audible; they are not part of the firmware.
 *
 * A scripted "tour" moves the knobs bar by bar and writes a timeline next to
 * the WAV so you can hear what each one does.
 *
 *   render [seed] [bpm] [out.wav]
 */
#include "seq.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SR 48000
#define BARS 32
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static seq_t S;
static FILE *g_log;

/* ------------------------------------------------------------ synths */

static float hz_from_volts(float v) { return 16.3516f * powf(2.0f, v); } /* 0 V = C0 */

static uint32_t g_noise = 22222;
static float noise(void)
{
    g_noise ^= g_noise << 13;
    g_noise ^= g_noise >> 17;
    g_noise ^= g_noise << 5;
    return (float)(int32_t)g_noise / 2147483648.0f;
}

/* Chamberlin state-variable filter, oversampled x2 for stability. */
typedef struct { float lp, bp; } svf_t;
static float svf_lp(svf_t *f, float x, float hz, float q)
{
    float fc = 2.0f * sinf((float)M_PI * fminf(hz, 9000.f) / (2.0f * SR));
    for(int i = 0; i < 2; i++)
    {
        float hp = x - f->lp - q * f->bp;
        f->bp += fc * hp;
        f->lp += fc * f->bp;
    }
    return f->lp;
}

typedef struct {
    float phase, fenv, aenv;
    bool  gate;
    svf_t f;
} bass_t;

static float bass_tick(bass_t *b, float volts, bool gate)
{
    if(gate && !b->gate)
        b->fenv = 1.0f; /* each attack re-opens the filter */
    b->gate = gate;
    float hz = hz_from_volts(volts);
    b->phase += hz / SR;
    if(b->phase >= 1.f)
        b->phase -= 1.f;
    /* polyBLEP saw */
    float t = b->phase, dt = hz / SR, saw = 2.f * t - 1.f;
    if(t < dt) { float x = t / dt; saw -= x + x - x * x - 1.f; }
    else if(t > 1.f - dt) { float x = (t - 1.f) / dt; saw -= x * x + x + x + 1.f; }

    b->fenv *= 0.99985f;                                   /* ~140 ms decay */
    float target = gate ? 1.f : 0.f;
    b->aenv += (target - b->aenv) * (gate ? 0.02f : 0.004f);
    float cut = 180.f + 2600.f * b->fenv * b->fenv + 3.f * hz;
    return svf_lp(&b->f, saw, cut, 0.18f) * b->aenv * 0.55f;
}

typedef struct {
    float phase, aenv;
    svf_t f;
} lead_t;

static float lead_tick(lead_t *l, float volts, bool gate)
{
    float hz = hz_from_volts(volts) * 2.0f; /* voice tuned an octave up */
    l->phase += hz / SR;
    if(l->phase >= 1.f)
        l->phase -= 1.f;
    float pulse = l->phase < 0.35f ? 1.f : -1.f;
    float target = gate ? 1.f : 0.f;
    l->aenv += (target - l->aenv) * (gate ? 0.01f : 0.0008f);
    return svf_lp(&l->f, pulse, 2200.f, 0.9f) * l->aenv * 0.16f;
}

typedef struct { float t, env, ph, hp_lp; bool on; } drum_t;

static float kick(drum_t *d)
{
    if(!d->on) return 0.f;
    d->t += 1.f / SR;
    float f = 45.f + 110.f * expf(-d->t * 30.f);
    d->ph += f / SR;
    float a = expf(-d->t * 7.f);
    if(a < 0.001f) d->on = false;
    return sinf(2.f * (float)M_PI * d->ph) * a * 0.9f;
}

static float snare(drum_t *d)
{
    if(!d->on) return 0.f;
    d->t += 1.f / SR;
    d->ph += 185.f / SR;
    float tone = sinf(2.f * (float)M_PI * d->ph) * expf(-d->t * 25.f);
    float n = noise();
    d->hp_lp += (n - d->hp_lp) * 0.2f;
    float a = expf(-d->t * 16.f);
    if(a < 0.001f) d->on = false;
    return (tone * 0.4f + (n - d->hp_lp) * 0.6f * a) * 0.55f;
}

static float hat(drum_t *d)
{
    if(!d->on) return 0.f;
    d->t += 1.f / SR;
    float n = noise();
    d->hp_lp += (n - d->hp_lp) * 0.5f;
    float a = expf(-d->t * 55.f);
    if(a < 0.001f) d->on = false;
    return (n - d->hp_lp) * a * 0.28f;
}

static float perc(drum_t *d)
{
    if(!d->on) return 0.f;
    d->t += 1.f / SR;
    d->ph += 1.f / SR;
    float s = (sinf(2.f * (float)M_PI * 560.f * d->ph) > 0 ? 1.f : -1.f)
              + (sinf(2.f * (float)M_PI * 845.f * d->ph) > 0 ? 1.f : -1.f);
    d->hp_lp += (s - d->hp_lp) * 0.15f;
    float a = expf(-d->t * 22.f);
    if(a < 0.001f) d->on = false;
    return d->hp_lp * a * 0.12f;
}

/* ------------------------------------------------------------ the tour */

static void say(int bar, const char *msg)
{
    printf("bar %2d  %s\n", bar + 1, msg);
    if(g_log)
        fprintf(g_log, "bar %2d  %s\n", bar + 1, msg);
}

/* Called at the start of each bar (just before its downbeat). */
static void tour(int bar)
{
    seq_knobs_t *b = seq_knobs(&S, SEQ_BASS);
    seq_knobs_t *l = seq_knobs(&S, SEQ_LEAD);
    switch(bar)
    {
    case 0:
        b->density = 0.25f; b->length = 0.3f; b->slide = 0.0f; b->jump = 0.0f;
        l->density = 0.0f;
        seq_drum_mute(&S, SEQ_SNARE, true);
        seq_drum_mute(&S, SEQ_PERC, true);
        say(bar, "kick + hat. bass DENSITY 0.25, LENGTH 0.3: the downbeats come first");
        break;
    case 4:
        b->density = 0.55f;
        seq_drum_mute(&S, SEQ_SNARE, false);
        say(bar, "bass DENSITY 0.55: notes are only ADDED to the same line; snare unmuted");
        break;
    case 8:
        b->length = 0.75f; b->slide = 0.3f;
        say(bar, "bass LENGTH 0.75 + SLIDE 0.3: longer, tied notes and 303 glides");
        break;
    case 12:
        l->density = 0.3f; l->length = 0.5f;
        say(bar, "lead enters: DENSITY 0.3, following the same chords");
        break;
    case 16:
        l->density = 0.55f; b->jump = 0.3f; b->density = 0.75f;
        say(bar, "lead DENSITY 0.55; bass JUMP 0.3 (octave jumps), DENSITY 0.75");
        break;
    case 20:
        seq_mutate(&S, SEQ_BASS, 0.25f);
        S.swing = 0.62f;
        say(bar, "MUTATE bass 25 %; SWING 0.62");
        break;
    case 24:
        seq_lock(&S, SEQ_BASS, true);
        seq_roll(&S, SEQ_LEAD);
        seq_drum_mute(&S, SEQ_PERC, false);
        say(bar, "LOCK bass (keeps its line, still follows chords); ROLL lead; perc in");
        break;
    case 27:
        seq_drum_queue(&S, 1);
        say(bar, "drum pattern 2 queued -> starts on the next bar line");
        break;
    case 28:
        b->octave = 1; l->length = 0.85f; l->tone_bias = 0.6f;
        say(bar, "bass OCTAVE +1 (transposes the locked line); lead LENGTH 0.85, TONE BIAS 0.6");
        break;
    default:
        break;
    }
}

static void busy_pattern(seq_drums_t *d)
{
    /* Pattern 2: kick adds a push, 16th hats (odd ones at 50 %), perc answers. */
    d->prob[SEQ_KICK][14] = 2;
    for(int k = 0; k < 16; k++)
        d->prob[SEQ_HAT][k] = (k % 2) ? 2 : 4;
    d->prob[SEQ_PERC][3] = 4;
    d->prob[SEQ_PERC][11] = 3;
    d->prob[SEQ_PERC][13] = 2;
}

/* ------------------------------------------------------------ wav */

static void put16(FILE *f, uint16_t v) { fputc(v & 255, f); fputc(v >> 8, f); }
static void put32(FILE *f, uint32_t v) { put16(f, v & 0xffff); put16(f, v >> 16); }

int main(int argc, char **argv)
{
    uint32_t    seed = argc > 1 ? (uint32_t)strtoul(argv[1], 0, 10) : 2026;
    float       bpm  = argc > 2 ? (float)atof(argv[2]) : 124.f;
    const char *out  = argc > 3 ? argv[3] : "tour.wav";

    char logpath[512];
    snprintf(logpath, sizeof logpath, "%s.txt", out);
    g_log = fopen(logpath, "w");
    if(g_log)
        fprintf(g_log, "seq-alchemy tour -- seed %u, %.0f BPM, A minor i-VI-III-VII\n", seed, bpm);

    seq_init(&S, seed);
    S.bpm = bpm;
    S.drums[0].prob[SEQ_PERC][10] = 3; /* something for perc in pattern 1 */
    busy_pattern(&S.drums[1]);

    uint32_t bar_ms   = (uint32_t)(4.f * 60000.f / bpm);
    uint32_t total_ms = bar_ms * BARS + 1500;
    uint32_t frames   = total_ms * (SR / 1000);

    FILE *f = fopen(out, "wb");
    if(!f)
    {
        perror(out);
        return 1;
    }
    uint32_t data = frames * 4;
    fwrite("RIFF", 1, 4, f); put32(f, 36 + data); fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16); put16(f, 1); put16(f, 2); put32(f, SR); put32(f, SR * 4);
    put16(f, 4); put16(f, 16); fwrite("data", 1, 4, f); put32(f, data);

    static bass_t bass;
    static lead_t lead;
    static drum_t dr[SEQ_DRUMS];
    static float  dl[2][SR]; /* lead delay, dotted 8th, ping-pong */
    uint32_t dlen = (uint32_t)(SR * 60.f / bpm * 0.75f), dpos = 0;
    bool     last_trig[SEQ_DRUMS] = {0};
    seq_out_t o;
    memset(&o, 0, sizeof o);

    int next_bar = 0;
    seq_process(&S, 0, NULL);
    for(uint32_t ms = 0; ms < total_ms; ms++)
    {
        /* Knob moves land a millisecond before each bar line. */
        if(next_bar < BARS && ms + 1 >= (uint32_t)next_bar * bar_ms)
        {
            if(next_bar == 0)
                seq_play(&S);
            tour(next_bar++);
        }
        if(ms == bar_ms * BARS)
            seq_stop(&S);
        seq_process(&S, ms * 1000u, &o);
        for(int d = 0; d < SEQ_DRUMS; d++)
        {
            if(o.trig[d] && !last_trig[d])
                dr[d] = (drum_t){.on = true};
            last_trig[d] = o.trig[d];
        }
        for(int i = 0; i < SR / 1000; i++)
        {
            float b  = bass_tick(&bass, o.pitch[SEQ_BASS], o.gate[SEQ_BASS]);
            float ld = lead_tick(&lead, o.pitch[SEQ_LEAD], o.gate[SEQ_LEAD]);
            float k = kick(&dr[SEQ_KICK]), sn = snare(&dr[SEQ_SNARE]);
            float h = hat(&dr[SEQ_HAT]), p = perc(&dr[SEQ_PERC]);

            float dL = dl[0][dpos], dR = dl[1][dpos];
            dl[0][dpos] = ld + dR * 0.35f;
            dl[1][dpos] = dL * 0.9f;
            dpos = (dpos + 1) % dlen;

            float L = b + k + sn + ld * 0.8f + dL * 0.5f + h * 1.1f + p * 0.7f;
            float R = b + k + sn + ld * 0.9f + dR * 0.5f + h * 0.8f + p * 1.1f;
            L = tanhf(L * 0.9f);
            R = tanhf(R * 0.9f);
            put16(f, (uint16_t)(int16_t)(L * 30000.f));
            put16(f, (uint16_t)(int16_t)(R * 30000.f));
        }
    }
    fclose(f);
    if(g_log)
        fclose(g_log);
    printf("wrote %s (%.1f s) and %s\n", out, total_ms / 1000.0, logpath);
    return 0;
}
