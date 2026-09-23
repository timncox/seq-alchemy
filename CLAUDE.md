---
status: exploratory
last_touched: 2026-09-23
---

# seq-alchemy

Generative + step sequencer firmware for the Hermetic Alchemy Lab V2, played
from a Launchpad Mini MK2 over USB host. Replaces Tim's Oxi One. Design:
`~/tim-os/docs/alchemy-sequencer-design.md`. The name is a placeholder
(`/rename-project`).

## Build

    make -C test        native tests, ASan + UBSan; run after touching core/

`core/` is HAL-free C99 (same shape as `~/tim-os/relay`). The Alchemy shim
(`alchemy/`) and the Launchpad host driver come later.

**Never run make with a module attached in DFU** (memory
`feedback_no_builds_with_board_in_dfu.md`).

## Hear it without hardware

    cc -std=c99 -O2 -Icore -o build/render tools/render.c core/seq.c -lm
    ./build/render [seed] [bpm] renders/tour.wav

Runs the core at 1 ms like the Lab and plays the CV through stand-in synths
(saw -> resonant LP -> VCA bass, pulse lead, four drums). A scripted 32-bar
tour moves the knobs; the timeline is written to `<wav>.txt`. `renders/` is
gitignored.
