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
const KEEP := ["/root/scripts", "/root/notes.txt", "/root/Documents"]

## name, argument kinds (n number, w word, t text; ? = optional), what it does
const VERBS := [
	["throttle", "n", "the throttle, 0 to 1 (Z is 1, X is 0)"],
	["stage", "", "fires the next stage (Space)"],
	["sas", "w", "SAS: off, stability, prograde, retrograde, normal, antinormal, radial_out, radial_in, target"],
	["point", "nn", "points the nose: pitch above the horizon, heading from north (degrees)"],
	["rcs", "w", "RCS on or off (R)"],
	["chutes", "", "arms the parachutes (P)"],
	["legs", "", "landing legs out / in (G)"],
	["brakes", "w", "wheel brakes on or off (B)"],
	["spin", "w", "spin hold on or off (Home)"],
	["drive", "n", "the Stillwater Drive's throttle, 0 to 1"],
	["mining", "w", "drills and refineries on or off (C)"],
	["say", "t", "puts a line on the flight screen"],
	["autostage", "w", "on: the programs stage when a stage is spent (the default); off: never"],
	["target", "t", "picks a vessel to target, by name (none: no target)"],
	["abort", "", "stops the program running and closes the throttle"],
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
var log: Array[String] = []   # what scripts have printed, and what was done, newest last
var _verbs_tsv := ""
var _said: Array[String] = []  # say lines waiting for the flight screen
var _frame := 0
var programs := FlightPrograms.new()
var _enc := {}     # the path ahead, looked at now and then: encounter, encounter_pe, time_to_soi
var _enc_t := -1e9


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
	for k in r:
		rows.append("%s\t%s" % [k, str(r[k])])
	machine.set_state("\n".join(rows) + "\n")
	machine.tick(SLICES)
	var said: String = machine.take_output()
	for line in said.split("\n", false):
		if not line.begins_with("kernel: script died -- exited with status 0"):
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


## Runs one line on the machine's shell, as a person at a terminal would.
func sh(line: String) -> String:
	return str(machine.sh(line)) if machine else "the computer is off\n"


func take_said() -> Array[String]:
	var out := _said.duplicate()
	_said.clear()
	return out


func _note(line: String) -> void:
	log.append(line)
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
		"stage":
			scene._stage()
		"sas":
			scene._sas_follow = ""
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
		"rcs":
			if _on(arg) != bool(t.get("rcs", false)):
				scene._toggle_rcs()
		"chutes":
			scene._deploy_chutes()
		"legs":
			scene._toggle_legs()
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
			programs.start(scene, w, t)
	_note("> " + line)


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
