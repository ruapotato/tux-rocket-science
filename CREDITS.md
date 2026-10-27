# Credits

## Tux
Tux, the Linux penguin, was created by **Larry Ewing** (lewing@isc.tamu.edu) using
The GIMP. Our penguins come from that lineage, but every penguin in this game is
our own original artwork.

The 3D Tux you walk around as on EVA (`scenes/tux_rig.tscn`) and its procedural
animator (`scripts/crew/tux_animator.gd`) come from the same author's own games,
SuperTux3D and The Legend of Tux (GPLv3), relicensed here under the AGPL.

## The ship's computer
The computer aboard every ship (`os/`: an RV64IM CPU, a kernel, a disk, a
userland with a shell and a small Python, `os/machine` and `os/guest`) and its
desktop with its apps and games (`scripts/os`) come from the same author's own
games NOMINAL and RUNBOOK (AGPL-3.0), brought over with their names and
behaviour kept; `os/core/computer.c`, `os/guest/ship.c` and
`scripts/os/flightapp.gd` connect it to this game's ships.

The desktop's font is DejaVu Sans Mono (`fonts/os/`, Bitstream Vera licence,
`fonts/os/LICENSE.DejaVu.txt`).

## Third-party code
- `trs_core/third_party/godot/gdextension_interface.h` is from the Godot Engine,
  © the Godot Engine contributors (Juan Linietsky, Ariel Manzur), under the MIT license.
- Godot Engine 4.x (MIT), the engine we run on: https://godotengine.org

## Disclaimer
PASA is a parody agency invented for this game. It has no affiliation with any
real space agency or game studio.
