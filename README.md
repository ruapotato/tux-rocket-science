# Tux Rocket Science

An open-source rocket-building and spaceflight sandbox made in Godot. Penguins run
the space program for **PASA**, the in-game agency. You build a rocket, get it to
orbit, plan a burn to the moon, land, and come home.

> Status: the vertical slice is playable end to end. Build a rocket, reach orbit, plan a burn to Krill, land,
> and come home. All verification gates are green (see below). Scope lives in `TUX_ROCKET_SCIENCE_HANDOFF.md`.

## Playing

`godot` (or open the project in Godot 4.7), then **First flight** for a guided hop in the Hopper, or
**Assembly Hangar** → build or load a craft → **LAUNCH**.

| Where | Keys |
|---|---|
| Hangar | click a part, then click a node or surface · **X** symmetry · **Q/E W/S A/D** rotate · arrows / PgUp / PgDn offset · **[ ]** thrust limit of the engine under the cursor (Shift: 20% steps) · **Del** · **Ctrl+Z/Y** · right-drag orbit · wheel zoom |
| Flight | **Z/X** full/cut throttle · **Shift/Ctrl** (or drag the THR bar) throttle · **WASD**/arrows steer relative to the view (W tips the nose away from the camera, D swings it right on screen) · **QE** roll · **T** SAS · **Space** stage · **P** parachute · **G** legs · **R** RCS: **IJKL** slide on screen, **H/N** forward/back · **,/.** or ◀◀ ▶▶ time warp · **V** surface/orbit speed · **M** map · **[ ]** switch vessel · **Esc** menu (revert) |
| Map | centred on your vessel · click the trajectory to add a maneuver node, drag its handles (Shift = fine) · **Tab** vessel / planet focus · **Skip to burn** fast-forwards to just before the node · **MNV** SAS points at the burn |
| Hangar | the craft hangs at a working height; while holding a part, free attach nodes show orange and the one it will snap to turns green · **F** frame craft · **PgUp/PgDn** move the view |

Fuel flows between parts that touch, but never through a decoupler, so an engine on a radial
decoupler needs its own tank. A **Siphon Feed Line** on a side tank, facing the core, pipes the
side tank's fuel into the core: the side tanks run dry first and drop away (see the stock
**Sidecar**). The hangar lists anything that won't work (no fuel, TWR below 1, a feed line that
reaches nothing, an engine firing into part of its own rocket) and likely surprises (no fins, no
parachute) under the readouts. Exhaust is hot: anything left in a lit engine's jet burns up, so
drop a spent stage before the engine above it lights. Engines tilted away from the rocket's axis
lose the sideways part of their thrust, and the hangar's thrust and Δv count only what is left.

**One world**: every launch joins the same saved world. **Esc → Space Center** leaves a craft
where it is (once it's landed or in space), and the main menu's **Craft in flight** list takes you
back aboard, recovers what's landed at home, or scraps debris. Time only passes while you fly. A
craft still on the pad is recovered before the next launch, and **Revert** puts the world back as
it was before the launch. Penguins aboard a craft left flying stay aboard until it comes home.

**RCS**: Puffer RV-4 blocks are small monopropellant jets. Put them on the flanks in a symmetric
set (4x is usual) near the centre of mass, add a Spritz mono tank (pods and the probe core carry a
little), then press **R** in flight. They add to the reaction wheels' turning only when the wheels
run out, and **IJKL / H N** slide the craft for docking and fine trims. Every RCS block drinks from
every mono tank on the vessel. The hangar shows the blocks, the mono and how much Δv a pure slide
gets from it.

Boosters go on a **Calving Radial Decoupler**: put the decoupler on the core's flank, then click
its orange node with a tank, engine or solid booster and it hangs upright beside the core
(**Q/E** turns it about its own axis). Parts come in two widths: the 1.25 m line and the wide
2.5 m line (Icebox-XM/XL tanks, Elephant Seal engine, Crack-2 decoupler); an **Iceberg
Adapter** joins the two, and a **Rookery Tri-Mount** hangs three 1.25 m engines under a wide
tank (see the stock **Rookery Heavy**). A **Snowball Probe Core** steers a craft with nobody
aboard.

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
