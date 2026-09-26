---
status: exploratory
last_touched: 2026-09-26
---

# seq-alchemy

Generative + step sequencer firmware for the Hermetic Alchemy Lab V2, played
from a Launchpad Mini MK3, a Launch Control XL and a Haute42 gamepad (XInput)
over USB host, through a powered hub. Replaces Tim's Oxi One. Design:
`~/tim-os/docs/alchemy-sequencer-design.md`. The name is a placeholder
(`/rename-project`).

## Build

    make -C test        native tests, ASan + UBSan; run after touching core/

    make -C alchemy libdaisy   once after cloning (git submodule update --init --recursive)
    make -C alchemy            firmware -> alchemy/build/seq_alchemy.bin
    make -C alchemy program-dfu   flash (module in DFU: hold B3 through boot)

`core/` is HAL-free C99 (same shape as `~/tim-os/relay`). `alchemy/` is the
Lab firmware; its header comment in `src/seq_alchemy.cpp` is the manual:
jack map, pages, and the Launchpad / XL / Haute42 layouts. Home preset slot
11. `picker`, `launchpad`, `usbh_hub_midi`, `usb_shared` and the `.lds` are
copies from mark-alchemy (branch track-page) -- fix in all the copies.

Gotchas:
- **Autosave only while stopped.** A preset save pauses the 1 ms poll for a
  few hundred ms; saving mid-play would drop beats. Stop to keep your work.
- **DTCM is tight** (.bss + stack live there on a bootloader build): big
  buffers go in AXI SRAM (`.axi_bss`) or SDRAM (`DSY_SDRAM_BSS`).
- **Launchpad mode has no HostLink**, so no `program-live`; hold B1 at
  power-up for Mac mode once, or use the picker.
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
