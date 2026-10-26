# Contributing to Tux Rocket Science

Thanks for wanting to help. This page is how the project is put together and
the few rules that keep it honest.

## The rules

1. **Every number shown is true and tested.** If the HUD, the hangar or the
   map shows a number, trs_core computed it and a gate checks it against an
   independent formula or a measured result. If you add a readout, add the check.
2. **The C core owns the simulation.** Physics, orbits, staging, fuel,
   heating, contacts, electricity: all in `trs_core/` (C11, no dependencies).
   GDScript draws, takes input, and does bookkeeping (crew, career). Godot's
   float<->text conversion isn't correctly rounded, so authoritative doubles
   cross between C and GDScript only as `PackedFloat64Array`, and trs_core
   formats every number it reports.
3. **Gates run headless.** Every feature has a gate that runs without a
   screen, in CI on Linux and Windows.
4. **Parts and worlds are data.** Parts, planets and the career live in
   JSON under `data/`. See [docs/MODDING.md](docs/MODDING.md).
5. **No outside IP.** Don't copy art, names or code from other space games
   or agencies. Our agency is PASA; our penguins are our own (the Tux lineage
   is credited in CREDITS.md).
6. **License header in every source file** (AGPL-3.0-or-later). CI checks it:
   `python3 tools/license_headers.py`.

## Building

You need CMake, Ninja and a C compiler (gcc or clang; MinGW on Windows), and
Godot 4.7.2 (put the binary in the repo root; it's ignored by git).

    cmake -S . -B build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build/linux
    ./Godot_v4.7.2-stable_linux.x86_64 --headless --import
    ./Godot_v4.7.2-stable_linux.x86_64          # play

The build puts the GDExtension library in `bin/`. If you only want to work
on GDScript, data or art, download `trs_core-bin` from the latest green CI
run (Actions tab) into `bin/` and skip the C toolchain.

## Running the gates

    ./build/linux/trs_tests --data data --all                      # native gates
    ./Godot_v4.7.2-stable_linux.x86_64 --headless -- --all-gates   # everything
    ./Godot_v4.7.2-stable_linux.x86_64 --headless -- --test-eva    # one gate
    ./Godot_v4.7.2-stable_linux.x86_64 -- --dev-gull-flight        # a dev script (with a screen: screenshots)

Dev scripts (`scripts/gates/dev/*.gd`, run as `--dev-<name>`) are for looking
at things: screenshots land in `~/.local/share/godot/app_userdata/Tux Rocket Science/dev_shots/`.
The monkey testers (`--dev-flight-monkey`, `--dev-hangar-monkey`) mash keys
and check the game stays consistent. Gates and dev runs use a scratch save
(`Sim.use_sandbox()`), never yours. Don't run two Godot gate processes at
once: they share that scratch save.

## Adding a gate

- **Native** (physics, in C): write `trs_gate_<name>(trs_log *, const char *data_dir)`
  in a `trs_core/src/gates_*.c` file and list it in `GATES[]` in
  `trs_core/src/gate.c`. Print one `ok  ` / `FAIL` line per check with the
  numbers it compared.
- **Scripted** (through Godot scenes): list it in `GATES[]` with a `NULL`
  function, and write `scripts/gates/test_<name>.gd` with
  `func run(runner) -> Dictionary` returning `{status, log}`.

## Where things are

| Path | What |
|---|---|
| `trs_core/src/` | the simulation: `vessel_physics.c` (forces, contacts, heating, power), `kepler.c`, `trajectory.c`, `world.c`, `docking.c`, `eva.c`, `world_save.c` |
| `trs_core/gdext/` | the GDExtension binding (`TrsSim` class): every method GDScript can call |
| `scripts/flight/` | flight scene, HUD, map, vessel visuals |
| `scripts/hangar/` | the hangar and `CraftModel` (craft editing) |
| `scripts/career/` | science, missions, R&D bookkeeping |
| `scripts/visual/part_visual.gd` | procedural part models (no imported art) |
| `data/` | parts, the star system, stock craft, career data |

## Pull requests

Run both gate suites before pushing. Describe what you checked, and include a
screenshot for anything visual.
