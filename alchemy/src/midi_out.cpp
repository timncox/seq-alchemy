/* midi_out -- see midi_out.h. */
#include "midi_out.h"

#include "daisy_seed.h"

namespace midiout
{

static daisy::UartHandler uart;
static bool               ready = false;
static bool               tried = false; /* a failed Init is not retried */

void Init()
{
    if(tried) return;
    tried = true;
    daisy::UartHandler::Config c;
    c.periph        = daisy::UartHandler::Config::Peripheral::USART_1;
    c.mode          = daisy::UartHandler::Config::Mode::TX;
    c.baudrate      = 31250;
    c.pin_config.tx = daisy::Pin(daisy::PORTB, 6); /* header pin 8 */
    c.pin_config.rx = daisy::Pin();                /* PB7 untouched */
    if(uart.Init(c) != daisy::UartHandler::Result::OK) return;

    /* libDaisy leaves the 16-byte TX FIFO off, which would hold the output
     * to two bytes per 1 ms poll (64 % of the line). FIFOEN can only change
     * while the USART is disabled. */
    USART1->CR1 &= ~USART_CR1_UE;
    USART1->CR1 |= USART_CR1_FIFOEN;
    USART1->CR1 |= USART_CR1_UE;
    ready = true;
}

void Drain(chord_midi_ring_t* ring)
{
    if(!ready) return;
    uint8_t b;
    while((USART1->ISR & USART_ISR_TXE_TXFNF) && chord_midi_ring_pop(ring, &b))
        USART1->TDR = b;
}

} // namespace midiout
