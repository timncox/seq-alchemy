/*
 * midi_out -- the rear-header MIDI out: USART1 TX on PB6 (header pin 8) at
 * 31,250 baud, fed from chord_midi's byte ring. The back link to Belt on a
 * second Lab (docs/alchemy-chord-vocoder-design.md, "Back link").
 *
 * Cable: this Lab's pin 8 (TX) to the other Lab's pin 7 (RX), its pin 8 to
 * this pin 7, and several grounds (2, 5, 12-14, 16-19). Nothing else. NEVER
 * a straight 1:1 ribbon: the header carries -12 V / +12 V (pins 1, 11) and
 * 3V3A (15), and a ribbon joins TX to TX.
 *
 * Main thread only (OnPoll): Drain() writes what the UART's FIFO takes and
 * returns, it never waits. The emulator has its own copy of this file that
 * records the bytes instead.
 */
#pragma once
#include "chord_midi.h"

namespace midiout
{

/* Claims USART1 and PB6. Not called until the output is first switched on,
 * so with Chord out = Off the pin is never driven. PB7 (RX) is left alone. */
void Init();

/* Move bytes from the ring into the UART until either runs out. */
void Drain(chord_midi_ring_t* ring);

} // namespace midiout
