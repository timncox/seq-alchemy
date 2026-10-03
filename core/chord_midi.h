/*
 * chord_midi -- the chord the sequencer is on, as MIDI bytes. No hardware.
 *
 * The back link to Belt on a second Lab (docs/alchemy-chord-vocoder-design.md,
 * "Back link"): the board calls chord_midi_update() after every seq_process()
 * and sends what lands in the byte ring out of USART1 at 31,250 baud.
 *
 * NOTES. The chord playing now (seq_chord_index) as 3 or 4 notes (prog.tones),
 * stacked in thirds from its root in close position. The root sits between
 * F#3 and F4 (MIDI 54..65), so every chord lies around middle C: the top
 * note is at most E5 in the seven-note scales, C6 for a pentatonic seventh. On a change the notes that leave are switched off
 * first, then the new ones on, so a receiver with four voices (Belt) never
 * runs out. Notes the two chords share are NOT retriggered: they stay held,
 * and Belt's harmony voice on that note does not jump. Edits to the key,
 * scale, tones or a chord's degree re-voice at once the same way.
 *
 * TRANSPORT. Notes sound only while seq_t.running. Stop (the player's, or
 * a lost J1 clock) switches every held note off. With the clock on, Start
 * (0xFA) goes out when the transport starts, when it rewinds to the top
 * while running (a J2 reset), and when the progression loops back to its
 * first chord (on that bar line, before its clock; the clock count carries
 * on); Stop (0xFC) when it stops; 0xF8 at 24 PPQN from the transport
 * position, so it follows J1 when seq does.
 *
 * NO STUCK NOTES. Every note-on is answered by its note-off: a change is
 * emitted whole or not at all (if the ring has no room it waits for the
 * next call, nothing is half-sent), and switching the output off, changing
 * the channel or stopping first releases what is held.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "seq.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CHORD_MIDI_MAX_NOTES 4
#define CHORD_MIDI_VELOCITY 100
#define CHORD_MIDI_RING 256 /* power of two */
#define CHORD_MIDI_ROOT_LO 54 /* F#3: the lowest a chord root goes */
#define CHORD_MIDI_MAX_CLOCKS 4 /* 0xF8 per call at most (a stalled poll) */

/* Bytes waiting for the UART. One producer, one consumer, same thread. */
typedef struct {
    uint8_t  buf[CHORD_MIDI_RING];
    uint16_t head, tail; /* free-running; head - tail = bytes waiting */
} chord_midi_ring_t;

typedef enum {
    CHORD_MIDI_OFF = 0,   /* nothing goes out (held notes are released) */
    CHORD_MIDI_NOTES,     /* note on/off for the chord */
    CHORD_MIDI_CLOCK,     /* notes + 0xF8 clock + 0xFA start / 0xFC stop */
    CHORD_MIDI_MODES
} chord_midi_mode_t;

typedef struct {
    uint8_t mode;     /* chord_midi_mode_t; set it, the next update follows */
    uint8_t channel;  /* 0..15 = MIDI channel 1..16 */

    /* runtime */
    uint8_t  held[CHORD_MIDI_MAX_NOTES]; /* notes on now, as sent */
    uint8_t  n_held;
    uint8_t  sent_channel; /* the channel the held notes went out on */
    bool     clock_started; /* a 0xFA went out with no 0xFC after it */
    int32_t  clock_tick;    /* 24-PPQN ticks sent since the start */
    double   last_pos;
    double   last_total;    /* the progression's length at last_pos */
    uint32_t dropped;       /* changes put off for want of ring room */
} chord_midi_t;

/* Off, channel 1. */
void chord_midi_init(chord_midi_t *m);

/* The notes of chord ci of a progression, lowest first; returns the count
 * (3 or 4). */
int chord_midi_voicing(const seq_prog_t *pr, int ci, uint8_t *notes);

/* Bring the output in line with the sequencer: emit into the ring whatever
 * note offs/ons, clock and start/stop it takes. Call after seq_process(). */
void chord_midi_update(chord_midi_t *m, const seq_t *s, chord_midi_ring_t *r);

/* The byte ring. */
void   chord_midi_ring_init(chord_midi_ring_t *r);
size_t chord_midi_ring_count(const chord_midi_ring_t *r);
size_t chord_midi_ring_free(const chord_midi_ring_t *r);
bool   chord_midi_ring_pop(chord_midi_ring_t *r, uint8_t *byte);

/* ------------------------------------------------------------------------
 * The front link: the chord as ONE CV (J10 on the Lab). The voltage is the
 * current chord's ROOT, 1 V/oct, 0 V = C, inside one octave: pitch class /
 * 12, so 0 .. 11/12 V. Belt snaps it to its own KEY / SCALE and builds the
 * diatonic chord there, so the two modules must share key and scale.
 *
 * It changes when the chord changes (on the bar, with the MIDI) and holds
 * otherwise. Stopped, or the J1 clock lost: it HOLDS the last root -- a CV
 * that jumped on stop would make Belt change chord. Key or scale edits made
 * while stopped show up when the transport next runs.
 *
 * REF holds exactly 0 V, for Belt's Learn (it measures this Lab's 0 V).
 * ------------------------------------------------------------------------ */
typedef enum {
    CHORD_CV_OFF = 0, /* the jack does its other job */
    CHORD_CV_ROOT,    /* the chord root, as above */
    CHORD_CV_REF      /* 0 V, for the receiver to calibrate against */
} chord_cv_mode_t;

typedef struct {
    float volts; /* the root held now */
} chord_cv_t;

/* The "Chord out" choices, in detent order. chord_out_modes() says what
 * each turns on: a chord_midi_mode_t and a chord_cv_mode_t. */
typedef enum {
    CHORD_OUT_OFF = 0,
    CHORD_OUT_MIDI,       /* MIDI notes */
    CHORD_OUT_MIDI_CLOCK, /* MIDI notes + clock / start / stop */
    CHORD_OUT_CV,         /* root CV */
    CHORD_OUT_CV_MIDI,    /* root CV + MIDI notes + clock */
    CHORD_OUT_CV_REF,     /* 0 V reference, no MIDI */
    CHORD_OUT_CHOICES
} chord_out_t;
void chord_out_modes(int choice, uint8_t *midi_mode, uint8_t *cv_mode);

/* The root of chord ci as volts, 0 .. 11/12. */
float chord_cv_root_volts(const seq_prog_t *pr, int ci);
/* Start holding the root of the chord at the transport's position. */
void  chord_cv_init(chord_cv_t *c, const seq_t *s);
/* Call after seq_process(); returns the volts for the jack. */
float chord_cv_update(chord_cv_t *c, const seq_t *s, int cv_mode);

#ifdef __cplusplus
}
#endif
