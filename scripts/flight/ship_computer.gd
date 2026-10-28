# Tux Rocket Science
# Copyright (C) 2026 Tux Rocket Science contributors
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as published
# by the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
## The ship's computer in flight (os/: NOMINAL's machine, a real RV64
## computer with a disk and a shell, by way of RUNBOOK's bridge). Each frame
## the flight hands it what the panels say, lets its scripts run a few
## slices of its CPU, and does what it has asked, through the same controls
## the keys use: `ship throttle 1` is Z, `ship stage` is Space. The machine
## sees the ship and asks; it never reaches past this.
##
## One computer per game: it boots once (about a tenth of a second) and
## keeps its disk while the game runs; /root/scripts and /root/notes.txt are
## kept in user://computer/ between runs.
class_name ShipComputer
extends RefCounted

const SLICES := 3         # cpu slices a frame for running scripts (50k instructions each)
const SAVE_DIR := "user://computer/"
const KEEP := ["/root/scripts", "/root/notes.txt", "/root/Documents", "/root/.editor_seen"]

## name, argument kinds (n number, w word, t text; ? = optional), what it does
const VERBS := [
	["throttle", "n", "the throttle, 0 to 1 (Z is 1, X is 0)"],
	["stage", "", "fires the next stage (Space)"],
	["sas", "w", "SAS: off, stability, prograde, retrograde, normal, antinormal, radial_out, radial_in, target"],
	["point", "nn", "points the nose: pitch above the horizon, heading from north (degrees)"],
	["rcs", "w", "RCS on or off (R)"],
	["chutes", "", "arms the parachutes (P)"],
	["legs", "w?", "landing legs: on (out) or off (in); nothing: out if in, in if out (G)"],
	["brakes", "w", "wheel brakes on or off (B)"],
	["spin", "w", "spin hold on or off (Home)"],
	["drive", "n", "the Stillwater Drive's throttle, 0 to 1"],
	["mining", "w", "drills and refineries on or off (C)"],
	["say", "t", "puts a line on the flight screen"],
	["beep", "w?", "a sound: beep, alarm or chime"],
	["autostage", "w", "on: the programs stage when a stage is spent (the default); off: never"],
	["target", "t", "picks a vessel to target, by name (none: no target)"],
	["abort", "", "stops the program running and closes the throttle"],
	["aim", "w", "points the nose along a direction and holds it: prograde, retrograde, normal, radial_out, up..."],
	["aim_along", "nnn", "points the nose along a burn: forward (prograde), sideways (normal), outward (radial) m/s"],
	["chart", "wn", "a point on the Flight app's graph: chart height 1200"],
	["ask", "t", "(scripts) a question the game works out: when_to_leave(), what_if()"],
	["await_space", "", "(scripts) waiting for the pilot's space: wait_user_trigger()"],
	["wake", "n", "(scripts) asleep until this game time: sleep() (time warp eases off before it)"],
	# the programs: started here, flown a frame at a time by the game (FlightPrograms)
	["launch", "n?", "PROGRAM: off the ground to an orbit with this apoapsis (m; 80 km on Floe)"],
	["circularize", "w?", "PROGRAM: rounds the orbit off at the apoapsis (or: periapsis)"],
	["burn", "t", "PROGRAM: burn prograde 120 (m/s), or burn prograde until apoapsis > 2000000"],
	["transfer", "wn?", "PROGRAM: plans and flies the burn to a moon or planet (pass height, m)"],
	["window", "w", "PROGRAM: warps to where the burn to that body starts, pointing prograde"],
	["warp", "w", "PROGRAM: time warp: seconds, apoapsis, periapsis, soi, node, or a body"],
	["capture", "n?", "PROGRAM: brakes at the periapsis into orbit (to this apoapsis, m)"],
	["land", "", "PROGRAM: down onto the ground below, on the engines (or the chutes)"],
	["hover", "nn?", "PROGRAM: holds a height above the ground (m), for seconds"],
	["node", "", "PROGRAM: flies the next maneuver node on the map"],
]

const PROGRAMS := ["launch", "circularize", "burn", "transfer", "window", "warp", "capture", "land", "hover", "node"]

static var _shared: ShipComputer  # the game's one computer

var machine: Object
var log: Array[String] = []   # what scripts have printed, and what was done, newest last (the last 200)
var log_count := 0            # lines ever noted: the log's newest are log[size - (log_count - seen)...]
var _verbs_tsv := ""
var _said: Array[String] = []  # say lines waiting for the flight screen
var _frame := 0
var programs := FlightPrograms.new()
var _enc := {}     # the path ahead, looked at now and then: encounter, encounter_pe, time_to_soi
var _enc_t := -1e9
# the things aboard, as a script's dot sees them (engine.throttle): see objects()
var _parts: Array = []        # this frame's core.vessel_part_states
var _names := {}              # part uid -> its name aboard ("Swivel 2")
var _answers := {}            # ask id -> [frame asked, {key: value}]: when_to_leave(), what_if()
var _await_space := -100      # the frame a script last waited for space (wait_user_trigger)
var space_presses := 0        # spaces a waiting script has had
var charts := {}              # chart(name, value): name -> [[t, value], ...]
var chart_frame := -1000      # the frame a chart last had a point
var _said_at := []            # [[frame, line], ...]: what scripts printed, lately (for the flight screen)
var _panel := {}
var _panel_frame := -1000
var aim_word := ""            # what aim() last pointed at (a direction's name)
var aim_vec := Vector3.ZERO   # or a direction
var aim_ph := []              # aim_at(): [pitch, heading], held against the horizon as it turns under the ship
var aim_fso := []             # aim_along(): [forward, sideways, outward], held to the orbit as it goes
var _wake := [-100, 0.0]      # [frame said, game time] a sleeping script wakes at
var _ship_id := [-1, -1]      # the vessel (id, serial) the aim was held for
var _session: FlightSession = null


static func get_shared() -> ShipComputer:
	if _shared == null:
		_shared = ShipComputer.new()
	return _shared


func _init() -> void:
	if not ClassDB.class_exists("TrsComputer"):
		return
	machine = ClassDB.instantiate("TrsComputer")
	machine.boot(4823)
	var rows := PackedStringArray()
	for v in VERBS:
		rows.append("%s\t%s\t%s" % v)
	_verbs_tsv = "\n".join(rows) + "\n"
	machine.set_verbs(_verbs_tsv)
	_restore()


## The ship's readings, as the machine sees them: metres, seconds,
## kilograms and m/s (two places: the shell's tests read the whole part),
## and words. With the programs and the session, also what the program is
## doing and what the path ahead meets.
static func readings(t: Dictionary, progs: FlightPrograms = null, session: FlightSession = null, fresh := false) -> Dictionary:
	var o: Dictionary = t.get("orbit", {})
	var r := {}
	r["name"] = str(t.get("name", "")).replace("\t", " ")
	r["body"] = str(t.get("body", ""))
	r["situation"] = str(t.get("situation", ""))
	r["biome"] = str(t.get("biome", ""))
	r["altitude"] = _n(t.get("altitude", 0.0))
	r["altitude_terrain"] = _n(t.get("altitude_terrain", 0.0))
	r["apoapsis"] = _n(o.ap_alt) if o.get("ap_alt") != null else "-1"
	r["periapsis"] = _n(o.get("pe_alt", 0.0))
	r["time_to_apoapsis"] = _n(o.t_to_ap) if o.get("t_to_ap") != null else "-1"
	r["time_to_periapsis"] = _n(o.t_to_pe) if o.get("t_to_pe") != null else "-1"
	r["period"] = _n(o.period) if o.get("period") != null else "-1"
	r["eccentricity"] = "%.4f" % float(o.get("ecc", 0.0))
	r["inclination"] = _n(o.get("inc_deg", 0.0))
	r["vertical_speed"] = _n(t.get("vertical_speed", 0.0))
	r["horizontal_speed"] = _n(t.get("horizontal_speed", 0.0))
	r["surface_speed"] = _n(t.get("surface_speed", 0.0))
	r["orbital_speed"] = _n(t.get("orbital_speed", 0.0))
	r["pitch"] = _n(t.get("pitch_deg", 0.0))
	r["heading"] = _n(t.get("heading_deg", 0.0))
	r["mass"] = _n(t.get("mass", 0.0))
	r["thrust_kn"] = _n(float(t.get("thrust", 0.0)) / 1000.0)
	r["max_thrust_kn"] = _n(float(t.get("thrust_max", 0.0)) / 1000.0)
	r["twr"] = _n(t.get("twr", 0.0))
	r["throttle_pct"] = _n(float(t.get("throttle", 0.0)) * 100.0)
	r["g_force"] = _n(t.get("g_force", 0.0))
	r["liquid"] = _n(t.get("liquid", 0.0))
	r["liquid_capacity"] = _n(t.get("liquid_capacity", 0.0))
	r["mono"] = _n(t.get("mono", 0.0))
	r["ec"] = _n(t.get("ec", 0.0))
	r["ec_capacity"] = _n(t.get("ec_capacity", 0.0))
	r["heat_pct"] = _n(float(t.get("heat_frac", 0.0)) * 100.0)
	r["dynamic_pressure"] = _n(t.get("dynamic_pressure", 0.0))
	r["in_atmosphere"] = "yes" if t.get("in_atmosphere", false) else "no"
	r["engines_lit"] = str(int(t.get("engines_lit", 0)))
	r["next_stage"] = str(int(t.get("next_stage", -1)))
	r["crew"] = str(int(t.get("crew", 0)))
	r["sas"] = str(t.get("sas", "off"))
	r["rcs"] = "on" if t.get("rcs", false) else "off"
	r["landed"] = "yes" if t.get("landed", false) else "no"
	r["lat"] = "%.4f" % float(t.get("lat_deg", 0.0))
	r["lon"] = "%.4f" % float(t.get("lon_deg", 0.0))
	r["dv_left"] = _n(t.get("dv_total", 0.0))
	var stages: Array = t.get("stages", [])
	r["stage_dv"] = _n(stages[0].get("dv_vac", 0.0)) if not stages.is_empty() else "0.00"
	if session != null:
		var b = JSON.parse_string(session.core.body_info(int(t.get("body_index", -1))))
		if b is Dictionary and not b.is_empty():
			var rr := float(t.get("altitude", 0.0)) + float(b.radius)
			r["gravity"] = _n(float(b.mu) / (rr * rr))
			r["surface_gravity"] = _n(b.surface_gravity)
			r["body_radius"] = _n(b.radius)
			r["atmosphere_height"] = _n(b.atmosphere_height)
		r["target"] = str(session.target_info().get("name", "none"))
		var shared := get_shared()
		var now := float(t.get("t", 0.0))
		if fresh or absf(now - shared._enc_t) > 2.0:
			shared._enc = _path_ahead(session, now)
			shared._enc_t = now
		r.merge(shared._enc, true)
	if progs != null:
		r["program"] = progs.name if progs.name != "" else "none"
		r["program_status"] = progs.status
		r["program_phase"] = progs.phase if progs.phase != "" else "-"
		r["program_note"] = progs.note.replace("\n", " ") if progs.note != "" else "-"
		r["program_id"] = str(progs.id)
		r["autostage"] = "on" if progs.autostage else "off"
	return r


static func _n(x) -> String:
	return "%.2f" % float(x)


## Where the path goes next: the body whose sphere it enters (encounter),
## how high it passes there, and how long until it gets there.
static func _path_ahead(session: FlightSession, now: float) -> Dictionary:
	var d := {"encounter": "none", "encounter_pe": "-1", "time_to_soi": "-1"}
	if session.telemetry().get("landed", false):
		return d
	var patches := session.predict(4, 6.0e6)
	if patches.size() > 1 and str(patches[0].get("end", "")) in ["soi_enter", "soi_exit"]:
		d.encounter = str(patches[1].body)
		d.encounter_pe = _n(patches[1].get("pe_alt", -1.0))
		d.time_to_soi = _n(float(patches[0].t_end) - now)
	return d


func update(scene: Node, t: Dictionary) -> void:
	if machine == null or t.is_empty():
		return
	var rows := PackedStringArray()
	var r := readings(t, programs, scene.session)
	_frame += 1
	r["frame"] = _frame  # which update this is: a script can tell a fresh reading from one it already acted on
	r["time"] = _n(t.get("t", 0.0))
	_parts = JSON.parse_string(scene.session.core.vessel_part_states(scene.session.active))
	if not _parts is Array:
		_parts = []
	_session = scene.session
	# another ship (a new flight, a switch): what was aimed at was that one's
	var who := [scene.session.active, scene.session.core.vessel_serial(scene.session.active)]
	if who != _ship_id:
		_let_go_aim()
		_ship_id = who
		charts.clear()
	_hold_aim(scene.session, t)
	r.merge(_aim_readings(t), true)
	r.merge(_stage_readings(t), true)
	r["space_presses"] = str(space_presses)
	for k in r:
		rows.append("%s\t%s" % [k, str(r[k])])
	machine.set_state("\n".join(rows) + "\n")
	machine.set_objects(objects(t, r, scene.session.core))
	machine.tick(SLICES)
	var said: String = machine.take_output()
	for line in said.split("\n", false):
		# (a script ending says so itself, or its error does: the kernel's
		# word on it is noise)
		if not (line.begins_with("kernel: script") and line.contains(" died -- ")):
			_note(line)
	var acts: String = machine.take_actions()
	for line in acts.split("\n", false):
		_do(scene, t, line)
	var was := programs.status
	programs.step(scene, t)
	for line in programs.said:
		_note(line)
	programs.said.clear()
	if was == "running" and not programs.running():
		_note("%s %s: %s" % [programs.name, programs.status, programs.note])


## What the flight screen's COMPUTER panel shows: each script running (its
## file, the line it's on, what it's doing), the program flying, what was
## said lately (the last ten seconds or so), and the charts while they're
## being drawn. Looked at every ten frames.
func panel() -> Dictionary:
	if machine == null:
		return {}
	if _frame - _panel_frame < 10:
		return _panel
	_panel_frame = _frame
	var out := {"scripts": [], "said": [], "charts": charts if _frame - chart_frame < 1200 else {}}
	for row in str(machine.scripts()).split("\n", false):
		var f := row.split("\t")
		if f.size() < 4 or f[1] != "1" or f[3] == "-i":
			continue
		var info := {"path": f[3], "pid": int(f[0]), "line": 0, "state": "running"}
		for l in str(machine.read_file("/run/py/%s.vars" % f[0])).split("\n", false):
			if l.begins_with("line "):
				info.line = int(l.substr(5))
			elif l.begins_with("state "):
				info.state = l.substr(6)
		out.scripts.append(info)
	for s in _said_at:
		if _frame - int(s[0]) < 600:
			out.said.append(str(s[1]))
	if programs.running():
		out["program"] = "%s: %s" % [programs.name, programs.note if programs.note != "" else programs.phase]
	_panel = out
	return out


## Is a script waiting for the pilot's space (wait_user_trigger)? Then the
## press is the script's, not a stage.
func wants_space() -> bool:
	return _frame - _await_space <= 2


## The game time a sleeping script wakes at (-1: none is asleep).
func wake_at() -> float:
	return float(_wake[1]) if _frame - int(_wake[0]) <= 2 else -1.0


func take_space() -> void:
	space_presses += 1
	_await_space = -100
	_note("space: the script goes on")


## Where what the script aimed at is now: aim_at's pitch and heading
## against the horizon here, aim_along's burn against the orbit now, a
## surface direction as the ship moves (prograde and the rest SAS holds by
## itself).
func _aim_dir(t: Dictionary) -> Vector3:
	if not aim_ph.is_empty():
		var p := deg_to_rad(float(aim_ph[0]))
		var h := deg_to_rad(float(aim_ph[1]))
		var up := FlightPrograms.v3(t.get("up"))
		var north := FlightPrograms.v3(t.get("north"))
		var east := FlightPrograms.v3(t.get("east"))
		return (up * sin(p) + (north * cos(h) + east * sin(h)) * cos(p)).normalized()
	if not aim_fso.is_empty() and _session != null:
		# the burn as the game reckons a maneuver node (what_if's very
		# axes), a second from now
		var n := [float(t.get("t", 0.0)) + 1.0, float(aim_fso[0]), float(aim_fso[1]), float(aim_fso[2])]
		return _session.node_vector([n], 0).normalized()
	if aim_word == "surface_retrograde" and float(t.get("surface_speed", 0.0)) < 1.5:
		return FlightPrograms.v3(t.get("up"))  # (all but stopped: against the motion is upright)
	if aim_word != "":
		return FlightPrograms.dir_of(t, aim_word)
	return aim_vec


## Keeps the SAS on what the script aimed at, frame by frame, unless a
## program is flying (one that burns where it was aimed, burn(dv), lets it).
func _hold_aim(s: FlightSession, t: Dictionary) -> void:
	if aim_word in ["prograde", "retrograde", "normal", "antinormal", "radial_out", "radial_in"]:
		return
	if aim_ph.is_empty() and aim_fso.is_empty() and not aim_word in ["surface_prograde", "surface_retrograde", "up"]:
		return
	if programs.running() and str(programs._a.get("dir", "")) != "here":
		return
	var d := _aim_dir(t)
	if d != Vector3.ZERO:
		# (a SAS mode picked by hand, or the keys, let the aim go first)
		s.set_sas("target", PackedFloat64Array([d.x, d.y, d.z]))


## How far the nose is from where aim()/aim_at() pointed it, and how fast
## the ship is turning (degrees, degrees a second).
func _aim_readings(t: Dictionary) -> Dictionary:
	var want := _aim_dir(t)
	var nose := FlightPrograms.v3(t.get("nose"))
	var err := rad_to_deg(nose.angle_to(want)) if want != Vector3.ZERO and nose != Vector3.ZERO else 180.0
	return {"aim_error": _n(err), "turn_rate": _n(rad_to_deg(float(t.get("spin", 0.0))))}


## The engines burning (or, between stages, the ones the next lights): their
## thrust together now (kN) and the speed their exhaust leaves at (m/s),
## for burn_time() and stopping_distance().
func _stage_readings(t: Dictionary) -> Dictionary:
	var f := 0.0
	var mdot := 0.0
	var next := int(t.get("next_stage", -1))
	var any_lit := false
	for p in _parts:
		any_lit = any_lit or bool(p.get("lit", false))
	for p in _parts:
		if not p.has("thrust_max"):
			continue
		if (any_lit and not p.get("lit", false)) or (not any_lit and int(p.stage) != next):
			continue
		var fm := float(p.thrust_max)
		f += fm
		if float(p.isp) > 0.0:
			mdot += fm / (float(p.isp) * 9.80665)
	var ve := f / mdot if mdot > 0.0 else 0.0
	return {"stage_thrust_kn": _n(f / 1000.0), "exhaust_speed": _n(ve), "dv_expended": _n(t.get("dv_expended", 0.0))}


## THE THINGS ABOARD, as a script's dot sees them: "path\tvalue\tflags"
## rows (see os/core/computer.h). ship.orbit.top is "orbit.top", an engine
## "part.<uid>" (engine.throttle is "part.12.throttle", writable), a list
## of them "@[part.12:Swivel|...]". Numbers are in the units the HUD
## speaks: m, s, m/s, kN, kg and percent.
func objects(t: Dictionary, r: Dictionary, core: Object) -> String:
	var rows := PackedStringArray()
	var o: Dictionary = t.get("orbit", {})
	# the ship's own, in plain words (ship.altitude and the rest of
	# `ship status` come from the readings)
	rows.append("height\t%s\t" % r.altitude_terrain)
	rows.append("speed\t%s\t" % r.surface_speed)
	rows.append("falling_speed\t%s\t" % _n(-float(t.get("vertical_speed", 0.0))))
	rows.append("delta_v\t%s\t" % r.dv_left)
	rows.append("thrust\t%s\t" % r.thrust_kn)
	rows.append("max_thrust\t%s\t" % r.max_thrust_kn)
	rows.append("throttle\t%s\tw" % r.throttle_pct)
	rows.append("stage\t%s\t" % r.next_stage)
	var sl := float(t.get("stage_liquid_capacity", 0.0)) + float(t.get("stage_solid_capacity", 0.0))
	rows.append("fuel\t%s\t" % _n(100.0 * (float(t.get("stage_liquid", 0.0)) + float(t.get("stage_solid", 0.0))) / sl if sl > 0.0 else 0.0))
	rows.append("landed\t%s\t" % ("true" if t.get("landed", false) else "false"))
	# its orbit: top and bottom are heights above the ground's mean (sea) level
	rows.append("orbit\t@orbit:orbit\t")
	var ap: String = r.apoapsis
	var pe: String = r.periapsis
	for k in [["top", ap], ["apoapsis", ap], ["bottom", pe], ["periapsis", pe], ["time_to_top", r.time_to_apoapsis],
			["time_to_apoapsis", r.time_to_apoapsis], ["time_to_bottom", r.time_to_periapsis],
			["time_to_periapsis", r.time_to_periapsis], ["period", r.period], ["eccentricity", r.eccentricity],
			["inclination", r.inclination], ["speed", r.orbital_speed], ["body", r.body]]:
		rows.append("orbit.%s\t%s\t" % k)
	rows.append("orbit.name\torbit\t")
	if o.has("sma"):
		rows.append("orbit.semi_major_axis\t%s\t" % _n(o.sma))
	# the body it is at
	var b: Dictionary = {}
	var bj = JSON.parse_string(core.body_info(int(t.get("body_index", -1))))
	if bj is Dictionary:
		b = bj
	if not b.is_empty():
		rows.append("body\t@body:%s\t" % b.name)
		rows.append("body.name\t%s\t" % b.name)
		for k in [["radius", b.radius], ["mu", b.mu], ["gravity", b.surface_gravity], ["atmosphere", b.atmosphere_height],
				["sphere", b.soi], ["day", b.rotation_period]]:
			rows.append("body.%s\t%s\t" % [k[0], _n(k[1])])
		rows.append("body.has_air\t%s\t" % ("true" if float(b.atmosphere_height) > 0.0 else "false"))
	# the parts, each by a name that is its own ("Swivel 2" when there are two)
	_names = part_names(_parts)
	var lists := {"parts": [], "engines": [], "boosters": [], "tanks": [], "decouplers": [], "parachutes": [], "legs": [], "pods": []}
	var by_kind := {"engine": "engines", "booster": "boosters", "tank": "tanks", "decoupler": "decouplers",
		"parachute": "parachutes", "leg": "legs", "pod": "pods"}
	var ship_thr := float(t.get("throttle", 0.0))
	for p in _order(_parts):
		var pre := "part.%d" % int(p.uid)
		var nm: String = _names.get(int(p.uid), str(p.name))
		var item := "%s:%s" % [pre, nm]
		lists.parts.append(item)
		var kind := str(p.kind)
		if by_kind.has(kind):
			lists[by_kind[kind]].append(item)
		rows.append("%s.name\t%s\t" % [pre, nm])
		rows.append("%s.kind\t%s\t" % [pre, kind])
		rows.append("%s.part\t%s\t" % [pre, p.part])
		rows.append("%s.stage\t%d\t" % [pre, int(p.stage)])
		rows.append("%s.temp\t%s\t" % [pre, _n(p.temp)])
		rows.append("%s.heat\t%s\t" % [pre, _n(100.0 * float(p.temp) / maxf(float(p.max_temp), 1.0))])
		if p.has("fuel_capacity") and not p.has("feed"):
			rows.append("%s.fuel\t%s\t" % [pre, _n(100.0 * float(p.fuel) / maxf(float(p.fuel_capacity), 1e-9))])
			rows.append("%s.fuel_kg\t%s\t" % [pre, _n(p.fuel)])
			rows.append("%s.capacity_kg\t%s\t" % [pre, _n(p.fuel_capacity)])
		if p.has("feed"):
			# an engine: the fuel is what is left in the tanks that feed it
			rows.append("%s.fuel\t%s\t" % [pre, _n(p.feed)])
			rows.append("%s.fuel_kg\t%s\t" % [pre, _n(p.feed_kg)])
			rows.append("%s.lit\t%s\t" % [pre, "true" if p.lit else "false"])
			var own := float(p.throttle)
			rows.append("%s.throttle\t%s\t%s" % [pre, _n(100.0 * (own if own >= 0.0 else ship_thr)), "w" if kind == "engine" else ""])
			rows.append("%s.thrust\t%s\t" % [pre, _n(float(p.thrust) / 1000.0)])
			rows.append("%s.max_thrust\t%s\t" % [pre, _n(float(p.thrust_max) / 1000.0)])
			rows.append("%s.isp\t%s\t" % [pre, _n(p.isp)])
			rows.append("%s.ignite\t@call\t" % pre)
			if kind == "engine":
				rows.append("%s.shutdown\t@call\t" % pre)
		if p.has("fired"):
			rows.append("%s.fired\t%s\t" % [pre, "true" if p.fired else "false"])
			rows.append("%s.decouple\t@call\t" % pre)
		if p.has("chute"):
			rows.append("%s.state\t%s\t" % [pre, p.chute])
			rows.append("%s.deploy\t@call\t" % pre)
			rows.append("%s.cut\t@call\t" % pre)
		if p.has("legs"):
			rows.append("%s.deployed\t%s\t" % [pre, "true" if p.legs else "false"])
			rows.append("%s.deploy\t@call\t" % pre)
			rows.append("%s.retract\t@call\t" % pre)
	for k in lists:
		rows.append("%s\t@[%s]\t" % [k, "|".join(lists[k])])
	# what the game has worked out for a script that asked (when_to_leave...)
	for id in _answers.keys():
		if _frame - int(_answers[id][0]) > 600:
			_answers.erase(id)
			continue
		var ans: Dictionary = _answers[id][1]
		for k in ans:
			rows.append("answer.%s.%s\t%s\t" % [id, k, str(ans[k])])
	return "\n".join(rows) + "\n"


## The parts in the order they go: the next stage's first, then the one
## after (the order the stage list reads), then the rest as built.
static func _order(parts: Array) -> Array:
	var out := parts.duplicate()
	var at := {}
	for i in out.size():
		at[int(out[i].uid)] = i
	out.sort_custom(func(a, b):
		if int(a.stage) != int(b.stage):
			return int(a.stage) > int(b.stage)
		return at[int(a.uid)] < at[int(b.uid)])
	return out


## Each part's name aboard: its catalog name, numbered in stage order when
## the ship has more than one of them ("Swivel 1", "Swivel 2").
static func part_names(parts: Array) -> Dictionary:
	var count := {}
	for p in parts:
		count[str(p.name)] = int(count.get(str(p.name), 0)) + 1
	var seen := {}
	var out := {}
	for p in _order(parts):
		var n := str(p.name)
		if int(count[n]) > 1:
			seen[n] = int(seen.get(n, 0)) + 1
			out[int(p.uid)] = "%s %d" % [n, int(seen[n])]
		else:
			out[int(p.uid)] = n
	return out


## Runs one line on the machine's shell, as a person at a terminal would.
func sh(line: String) -> String:
	return str(machine.sh(line)) if machine else "the computer is off\n"


func take_said() -> Array[String]:
	var out := _said.duplicate()
	_said.clear()
	return out


func _note(line: String) -> void:
	log.append(line)
	log_count += 1
	if not line.begins_with("> "):
		_said_at.append([_frame, line])
		if _said_at.size() > 8:
			_said_at = _said_at.slice(_said_at.size() - 8)
	if log.size() > 200:
		log = log.slice(log.size() - 200)


func _on(v: String) -> bool:
	return v in ["on", "1", "yes", "true"]


## What the machine asked for, done the way the keys do it.
func _do(scene: Node, t: Dictionary, line: String) -> void:
	var w := line.split(" ", false)
	if w.is_empty():
		return
	var s: FlightSession = scene.session
	var arg := w[1] if w.size() > 1 else ""
	match w[0]:
		"throttle":
			scene._throttle = clampf(float(arg), 0.0, 1.0)
			s.set_throttle(scene._throttle)
			engines_follow(s)
		"set":
			_set_reading(scene, t, w)
		"call":
			_call_part(scene, t, w)
		"ask":
			_ask(scene, t, w)
		"await_space":
			_await_space = _frame
			return  # (said every frame it waits: not one for the log)
		"wake":
			_wake = [_frame, float(arg)]
			return
		"chart":
			if w.size() >= 3 and w[2].is_valid_float():
				var c: Array = charts.get(w[1], [])
				c.append([float(t.get("t", 0.0)), float(w[2])])
				if c.size() > 600:
					c = c.slice(c.size() - 600)
				charts[w[1]] = c
				chart_frame = _frame
			return
		"aim":
			if not arg in FlightPrograms.DIRS or arg == "here":
				_note("aim: '%s' is not a direction (%s)" % [arg, ", ".join(FlightPrograms.DIRS.slice(0, 9))])
				return
			aim_word = arg
			aim_vec = Vector3.ZERO
			aim_ph = []
			aim_fso = []
			scene._sas_follow = ""
			if arg in ["prograde", "retrograde", "normal", "antinormal", "radial_out", "radial_in"]:
				s.set_sas(arg)
			else:
				_hold_aim(s, t)
		"aim_along":
			aim_word = ""
			aim_ph = []
			aim_fso = [float(w[1]), float(w[2]), float(w[3])]
			scene._sas_follow = ""
			_hold_aim(s, t)
		"stage":
			scene._stage()
		"sas":
			scene._sas_follow = ""
			_let_go_aim()
			scene._on_sas(arg)
		"point":
			# pitch above the local horizon, heading clockwise from north
			var p := deg_to_rad(float(w[1]))
			var h := deg_to_rad(float(w[2]))
			var up: Array = t.up
			var north: Array = t.north
			var east: Array = t.east
			var d := PackedFloat64Array()
			for k in 3:
				d.append(float(up[k]) * sin(p) + (float(north[k]) * cos(h) + float(east[k]) * sin(h)) * cos(p))
			scene._sas_follow = ""
			s.set_sas("target", d)
			aim_word = ""
			aim_fso = []
			aim_ph = [float(w[1]), float(w[2])]
			aim_vec = Vector3(d[0], d[1], d[2])
		"rcs":
			if _on(arg) != bool(t.get("rcs", false)):
				scene._toggle_rcs()
		"chutes":
			scene._deploy_chutes()
		"legs":
			if arg == "":
				scene._toggle_legs()
			else:
				s.set_legs(_on(arg))
		"brakes":
			if _on(arg) != bool(t.get("brakes", false)):
				scene._toggle_brakes()
		"spin":
			if _on(arg) != (float(t.get("spin_hold", 0.0)) != 0.0):
				scene._toggle_spin()
		"drive":
			s.core.vessel_set_drive(s.active, clampf(float(arg), 0.0, 1.0))
		"mining":
			s.core.vessel_set_mining(s.active, _on(arg))
		"say":
			_said.append(line.substr(4))
		"beep":
			Sfx.play({"alarm": "alarm", "chime": "chime"}.get(arg, "click"))
		"autostage":
			programs.autostage = _on(arg)
		"abort":
			programs.stop("aborted")
			scene._throttle = 0.0
			s.set_throttle(0.0)
		"target":
			var want := line.substr(7).strip_edges().to_lower()
			s.target = -1
			if want != "none" and want != "":
				for v in JSON.parse_string(s.core.vessel_list()):
					if int(v.id) != s.active and str(v.name).to_lower().begins_with(want):
						s.target = int(v.id)
						break
		_ when w[0] in PROGRAMS:
			# burn(dv) along the nose is aiming and burning, the computer's
			# from the start; the programs that fly themselves are the
			# Guidance Computer's (a career researches it)
			var why := "" if w[0] == "burn" and w.size() > 1 and w[1] == "here" else Career.computer_refusal("guidance")
			if why != "":
				programs.refuse(w[0], why)
			else:
				programs.start(scene, w, t)
	# (what a script says every frame -- a throttle, an aim, a reading set --
	# is the ship's to show, not the log's: it would push out what matters)
	if not w[0] in ["throttle", "point", "aim", "aim_along", "set"]:
		_note("> " + line)


func _let_go_aim() -> void:
	aim_word = ""
	aim_vec = Vector3.ZERO
	aim_ph = []
	aim_fso = []


## Every engine back on the ship's throttle (the keys, a program, throttle()).
static func engines_follow(s: FlightSession) -> void:
	for p in get_shared()._parts:
		if p.has("throttle") and float(p.throttle) >= 0.0 and str(p.kind) == "engine":
			s.core.part_action(s.active, int(p.uid), "throttle", -1.0)
			p.throttle = -1.0


## `set part.12.throttle 100`: a script set one of a thing's readings.
func _set_reading(scene: Node, t: Dictionary, w: PackedStringArray) -> void:
	if w.size() < 3:
		return
	var s: FlightSession = scene.session
	var path := w[1].split(".")
	var v := float(w[2])
	if path.size() == 1 and path[0] == "throttle":
		scene._throttle = clampf(v / 100.0, 0.0, 1.0)
		s.set_throttle(scene._throttle)
		engines_follow(s)
	elif path.size() == 3 and path[0] == "part" and path[2] == "throttle":
		var thr := clampf(v / 100.0, 0.0, 1.0)
		var was := int(t.get("engines_lit", 0))
		JSON.parse_string(s.core.part_action(s.active, int(path[1]), "throttle", thr))
		if int(s.telemetry().get("engines_lit", 0)) > was:
			Sfx.play("ignite")
	s._telemetry_tick = -1


## `call part.12.decouple`: a script told a part to do something.
func _call_part(scene: Node, t: Dictionary, w: PackedStringArray) -> void:
	var s: FlightSession = scene.session
	var path := w[1].split(".") if w.size() > 1 else PackedStringArray()
	if path.size() != 3 or path[0] != "part":
		return
	var what := path[2]
	var value := 0.0
	match what:
		"retract": what = "deploy"
		"deploy": value = 1.0
	var was := int(t.get("engines_lit", 0))
	var r = JSON.parse_string(s.core.part_action(s.active, int(path[1]), what, value))
	s._telemetry_tick = -1
	if r is Dictionary and r.get("ok", false):
		if not (r.get("new", []) as Array).is_empty():
			Sfx.play("stage")
		elif int(s.telemetry().get("engines_lit", 0)) > was:
			Sfx.play("ignite")
		else:
			Sfx.play("click")
	else:
		_note("%s: it can't %s now" % [_names.get(int(path[1]), "that part"), path[2]])


## `ask <id> leave krill` / `ask <id> whatif <pro> <nrm> <rad> <at>`: worked
## out now, the answer left in the table as answer.<id>.* for the script.
func _ask(scene: Node, t: Dictionary, w: PackedStringArray) -> void:
	if w.size() < 3:
		return
	var ans := {}
	var why := Career.computer_refusal("planner")
	match w[2] if why == "" else "":
		"":
			ans = {"error": why}
		"leave":
			ans = programs.plan_leave(scene, t, w[3] if w.size() > 3 else "", float(w[4]) if w.size() > 4 else -1.0)
		"whatif":
			if w.size() >= 7:
				ans = programs.what_if(scene, t, [float(w[6]), float(w[3]), float(w[4]), float(w[5])])
	for k in ans.keys():
		if ans[k] is float or ans[k] is int:
			ans[k] = _n(ans[k])
		else:
			ans[k] = str(ans[k]).replace("\t", " ").replace("\n", " ")
	ans["done"] = "true"
	_answers[w[1]] = [_frame, ans]


## Your scripts and notes, kept between runs of the game.
func save() -> void:
	if machine == null:
		return
	DirAccess.make_dir_recursive_absolute(SAVE_DIR)
	var files := {}
	for p in KEEP:
		_collect(p, files)
	var f := FileAccess.open(SAVE_DIR + "files.json", FileAccess.WRITE)
	if f:
		f.store_string(JSON.stringify(files))


func _collect(path: String, out: Dictionary) -> void:
	var names: String = machine.list_dir(path)
	if names == "":
		# a file (or nothing)
		var body: String = machine.read_file(path)
		if body != "":
			out[path] = body
		return
	for n in names.split("\n", false):
		_collect(path.trim_suffix("/") + "/" + n, out)


func _restore() -> void:
	if not FileAccess.file_exists(SAVE_DIR + "files.json"):
		return
	var d = JSON.parse_string(FileAccess.get_file_as_string(SAVE_DIR + "files.json"))
	if d is Dictionary:
		for p in d:
			machine.write_file(p, str(d[p]))
