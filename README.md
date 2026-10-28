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
| Flight | **Z/X** full/cut throttle · **Shift/Ctrl** (or drag the THR bar) throttle · **WASD**/arrows steer relative to the view (W tips the nose away from the camera, D swings it right on screen) · **QE** roll · **T** SAS · **Space** stage · **P** parachute · **G** legs · **R** RCS: **IJKL** slide on screen, **H/N** forward/back · **Y** target · **U** undock · **F** EVA · **O** science · **B** brakes · **F5 / F9** quicksave / load · **,/.** or ◀◀ ▶▶ time warp · **V** surface/orbit speed · **M** map · **[ ]** switch vessel · **Esc** menu (revert) |
| Map | centred on your vessel · click the trajectory to add a maneuver node, drag its handles (Shift = fine) · **Tab** vessel / planet focus · **◀ orbit earlier / orbit later ▶** moves a burn by whole orbits · **Skip to burn** fast-forwards to just before the node · **MNV** SAS points at the burn |
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

**Action groups**: in the hangar, hover a part and press **1-9** to put it (and its symmetric
copies) into that group, or take it out; the readouts list what each group holds. In flight the
same key fires the group: engines toggle on and off (a solid booster only lights), parachutes
arm, legs go down or up, wheels toggle the brakes, and a docked port lets go.

The staging list's **+ new stage** opens an empty stage card: drag part chips (or a whole stage)
onto it.

**Steering** comes from three places. Reaction wheels in pods and probe cores work anywhere.
**Flipper Fins** steer only in air: the white flap on each fin's trailing edge swings with the
stick, and the faster and lower you fly, the harder they bite. Fins further aft turn the rocket
more, and nothing moves them in vacuum. **RCS** jets work anywhere there's mono. Engine gimbal
helps whenever an engine burns. With SAS on, letting go of the keys stops the turn where it is and
holds that attitude (it doesn't swing back to where you started).

**Power**: pods and probe cores carry batteries, and reaction torque runs on them (about 0.1 kW
per kN m at full effort; probe cores also draw a little all the time, and a crew switches its
pod off when it isn't steering). Out of power, the wheels
stop and an uncrewed probe can't be steered at all. **Sunfin Solar Panels** give 1.5 kW facing
the sun at Floe's distance (more nearer Austra, none in a planet's shadow; their face looks out
from the hull), **Krill-Cell Batteries** store more, and a **Squawk Antenna** sends experiment
results home from wherever you are for half their worth (O, then Send) at 20 kJ per research
point. Electricity keeps flowing through time warp too: panels charge and probe cores draw on
rails and while landed (no wheels turn there), and eclipses count.

**Planes**: Gannet Wings lift (their flaps are ailerons), the Skua jet engine breathes air (lots of
push for little fuel, more in thick air, nothing in space or past about 520 m/s), and Waddle-Wheel
gear rolls along the craft's nose, grips sideways and brakes with **B**; the nose gear steers.
A craft with wheels launches from the **runway** (the hangar's From: button), lying on its wheels
nose east, brakes on. Try the stock **Gull**: SPACE starts the jet, Z, B, and pull up at about
100 m/s. The STAGES panel's Δv counts rockets only; a JET line says how long the fuel lasts.

**Transfer windows**: on the map, focus another planet (Tab) or Austra to see when the next
Hohmann window opens (how far the target should lead your planet, and how far it does now), the
burn to leave from your orbit, and the trip time. **Plan transfer to ...** puts a maneuver node there for you: a burn near
the window whose path enters the planet's sphere of influence. Once you're on the way round
Austra, **Plan correction to ...** finds a small burn that brings your pass down to just above
its air (or, airless, its surface).

**Kelp**: the second planet, sunward of Floe: green, a little bigger, with 1.2x Floe's surface
gravity and air three and a half times as thick reaching 110 km up. Parachutes bite hard there,
coming in is hot, and climbing back out takes a real rocket. Its research is worth 7x Floe's. Kelp has a little moon, **Pebble** (60 km across, a twentieth of Floe's gravity).
Planet looks live in `data/bodies/` ("look": colours, sky, haze, clouds), so new worlds need no code.

**Narwhal**: the cold blue world out past Floe (twice as far from Austra, about three months'
coast on a Hohmann transfer), 0.6x Floe's gravity under thin air that tops out at 45 km: a
parachute slows you but won't land you, so bring an engine for the last of it. Its moon **Tusk**
is small and bright. Research there is worth 10-12x Floe's.

**The signals**: once you reach orbit, PASA's dish hears a hum from Krill that nothing natural
makes. Research the **Listening Post** and fly an **Earhole Signal Scanner** (3,000 km range, a
trickle of power) anywhere Krill's surface is in sight: when the source rises over the horizon the
scanner picks it up, gives its distance and heading, and charts it for good (it shows on the map and
on the HUD whenever you're at that world). Land beside it and walk a penguin up to it. What the
penguins find on Krill answers from Kelp and Narwhal, and what they find there is no research of
PASA's: the **Hearth Core** and the **Stillwater Drive**, the alien parts that can take penguins to
another star. The last tone comes from a world around Lantern. Mission Control tells the story as
you go; the relics' places are data (`"relics"` in `data/bodies/`), and the geometry (distance,
heading, horizon) is checked by `test-relics`, the chain itself by `test-signals`.

**Between the stars**: Austra has neighbours. **Lantern**, 4.2 light-years away (`data/bodies/lantern_system.json`),
has three worlds: hot **Cinder**, green **Haven** with air like Floe's, and Haven's moon **Wisp**.
Past a star's heliopause (Austra's is 40 Gm out, beyond Narwhal) a ship flies by special
relativity: its proper velocity, its own clock and the home calendar are all tracked exactly
(`test-relativity`, `test-interstellar`). The alien **Stillwater Drive** pulls every atom of the ship
alike, so nobody aboard feels it (the G meter reads 0): 100 g out between the stars, at most 1 g
and never past 1% of light speed inside a star's heliopause. It needs a **Hearth Core** (45 MW).
Out there the throttle runs it, warp runs on the ship's clock (and eases off as you brake), and
the **COURSE** panel flies you to a star: flat out, turning round exactly halfway, stopping just
short of its heliopause. Austra to Lantern at 100 g takes 43 days of ship time while Floe's
calendar runs on 4.22 years; at 100000x warp that is under a minute of real time. The HUD shows
the speed as a fraction of light, gamma, the ship and home clocks, each star's distance and
closing speed, and the distance and ship time it would take to stop. The sky shows what a ship
moving that fast would really see: stars crowd toward the way ahead (aberration), turn blue and
brighten there, and redden and fade behind (Doppler shift of each star's light). Things aboard
look normal: they move with you.

**Biomes**: every world with ground is split into named regions (Floe's Glacier Shelf, the Puffin
Spires, Frozen Seas, Highlands, Tundra and Polar Caps; Krill's Crater Floors and Rims; Kelp's
Basins and Meadows...). The status line names the one below you when you're low. Landed and
low-flying results count separately in each biome, so a crew report from Krill's Crater Floors
is worth as much as the first one from its Highlands. Biomes are data (`"biomes"` in
`data/bodies/`), checked by `test-biomes`.

**Mining**: a **Pecker Ore Drill** fitted low on a lander digs ore into a **Gizzard Ore Bin** while
the craft rests on the ground (its part within 2 m of it), at a rate set by the world's ore
(Floe is poor, Krill and Tusk rich). A **Sifter Refinery** turns ore into liquid propellant
(0.8 kg for every kg) and fills every propellant tank aboard, landed or in orbit. Press **C** to
switch them on; they draw real power (0.75 kW a drill, 2 kW a refinery), stop when the bin or
tanks are full or the batteries are flat, and keep going through time warp on a landed craft.
The PROPELLANT panel says what they are doing, or why not.

**Career**: the main menu's first button switches between the **Sandbox** (every part, free; your
original save) and a **Career** (its own world, crew and program). A career starts with ƒ25,000
and Basic Rocketry. Every part has a cost; launching pays for the craft and recovering it at
Floe refunds 90%. **Mission Control** lists PASA's missions (first hop, space, orbit, a spacewalk,
Krill, rendezvous, docking...): they pay funds and research when you do them. **R&D** spends
research to unlock the rest of the parts. Mission Control also keeps three **surveys** standing: a
particular result not yet brought home (say, a temperature reading flying high over Floe), paid in
funds and extra research when it comes home; each done is replaced, and new worlds join as you
reach them.

**Science**: pods carry crew reports, penguins on EVA make EVA reports and (standing on the
ground) surface samples, and the Science tab has a thermometer, a barometer and the Snowflake
Lab. Press **O** (or SCIENCE) in flight to see what each would bring home from where you are,
and run it. A result is worth base x body x situation (landed, flying low or high, in space
near or high over; Krill is worth 4x Floe) and counts once the craft is recovered at Floe; each
repeat of the same result is worth half the last. Everything is in `data/career/`.

**EVA**: **F** sends a penguin out of the pod you're flying. Out there, **WASD** walks (relative
to the view, up to 1.6 m/s, as far as friction lets the feet push), **Space** jumps (2.2 m/s off
the ground: knee height on Floe, a couple of metres on Krill), and **R** arms the jetpack: then
WASD and **Shift / Ctrl** push it about on a few kilograms of mono. Walk up to a pod with a free
seat (or a **Waddle Ladder** on its craft: the penguin climbs it to the hatch) and **F** climbs
back in. The Hopper and the Krill Lander carry ladders; fit one to any rocket whose pod stands
out of reach of the ground.
**G** plants a PASA flag where the penguin stands (it stays there for good). A penguin on EVA is a vessel of its own: it can be left on Krill, switched to with **[ ]**,
recovered at home, or lost.

**Living aboard**: habs (the **Burrow Hab**, the five-metre **Rookery Wide Hab**), the **Iceframe
Crew Spine**'s crawl tube, the **Six-Way Huddle Junction** (build sideways off any of its faces)
and the **Carousel Spin Ring** have rooms inside, and rooms that meet are one space. Press
**Enter** in flight and one of the crew gets up and walks about in them. Inside, a penguin feels
exactly what the ship does to it: floating in orbit, weight toward the stern while the engines
push (thrust / mass of it), weight out toward the rim of a spinning ring (w^2 r, with Coriolis
on the way down a spoke), the ground's pull on the pad, and nothing at all from the Stillwater
Drive, which pulls every atom alike. **Home** is the spin hold: SAS keeps the nose where it is
and the wheels turn the ship about its long axis at its ring's design rate (7.5 rpm: about 1 g on
a deck 16 m out). Ships that big are built in orbit: the hangar's **From: Orbital Yard** starts a
craft in a 250 km circular orbit of Floe (a career opens it with Orbital Shipyard in R&D). The
stock **Waddle Ark** has all of it.

**Building past tubes**: the hangar resizes parts. Hover one (or hold it) and **[ ]** change its
length, **; '** its width, **, .** its depth (Shift for fine steps); what hangs on it moves out
with its faces, and its propellant, mass, batteries, crew and rooms go with its volume. The
**Icebox Stretch Tank** runs from a stub to 8 m by 5 m; the **Floe-Block Hull**, **Square Icebox
Tank**, **Prow Wedge** and **Igloo Box Hab** are flat-sided boxes with a node on every face, and
anything sticks to their sides square to the face it lands on; wings resize too. **X** cycles
radial symmetry and now **mirror**: a wing on one side gets its mirror image on the other. And
the alien tech: the **Hearth Torch** (800 kN at an exhaust speed fifteen times a rocket's, on the
Hearth Core's 40 MW: it gives what the power pays for) and **Glasswing** hulls and prows, a third
the weight and good to 4000 K. The stock **Skua** is a single-stage spaceship on them: from the
pad to orbit, to Krill and on, its thrust weight in the cabin.

**The ship's computer**: every ship carries a whole little machine: an RV64 CPU (now with
floating point), a kernel, a disk, a shell, and NOMINAL's MATE-style desktop with its games,
by way of RUNBOOK. **~** goes round the views: the rocket, the penguins inside (where the
computer's screen is lit on the capsule's console, or on a hab's wall: click it), the computer
full screen, and back to the rocket. **F2** goes straight to the computer.

Its **Flight** app shows what the panels say and has a button for each thing the ship can be
told. Every button is the shell line it runs, so you can copy it into a script. At the terminal,
`ship status` prints the readings and `ship help` lists the commands. Every reading is also a
file (`cat /dev/ship/apoapsis`), and so is every command (`echo 0 > /dev/ship/throttle`).

**Scripting the ship.** Its Python works on the ship's own parts, by name, with your logic:

```python
lifter = ship.part("Walrus")                  # a part by its name (the first words do)
drop = ship.part("Crack-1 Stack Decoupler 1")
lifter.throttle = 100                         # its own throttle, in percent: lights it
while ship.orbit.top < 100 * km:
    aim_at(lean_for(ship.altitude), 90)       # pitch, heading: held against the horizon
    if not staged and lifter.fuel < 0.5:      # % left in the tanks that feed it
        drop.decouple()
        ...
top = ship.orbit.top
extra = speed_needed(top) - speed_at(top)     # the maths: nothing here moves the ship
sleep(ship.orbit.time_to_top - burn_time(extra) / 2)
aim_at(0, 90)
burn(extra)
print(f"In orbit: {ship.orbit.bottom / km:.1f} x {ship.orbit.top / km:.1f} km")
```

- **The ship and its parts:** `ship.altitude`, `.height`, `.speed`, `.falling_speed`, `.delta_v`,
  `.orbit.top`/`.bottom`/`.time_to_top`/`.period`, `.body.gravity`/`.radius`, and lists of parts:
  `ship.engines`, `.boosters`, `.tanks`, `.decouplers`, `.parachutes`, `.legs`. A part has its
  readings (`fuel`, `thrust`, `lit`, `heat`) and the things it does (`ignite()`, `shutdown()`,
  `decouple()`, `deploy()`). An engine's own `throttle` can be set, and setting it lights it.
- **Aiming and burning:** `aim("prograde")`, `aim_at(pitch, heading)`, `aim_along(forward,
  sideways, outward)`, `throttle(100)`, `burn(dv)`, `stage()`, `sleep(s)` (time warp eases off
  before the script wakes), `wait_user_trigger()` (waits for your space, and the press is the
  script's, not a stage), `chart("height", ship.height)` (a graph in the Flight app).
- **The maths:** `speed_needed(height)`, `speed_at(height)`, `burn_time(dv)`, `time_to("top")`,
  `stopping_distance()`; and from the **Flight Planner**: `when_to_leave("krill")` (wait, Δv,
  travel time, pass height) and `what_if(forward, sideways, outward, when)`, which predicts the
  path as if that burn were made without making it, so a script can try course corrections for
  free.
- **The language:** real numbers, f-strings, `def`, lists and dicts, `"a,b".split(",")`,
  `sqrt`/`sin`/`atan2`/`pi`, and `km = 1000`.

The **Script Editor** on the computer's desktop colours Python and suggests what fits as you
type: every call with what it takes, and after a dot what that thing has, read from the ship as
it is. After an engine's name it offers `throttle`, `fuel` and `ignite()`, and inside
`ship.part("` it offers the names of the parts aboard. **Run** starts the script. The **watch**
shows every variable as it flies, the line it's on is marked, a click in the gutter sets a
breakpoint, and **Step** runs one line. Its **console** runs Python on the ship there and then
and keeps its variables (`m = ship.mass`, then `m / 1000`). In the hangar, **Make script from
these stages** writes your rocket as a script: each part by its stage, and each stage waiting for
your space. Swap a wait for your own test and that stage goes by itself.

The **Guidance Computer** (R&D in a career, always in the sandbox) also flies whole
**programs**, a frame at a time, through the same controls the keys use: `launch(80000)`,
`circularize()`, `transfer("krill")`, `warp_to("krill")`, `capture(30000)`, `land()`, `hover(30,
10)` and `node()`. They're `ship` commands too (`ship land`); any flying key takes the controls
back; and one that fails stops the script and says why.

`/root/examples` has scripts that work: `to_orbit.py` (the pad to orbit by your own logic),
`krill_by_hand.py` (`when_to_leave`, a `what_if` search for the course fix, a capture and a
landing written out), `orbit.py`, `krill.py` (the programs, pad to Krill's surface), `land.py`,
`hop.py` and `ascent.py`. Your own go in `/root/scripts`, which is kept between games. For the
long coast, it has its games: 2048, snake, worms, mines, solitaire, a falling-sand box and more.

**Life on the ground**: Floe's tundra and snowfields grow groves of frost pines and crowberry,
wild penguin colonies waddle about the Glacier Shelf by the pad, seals lie on the frozen seas and
puffins wheel over the Puffin Spires; Kelp has meadows of grass and glowing puffballs under
frond trees, six-legged grazers and drifting jellies in the air; Haven, round Lantern, has
broadleaf woods, flowers and herds of hornbucks under flocks of birds; Narwhal's brine flats are
crusted with pink mats; every world has its boulders. Where each one is follows from the world's
data alone (`life` in `data/bodies`), so it is the same every visit. It is scenery: a lander
comes down through a tree.

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
| `os/` | The ship's computer: NOMINAL's machine (`os/machine`), the bridge to the ship (`os/core`), its guest programs (`os/guest`, built with `os/tools/mkguest.sh`). |
| `scripts/os/` | The computer's desktop and apps (NOMINAL's), and the Flight app. |
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

## Contributing and modding

See [CONTRIBUTING.md](CONTRIBUTING.md) for how the project fits together and how
to add a gate, and [docs/MODDING.md](docs/MODDING.md) for the part, planet and
career data formats.

## License

AGPL-3.0-or-later. See `LICENSE` and `CREDITS.md`. There is no imported art or
audio: every model, texture and sound is generated by the code.
