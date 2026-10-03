/* chord_midi -- see chord_midi.h. */
#include "chord_midi.h"

#include <math.h>
#include <string.h>

#define NOTE_OFF 0x80
#define NOTE_ON 0x90
#define MIDI_CLOCK 0xF8
#define MIDI_START 0xFA
#define MIDI_STOP 0xFC
#define TICKS_PER_CLOCK (SEQ_PPQN / 24)

/* ---------------------------------------------------------------- ring */

void chord_midi_ring_init(chord_midi_ring_t *r) { memset(r, 0, sizeof *r); }

size_t chord_midi_ring_count(const chord_midi_ring_t *r)
{
    return (uint16_t)(r->head - r->tail);
}

size_t chord_midi_ring_free(const chord_midi_ring_t *r)
{
    return CHORD_MIDI_RING - chord_midi_ring_count(r);
}

static void ring_put(chord_midi_ring_t *r, uint8_t b)
{
    r->buf[r->head % CHORD_MIDI_RING] = b;
    r->head++;
}

bool chord_midi_ring_pop(chord_midi_ring_t *r, uint8_t *byte)
{
    if(r->head == r->tail)
        return false;
    *byte = r->buf[r->tail % CHORD_MIDI_RING];
    r->tail++;
    return true;
}

/* ---------------------------------------------------------------- notes */

void chord_midi_init(chord_midi_t *m)
{
    memset(m, 0, sizeof *m);
    m->mode = CHORD_MIDI_OFF;
}

int chord_midi_voicing(const seq_prog_t *pr, int ci, uint8_t *notes)
{
    if(ci < 0 || ci >= SEQ_MAX_CHORDS)
        ci = 0;
    int tones = pr->tones < 3 ? 3 : pr->tones > 4 ? 4 : pr->tones;
    int deg   = pr->chord[ci].degree;
    int base  = 60 + pr->root % 12 + seq_degree_semis(pr->scale, deg);
    while(base > CHORD_MIDI_ROOT_LO + 11)
        base -= 12;
    while(base < CHORD_MIDI_ROOT_LO)
        base += 12;
    int root = seq_degree_semis(pr->scale, deg);
    for(int i = 0; i < tones; i++)
        notes[i] = (uint8_t)(base + seq_degree_semis(pr->scale, deg + 2 * i) - root);
    return tones;
}

/* The progression's length in ticks (as seq.c counts it). */
static double prog_ticks(const seq_prog_t *pr)
{
    double t = 0;
    for(int i = 0; i < pr->count; i++)
        t += (double)(pr->chord[i].bars ? pr->chord[i].bars : 1) * SEQ_BAR;
    return t > 0 ? t : SEQ_BAR;
}

static bool has(const uint8_t *notes, int n, uint8_t x)
{
    for(int i = 0; i < n; i++)
        if(notes[i] == x)
            return true;
    return false;
}

void chord_midi_update(chord_midi_t *m, const seq_t *s, chord_midi_ring_t *r)
{
    const bool    on      = m->mode != CHORD_MIDI_OFF && m->mode < CHORD_MIDI_MODES;
    const bool    run     = on && s->running;
    const bool    clocked = run && m->mode == CHORD_MIDI_CLOCK;
    const uint8_t ch      = m->channel & 0x0F;

    /* What should be sounding. */
    uint8_t want[CHORD_MIDI_MAX_NOTES];
    int     nw = run ? chord_midi_voicing(&s->prog, seq_chord_index(s), want) : 0;

    /* A new channel: release everything on the old one first, then the
     * whole chord goes on again on the new one. */
    const bool moved = m->n_held && m->sent_channel != ch;

    uint8_t offs[CHORD_MIDI_MAX_NOTES], ons[CHORD_MIDI_MAX_NOTES];
    int     noff = 0, non = 0;
    for(int i = 0; i < m->n_held; i++)
        if(moved || !has(want, nw, m->held[i]))
            offs[noff++] = m->held[i];
    for(int i = 0; i < nw; i++)
        if(moved || !has(m->held, m->n_held, want[i]))
            ons[non++] = want[i];

    /* Transport messages. A restart is the transport rewinding to the top
     * while it runs (seq_play again, a J2 reset). */
    const bool restart = clocked && m->clock_started && s->pos < m->last_pos;
    /* The progression looping back to its first chord while it runs gets
     * a Start too (Tim, 2026-10-03), sent in the call that crosses the bar
     * line, so before that bar's clock. The clock count carries on: only a
     * real start or rewind sets it back to 0. A progression edited on the
     * way (its length changed) is not a loop. */
    const double total = prog_ticks(&s->prog);
    const bool   wrap  = clocked && m->clock_started && !restart && total == m->last_total
                      && s->pos > m->last_pos && m->last_pos >= 0
                      && fmod(s->pos, total) < fmod(m->last_pos, total);
    const bool start   = clocked && (!m->clock_started || restart || wrap);
    const bool stop    = m->clock_started && !clocked;
    int32_t    tick    = m->clock_tick;
    int        clocks  = 0;
    if(clocked)
    {
        if(start && !wrap)
            tick = 0;
        /* Clocks due by now: one at tick 0, then every 4 ticks. */
        int32_t due = s->pos < 0 ? 0 : (int32_t)(s->pos / TICKS_PER_CLOCK) + 1;
        clocks      = due - tick;
        if(clocks < 0)
            clocks = 0;
        if(clocks > CHORD_MIDI_MAX_CLOCKS)
            clocks = CHORD_MIDI_MAX_CLOCKS; /* the rest follow next call */
    }

    size_t need = 3u * (size_t)(noff + non) + (stop ? 1u : 0u) + (start ? 1u : 0u)
                  + (size_t)clocks;
    if(need == 0)
    {
        m->last_pos   = s->pos;
        m->last_total = total;
        return;
    }
    if(chord_midi_ring_free(r) < need)
    {
        m->dropped++; /* all or nothing: try again on the next call */
        return;
    }

    if(stop)
        ring_put(r, MIDI_STOP);
    for(int i = 0; i < noff; i++)
    {
        ring_put(r, (uint8_t)(NOTE_OFF | m->sent_channel));
        ring_put(r, offs[i]);
        ring_put(r, 0);
    }
    if(start)
        ring_put(r, MIDI_START);
    for(int i = 0; i < clocks; i++)
        ring_put(r, MIDI_CLOCK);
    for(int i = 0; i < non; i++)
    {
        ring_put(r, (uint8_t)(NOTE_ON | ch));
        ring_put(r, ons[i]);
        ring_put(r, CHORD_MIDI_VELOCITY);
    }

    /* Held = what is sounding now: the wanted notes (all of them are on
     * after this), on this channel. */
    memcpy(m->held, want, sizeof want);
    m->n_held        = (uint8_t)nw;
    m->sent_channel  = ch;
    m->clock_started = clocked;
    m->clock_tick    = clocked ? tick + clocks : 0;
    m->last_pos      = s->pos;
    m->last_total    = total;
}

/* ---------------------------------------------------------------- CV */

void chord_out_modes(int choice, uint8_t *midi_mode, uint8_t *cv_mode)
{
    static const uint8_t kMidi[CHORD_OUT_CHOICES] = {
        CHORD_MIDI_OFF, CHORD_MIDI_NOTES, CHORD_MIDI_CLOCK,
        CHORD_MIDI_OFF, CHORD_MIDI_CLOCK, CHORD_MIDI_OFF};
    static const uint8_t kCv[CHORD_OUT_CHOICES] = {
        CHORD_CV_OFF, CHORD_CV_OFF, CHORD_CV_OFF,
        CHORD_CV_ROOT, CHORD_CV_ROOT, CHORD_CV_REF};
    if(choice < 0 || choice >= CHORD_OUT_CHOICES)
        choice = CHORD_OUT_OFF;
    *midi_mode = kMidi[choice];
    *cv_mode   = kCv[choice];
}

float chord_cv_root_volts(const seq_prog_t *pr, int ci)
{
    if(ci < 0 || ci >= SEQ_MAX_CHORDS)
        ci = 0;
    int pc = (pr->root + seq_degree_semis(pr->scale, pr->chord[ci].degree)) % 12;
    if(pc < 0)
        pc += 12;
    return (float)pc / 12.0f;
}

void chord_cv_init(chord_cv_t *c, const seq_t *s)
{
    c->volts = chord_cv_root_volts(&s->prog, seq_chord_index(s));
}

float chord_cv_update(chord_cv_t *c, const seq_t *s, int cv_mode)
{
    if(cv_mode == CHORD_CV_REF)
        return 0.0f; /* the held root is kept for when REF is left */
    if(s->running)
        c->volts = chord_cv_root_volts(&s->prog, seq_chord_index(s));
    return c->volts;
}
