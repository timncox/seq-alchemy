---
status: exploratory
last_touched: 2026-09-26
---

# seq-alchemy

Generative + step sequencer firmware for the Hermetic Alchemy Lab V2, played
from a Launchpad Mini MK3, a Launch Control XL and a Haute42 gamepad (XInput)
over USB host, through a powered hub. Design:
`~/tim-os/docs/alchemy-sequencer-design.md`. The name is a placeholder
(`/rename-project`).

## Build

    make -C test        native tests, ASan + UBSan; run after touching core/

    make -C alchemy libdaisy   once after cloning (git submodule update --init --recursive)
    make -C alchemy            firmware -> alchemy/build/seq_alchemy.bin
    make -C alchemy program-dfu   flash (module in DFU: hold B3 through boot)

`core/` is HAL-free C99 (same shape as `~/tim-os/relay`). `alchemy/` is the
Lab firmware; its header comment in `src/seq_alchemy.cpp` is the manual:
jack map, pages, and the Launchpad / XL / Haute42 layouts. Home preset slot 5
(6-15 belong to the other firmwares; 11 is Plaits). `picker`, `launchpad`, `usbh_hub_midi`, `usb_shared` and the `.lds` are
copies from mark-alchemy (branch track-page) -- fix in all the copies.

Gotchas:
- **Autosave only while stopped.** A preset save pauses the 1 ms poll for a
  few hundred ms; saving mid-play would drop beats. Stop to keep your work.
- **DTCM is tight** (.bss + stack live there on a bootloader build): big
  buffers go in AXI SRAM (`.axi_bss`) or SDRAM (`DSY_SDRAM_BSS`).
- **Launchpad mode has no HostLink**, so no `program-live`; hold B1 at
  power-up for Mac mode once, or use the picker.
- Drums: programmed steps + a generator per drum pattern (GEN 1-4
  thresholds, EUCLID 1-4) on the DRUMS pot page; `/seq/drums.txt` is
  imported at power-up only when its CRC changed (`drums_file_crc`), and
  the file stays on the card. The manual (`docs/`, `python3 docs/build.py`)
  ports the generators to JS: keep them in step with seq.c.
- Chord out (Settings pot 2, default Off; channel on pot 5): the current
  chord as MIDI on the rear header, USART1 TX = header pin 8, for Belt on
  a second Lab. `core/chord_midi.[ch]` (tested) makes the bytes,
  `alchemy/src/midi_out.cpp` is the only file that touches the UART (the
  alchemy-lab emulator swaps in its own). Cable: crossed TX/RX + grounds,
  never a straight ribbon -- the header carries +-12 V. CV choices put the
  chord ROOT on J10 (pitch class / 12 V; perc silent; CV ref = 0 V).
- **J9/J10 go through `codec_volts()`**: the codec's DC output inverts
  (-8.667 V per +1 sample, alchemy-sdk 4eb0e4c / v0.12); the pinned v0.11
  SDK doesn't know, so seq pre-scales. It passes through once the SDK has
  `v2_codec_cv.h` -- don't add a second correction when bumping the SDK.
- Persistence is `seq_state_write/read` (field by field, clamped on read);
  bump `SEQ_STATE_VERSION` when the layout changes.

**Never run make with a module attached in DFU** (memory
`feedback_no_builds_with_board_in_dfu.md`).

## Hear it without hardware

    cc -std=c99 -O2 -Icore -o build/render tools/render.c core/seq.c -lm
    ./build/render [seed] [bpm] renders/tour.wav

Runs the core at 1 ms like the Lab and plays the CV through stand-in synths
(saw -> resonant LP -> VCA bass, pulse lead, four drums). A scripted 32-bar
tour moves the knobs; the timeline is written to `<wav>.txt`. `renders/` is
gitignored.
