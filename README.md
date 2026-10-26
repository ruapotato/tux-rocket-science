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
| Flight | **Z/X** full/cut throttle · **Shift/Ctrl** (or drag the THR bar) throttle · **WASD**/arrows steer relative to the view (W tips the nose away from the camera, D swings it right on screen) · **QE** roll · **T** SAS · **Space** stage · **P** parachute · **G** legs · **R** RCS: **IJKL** slide on screen, **H/N** forward/back · **Y** target · **U** undock · **F** EVA · **O** science · **B** brakes · **,/.** or ◀◀ ▶▶ time warp · **V** surface/orbit speed · **M** map · **[ ]** switch vessel · **Esc** menu (revert) |
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

**Docking**: put a **Huddle-Lock Docking Port** on each craft. In flight, **Y** picks the other
craft as target. The map draws the target's orbit in magenta and marks the **closest approach** on both
paths (how close, when, and how fast you'd pass), counting any burns you've planned, so you can
drag a maneuver node until the pass gets close. The target panel shows range, closing speed and, near a free port pair, how
well the ports line up; the PORT line turns green when they would latch. SAS **DOCK** points your
port at theirs, **RV-** points against your relative speed (burn to stop), and RCS slides you
onto the line. Ports latch when they meet within 10° of face to face, closing at under 0.8 m/s.
The two craft become one: fuel flows across, crew stay where they are. **U** pushes them apart.
To try it, launch the stock **Igloo Station** to orbit, leave it (**Esc → Space Center**), then fly a
**Puffin Ferry** up to meet it.

**One world**: every launch joins the same saved world. **Esc → Space Center** leaves a craft
where it is (once it's landed or in space), and the main menu's **Craft in flight** list takes you
back aboard, recovers what's landed at home, or scraps debris (**Scrap all debris** clears every spent
stage at once; anything with a pod, probe core or reaction wheel is kept). Going back aboard, the
mission clock still counts from that craft's liftoff. Time only passes while you fly. A
craft still on the pad is recovered before the next launch, and **Revert** puts the world back as
it was before the launch. Penguins aboard a craft left flying stay aboard until it comes home.

**RCS**: Puffer RV-4 blocks are small monopropellant jets. Put them on the flanks in a symmetric
set (4x is usual) near the centre of mass, add a Spritz mono tank (pods and the probe core carry a
little), then press **R** in flight. They add to the reaction wheels' turning only when the wheels
run out, and **IJKL / H N** slide the craft for docking and fine trims. Every RCS block drinks from
every mono tank on the vessel. The hangar shows the blocks, the mono and how much Δv a pure slide
gets from it.

**Time warp**: **.** steps up through 2x, 3x, 4x (physics warp, allowed anywhere) and on into the
rails rates; under thrust or in the air 4x is the top. Above the air with the engines idle, warp runs on rails up to 100000x. Nothing
burns on rails, so throttling up drops straight back to normal time and the engine really fires.
Slowing down is always allowed. Diving toward the ground (or the top of the air), the warp eases
itself down a level at a time and always keeps at least 15 s of real time before you'd hit. A
flyby whose low point safely clears the surface isn't slowed. On the map, a path that dips into
the air carries on in dim red to where it would hit the ground without drag.

The staging list's **+ new stage** opens an empty stage card: drag part chips (or a whole stage)
onto it.

**Steering** comes from three places. Reaction wheels in pods and probe cores work anywhere.
**Flipper Fins** steer only in air: the white flap on each fin's trailing edge swings with the
stick, and the faster and lower you fly, the harder they bite. Fins further aft turn the rocket
more, and nothing moves them in vacuum. **RCS** jets work anywhere there's mono. Engine gimbal
helps whenever an engine burns. With SAS on, letting go of the keys stops the turn where it is and
holds that attitude (it doesn't swing back to where you started).

**Power**: pods and probe cores carry batteries, and reaction torque runs on them (about 0.1 kW
per kN m at full effort; probe cores also draw a little all the time). Out of power, the wheels
stop and an uncrewed probe can't be steered at all. **Sunfin Solar Panels** give 1.5 kW facing
the sun at Floe's distance (more nearer Austra, none in a planet's shadow; their face looks out
from the hull), **Krill-Cell Batteries** store more, and a **Squawk Antenna** sends experiment
results home from wherever you are for half their worth (O, then Send) at 20 kJ per research
point. Electricity flows in normal time; on-rails warp is frozen time for it as for fuel.

**Planes**: Gannet Wings lift (their flaps are ailerons), the Skua jet engine breathes air (lots of
push for little fuel, more in thick air, nothing in space or past about 520 m/s), and Waddle-Wheel
gear rolls along the craft's nose, grips sideways and brakes with **B**; the nose gear steers.
A craft with wheels launches from the **runway** (the hangar's From: button), lying on its wheels
nose east, brakes on. Try the stock **Gull**: SPACE starts the jet, Z, B, and pull up at about
100 m/s. The STAGES panel's Δv counts rockets only; a JET line says how long the fuel lasts.

**Kelp**: the second planet, sunward of Floe: green, a little bigger, with 1.2x Floe's surface
gravity and air three and a half times as thick reaching 110 km up. Parachutes bite hard there,
coming in is hot, and climbing back out takes a real rocket. Its research is worth 7x Floe's.
Planet looks live in `data/bodies/` ("look": colours, sky, haze, clouds), so new worlds need no code.

**Career**: the main menu's first button switches between the **Sandbox** (every part, free; your
original save) and a **Career** (its own world, crew and program). A career starts with ƒ25,000
and Basic Rocketry. Every part has a cost; launching pays for the craft and recovering it at
Floe refunds 90%. **Mission Control** lists PASA's missions (first hop, space, orbit, a spacewalk,
Krill, rendezvous, docking...): they pay funds and research when you do them. **R&D** spends
research to unlock the rest of the parts.

**Science**: pods carry crew reports, penguins on EVA make EVA reports and (standing on the
ground) surface samples, and the Science tab has a thermometer, a barometer and the Snowflake
Lab. Press **O** (or SCIENCE) in flight to see what each would bring home from where you are,
and run it. A result is worth base x body x situation (landed, flying low or high, in space
near or high over; Krill is worth 4x Floe) and counts once the craft is recovered at Floe; each
repeat of the same result is worth half the last. Everything is in `data/career/`.

**EVA**: **F** sends a penguin out of the pod you're flying. Out there, **WASD** walks (relative
to the view, up to 1.6 m/s, as far as friction lets the feet push), **Space** jumps (2.2 m/s off
the ground: knee height on Floe, a couple of metres on Krill), and **R** arms the jetpack: then
WASD and **Shift / Ctrl** push it about on a few kilograms of mono. Walk up to any part of a
craft with a free seat and **F** climbs back in (the penguin climbs the hull to the hatch).
A penguin on EVA is a vessel of its own: it can be left on Krill, switched to with **[ ]**,
recovered at home, or lost.

**Nose cones**: a flat tank top meets the air head-on and takes the full heating. A **Beak
Nosecone** (or the **Big Beak** for the wide tanks) meets it at a slant, takes less than half
of it, and keeps the tank behind it cool. It also cuts the drag.

The flight screen's **STAGES** panel lists every SPACE press still to come, next first: what it
lights, drops or opens, and the Δv, burn time and TWR of the burn it starts.

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
