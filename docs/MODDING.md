# Modding Tux Rocket Science

Everything you fly, and everywhere you fly to, is data. Edit the JSON under
`data/` (or add your own entries) and restart the game. Units are SI unless
a field says otherwise: metres, kilograms, seconds, newtons, pascals, kelvin.

## Parts: `data/parts/stock_parts.json`

`{"parts": [ {...}, ... ]}`. A part's local frame: **+Y** up its axis (a
rocket is built nose-up), and for a surface-attached part **+Z** points out
from the hull it's on.

### Every part

| Field | Meaning (default) |
|---|---|
| `id` | unique, `a-z0-9_` |
| `name`, `description` | shown in the hangar |
| `category` | hangar tab: `pods`, `tanks`, `engines`, `structural`, `aero`, `utility`, `control`, `science` (anything else is hidden) |
| `mass` | dry mass, kg |
| `cost` | funds, in a career |
| `fuel_liquid`, `fuel_solid`, `fuel_mono` | propellant capacity, kg |
| `crew` | seats |
| `torque` | reaction torque, N m |
| `crash_tolerance` | impact speed it survives, m/s (8) |
| `surface_attach` | can go on a hull's side (true) |
| `science` | experiments it carries: ids from `data/career/science.json` |
| `nodes` | stack nodes: `[{name, pos [x,y,z], dir [x,y,z], radius}]` (`top` / `bottom` by convention) |

### `shape`

Shape `box` (and `wedge`, `slab`) makes a flat-sided hull: `width` (X), `depth` (Z) and `height`
(Y), with `width_top` / `depth_top` narrowing it toward the top (a wedge). Mass properties,
ground and vessel contacts and surface attachment all use the box.

`type` picks the model: `adapter`, `antenna`, `booster`, `capsule`, `cone`,
`cylinder`, `dock`, `engine`, `fin`, `hose`, `instrument`, `jet`, `lab`,
`leg`, `mount`, `panel`, `penguin`, `probe`, `pylon`, `rcs`, `shield`,
`sphere`, `wheel`. Physics reads
`radius` (or `radius_bottom`), `radius_top`, `height` (or `length` / `root`),
and for fins `span`.

### `aero`

| Field | Meaning (default) |
|---|---|
| `cd_top`, `cd_bottom`, `cd_side` | drag coefficient per face (0.8, 0.8, 1.0) |
| `area_axial`, `area_side` | reference areas, m^2 (from the shape) |
| `cop` | cross-flow centre of pressure, fraction of height from centre (0) |
| `plate` | a flat plate (fins and wings: true for shape `fin`) |
| `lift_slope` | plates: normal-force slope per radian |
| `control_deflection_deg` | plates: control flap travel (0: fixed) |

### `thermal`

`max_temp` K (1500), `skin_heat` J/(m^2 K) (8000), `emissivity` (0.8). A
tapering leading end takes less heating than a flat one (see
`trs_lead_slant`).

### `engine`

| Field | Meaning |
|---|---|
| `propellant` | `liquid` or `solid` |
| `thrust_vac` | vacuum thrust, N (a jet: static sea-level thrust) |
| `isp_vac`, `isp_sl` | specific impulse, s |
| `gimbal_deg` | steering range |
| `air_breathing` | a jet: flow x density/`rho_ref` x (1 - (v/`v_max`)^2), none in vacuum |
| `v_max`, `rho_ref` | jets (600 m/s, 1.225 kg/m^3) |

### Other blocks

| Block | Fields |
|---|---|
| `rcs` | `thrust` (N per nozzle), `isp_vac`, `isp_sl`, `nozzles: [{pos, dir}]` (up to 6) |
| `decoupler` | `kind` (`stack` / `radial`), `impulse` N s |
| `parachute` | `semi_cda`, `full_cda` m^2, `min_pressure` Pa, `full_altitude` m, `max_speed` m/s, `inflate_time` s |
| `docking` | `capture` m, `max_speed` m/s, `max_angle_deg`, `undock_impulse` N s |
| `feed_line` | `reach` m |
| `leg` | `length`, `spring` N/m, `damper` N s/m, `max_load_speed`, `hinge`, `deploy_angle_deg`, `stowed_fraction` |
| `wheel` | `length` (out along +Z), `spring`, `damper`, `max_load_speed`, `mu_roll`, `mu_brake`, `mu_side`, `steer_deg` |
| `power` | `battery_kj`, `draw_kw`, `torque_kw_per_knm`, `solar_kw` (face-on at `solar_ref_m` from the star; face is +Z), `antenna_kj_per_point`, `antenna_fraction` |
| `drive` | the Stillwater Drive: `accel_g`, `kw` (the whole ship is pushed alike, whatever it weighs); `power.generator_kw` is a reactor's steady output |
| `mining` | `ore_kg` (a bin), `drill_kg_s`, `drill_kw`, `drill_reach` (m from the part to the ground), `refine_kg_s`, `refine_yield`, `refine_kw` |
| `eva` | `walk_speed`, `jump_speed`, `reach` (the penguin) |
| `scanner` | `range_km`: a Signal Scanner hears a relic in sight of it this far away (with the power on) |
| `resize` | `kind` (`round`: X and Z together, or `box`), `min`, `max` ([x, y, z] multipliers): the hangar resizes it in that range; mass, propellant, batteries, ore and crew scale with the volume, nodes, rooms and areas with the size |
| `engine.kw` | an engine that runs on electricity (the Hearth Torch): what it draws at full thrust; short of it, it gives what the power pays for |
| `interior` | rooms a penguin can walk about in: `volumes: [...]`, part-local, each a `cylinder` (`radius`, `y0`, `y1` along +Y), an `annulus` (`r0`, `r1`: a spin ring's deck is its outer wall), a `box` (`min`, `max`, `yaw_deg`) or `spokes` (`count` boxes `width` square from `r0` out to `r1`, between `y0` and `y1`), with optional `windows: [{at, count, size}]` (a row round a cylinder's wall at height `at`, or round an annulus's side walls at radius `at`). Rooms that touch or overlap are one space: make them reach the part's ends so they meet the next part's. `spin_rpm` is the rate the spin hold turns a craft with this part at. |

After changing parts, run `./build/linux/trs_tests --data data --all`: the
gates check the stock parts still behave.

## The star system: `data/bodies/austra_system.json`

`{"name", "bodies": [...]}`, the star first. A system file may list `"neighbours"` (other system files beside it), each placing its own star with `"galactic_position_ly": [x, y, z]` light-years from Austra. Each body:

| Field | Meaning |
|---|---|
| `id`, `name`, `kind` | `star`, `planet`, `moon` |
| `heliopause`, `luminosity` | stars: where the space between the stars begins (m; leave it out for none), and brightness for solar power (Austra = 1) |
| `parent` | id of what it orbits |
| `mu` | gravitational parameter, m^3/s^2 |
| `radius` | m |
| `rotation_period`, `rotation_at_epoch_deg`, `tidally_locked` | spin |
| `orbit` | `a` m, `e`, `inc_deg`, `lan_deg`, `argp_deg`, `mean_anomaly_deg` |
| `atmosphere` | `height` m, `sea_level_pressure` Pa, `sea_level_density` kg/m^3, `scale_height` m |
| `terrain` | `seed`, `amplitude`, `base_freq`, `octaves`, `persistence`, `offset`, `ridge`, `crater_freq`, `crater_depth`, `mountain_amplitude`, `mountain_freq`, `mountain_coverage`, `ranges: [{name, lat_deg, lon_deg, radius_km, height}]` |
| `launch_site` | `name`, `lat_deg`, `lon_deg`, `altitude`, `flat_radius` (the runway runs east from the pad) |
| `look` | `style`: `ice` (snow, blue ice, sea ice), `rock` (regolith), `moss` (grass and moss up close, soil on banks, snow above `snow_line` m and at the poles unless `polar_snow` is false, still water below `sea_level` m in the `basin` colour) or `ash` (black basalt and pale ash, slag glowing through its cracks below `lava_level` m); `sky` [r,g,b], `tau` (haze per colour), `clouds`, and colours `low`, `high`, `slope`, `rock`, `basin`, `range`; `tint` multiplies the far-view colours of the `ice` and `rock` styles |
| `landmarks` | named places shaped into the ground (up to 12): `{id, name, kind, lat_deg, lon_deg, text}` with, for a `volcano`, `radius_km`, `height` (m, to the rim) and `depth` (its caldera's); a `crater`, `radius_km`, `depth` and `height` (the rim's); a `canyon`, `length_km`, `width_km`, `depth` and `bearing_deg` (its way, clockwise from north; it wanders); a `mesa`, `radius_km` and `height`. The first time a ship comes close enough to see one (within its size, and more the higher it is) the flight screen says `Discovered:` and its `text`, it pays research, the map labels it after, and a script reads it as `ship.landmark` |
| `ore` | ore abundance 0..1: drills dig `drill_kg_s` x this |
| `biomes` | `[{id, name, abs_lat_min_deg, abs_lat_max_deg, height_min, height_max, near: {lat_deg, lon_deg, radius_km}}]`: a point is in the first biome whose every condition holds; the last one, with none, catches the rest. Landed and low-flying science counts separately per biome |
| `relics` | `[{id, name, lat_deg, lon_deg}]`: signal sources standing on the ground there; a scanner hears one when it is above the relic's horizon and in range. A visual for each id is in `scripts/flight/relic_visual.gd` (an unknown id gets a plain standing stone) |
| `life` | plants and animals (up to 12 kinds): `{id, shape, per_km2, herd, spread, biomes, max_slope_deg, height_min, height_max, size: [min, max] m, color, color2, range}`; `per_km2` groups (a grove, a herd) of `herd` within `spread` m; only in the listed `biomes` (none = anywhere), on ground no steeper than `max_slope_deg`, never by the launch site or a relic. Animals add `"animal": true`, `loop` (m they wander round their spot), `fly` (m above the ground), `speed` m/s, and for grazers `legs`, `horns`. Shapes: `pine`, `broadleaf`, `frond`, `shrub`, `grass`, `flower`, `puffball`, `crystal`, `mat`, `rock`; animals `penguin`, `seal`, `bird`, `grazer`, `drifter`. `range` is how far away it is drawn. Scenery: nothing collides with it |

Spheres of influence come from the masses. Adding a body after the existing
ones keeps old saves loadable.

## The career: `data/career/`

- `science.json`: `situations` (`value`, `phrase`, `biome`: counted per biome), `bodies` (`value`, the
  heights that split low from high), `experiments` (`base`, `situations`,
  `eva`), and `repeat_factor`.
- `tech.json`: R&D `nodes`: `{id, name, cost, requires, parts}` (and `site`:
  a launch site it opens, `orbit` for the Orbital Yard). A node with
  `found_by` (a mission id) can't be bought: it opens when that mission is
  done. Parts in no node are always available.
- `missions.json`: `start_funds`, `recovery_refund`, and `missions`:
  `{id, title, text, goal: {kind, body, value, relic}, funds, research, after, story}`.
  Goal kinds: `altitude`, `space`, `orbit`, `soi`, `landed`, `eva_space`,
  `eva_landed`, `recover_from`, `flag`, `rendezvous`, `docked`, `refine`, `hear`
  (a scanner has heard `relic`) and `investigate` (a penguin on foot within
  25 m of `relic`). `story` is shown when the mission is done.

## Stock craft: `data/craft/*.craft.json`

Built in the hangar and saved (Ctrl+S); copy the file from
`~/.local/share/godot/app_userdata/Tux Rocket Science/craft/` into
`data/craft/` to make it stock.

## The ship's computer: scripts, readings, commands

The computer (F2 in flight) sees the ship and asks; it can't reach past the
verbs the game declares.

- **Readings** come from `ShipComputer.readings()` in
  `scripts/flight/ship_computer.gd`: whole metres, seconds, kilograms and m/s
  (two decimal places; the shell's tests read the whole part), plus words. Each is
  `/dev/ship/<key>`, and `ship status` / `ship json` prints them all. `frame`
  counts the game's updates. A script loops many times inside one frame, so
  wait for a new `frame` before doing something you want done once.
- **Commands** are `ShipComputer.VERBS`: a name, the argument kinds (`n` a
  number, `w` a word, `t` the rest of the line; a trailing `?` makes the last
  one optional) and a help line. The machine checks the arguments and queues
  the line, the same line said twice in one frame is done once, and
  `ShipComputer._do()` carries it out through the flight scene's own
  controls. A new verb takes a row in `VERBS` and a branch in `_do()`.
- **Programs** are `scripts/flight/flight_programs.gd` (`FlightPrograms`): each verb in
  `ShipComputer.PROGRAMS` starts one. A program is a `_begin_<name>` that checks it can run
  and a step function the game calls each frame, ending with `_done(note)` or `_fail(why)`.
  The machine reads `program`, `program_status`, `program_note` and `program_id`; the
  flight library's calls in `os/guest/py.c` (`PROGRAM(...)`) start one and wait for it.
- **The things aboard** (the dot: `ship.part("Walrus").fuel`, `ship.orbit.top`) are the
  table `ShipComputer.objects()` builds each frame: `path\tvalue\tflags` rows such as
  `part.12.fuel`, `orbit.top`. A value may be another thing (`@part.12:Walrus`), a list of
  them (`@[...]`) or `@call` (something it does); `w` marks one a script may set. A set
  arrives as `set <path> <value>` and a call as `call <path>`, both in `ShipComputer._do()`;
  the parts are told through `core.part_action(id, uid, what, value)`
  (`trs_world_part_action`). A new reading is a row in `objects()` (and a line in
  `scripts/os/pylib.gd`, so the Script Editor suggests it).
- **Questions the game works out** (`when_to_leave`, `what_if`) are `ask <id> ...` lines:
  `ShipComputer._ask()` answers into `answer.<id>.*` rows, and the script waits for them.
- **The debugger:** a script in the background writes `/run/py/<pid>.vars` (its line, its
  state and every variable) and reads `/run/py/<pid>.ctl` (`break 12 30`, `mode run|pause`,
  `go <n>`, `step <n>`). `py -i` is the console: it runs what's written to
  `/run/py/console.in`, one piece per line of `\x1e`. The Script Editor is
  `scripts/os/codeapp.gd`.
- **R&D:** a tech node with `"computer": "planner"` or `"guidance"` opens that part of the
  computer in a career (`Career.computer_unlocked()`).
- **Numbers:** the CPU has the F and D extensions (`os/machine/cpu_fp.inc`). `/bin/py` is built
  for rv64imfd, and its `sin` and friends are the `SYS_math` syscall.
- **Scripts:** `/bin/py` or `/bin/sh` (for `*.sh`). In py, `ship()` returns
  the JSON, `json()` parses it, and `do("verb args")` sends a command. The
  examples on the disk are in `os/machine/image.c` (search for
  `/root/examples`). The guest programs are C in `os/guest/`; rebuild
  `os/machine/guestbin.h` with `cd os && tools/mkguest.sh` (clang with the
  riscv64 target, and ld.lld).
