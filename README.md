# Tux Rocket Science

An open-source rocket-building and spaceflight sandbox made in Godot. Penguins run
the space program for **PASA**, the in-game agency. You build a rocket, get it to
orbit, plan a burn to the moon, land, and come home.

> Status: the vertical slice is playable end to end. Build a rocket, reach orbit, plan a burn to Krill, land,
> and come home. All verification gates are green (see below). Scope lives in `TUX_ROCKET_SCIENCE_HANDOFF.md`.

## Playing

`godot` (or open the project in Godot 4.7), then **Assembly Hangar** → build or load a craft → **LAUNCH**.

| Where | Keys |
|---|---|
| Hangar | click a part, then click a node or surface · **X** symmetry · **Q/E W/S A/D** rotate · arrows / PgUp / PgDn offset · **Del** · **Ctrl+Z/Y** · right-drag orbit · wheel zoom |
| Flight | **Z/X** full/cut throttle · **Shift/Ctrl** throttle · **WASDQE** attitude · **T** SAS · **Space** stage · **,/.** warp · **G** legs · **V** surface/orbit speed · **M** map · **[ ]** switch vessel · **Esc** menu (revert) |
| Map | click the trajectory to add a maneuver node, drag its handles (Shift = fine) · **Tab** focus body · **Warp to node** · **MNV** SAS points at the burn |

Press **F1** in flight for the controls (shown automatically the first time). All sound is
synthesized live (engine rumble that thins out in vacuum, wind, pyros, parachutes); the volume
is in the pause menu.

Penguins are persistent (`user://crew.json`): they react live to G-force, spin, freefall and danger,
remember their records, and spend a flight in the infirmary if a landing goes badly. Nobody dies at PASA.

## Layout

| Path | What lives there |
|---|---|
| `trs_core/` | The simulation, written in C. It owns every authoritative number, in `double`. |
| `trs_core/src/` | Core sim library. Pure C with no Godot dependency, so it runs headless. |
| `trs_core/gdext/` | Pure-C GDExtension binding (no godot-cpp) that exposes the sim to Godot as `TrsSim`. |
| `trs_core/tests/` | `trs_tests`, a standalone gate runner that needs no Godot. |
| `data/` | Data-driven content: celestial bodies, parts, and stock craft (JSON). |
| `scripts/`, `scenes/` | GDScript glue and Godot scenes. These are views only. |
| `scripts/gates/` | Scripted gates that drive the game blind. |
| `bin/` | Built extension libraries and `trs_core.gdextension`. |

## Build

Requirements: CMake ≥ 3.16, Ninja (optional), a C11 compiler, and Godot 4.7 (stock, single precision).

```sh
# Linux
cmake -S . -B build/linux -G Ninja && cmake --build build/linux
# Windows (cross-compiled from Linux with mingw-w64)
cmake -S . -B build/windows -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake && cmake --build build/windows
```

Both builds write the extension into `bin/`.

## Verification gates

```sh
tools/run_gates.sh                     # build, then run every gate
./build/linux/trs_tests --all          # native gates only, no Godot
godot --headless -- --test-kepler      # a single gate through the engine
godot --headless -- --all-gates --strict   # also treat PENDING gates as failures
godot -- --screens                         # renders hangar/launchpad/orbit/map/moon_surface to screens/
godot --headless -- --control-port=7373 --blind   # JSON-lines control socket (see scripts/control/control_api.gd)
WINE_GODOT=…/Godot_v4.7.2-stable_win64_console.exe tools/run_gates.sh   # + Windows build under wine64
```

The first time you run a fresh checkout, run `godot --headless --import` once so
Godot registers the extension.

## License

AGPL-3.0-or-later. See `LICENSE` and `CREDITS.md`.
