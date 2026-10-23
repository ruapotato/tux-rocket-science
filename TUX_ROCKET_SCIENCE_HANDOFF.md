# Tux Rocket Science — Vertical Slice Handoff

## Pitch
An open-source rocket-building and spaceflight sandbox in Godot. Penguins run the space program for **PASA** (the in-game agency). The target is the feel of early Kerbal Space Program 1: a strong rocket editor, real staging, and a flight sim good enough that getting to orbit, then the moon, is the whole fun. Everything else gets built on top of this later.

## Hard rules
1. **IP:** No Kerbal names, art, planet names, part names, or "Space Program" branding. All bodies, parts, and art are original. No NASA name or insignia; PASA is our own parody, with a logo of spaghetti and a fish. Tux lineage is fine: credit Larry Ewing in CREDITS, but draw our own penguins.
2. **The sim is the source of truth.** A C GDExtension (`trs_core`) owns all world state in `double`. Godot nodes are views only. Never read authoritative position, velocity, or mass back from a `Node3D`.
3. **Stock Godot 4.x, single precision.** Do not compile the engine. Precision comes from doubles in `trs_core`, a floating origin, and scaled space.
4. **Native for hot paths, GDScript for glue.** Orbit propagation, vessel physics, aero, and terrain LOD go in C. UI, editor interaction, and scene wiring go in GDScript.
5. **Every number shown to the player is true.** Delta-v, TWR, apoapsis, and periapsis readouts come from `trs_core` and are covered by tests.
6. **Data-driven content.** Parts and celestial bodies are defined in JSON or `.tres` files, not hardcoded, so modding is possible later at no cost.
7. **License:** AGPL-3.0, with a header in every source file.

## Architecture
- **`trs_core/` (C, GDExtension):**
  - Bodies: a fixed star system with on-rails Kepler orbits.
  - Vessels: a part graph with mass, fuel, and thrust.
  - Integration:
    - **Physics mode** while under thrust or in atmosphere: fixed 50 Hz step with RK4 or a symplectic integrator, in doubles.
    - **On-rails mode** when coasting outside atmosphere, analytic and drift-free.
    - Switching between modes must conserve the orbit; this is tested.
  - Patched conics: sphere-of-influence transitions and orbit prediction for map view.
  - Atmosphere: exponential density, drag from part area and Cd, and static pressure, which feeds into engine Isp.
  - Fully headless: runnable with no renderer.
- **Floating origin:** The active vessel stays near (0,0,0). Each frame, convert double positions to vessel-relative floats for rendering.
- **Scaled space:** Distant bodies render on a second camera layer at reduced scale. Local terrain is only real within physics range.
- **Vessel rigidity:** Treat each connected assembly as one rigid body. Decoupling splits it into new bodies. No per-part joint wobble in the slice. Structural failure is optional: break above a force threshold.
- **Terrain:** Cube-sphere quadtree LOD with simple procedural height. It only needs to be good enough to land on.

## The star system (original, scaled down like KSP)
- **Star:** One star, with a name we invent.
- **Home planet:** Icy, radius ~600 km, surface gravity ~9.8 m/s², atmosphere ~70 km. Has a launch site on a glacier shelf.
- **Moon:** One airless moon, with SOI and gravity chosen so a landing and return is achievable with a mid-size rocket.
- **Names:** Use the working names `Floe` (planet) and `Krill` (moon), or propose better ones. Must not collide with KSP names.

## Vertical slice scope

### Rocket editor (PASA Assembly Hangar)
- Part catalog with about 15 parts:
  - 1-seat and 3-seat crew pods
  - 3 tank sizes
  - 3 liquid engines (lifter, vacuum, small)
  - 1 solid booster
  - Stack and radial decouplers
  - Fins
  - Parachute
  - Landing legs
  - Nosecone
  - Reaction wheel
- Stack attach nodes and radial (surface) attach, with symmetry of 1/2/3/4/6.
- Snap, rotate, offset, delete, and undo/redo.
- Live readouts: mass, per-stage delta-v (vacuum and sea level), and per-stage TWR, all computed by `trs_core`.
- Staging list: auto-generated, editable by drag-and-drop.
- Save and load craft as JSON. Ship 3 stock craft:
  - A suborbital hopper
  - An orbiter
  - A moon lander

### Flight
- Throttle, staging (spacebar), WASD/QE attitude, and SAS stability assist.
- Physics warp (up to 4x) and on-rails time warp (up to ~100,000x). Warp is refused under thrust or in atmosphere.
- Navball, altitude, speed (surface/orbit toggle), fuel per stage, and G-force.
- **Map view:**
  - Current orbit with apoapsis/periapsis and SOI encounters.
  - **Maneuver nodes:** prograde, normal, and radial handles, a burn time estimate, and a predicted trajectory.
- Parachute deploy, legs, and crash detection (parts destroyed above an impact speed threshold).
- Revert to launch and revert to hangar.

### Penguins
- The crew pod shows a penguin portrait that reacts to G-force, spin, and freefall, with excited and panicked states.
- Crew are named and persist across flights. This is the slice's main charm, so make it good.

### PASA
- Main menu and hangar branding.
- Mission control voice lines are optional text toasts (e.g. "PASA, we have a problem").

## Non-goals for the slice
Career, science, funds, contracts, heating/reentry, EVA, docking, multiplayer, colonies, a mod API, and console ports.

## Verification gates (must pass on every commit; headless)
Run with `godot --headless -- --<gate>` or a standalone `trs_core` test binary:
- `--test-kepler`: Propagate orbits for 1,000 periods on rails. Specific energy and the elements must drift by less than 1e-9 relative.
- `--test-mode-switch`: Toggle on-rails and physics 1,000 times on a coasting orbit. Apoapsis and periapsis must drift by less than 1 m.
- `--test-deltav`: Burn each stock craft to empty in vacuum. Measured delta-v must match the editor readout (Tsiolkovsky) within 0.5%.
- `--test-ascent`: A scripted autopilot flies the stock orbiter to a stable orbit (periapsis > 75 km) from the launch site.
- `--test-moon`: A scripted mission takes the stock lander through transfer, SOI change, capture, landing, and return to home SOI.
- `--test-staging`: Decoupling splits the assembly, and both halves conserve momentum.
- `--test-craft-roundtrip`: Save and load every craft. Output must be byte-identical and give the same readouts.
- `--screens`: Render fixed camera shots (hangar, launchpad, orbit, map view, moon surface) to PNG for visual review.

Add a **control socket** (JSON over TCP or stdin) that can load a craft, set throttle and attitude, stage, warp, and query state. All gates and agent debugging drive the game blind through it. The 3D view is never required to verify behavior.

## Milestones
1. **M0:** Repo skeleton, GDExtension builds on Linux and Windows, headless runner, CI running the gates (empty).
2. **M1:** `trs_core` with bodies, Kepler on-rails, patched conics, and `--test-kepler` and `--test-mode-switch` green.
3. **M2:** Vessel model (parts, mass, fuel, thrust, staging), atmosphere and drag, and `--test-deltav` and `--test-staging` green.
4. **M3:** Rocket editor with full attach, symmetry, staging, readouts, and save/load. `--test-craft-roundtrip` green.
5. **M4:** Flight scene with floating origin, scaled space, navball/HUD, warp, terrain LOD, and parachutes/legs/crashes. `--test-ascent` green.
6. **M5:** Map view with maneuver nodes. `--test-moon` green.
7. **M6:** Penguin portraits, crew persistence, PASA branding, stock craft polish, and the `--screens` review pass.

## Done means
A new player can build a rocket, reach orbit, plan a burn to the moon, land, and get home, with every gate green. Stop and report at each milestone with gate output and screenshots.
