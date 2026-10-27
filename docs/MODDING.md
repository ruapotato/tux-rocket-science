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
| `look` | `style` (`ice`, `rock`, `moss`), `sky` [r,g,b], `tau` (haze per colour), `clouds`, and colours `low`, `high`, `slope`, `rock`, `basin`, `range`; `tint` multiplies the far-view colours of the `ice` and `rock` styles |
| `ore` | ore abundance 0..1: drills dig `drill_kg_s` x this |
| `biomes` | `[{id, name, abs_lat_min_deg, abs_lat_max_deg, height_min, height_max, near: {lat_deg, lon_deg, radius_km}}]`: a point is in the first biome whose every condition holds; the last one, with none, catches the rest. Landed and low-flying science counts separately per biome |
| `relics` | `[{id, name, lat_deg, lon_deg}]`: signal sources standing on the ground there; a scanner hears one when it is above the relic's horizon and in range. A visual for each id is in `scripts/flight/relic_visual.gd` (an unknown id gets a plain standing stone) |

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
