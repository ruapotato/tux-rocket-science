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
## The ship's programs: the things a flight computer flies for you, a frame
## at a time, through the same controls the keys use. The ship's computer
## starts one (`ship launch 80000`, or launch(80000) in a script) and reads
## how it is going (program, program_status, program_note); one at a time,
## and any key that flies the ship takes the controls back.
##
##   launch [apoapsis]          pad (or any ground) to orbit, staging itself
##   circularize [apoapsis|periapsis]
##   burn <dir> <dv>            a delta-v along prograde, normal, radial_out...
##   burn <dir> until <reading> <op> <value>
##                              (until encounter == krill: until the path
##                              meets it)
##   transfer <body> [pe]       plans the burn to a moon or planet and flies it
##   window <body>              warps to where that burn would start
##   warp <seconds|apoapsis|periapsis|soi|node|body>
##   capture [apoapsis]         at the periapsis, into an orbit of the body
##   land                       down onto the ground below, wherever it is
##   land <lat> <lon>           down at that place (from orbit, or a hop
##   land near <relic|landmark> from the ground): the orbit turned to pass
##                              over it, the burn down timed to come down
##                              by it, and the last of the way flown low
##   hover <height> [seconds]   climbs or sinks to a height above the ground
##                              and holds it (seconds: counted from there)
##   node                       flies the next maneuver node on the map
##   drive_to <body> [altitude] on the Stillwater Drive (inside a star's
##                              heliopause, 1 g, warp and all) to a world of
##                              this star and into a circular orbit round it
class_name FlightPrograms
extends RefCounted

const LEAD := 30.0          # s: a warp before a burn stops this long before it
const STAGE_COOL := 1.0     # s between two automatic stagings

var name := ""              # the program running (or last run)
var status := "idle"        # idle, running, done, failed
var note := ""              # what it is doing, or what it did, or why it stopped
var phase := ""
var id := 0                 # counts programs started (a script tells its own from the last)
var autostage := true       # stage when the stage being burned is spent

var _scene: Node
var _s: FlightSession
var _api: ControlAPI
var _ascent: AscentAutopilot
var _a := {}                # the program's own working state
var _cool := 0.0
var _aim := Vector3.ZERO     # where the program last had the nose pointed (zero: anywhere)
var _flat_t := 0.0          # s it has wanted to turn with nothing to turn the ship
var _shield_held := false   # the stage burning ran dry, and the next would drop a heat shield
var _last_t := -1.0
var _bodies := {}           # body index -> body_info
var said: Array[String] = [] # what the programs have to tell the console, newest last


func _say(line: String) -> void:
	said.append("%s: %s" % [name, line])


func running() -> bool:
	return status == "running"


# ---- starting and stopping --------------------------------------------------

## Starts the program `words` asks for ("launch", "80000"). A refusal is a
## program that failed at once: the script hears why the same way.
func start(scene: Node, words: PackedStringArray, t: Dictionary) -> void:
	if running():
		_end("failed", "%s was stopped for %s" % [name, " ".join(words)])
	_scene = scene
	_s = scene.session
	if _api == null or _api.session != _s:
		_api = ControlAPI.new(_s)
	id += 1
	name = words[0]
	status = "running"
	note = ""
	phase = ""
	_a = {}
	_ascent = null
	_aim = Vector3.ZERO
	_flat_t = 0.0
	_shield_held = false
	_last_t = float(t.get("t", 0.0))
	_scene._sas_follow = ""
	ShipComputer.engines_follow(_s)  # a program flies the ship's throttle: every engine follows it
	var why := ""
	match name:
		"launch": why = _begin_launch(words, t)
		"circularize": why = _begin_circularize(words, t)
		"burn": why = _begin_burn(words, t)
		"transfer", "window": why = _begin_transfer(words, t)
		"warp": why = _do_warp(words, t)
		"capture": why = _begin_capture(words, t)
		"land": why = _begin_land(words, t)
		"hover": why = _begin_hover(words, t)
		"node": why = _begin_node(t)
		"drive_to": why = _begin_drive(words, t)
		_: why = "no such program: " + name
	if why != "":
		_end("failed", why)


## WORKED OUT, NOT FLOWN: what a script's when_to_leave("krill") asks. The
## burn to that body from this orbit: {wait (s until it), at (when), dv,
## prograde, normal, radial (m/s), travel (s from the burn into its
## sphere), closest (m up as it passes, as planned), body}, or {error}.
func plan_leave(scene: Node, t: Dictionary, body: String, want := -1.0) -> Dictionary:
	_scene = scene
	_s = scene.session
	var to := _body_named(body)
	if to < 0:
		return {"error": "no body called '%s'" % body}
	var here := int(t.get("body_index", -1))
	var info: Dictionary = JSON.parse_string(_s.core.body_info(to))
	if to == here:
		return {"error": "the ship is already at %s" % info.name}
	var o: Dictionary = t.get("orbit", {})
	if t.get("landed", false) or o.get("ap_alt") == null:
		return {"error": "a transfer starts from a closed orbit (get into one first)"}
	if want < 0.0:
		want = float(info.atmosphere_height) + 40000.0 if float(info.get("atmosphere_height", 0.0)) > 0.0 else maxf(20000.0, float(info.radius) * 0.15)
	var moon := int(info.parent) == here
	var home := int(_body(t).get("parent", -1)) == to
	var node := []
	if moon:
		node = _plan_moon(t, str(info.id), want)
	elif home:
		node = _plan_home(t, str(info.id), float(info.atmosphere_height) * 0.5 if float(info.get("atmosphere_height", 0.0)) > 0.0 else want)
	else:
		var r: PackedFloat64Array = _s.core.plan_transfer(_s.active, to, want, false)
		if r.size() >= 6 and r[4] > 0.5:
			node = [r[0], r[1], r[2], r[3]]
	if node.is_empty():
		return {"error": "no way to %s found from this orbit" % info.name}
	var pp := _patch_in(node, str(info.id), 6.0e5 if moon else 1.0e8)
	return {"body": str(info.name), "wait": float(node[0]) - float(t.t), "at": float(node[0]),
		"dv": Vector3(node[1], node[2], node[3]).length(), "prograde": node[1], "normal": node[2], "radial": node[3],
		"travel": float(pp.get("t_start", node[0])) - float(node[0]), "closest": float(pp.get("pe_alt", -1.0))}


## WORKED OUT, NOT FLOWN: what_if(prograde, normal, radial, at). The path as
## if that burn were made then: {top, bottom (m, the orbit after it; top
## -1 when it escapes), meets (the next body whose sphere it enters, or
## none), closest (m up at that body), arrive (s from now into its sphere)}.
func what_if(scene: Node, t: Dictionary, node: Array) -> Dictionary:
	_scene = scene
	_s = scene.session
	var out := {"top": -1.0, "bottom": -1.0, "meets": "none", "closest": -1.0, "arrive": -1.0}
	var patches: Array = _s.trajectory([node], 6, 6.0e6)
	var after := -1
	for i in patches.size():
		if float(patches[i].t_end) > float(node[0]) + 0.5:
			after = i
			break
	if after < 0:
		return out
	var p0: Dictionary = patches[after]
	out.top = float(p0.ap_alt) if p0.get("ap_alt") != null else -1.0
	out.bottom = float(p0.pe_alt)
	for i in range(after + 1, patches.size()):
		if str(patches[i - 1].get("end", "")) == "soi_enter":
			var b: Dictionary = JSON.parse_string(_s.core.body_info(int(patches[i].body_index)))
			out.meets = str(b.get("name", patches[i].body))
			out.closest = float(patches[i].pe_alt)
			out.arrive = float(patches[i].t_start) - float(t.t)
			break
	return out


## A program the computer can't fly yet (why: in words): it fails at once,
## as one that couldn't start does, so a waiting script hears why.
func refuse(what: String, why: String) -> void:
	if running():
		_end("failed", "%s was stopped for %s" % [name, what])
	id += 1
	name = what
	status = "failed"
	note = why
	phase = ""


## Stops what is running (the throttle closed): `abort`, or a hand on the keys.
func stop(why: String) -> void:
	if running():
		_thr(0.0)
		_end("failed", why)


func _end(how: String, what: String) -> void:
	status = how
	note = what
	phase = ""
	if _s != null:
		_thr(0.0)


func _done(what: String) -> void:
	_end("done", what)


func _fail(what: String) -> void:
	# (dry with propellant still aboard: the engines are what's gone)
	if what.begins_with("out of propellant") and not _t_now.is_empty() and float(_t_now.get("liquid", 0.0)) > 1.0 \
			and float(_t_now.get("thrust_max", 0.0)) <= 0.0 and _engines_left(_t_now) == 0:
		what = "no engine left" + what.substr(len("out of propellant"))
	_end("failed", what)


var _t_now := {}  # this frame's telemetry


func _engines_left(_t: Dictionary) -> int:
	var n := 0
	for ps in JSON.parse_string(_s.core.vessel_part_states(_s.active)):
		n += 1 if str(ps.get("kind", "")) == "engine" else 0
	return n


# ---- each frame ---------------------------------------------------------------

func step(scene: Node, t: Dictionary) -> void:
	if not running():
		return
	_scene = scene
	_s = scene.session
	_t_now = t
	if not t.get("alive", false):
		_fail("the ship was lost")
		return
	var now := float(t.t)
	var dt := maxf(now - _last_t, 0.0)
	_last_t = now
	_cool -= dt
	# the stage under a heat shield spent, and the lander's engine above it
	# not to be fired: nothing can make this burn (except the land program,
	# which drops the shield through the air)
	if _shield_held and float(t.get("thrust_max", 0.0)) <= 0.0 and name != "land":
		var o: Dictionary = t.get("orbit", {})
		if name == "capture" and o.get("ap_alt") != null:
			_done("in orbit of %s, %s (the stage under the heat shield is spent)" % [_body(t).get("name", "?"), _orbit_text(t)])
		else:
			_fail("the stage under the heat shield is spent: the lander's engine can't fire with the shield still under it")
		return
	# nothing to turn the ship with: a wait to point the way would be for ever
	if _aim != Vector3.ZERO and not _aligned(t, _aim, 10.0) and not t.get("powered", true) and float(t.get("ec_capacity", 0.0)) > 0.0 \
			and float(t.get("thrust", 0.0)) <= 0.0 and not (t.get("rcs", false) and float(t.get("mono", 0.0)) > 0.0):
		_flat_t += dt
		if _flat_t > 30.0:
			_fail("the batteries are flat: nothing can turn the ship (a solar panel, or a bigger battery, keeps the wheels going)")
			return
	else:
		_flat_t = 0.0
	match name:
		"launch": _launch(t, dt)
		"circularize": _circularize(t)
		"burn": _burn(t, dt)
		"transfer", "window": _transfer(t)
		"capture": _capture(t)
		"warp": _warp(t)
		"land": _land(t)
		"hover": _hover(t)
		"node": _node(t)
		"drive_to": _drive(t)


# ---- controls -------------------------------------------------------------------

func _thr(x: float) -> void:
	_scene._throttle = x
	_s.set_throttle(x)
	# (the programs fly the engines: a Stillwater Drive aboard, following
	# the throttle, would push on top of them, unplanned; drive_to has it)
	_s.core.vessel_set_drive(_s.active, 0.0)


func _point(d: Vector3) -> void:
	var n := d.normalized()
	_aim = n
	_scene._sas_follow = ""
	_s.set_sas("target", PackedFloat64Array([n.x, n.y, n.z]))


func _sas(mode: String) -> void:
	_aim = Vector3.ZERO if mode == "stability" else dir_of(_s.telemetry(), mode)
	_scene._sas_follow = ""
	_s.set_sas(mode)


static func v3(a) -> Vector3:
	return Vector3(float(a[0]), float(a[1]), float(a[2])) if a is Array and a.size() == 3 else Vector3.ZERO


func _aligned(t: Dictionary, d: Vector3, deg := 4.0) -> bool:
	return d != Vector3.ZERO and v3(t.nose).angle_to(d) < deg_to_rad(deg)


func _accel(t: Dictionary) -> float:
	return float(t.get("thrust_max", 0.0)) / maxf(float(t.mass), 1.0)


func _body(t: Dictionary) -> Dictionary:
	var i := int(t.get("body_index", -1))
	if not _bodies.has(i):
		_bodies[i] = JSON.parse_string(_s.core.body_info(i)) if i >= 0 else {}
	return _bodies[i]


func _body_named(word: String) -> int:
	var w := word.to_lower()
	var core = _s.core
	var i: int = core.body_find(w)
	if i >= 0:
		return i
	for k in range(64):
		var info = JSON.parse_string(core.body_info(k))
		if not info is Dictionary or info.is_empty():
			break
		if str(info.get("name", "")).to_lower() == w:
			return k
	return -1


## A direction by name, in the frame SAS targets use.
static func dir_of(t: Dictionary, word: String) -> Vector3:
	var up := v3(t.up)
	var v := v3(t.velocity)
	var sv := v3(t.surface_velocity)
	match word:
		"prograde": return v.normalized()
		"retrograde": return -v.normalized()
		"normal": return up.cross(v).normalized()
		"antinormal": return -up.cross(v).normalized()
		"radial_out": return (up - v.normalized() * up.dot(v.normalized())).normalized()
		"radial_in": return -(up - v.normalized() * up.dot(v.normalized())).normalized()
		"surface_prograde": return sv.normalized()
		"surface_retrograde": return -sv.normalized()
		"up": return up
		"here": return v3(t.nose)
	return Vector3.ZERO


const DIRS := ["prograde", "retrograde", "normal", "antinormal", "radial_out", "radial_in", "surface_prograde",
	"surface_retrograde", "up", "here"]


## The stage being burned is spent: drop it and light the next, but never
## the press that only drops things with no engine after it (the crew
## pod's own decoupler, kept for coming home). True when it staged.
func _autostage(t: Dictionary) -> bool:
	if not autostage or _cool > 0.0 or int(t.get("next_stage", -1)) < 0 or float(t.get("throttle", 0.0)) <= 0.0:
		return false
	var stages: Array = t.get("stages", [])
	var spent: bool = float(t.get("thrust_max", 0.0)) <= 0.0 or stages.is_empty() or float(stages[0].get("dv_vac", 0.0)) < 0.5
	if not spent:
		return false
	var lights_later := false
	for p in t.get("stage_plan", []):
		if int(p.stage) <= int(t.next_stage) and int(p.engines) > 0:
			lights_later = true
	if not lights_later:
		return false
	if FlightPrograms.fires_into_shield(_s.core, _s.active, int(t.next_stage)):
		return false  # (its jet would burn through what holds the shield: the land program drops that first)
	if FlightPrograms.shield_press(_s.core, _s.active, int(t.next_stage), false) and not _a.get("no_air_path", false):
		_shield_held = float(t.get("thrust_max", 0.0)) <= 0.0
		return false  # (the heat shield goes when the land program says, through the air: never as a spent stage)
	_scene._stage()
	_cool = STAGE_COOL
	return true


## Nothing left to burn and nothing to stage onto.
func _dry(t: Dictionary) -> bool:
	if float(t.get("thrust_max", 0.0)) > 0.0:
		return false
	for p in t.get("stage_plan", []):
		if int(p.stage) <= int(t.get("next_stage", -1)) and int(p.engines) > 0:
			return false
	return true


func _warp_until(t_target: float) -> String:
	if t_target <= _s.time() + 1.0:
		return ""
	return _s.warp_to(t_target)


static func km(m: float) -> String:
	return "%.1f km" % (m / 1000.0)


static func span(s: float) -> String:
	s = absf(s)
	if s < 120.0:
		return "%d s" % roundi(s)
	if s < 7200.0:
		return "%d min" % roundi(s / 60.0)
	if s < 172800.0:
		return "%.1f h" % (s / 3600.0)
	return "%.1f days" % (s / 86400.0)


func _orbit_text(t: Dictionary) -> String:
	var o: Dictionary = t.get("orbit", {})
	return "%s x %s" % [km(float(o.get("pe_alt", 0.0))), km(float(o.ap_alt)) if o.get("ap_alt") != null else "escape"]


# ---- launch ------------------------------------------------------------------------

func _begin_launch(w: PackedStringArray, t: Dictionary) -> String:
	# (a ship on its legs is on the ground, if they bounce it a hair off it now and then)
	var resting: bool = (int(t.get("contacts", 0)) > 0 or float(t.get("altitude_terrain", 1e9)) < 30.0) and float(t.get("surface_speed", 99.0)) < 5.0
	if not (t.get("landed", false) or resting or str(t.situation) in ["prelaunch", "landed", "splashed"]):
		return "launch is from the ground (the ship is %s, %.0f m up at %.1f m/s, %d contacts)" % [t.situation, float(t.get("altitude_terrain", -1.0)),
			float(t.get("surface_speed", -1.0)), int(t.get("contacts", -1))]
	var b := _body(t)
	var atm := float(b.get("atmosphere_height", 0.0))
	var ap := float(w[1]) if w.size() > 1 else (maxf(80000.0, atm + 10000.0) if atm > 0.0 else maxf(15000.0, float(b.radius) * 0.1))
	if atm > 0.0 and ap < atm + 6000.0:
		return "an orbit at %s is inside %s's air (it goes up to %s)" % [km(ap), b.name, km(atm)]
	_a.ap = ap
	_a.atm = atm
	if atm > 0.0:
		_ascent = AscentAutopilot.new(_api)
		_ascent.target_ap = ap + 2000.0
		_ascent.target_pe = maxf(ap - 4000.0, atm + 3000.0)
		note = "to a %s orbit of %s" % [km(ap), b.name]
	else:
		_thr(1.0)
		_point(v3(t.up))
		if str(t.situation) == "prelaunch" or float(t.get("thrust_max", 0.0)) <= 0.0:
			_scene._stage()
			_cool = STAGE_COOL
		phase = "rise"
		note = "straight up, then east to a %s orbit" % km(ap)
	return ""


func _launch(t: Dictionary, dt: float) -> void:
	if _ascent != null:
		var finished := _ascent.step(maxf(dt, 0.02))
		_scene._throttle = _s.throttle_set
		phase = _ascent.phase
		if finished:
			if _ascent.phase == "done":
				_done("in orbit, " + _orbit_text(t))
			else:
				_fail("%s at %s (%s)" % [_ascent.phase.replace("_", " "), km(float(t.altitude)), _orbit_text(t)])
		return
	_airless_launch(t)


## No air: clear the ground, then put nearly all the thrust into orbital
## speed, holding a climb toward the target height (Krill's way off).
func _airless_launch(t: Dictionary) -> void:
	var o: Dictionary = t.get("orbit", {})
	var ap: float = _a.ap
	var b := _body(t)
	var g := float(b.get("surface_gravity", 1.0))
	if phase in ["rise", "climb"] and _dry(t):
		_fail("out of propellant climbing, %s" % _orbit_text(t))
		return
	_autostage(t)
	var up := v3(t.up)
	match phase:
		"rise", "climb":
			if o.get("ap_alt") != null and float(o.ap_alt) > ap:
				_sas("prograde")
				_thr(0.0)
				phase = "coast"
				return
			var accel := _accel(t)
			var dir := up
			if float(t.altitude_terrain) > 250.0 and accel > 0.0:
				phase = "climb"
				var r := float(t.altitude) + float(b.radius)
				var vs := float(t.vertical_speed)
				var vh := v3(t.velocity) - up * vs
				var vs_t := clampf((ap - float(t.altitude)) / 25.0, -15.0, 60.0)
				var a_up := g - vh.length_squared() / r + 0.8 * (vs_t - vs)
				var s_up := clampf(a_up / accel, 0.0, 0.97)
				var fwd: Vector3 = vh.normalized() if vh.length() > 20.0 else v3(t.east)
				dir = fwd * sqrt(1.0 - s_up * s_up) + up * s_up
			_point(dir)
			_thr(1.0)
		"coast":
			_sas("prograde")
			if o.get("t_to_ap") == null or float(o.t_to_ap) < 15.0 or float(t.vertical_speed) < 0.0:
				phase = "circularize"
		"circularize":
			if float(o.get("pe_alt", 0.0)) > ap * 0.75:
				_done("in orbit, " + _orbit_text(t))
				return
			if _dry(t):
				_fail("out of propellant rounding the orbit, " + _orbit_text(t))
				return
			var vel := v3(t.velocity)
			var horiz := (vel - up * vel.dot(up)).normalized()
			var lift := clampf(-float(t.vertical_speed) * 0.03, -0.2, 0.5)
			_point(horiz * cos(lift) + up * sin(lift))
			_thr(1.0)


# ---- circularize ----------------------------------------------------------------

func _begin_circularize(w: PackedStringArray, t: Dictionary) -> String:
	var at := w[1] if w.size() > 1 else "apoapsis"
	if at in ["ap", "apo"]:
		at = "apoapsis"
	if at in ["pe", "peri"]:
		at = "periapsis"
	if not at in ["apoapsis", "periapsis"]:
		return "circularize at the apoapsis or the periapsis, not '%s'" % at
	var o: Dictionary = t.get("orbit", {})
	if at == "apoapsis" and o.get("ap_alt") == null:
		return "on an escape path there is no apoapsis to round off at"
	if t.get("landed", false):
		return "the ship is on the ground"
	_a.at = at
	phase = "warp"
	note = "at the %s" % at
	return ""


## What is still to do, in m/s: the speed of a circle here against the
## speed along the horizon now (positive: speed up).
func _circ_need(t: Dictionary) -> float:
	var b := _body(t)
	var up := v3(t.up)
	var vel := v3(t.velocity)
	var r := float(t.altitude) + float(b.radius)
	var vh := (vel - up * vel.dot(up)).length()
	return sqrt(float(b.mu) / r) - vh


func _circularize(t: Dictionary) -> void:
	var o: Dictionary = t.get("orbit", {})
	var apo: bool = _a.at == "apoapsis"
	var up := v3(t.up)
	var vel := v3(t.velocity)
	var horiz := (vel - up * vel.dot(up)).normalized()
	var need := _circ_need(t)
	match phase:
		"warp":
			var bt := maxf(float(_s.core.burn_time(_s.active, absf(need))), 0.0)
			_a.bt = bt
			var tt = o.get("t_to_ap") if apo else o.get("t_to_pe")
			if tt != null:
				var stopped := _warp_until(float(t.t) + float(tt) - bt * 0.5 - LEAD)
				if stopped != "" and stopped.begins_with("refused"):
					pass  # (not allowed to warp here: wait it out)
			phase = "align"
		"align":
			var d := horiz if need > 0.0 else -horiz
			_point(d)
			var tt = o.get("t_to_ap") if apo else o.get("t_to_pe")
			var close: bool = tt == null or float(tt) < float(_a.get("bt", 0.0)) * 0.5 + 1.0 or (apo and float(t.vertical_speed) < 0.0) or (not apo and float(t.vertical_speed) > 0.0)
			if close:
				phase = "burn"
		"burn":
			if absf(need) < 0.4 or (apo and float(o.get("pe_alt", 0.0)) > float(t.altitude) * 0.995 and need < 2.0):
				_done("round at %s (%s)" % [km(float(t.altitude)), _orbit_text(t)])
				return
			if _dry(t):
				_fail("out of propellant %d m/s short (%s)" % [roundi(absf(need)), _orbit_text(t)])
				return
			_autostage(t)
			var lift := clampf(-float(t.vertical_speed) * 0.02, -0.15, 0.3)
			var d := (horiz if need > 0.0 else -horiz) * cos(lift) + up * sin(lift)
			_point(d)
			var accel := _accel(t)
			_thr(clampf(absf(need) / maxf(accel * 1.5, 0.01), 0.02, 1.0) if _aligned(t, d, 8.0) else 0.0)


# ---- burn ------------------------------------------------------------------------------

const OPS := [">", ">=", "<", "<=", "==", "!="]


func _begin_burn(w: PackedStringArray, t: Dictionary) -> String:
	if w.size() < 3:
		return "burn <direction> <m/s>, or burn <direction> until <reading> <comparison> <value>"
	var dir := w[1]
	if not dir in DIRS:
		return "'%s' is not a direction: %s" % [dir, ", ".join(DIRS)]
	_a.dir = dir
	if w[2] == "until":
		if w.size() < 6:
			return "burn %s until <reading> <comparison> <value> (until apoapsis > 2000000)" % dir
		if not w[4] in OPS:
			return "'%s' is not a comparison: %s" % [w[4], " ".join(OPS)]
		var r := ShipComputer.readings(t, self, _s)
		if not r.has(w[3]):
			return "no such reading: %s (ship status lists them)" % w[3]
		_a.key = w[3]
		_a.op = w[4]
		_a.want = " ".join(w.slice(5)).to_lower()
		note = "%s until %s %s %s" % [dir, w[3], w[4], _a.want]
	else:
		if not w[2].is_valid_float():
			return "burn %s wants m/s, not '%s'" % [dir, w[2]]
		_a.dv = float(w[2])
		note = "%d m/s %s" % [roundi(_a.dv), dir]
	_a.start = float(t.get("dv_expended", 0.0))
	_a.t_align = float(t.t)
	phase = "align"
	return ""


func _met(t: Dictionary) -> bool:
	var r := ShipComputer.readings(t, self, _s, _a.key == "encounter")
	var got: String = str(r.get(_a.key, "")).to_lower()
	var want: String = _a.want
	var c := 0
	if got.is_valid_float() and want.is_valid_float():
		c = signi(int(signf(float(got) - float(want))))
		_a.rate_v = float(got)
	else:
		c = 0 if got == want else (-1 if got < want else 1)
	match _a.op:
		">": return c > 0
		">=": return c >= 0
		"<": return c < 0
		"<=": return c <= 0
		"==": return c == 0
		"!=": return c != 0
	return false


func _burn(t: Dictionary, dt: float) -> void:
	var d := dir_of(t, _a.dir)
	if _a.dir == "here":
		pass  # where the script pointed it (aim, aim_at), held as it was
	elif _a.dir in ["prograde", "retrograde", "normal", "antinormal", "radial_out", "radial_in"]:
		_sas(_a.dir)
	else:
		_point(d)
	match phase:
		"align":
			_thr(0.0)
			if _aligned(t, d) or float(t.t) - float(_a.t_align) > 30.0:
				phase = "burn"
		"trim":
			var r := ShipComputer.readings(t, self, _s, true)
			var pe := float(r.get("encounter_pe", "-1"))
			var b: Dictionary = JSON.parse_string(_s.core.body_info(_body_named(_a.want)))
			var floor_pe := float(b.get("atmosphere_height", 0.0)) + maxf(15000.0, float(b.get("radius", 0.0)) * 0.1)
			if str(r.get("encounter", "")) != _a.want or pe > float(_a.pass_pe) or pe < floor_pe \
					or float(t.get("dv_expended", 0.0)) - float(_a.trim_from) > 40.0:
				_done("the path meets %s, passing %s up (%s)" % [str(b.get("name", _a.want)), km(float(_a.pass_pe)), _orbit_text(t)])
				return
			_a.pass_pe = pe
			_thr(0.1)
		"burn":
			if _a.has("dv"):
				var left: float = float(_a.dv) - (float(t.get("dv_expended", 0.0)) - float(_a.start))
				if left <= 0.05:
					_done("%d m/s %s burnt (%s)" % [roundi(_a.dv), _a.dir, _orbit_text(t)])
					return
				if _dry(t):
					_fail("out of propellant, %d m/s short" % roundi(left))
					return
				_autostage(t)
				_thr(clampf(left / maxf(_accel(t) * 1.5, 0.01), 0.02, 1.0))
			else:
				var before = _a.get("rate_v")
				if _met(t):
					if _a.key == "encounter" and _a.op == "==":
						# met at the edge of its sphere: a little more, while
						# that takes the pass deeper
						_a.pass_pe = float(ShipComputer.readings(t, self, _s).get("encounter_pe", "-1"))
						_a.trim_from = float(t.get("dv_expended", 0.0))
						phase = "trim"
						_thr(0.1)
						return
					_done("%s %s %s: %s" % [_a.key, _a.op, _a.want, _orbit_text(t)])
					return
				if _dry(t):
					_fail("out of propellant before %s %s %s" % [_a.key, _a.op, _a.want])
					return
				_autostage(t)
				# ease off as a number closes on what is wanted (2 s out)
				var thr := 1.0
				var now = _a.get("rate_v")
				if before != null and now != null and dt > 0.0 and str(_a.want).is_valid_float():
					var rate: float = (float(now) - float(before)) / dt
					var gap := float(_a.want) - float(now)
					if rate != 0.0 and signf(rate) == signf(gap):
						thr = clampf(gap / rate / 2.0, 0.03, 1.0)
				_thr(thr)


# ---- transfer and window --------------------------------------------------------

func _begin_transfer(w: PackedStringArray, t: Dictionary) -> String:
	if w.size() < 2:
		return "%s to where? %s krill" % [name, name]
	var to := _body_named(w[1])
	if to < 0:
		return "no body called '%s'" % w[1]
	var here := int(t.get("body_index", -1))
	var info: Dictionary = JSON.parse_string(_s.core.body_info(to))
	if to == here:
		return "the ship is already at %s" % info.name
	var o: Dictionary = t.get("orbit", {})
	if t.get("landed", false) or o.get("ap_alt") == null or float(o.get("pe_alt", 0.0)) < float(_body(t).get("atmosphere_height", 0.0)):
		return "a transfer starts from a closed orbit above the air (launch first)"
	var want := float(w[2]) if w.size() > 2 and w[2].is_valid_float() else (
		float(info.atmosphere_height) + 40000.0 if float(info.get("atmosphere_height", 0.0)) > 0.0 else maxf(20000.0, float(info.radius) * 0.15))
	_a.to = to
	_a.to_id = str(info.id)
	_a.to_name = str(info.name)
	_a.want = want
	var node := []
	_a.moon = int(info.parent) == here
	_a.home = int(_body(t).get("parent", -1)) == to  # off a moon to the world it goes round
	if _a.home and not (w.size() > 2 and w[2].is_valid_float()):
		want = float(info.atmosphere_height) * 0.5 if float(info.get("atmosphere_height", 0.0)) > 0.0 else want
		_a.want = want
	if _a.moon:
		node = _plan_moon(t, str(info.id), want)
	elif _a.home:
		node = _plan_home(t, str(info.id), want)
	else:
		var r: PackedFloat64Array = _s.core.plan_transfer(_s.active, to, want, false)
		if r.size() >= 6 and r[4] > 0.5:
			node = [r[0], r[1], r[2], r[3]]
	if node.is_empty():
		if _a.moon or _a.home:
			return "no way to %s found from this orbit" % info.name
		# (the search can miss a narrow way: from a little further round, again)
		_a.replans = 0
		phase = "replan"
		note = "looking for the way to %s" % info.name
		return ""
	_a.node = node
	_s.nodes = [node]
	var pp := _patch_in(node, str(info.id), 6.0e5 if _a.moon or _a.home else 1.0e8)
	_say("planned: %d m/s prograde in %s, into %s's sphere %s later, passing %s up" % [roundi(Vector3(node[1], node[2], node[3]).length()),
		span(float(node[0]) - float(t.t)), info.name, span(float(pp.get("t_start", node[0])) - float(node[0])), km(float(pp.get("pe_alt", -1.0)))])
	if name == "window":
		note = "warping to the %s window" % info.name
	else:
		note = "%d m/s prograde in %s to %s" % [roundi(Vector3(node[1], node[2], node[3]).length()), span(float(node[0]) - float(t.t)), info.name]
	phase = "warp"
	return ""


## The patch of a path (with one burn) inside `body`, or {}.
func _patch_in(node: Array, body: String, horizon := 6.0e5) -> Dictionary:
	for p in _s.trajectory([node], 6, horizon):
		if p.body == body:
			return p
	return {}


func _score(p: Dictionary, want: float) -> float:
	if p.is_empty():
		return 1e12
	if float(p.pe_alt) < 8000.0:
		return 1e9 + absf(float(p.pe_alt) - want)
	return absf(float(p.pe_alt) - want)


## A Hohmann burn to a moon of the body the ship orbits, found on the
## patched path: when (round the orbit) and how much (prograde) to pass the
## moon `want` up. MoonMission's search, for any moon.
func _plan_moon(t: Dictionary, body: String, want: float) -> Array:
	var here := _body(t)
	var moon: Dictionary = JSON.parse_string(_s.core.body_info(_s.core.body_find(body)))
	var mu := float(here.mu)
	var r1 := float(t.orbit.sma)
	var r2 := float(moon.sma)
	var at := (r1 + r2) * 0.5
	var dv := sqrt(mu * (2.0 / r1 - 1.0 / at)) - sqrt(mu / r1)
	var period := float(t.orbit.period)
	var best := []
	var best_s := INF
	for k in 120:
		var node := [float(t.t) + 120.0 + k * period / 120.0, dv + 25.0, 0.0, 0.0]
		var sc := _score(_patch_in(node, body), want)
		if sc < best_s:
			best_s = sc
			best = node
	if best.is_empty() or best_s >= 1e12:
		return []
	var steps := [period / 120.0, 4.0]
	for it in 40:
		var improved := false
		for dim in [0, 1]:
			for sgn in [-1.0, 1.0]:
				var cand: Array = best.duplicate()
				cand[dim] += sgn * steps[dim]
				var sc := _score(_patch_in(cand, body), want)
				if sc < best_s:
					best_s = sc
					best = cand
					improved = true
		if not improved:
			steps[0] *= 0.5
			steps[1] *= 0.5
		if best_s < 1500.0:
			break
	return best


## Off a moon to the world it goes round: a prograde burn that leaves the
## moon backwards along its orbit, so the ship falls in to `want` m over
## the world (its air, to come down in).
func _plan_home(t: Dictionary, body: String, want: float) -> Array:
	var moon := _body(t)
	var world: Dictionary = JSON.parse_string(_s.core.body_info(_s.core.body_find(body)))
	# patched conics: the excess speed that drops a moon-distance orbit's
	# low point to `want`, then the burn that leaves the moon with it
	var rk := float(moon.sma)
	var rp := float(world.radius) + want
	var v_ap := sqrt(float(world.mu) * (2.0 / rk - 2.0 / (rk + rp)))
	var v_inf := sqrt(float(world.mu) / rk) - v_ap
	var r0 := float(t.orbit.sma)
	var dv := sqrt(v_inf * v_inf + 2.0 * float(moon.mu) / r0) - sqrt(float(moon.mu) / r0)
	var period := float(t.orbit.period)
	var best := []
	var best_s := INF
	for k in 120:
		var node := [float(t.t) + 120.0 + k * period / 120.0, dv, 0.0, 0.0]
		var sc := _score(_patch_in(node, body), want)
		if sc < best_s:
			best_s = sc
			best = node
	if best.is_empty() or best_s >= 1e12:
		return []
	var steps := [period / 120.0, 5.0]
	for it in 60:
		var improved := false
		for dim in [0, 1]:
			for sgn in [-1.0, 1.0]:
				var cand: Array = best.duplicate()
				cand[dim] += sgn * steps[dim]
				var sc := _score(_patch_in(cand, body), want)
				if sc < best_s:
					best_s = sc
					best = cand
					improved = true
		if not improved:
			steps[0] *= 0.5
			steps[1] *= 0.5
		if best_s < 1000.0:
			break
	return best


## A small three-axis correction a while out, if the pass has drifted.
func _plan_correction(t: Dictionary) -> Array:
	var body: String = _a.to_id
	var want: float = _a.want
	if not (_a.get("moon", false) or _a.get("home", false)):
		# out among the planets: the core's own search, a small burn now
		var r: PackedFloat64Array = _s.core.plan_transfer(_s.active, int(_a.to), want, true)
		var p0 := _patch_in([float(t.t) + 1.0, 0.0, 0.0, 0.0], body, 1.0e8)
		_say("after the burn: %s" % ("passing %s up, %s from now" % [km(float(p0.pe_alt)), span(float(p0.t_start) - float(t.t))] if not p0.is_empty() else "the path misses %s" % _a.to_name))
		if not p0.is_empty() and absf(float(p0.pe_alt) - want) < maxf(5000.0, want * 0.05):
			return []
		if r.size() < 6 or r[4] < 0.5:
			return []
		return [r[0], r[1], r[2], r[3]]
	var base := [float(t.t) + 300.0, 0.0, 0.0, 0.0]
	var best := base
	var best_s := _score(_patch_in(base, body), want)
	var bp := _patch_in(base, body)
	_say("after the burn: %s" % ("passing %s up, %s from now" % [km(float(bp.pe_alt)), span(float(bp.t_start) - float(t.t))] if not bp.is_empty() else "the path misses %s" % _a.to_name))
	if best_s < 5000.0:
		return []
	# (each m/s of correction counts as 30 m of pass height: the nearest fix)
	var cost := func(n: Array) -> float:
		return _score(_patch_in(n, body), want) + 30.0 * Vector3(n[1], n[2], n[3]).length()
	var sv := 10.0
	for it in 100:
		var improved := false
		for dim in [1, 2, 3]:
			for sgn in [-1.0, 1.0]:
				var cand: Array = best.duplicate()
				cand[dim] += sgn * sv
				var sc: float = cost.call(cand)
				if sc < best_s:
					best_s = sc
					best = cand
					improved = true
		if not improved:
			sv *= 0.5
			if sv < 0.02:
				break
	return best if absf(best[1]) + absf(best[2]) + absf(best[3]) > 0.05 else []


## Flies _a.node: warp to it, point along it, burn it out. True when burnt.
func _fly_node(t: Dictionary) -> bool:
	var node: Array = _a.node
	match phase:
		"warp":
			var v: Vector3 = _s.node_vector([node], 0)
			_a.vec = v
			_a.dv = v.length()
			var bt := float(_s.core.burn_time(_s.active, v.length()))
			if bt < 0.0:
				bt = v.length() / maxf(_accel(t), 0.5)
			_a.bt = bt
			_point(v)
			var lead := 20.0 if name == "window" else LEAD
			_warp_until(float(node[0]) - bt * 0.5 - lead)
			phase = "align"
		"align":
			if name == "window":
				# pointing prograde, the burn's start to the frame
				_sas("prograde")
				_thr(0.0)
				if float(t.t) >= float(node[0]) - float(_a.bt) * 0.5:
					_s.nodes = []
					_done("%s's window: burn prograde now (%d m/s, about %s)" % [_a.to_name, roundi(float(_a.dv)), span(float(_a.bt))])
				return false
			_point(_a.vec)
			_thr(0.0)
			# on time, and pointing true (a late start costs less than a burn
			# off its line)
			var due := float(node[0]) - float(_a.bt) * 0.5
			if float(t.t) >= due and (_aligned(t, _a.vec, 2.0) or float(t.t) > due + 20.0):
				_a.start = float(t.get("dv_expended", 0.0))
				phase = "burn"
		"burn":
			_point(_a.vec)
			var left: float = float(_a.dv) - (float(t.get("dv_expended", 0.0)) - float(_a.start))
			if left <= 0.05:
				_thr(0.0)
				return true
			if _dry(t):
				_fail("out of propellant, %d m/s of the burn short" % roundi(left))
				return false
			_autostage(t)
			_thr(clampf(left / maxf(_accel(t) * 1.5, 0.01), 0.02, 1.0) if _aligned(t, _a.vec, 3.0) else 0.0)
	return false


func _transfer(t: Dictionary) -> void:
	if phase == "replan":
		_a.replans = int(_a.replans) + 1
		if int(_a.replans) > 8:
			_fail("no way to %s found from this orbit (looked from all the way round it)" % _a.to_name)
			return
		_warp_until(float(t.t) + float(t.orbit.get("period", 3600.0)) / 8.0)
		var r: PackedFloat64Array = _s.core.plan_transfer(_s.active, int(_a.to), float(_a.want), false)
		if r.size() >= 6 and r[4] > 0.5:
			_a.node = [r[0], r[1], r[2], r[3]]
			_s.nodes = [_a.node]
			phase = "warp"
			note = "%d m/s prograde in %s to %s" % [roundi(Vector3(r[1], r[2], r[3]).length()), span(float(r[0]) - _s.time()), _a.to_name]
			_say("planned (looking again from further round): %s" % note)
		return
	if phase in ["warp", "align", "burn"]:
		if _fly_node(t):
			_s.nodes = []
			if int(_a.get("corrections", 0)) >= 2:
				_finish_transfer()
				return
			phase = "coast"
		return
	if phase == "coast":
		# corrections: a little way out, and again halfway, if the pass has
		# drifted (a long burn is never quite the planned instant)
		var first := int(_a.get("corrections", 0)) == 0
		if first:
			_warp_until(float(t.t) + (600.0 if _a.get("moon", false) or _a.get("home", false) else 86400.0))
		else:
			var p := _patch_in([_s.time() + 1.0, 0.0, 0.0, 0.0], _a.to_id, 6.0e5 if _a.get("moon", false) or _a.get("home", false) else 1.0e8)
			if not p.is_empty():
				_warp_until(_s.time() + (float(p.t_start) - _s.time()) * 0.5)
		_a.corrections = int(_a.get("corrections", 0)) + 1
		var c := _plan_correction(_s.telemetry())
		if not c.is_empty():
			_say("correction: %.1f m/s, then passing %s up" % [Vector3(c[1], c[2], c[3]).length(), km(float(_patch_in(c, _a.to_id, 6.0e5 if _a.get("moon", false) or _a.get("home", false) else 1.0e8).get("pe_alt", -1.0)))])
		if c.is_empty():
			_finish_transfer()
			return
		_a.node = c
		phase = "warp"


func _finish_transfer() -> void:
	var p := _patch_in([_s.time() + 1.0, 0.0, 0.0, 0.0], _a.to_id, 6.0e5 if _a.get("moon", false) or _a.get("home", false) else 1.0e8)
	_s.nodes = []
	if p.is_empty():
		_fail("the burn is done but the path misses %s" % _a.to_name)
	else:
		_done("on the way: into %s's sphere in %s, passing %s up" % [_a.to_name, span(float(p.t_start) - _s.time()), km(float(p.pe_alt))])


# ---- maneuver node -----------------------------------------------------------------

func _begin_node(t: Dictionary) -> String:
	if _s.nodes.is_empty():
		return "no maneuver node: put one on the map (M) first"
	_a.node = _s.nodes[0]
	phase = "warp"
	note = "the node in %s" % span(float(_a.node[0]) - float(t.t))
	return ""


func _node(t: Dictionary) -> void:
	if _fly_node(t):
		if not _s.nodes.is_empty():
			_s.nodes.remove_at(0)
		_done("node burnt: %s" % _orbit_text(t))


# ---- warp ------------------------------------------------------------------------------

func _do_warp(w: PackedStringArray, t: Dictionary) -> String:
	if w.size() < 2:
		return "warp <seconds|apoapsis|periapsis|soi|node|body>"
	var what := w[1].to_lower()
	var o: Dictionary = t.get("orbit", {})
	var now := float(t.t)
	var to := -1.0
	var said := ""
	if what.is_valid_float():
		to = now + float(what)
		said = span(float(what))
	elif what in ["apoapsis", "ap"]:
		if o.get("t_to_ap") == null:
			return "no apoapsis ahead (an escape path)"
		to = now + float(o.t_to_ap)
		said = "the apoapsis"
	elif what in ["periapsis", "pe"]:
		if o.get("t_to_pe") == null:
			return "no periapsis ahead"
		to = now + float(o.t_to_pe)
		said = "the periapsis"
	elif what == "node":
		if _s.nodes.is_empty():
			return "no maneuver node"
		var bt := maxf(float(_s.core.burn_time(_s.active, _s.node_vector(_s.nodes, 0).length())), 0.0)
		to = float(_s.nodes[0][0]) - bt * 0.5 - LEAD
		said = "%d s before the node's burn" % roundi(LEAD)
	else:
		# the next sphere: any (soi), or the one of a body by name
		var patches := _s.predict(6, 6.0e6)
		for i in range(patches.size() - 1):
			var p: Dictionary = patches[i]
			if p.end in ["soi_enter", "soi_exit"] and (what == "soi" or str(patches[i + 1].body).to_lower() == what):
				to = float(p.t_end) + 5.0
				said = "%s's sphere" % str(patches[i + 1].body).capitalize()
				break
		if to < 0.0:
			return "the path never reaches %s" % ("another sphere" if what == "soi" else what)
	if to <= now + 0.5:
		_done("nothing to wait for")
		return ""
	_a.to = to
	_a.said = said
	_a.from = now
	_a.frames = 0
	phase = "settle"
	note = "to %s" % said
	return ""


## The engines off and still first (rails warp never burns), then the jump.
func _warp(t: Dictionary) -> void:
	_thr(0.0)
	_a.frames = int(_a.frames) + 1
	if float(t.get("thrust", 0.0)) > 0.0 and int(_a.frames) < 300:
		return
	var why := _s.rails_refusal()
	if why != "":
		if why.contains("thrust") and int(_a.frames) < 300:
			return
		_fail("could not warp: " + why)
		return
	var stopped := _s.warp_to(float(_a.to))
	_done("warped %s to %s%s" % [span(_s.time() - float(_a.from)), _a.said, "" if stopped == "" else " (stopped early: %s)" % stopped])


# ---- capture ----------------------------------------------------------------------------

func _begin_capture(w: PackedStringArray, t: Dictionary) -> String:
	var o: Dictionary = t.get("orbit", {})
	if t.get("landed", false):
		return "the ship is on the ground"
	if o.get("t_to_pe") == null:
		return "no periapsis ahead to capture at"
	_a.want_ap = float(w[1]) if w.size() > 1 and w[1].is_valid_float() else float(o.get("pe_alt", 0.0)) + 25000.0
	# the low point in the air (or skimming the ground): up out of it first,
	# a sideways push while still far out, before the braking at it
	var b := _body(t)
	var atm := float(b.get("atmosphere_height", 0.0))
	_a.safe_pe = atm + 30000.0 if atm > 0.0 else maxf(15000.0, float(b.radius) * 0.03)
	_a.want_ap = maxf(_a.want_ap, float(_a.safe_pe) + 1000.0)
	if _a.want_ap < float(o.get("pe_alt", 0.0)):
		_a.want_ap = float(o.pe_alt) + 1000.0
	phase = "lift" if float(o.get("pe_alt", 0.0)) < float(_a.safe_pe) - 15000.0 else "warp"
	note = "into orbit of %s" % _body(t).get("name", "?")
	return ""


func _capture(t: Dictionary) -> void:
	var o: Dictionary = t.get("orbit", {})
	var b := _body(t)
	match phase:
		"raise":
			var ttap = o.get("t_to_ap")
			if ttap != null and float(ttap) > LEAD + 10.0 and not _a.get("raising", false):
				_thr(0.0)
				_sas("prograde")
				_warp_until(float(t.t) + float(ttap) - LEAD)
				return
			if ttap != null and float(ttap) > 15.0 and not _a.get("raising", false):
				_thr(0.0)
				_sas("prograde")
				return
			_a.raising = true
			var atm2 := float(b.get("atmosphere_height", 0.0))
			if float(o.get("pe_alt", 0.0)) >= atm2 + 20000.0 or float(t.get("thrust_max", 0.0)) <= 0.0 and _shield_held:
				_thr(0.0)
				_done("in orbit of %s, %s" % [b.name, _orbit_text(t)])
				return
			if _dry(t):
				_fail("out of propellant raising the low point out of the air: %s" % _orbit_text(t))
				return
			_autostage(t)
			var dpro := dir_of(t, "prograde")
			_sas("prograde")
			_thr(clampf((atm2 + 20000.0 - float(o.get("pe_alt", 0.0))) / 20000.0, 0.05, 1.0) if _aligned(t, dpro, 5.0) else 0.0)
		"lift":
			if float(o.get("pe_alt", 0.0)) >= float(_a.safe_pe):
				_thr(0.0)
				_say("the low point raised out of harm's way: %s" % km(float(o.pe_alt)))
				phase = "warp"
				return
			if _dry(t):
				_fail("out of propellant raising the low point: %s" % _orbit_text(t))
				return
			_autostage(t)
			var d := dir_of(t, "radial_out")
			_point(d)
			_thr(clampf((float(_a.safe_pe) - float(o.get("pe_alt", 0.0))) / 20000.0, 0.05, 1.0) if _aligned(t, d, 5.0) else 0.0)
		"warp":
			var mu := float(b.mu)
			var rp := float(o.pe_alt) + float(b.radius)
			var v_pe := sqrt(maxf(mu * (2.0 / rp - 1.0 / float(o.sma)), 0.0))
			var dv := absf(v_pe - sqrt(mu / rp))
			var bt := float(_s.core.burn_time(_s.active, dv))
			if bt < 0.0:
				bt = dv / maxf(_accel(t), 0.5)
			_a.bt = bt
			_warp_until(float(t.t) + float(o.t_to_pe) - bt * 0.5 - LEAD)
			_sas("retrograde")
			phase = "align"
		"align":
			_sas("retrograde")
			var ttp = o.get("t_to_pe")
			if ttp == null or float(ttp) < float(_a.bt) * 0.5 or float(t.vertical_speed) > 0.0:
				phase = "burn"
		"burn":
			_sas("retrograde")
			if o.get("ap_alt") != null and float(o.ap_alt) <= float(_a.want_ap):
				_thr(0.0)
				var atm := float(b.get("atmosphere_height", 0.0))
				if atm > 0.0 and float(o.get("pe_alt", 0.0)) < atm + 10000.0:
					# (a long burn round the low point drags it down: up out of
					# the air again, at the high point)
					phase = "raise"
					return
				_done("in orbit of %s, %s" % [b.name, _orbit_text(t)])
				return
			if _dry(t):
				_fail("out of propellant, %s" % _orbit_text(t))
				return
			_autostage(t)
			var thr := 1.0
			if o.get("ap_alt") != null:
				thr = clampf((float(o.ap_alt) - float(_a.want_ap)) / maxf(float(_a.want_ap), 1.0) * 4.0, 0.05, 1.0)
			_thr(thr if _aligned(t, dir_of(t, "retrograde"), 10.0) else 0.0)


# ---- land ---------------------------------------------------------------------------------

func _begin_land(w: PackedStringArray, t: Dictionary) -> String:
	var b := _body(t)
	_a.atm = float(b.get("atmosphere_height", 0.0))
	_a.g = float(b.surface_gravity)
	if w.size() > 1:
		var why := _land_target(w, t)
		if why != "":
			return why
		return _begin_land_at(t)
	if t.get("landed", false) and float(t.surface_speed) < 1.0:
		return "the ship is on the ground already"
	var o: Dictionary = t.get("orbit", {})
	if _a.atm > 0.0:
		phase = "deorbit" if float(o.get("pe_alt", 0.0)) > _a.atm * 0.45 else "entry"
	else:
		var floor_pe := maxf(8000.0, float(b.radius) * 0.03)
		_a.low = floor_pe
		phase = "lower" if float(o.get("pe_alt", 0.0)) > floor_pe * 1.5 and o.get("ap_alt") != null else "brake"
	_a.t0 = float(t.t)
	note = "down onto %s" % b.name
	return ""


## The ground under a rocket on its engine, gently: a falling speed that
## shrinks with the height above the ground as fast as the engine can
## shrink it, and the sideways drift nulled. Returns the throttle.
func _descend(t: Dictionary) -> float:
	var g: float = _a.g
	var agl := float(t.altitude_terrain)
	var accel := _accel(t)
	if accel <= 0.0:
		return 0.0
	var brake := maxf(accel - g, 0.5) * 0.45
	# the last few metres at a walking pace, legs first (by the feet's own
	# height: on a tall lander they're a long way under its middle)
	var feet := float(t.get("clearance", agl - 5.0))
	var vt := -minf(1.0 + sqrt(2.0 * brake * maxf(feet - 1.0, 0.0)), 250.0)
	var up := v3(t.up)
	var vs := float(t.vertical_speed)
	var vh := v3(t.surface_velocity) - up * vs
	# over the level spot picked for the legs (see _pick_spot): drift to it,
	# slower the nearer it is and the lower, and wait up high till over it
	var v_want := Vector3.ZERO
	if _a.has("spot"):
		var off := _spot_offset(t)
		var d := off.length()
		if d > 1.0:
			# (none at all the last few metres: the legs go down still)
			v_want = off / d * minf(minf(d * 0.25, 15.0), 1.0 + agl * 0.1) * clampf((feet - 2.0) / 10.0, 0.0, 1.0)
		if d > 12.0:
			vt = maxf(vt, -maxf(agl - 25.0, 0.0) * 0.25)
	# still sliding across that low: hang there till it's off (a tall
	# ship set down sliding tips over)
	# (not too fast down the last tens of metres, and slow while still
	# sliding: what's left across is taken off by leaning, and near the
	# ground a lean is what tips a tall ship)
	vt = maxf(vt, -(1.0 + feet * 0.25))
	if feet < 30.0 and vh.length() > 1.0:
		vt = maxf(vt, -1.0)
	if feet < 10.0 and vh.length() > 0.6:
		vt = maxf(vt, -0.3)
	if feet < 4.0 and vh.length() > 0.4:
		vt = maxf(vt, 0.3)  # (and up a little, till it is still, before the legs meet the ground)
	var a_up := g + 1.2 * (vt - vs)
	var stop := vs * vs / (2.0 * maxf(feet - 0.5, 0.2))
	if vs < 0.0 and stop > brake:
		# (falling faster than the profile can stop by the ground: never
		# less than what does; on a heavy world the profile alone lags)
		a_up = maxf(a_up, g + stop * 1.05)
	var a_h := (v_want - vh) * (1.5 if feet < 10.0 else 0.6)
	# (and the last of the way upright: the legs meet the ground together)
	var tilt := maxf(a_up, 0.0) * tan(deg_to_rad(35.0 if feet > 1.5 else 3.0))
	if a_h.length() > tilt:
		a_h = a_h.normalized() * tilt
	if a_up <= 0.0:
		_point(up)
		return 0.0
	var want := up * a_up + a_h
	_point(want)
	if feet < 0.3 and absf(vs) < 1.5:
		return 0.0
	return clampf(want.length() / accel, 0.0, 1.0)


## Level ground for the legs near where the ship is coming down: the
## nearest within 300 m tilted less than LEVEL (or the least tilted:
## a top-heavy lander can tip on steeper).
func _pick_spot(t: Dictionary) -> void:
	var sp: PackedFloat64Array = _s.core.level_spot(int(t.body_index), float(t.lat_deg), float(t.lon_deg), 300.0, LEVEL)
	_a.spot = [sp[0], sp[1]]


## From the ship to its spot, along the ground (m, the telemetry's axes).
func _spot_offset(t: Dictionary) -> Vector3:
	var R := float(_body(t).get("radius", 1.0))
	var la := deg_to_rad(float(t.lat_deg))
	var dn := deg_to_rad(float(_a.spot[0]) - float(t.lat_deg)) * R
	var de := deg_to_rad(wrapf(float(_a.spot[1]) - float(t.lon_deg), -180.0, 180.0)) * R * cos(la)
	return v3(t.north) * dn + v3(t.east) * de


func _land(t: Dictionary) -> void:
	var o: Dictionary = t.get("orbit", {})
	var agl := float(t.altitude_terrain)
	if phase != "settle" and float(t.surface_speed) < 2.0 and (t.get("landed", false) \
			or phase == "descent" and int(t.get("contacts", 0)) > 0 and float(t.get("clearance", 99.0)) < 0.5):
		phase = "settle"  # (on the legs as they first touch: eased onto them, below)
	match phase:
		"round":
			_land_round(t)
		"oval":
			_land_oval(t)
		"trim":
			_land_trim(t)
		"fix":
			# (along the orbit's normal: + moves where it comes down that way)
			var d := dir_of(t, "normal") * signf(float(_a.fix_dv))
			_point(d)
			var left := absf(float(_a.fix_dv)) - (float(t.get("dv_expended", 0.0)) - float(_a.fix_start))
			if left <= 0.05:
				_thr(0.0)
				phase = "time"
				return
			if _dry(t):
				_fail("out of propellant turning the orbit")
				return
			_autostage(t)
			_thr(clampf(left / maxf(_accel(t) * 1.5, 0.01), 0.02, 1.0) if _aligned(t, d, 4.0) else 0.0)
		"aim":
			_land_aim(t)
		"time":
			_land_time(t)
		"home":
			_land_home(t)
		"shed":
			_land_shed(t)
		"deorbit":
			# drop the low point deep into the air, then let it do the work
			var down := FlightPrograms.down_dir(t, float(_a.get("tilt", 0.0)))
			_point(down)
			if float(o.get("pe_alt", 0.0)) < _a.atm * 0.4:
				_thr(0.0)
				if _a.get("target", false):
					phase = "trim"
					return
				phase = "entry"
				_warp_until(float(t.t) + 1.0e6)  # (stops at the air)
				return
			if _dry(t):
				_fail("out of propellant coming down: %s" % _orbit_text(t))
				return
			if float(t.get("thrust_max", 0.0)) <= 0.0 and FlightPrograms.shield_press(_s.core, _s.active, int(t.get("next_stage", -1)), false):
				_fail("the stage under the heat shield ran dry before the burn down was done (%s): the lander's own engine can't fire with the shield still under it" % _orbit_text(t))
				return
			_autostage(t)
			_thr(1.0 if _aligned(t, down, 8.0) else 0.0)
		"entry":
			_thr(0.0)
			_clear_shield(t)
			_sas("retrograde" if t.get("in_atmosphere", false) and float(t.surface_speed) > 500.0 else "surface_retrograde")
			if _a.get("target", false) and _handover(t):
				_a.braking = true
				if FlightPrograms.shield_press(_s.core, _s.active, int(t.get("next_stage", -1)), false):
					phase = "shed"
					_a.shed = 0
				else:
					phase = "home"
				return
			if float(t.altitude) < _a.atm * 0.45 and float(t.vertical_speed) < 0.0 and not _a.get("chutes", false):
				if int(t.get("chutes", 0)) > 0 and not (_a.get("target", false) and _powered_finish(t)):
					_scene._deploy_chutes()
				_a.chutes = true
			if not t.get("landed", false) and agl < 4000.0 and float(t.surface_speed) < 350.0 and float(t.get("thrust_max", 0.0)) > 0.0 and not _a.get("chutes_open", false):
				if int(t.get("chutes_open", 0)) > 0:
					_a.chutes_open = true
				elif float(t.surface_speed) > 12.0:
					phase = "descent"
					if not _a.get("legs", false):
						_scene.session.set_legs(true)
						_a.legs = true
		"lower":
			# bring the low point down near the ground, then fall to it:
			# the braking is cheapest done low and fast
			_sas("retrograde")
			if float(o.get("pe_alt", 0.0)) <= float(_a.low):
				_thr(0.0)
				phase = "fall"
				return
			if _dry(t):
				_fail("out of propellant lowering the orbit: %s" % _orbit_text(t))
				return
			_autostage(t)
			var gap := float(o.get("pe_alt", 0.0)) - float(_a.low)
			_thr(clampf(gap / 20000.0, 0.05, 1.0) if _aligned(t, dir_of(t, "retrograde"), 6.0) else 0.0)
		"fall":
			_thr(0.0)
			_sas("surface_retrograde")
			var ttp = o.get("t_to_pe")
			if ttp != null and float(ttp) > 90.0 and float(t.vertical_speed) < 0.0 or (ttp != null and float(ttp) > 90.0 and _s.time() < float(t.t) + 1.0):
				_warp_until(float(t.t) + float(ttp) - 90.0)
			phase = "home" if _a.get("target", false) else "brake"
		"brake":
			if not _a.get("legs", false):
				_scene.session.set_legs(true)
				_a.legs = true
			var d := dir_of(t, "surface_retrograde")
			_point(d)
			if float(t.horizontal_speed) < 40.0 or agl < 6000.0:
				_thr(0.0)
				phase = "descent"
				return
			if _dry(t):
				_fail("out of propellant braking, %d m/s over the ground" % roundi(float(t.horizontal_speed)))
				return
			_autostage(t)
			_thr(1.0 if _aligned(t, d, 6.0) else 0.0)
		"descent":
			if _dry(t):
				_fail("out of propellant %d m up, falling at %d m/s" % [roundi(agl), roundi(-float(t.vertical_speed))])
				return
			if not _a.has("spot") and agl < 900.0 and float(t.horizontal_speed) < 40.0:
				_pick_spot(t)
			_autostage(t)
			_thr(_descend(t))
		"settle":
			# (the weight onto the legs gently, held upright on the engine:
			# a tall ship set down all at once rocks, and on a slope goes
			# over)
			if not _a.has("settle_t0"):
				_a.settle_t0 = float(t.t)
			var since := float(t.t) - float(_a.settle_t0)
			var accel := _accel(t)
			var share := maxf(0.0, 0.7 - since * 0.12)
			if share > 0.0 and accel > 0.0 and not _dry(t):
				_point(v3(t.up))
				_thr(clampf(share * float(_a.g) / accel, 0.0, 1.0))
			else:
				_thr(0.0)
				_sas("stability")
			if float(t.surface_speed) < 0.3:
				_a.still = float(_a.get("still", 0.0)) + 1.0
			if t.get("landed", false) and float(_a.get("still", 0.0)) > 20.0:
				_done("down on %s at %.2f, %.2f, %s, %.1f m/s at touchdown" % [
					_body(t).get("name", "?"), float(t.lat_deg), float(t.lon_deg), "upright" if float(t.pitch_deg) > 60.0 else "NOT upright",
					float(t.get("max_impact", 0.0))])


# ---- land at a place --------------------------------------------------------------

const LEVEL := 0.0006  # 1 - cos of the steepest ground the legs go down on (2 degrees: a tall ship on springy legs goes over on a dune's face)
const RELIC_STANDOFF := 50.0  # m: set down this far from a relic's middle, and walk
const AT_PLACE := 40.0        # m: near enough the place to come straight down


## `land <lat> <lon>` or `land near <name>`: where (_a.tlat, _a.tlon,
## _a.tname, _a.standoff), or why not.
func _land_target(w: PackedStringArray, t: Dictionary) -> String:
	var b := _body(t)
	_a.standoff = 0.0
	if w.size() >= 3 and w[1].is_valid_float() and w[2].is_valid_float():
		_a.tlat = clampf(float(w[1]), -90.0, 90.0)
		_a.tlon = wrapf(float(w[2]), -180.0, 180.0)
		_a.tname = "%.2f, %.2f" % [_a.tlat, _a.tlon]
		return ""
	var words := w.slice(2) if w[1] == "near" else w.slice(1)
	if words.is_empty() or w[1].is_valid_float():
		return "land, land <lat> <lon>, or land near <a relic or landmark>"
	var want := " ".join(words).to_lower()
	for r in b.get("relics", []):
		if want in str(r.id).to_lower() or want in str(r.name).to_lower():
			if Career.is_career() and not str(r.id) in Career.heard:
				return "%s isn't charted: a Signal Scanner has to hear it first" % r.name
			_a.tlat = float(r.lat_deg)
			_a.tlon = float(r.lon_deg)
			_a.tname = str(r.name)
			_a.standoff = RELIC_STANDOFF
			return ""
	for lm in Sim.landmarks(str(b.id)):
		if want in str(lm.get("id", "")).to_lower() or want in str(lm.get("name", "")).to_lower():
			_a.tlat = float(lm.lat_deg)
			_a.tlon = float(lm.lon_deg)
			_a.tname = str(lm.name)
			return ""
	return "nothing called '%s' on %s" % [want, b.name]


func _begin_land_at(t: Dictionary) -> String:
	var b := _body(t)
	# a ship with the propellant to stop above the air (the Hearth Torch's
	# kind) comes down as if there were none: braked before the air, and
	# through it slowly on the engine (no fire, no guessing at the air)
	_a.air_top = _a.atm
	var v_orb := sqrt(float(b.mu) / (float(b.radius) + maxf(float(t.altitude), 0.0)))
	var dv_have := float(t.get("dv_total", 0.0))
	if _a.atm > 0.0 and _finish_accel(t) > float(_a.g) * 1.5 and (dv_have > v_orb * 1.5 + 3000.0 \
			or _a.atm < 60000.0 and dv_have > v_orb * 1.3 + 1500.0):
		# (and thin air like Narwhal's barely slows a heavy lander anyway)
		_a.atm = 0.0
		_a.no_air_path = true
	_a.bi = int(t.body_index)
	_a.R = float(b.radius)
	_a.mu = float(b.mu)
	_a.period = float(b.get("rotation_period", 0.0))
	_a.rot0 = float((_s.core.body_state(_a.bi) as PackedFloat64Array)[6])
	_a.rot_t = float(t.t)
	_a.target = true
	var o: Dictionary = t.get("orbit", {})
	var orbiting: bool = o.get("ap_alt") != null and float(o.get("pe_alt", -1.0)) > maxf(_a.atm, 0.0) and not t.get("landed", false)
	if orbiting and _a.atm > 0.0 and float(o.get("ecc", 0.0)) > 0.3:
		# a long oval over air: down from its high point (a small burn
		# there drops the low point into the air), the revolution chosen
		# by trying each on a copy of the world, trimmed on the long fall
		phase = "oval"
	elif orbiting:
		# (round first: the timing of the way down is laid on a circle)
		phase = "round" if float(o.get("ecc", 0.0)) > 0.003 else "aim"
	elif _a.atm > 0.0 and t.get("in_atmosphere", false) and float(t.surface_speed) > 400.0:
		phase = "entry"
	else:
		if float(t.get("thrust_max", 0.0)) <= 0.0 and int(t.get("next_stage", -1)) < 0:
			return "no engine to fly there on"
		if t.get("landed", false) and float(t.get("thrust_max", 0.0)) <= 0.0:
			_scene._stage()
		phase = "home"
		_a.braking = true
	note = "down by %s" % _a.tname
	_say("to come down by %s, %s from here along the ground" % [_a.tname, km(_ground_dist(t))])
	return ""


## The place's direction from the body's middle (unit, sim axes) at time tt.
func _tdir(tt: float) -> Vector3:
	var la := deg_to_rad(float(_a.tlat))
	var lo := deg_to_rad(float(_a.tlon))
	var f := Vector3(cos(la) * cos(lo), cos(la) * sin(lo), sin(la))
	var rot := float(_a.rot0) + (TAU * (tt - float(_a.rot_t)) / float(_a.period) if float(_a.period) != 0.0 else 0.0)
	return Vector3(cos(rot) * f.x - sin(rot) * f.y, sin(rot) * f.x + cos(rot) * f.y, f.z)


## Along the orbit, to where the place will be `ahead` s from now: the angle
## round from the ship (rad, 0..TAU), and how far off the orbit's plane it
## is then (rad, + toward the orbit's normal).
func _track(t: Dictionary, ahead: float) -> Vector2:
	var r := v3(t.position)
	var h := r.cross(v3(t.velocity)).normalized()
	var d := _tdir(float(t.t) + ahead)
	var p := (d - h * d.dot(h)).normalized()
	var rh := r.normalized()
	return Vector2(fposmod(atan2(rh.cross(p).dot(h), rh.dot(p)), TAU), asin(clampf(d.dot(h), -1.0, 1.0)))


## The ship's angular rate round the body (rad/s).
static func _rate(t: Dictionary) -> float:
	var r := v3(t.position)
	return r.cross(v3(t.velocity)).length() / maxf(r.length_squared(), 1.0)


## Seconds until the ship is over the place (round its orbit, by Kepler).
func _pass_time(t: Dictionary) -> float:
	var ahead := 0.0
	for i in 5:
		ahead = _time_round(t, _track(t, ahead).x)
	return ahead


## Seconds for the ship to go `angle` rad further round its orbit (an
## ellipse's slow and fast parts and all).
func _time_round(t: Dictionary, angle: float) -> float:
	var r := v3(t.position)
	var v := v3(t.velocity)
	var mu := float(_a.mu)
	var h := r.cross(v)
	var e_vec := v.cross(h) / mu - r.normalized()
	var e := e_vec.length()
	var a := 1.0 / (2.0 / r.length() - v.length_squared() / mu)
	if e < 0.01 or a <= 0.0 or e >= 1.0:
		return angle / _rate(t)
	var n := sqrt(mu / (a * a * a))
	var hn := h.normalized()
	var nu0 := atan2(hn.dot(e_vec.normalized().cross(r.normalized())), e_vec.normalized().dot(r.normalized()))
	var m0 := FlightPrograms._mean_anomaly(nu0, e)
	var m1 := FlightPrograms._mean_anomaly(nu0 + angle, e)
	var turns := floorf(angle / TAU)
	var dm := fposmod(m1 - m0, TAU) + turns * TAU
	return dm / n


static func _mean_anomaly(nu: float, e: float) -> float:
	var ea := 2.0 * atan(sqrt((1.0 - e) / (1.0 + e)) * tan(nu * 0.5))
	return ea - e * sin(ea)


## From the ship to the place along the ground (m, the telemetry's axes).
func _place_offset(t: Dictionary) -> Vector3:
	var R := float(_a.R)
	var la := deg_to_rad(float(t.lat_deg))
	var dn := deg_to_rad(float(_a.tlat) - float(t.lat_deg)) * R
	var de := deg_to_rad(wrapf(float(_a.tlon) - float(t.lon_deg), -180.0, 180.0)) * R * cos(la)
	return v3(t.north) * dn + v3(t.east) * de


## Great-circle distance from the ship to the place (m).
func _ground_dist(t: Dictionary) -> float:
	var la1 := deg_to_rad(float(t.lat_deg))
	var lo1 := deg_to_rad(float(t.lon_deg))
	var la2 := deg_to_rad(float(_a.tlat))
	var lo2 := deg_to_rad(float(_a.tlon))
	var a := Vector3(cos(la1) * cos(lo1), cos(la1) * sin(lo1), sin(la1))
	var b := Vector3(cos(la2) * cos(lo2), cos(la2) * sin(lo2), sin(la2))
	return a.angle_to(b) * float(_a.R)


## "oval": down from the high point of a long orbit over air. On copies of
## the world, the burn down made at each of the next high points: the one
## that comes down nearest the place is flown, then trimmed on the way.
func _land_oval(t: Dictionary) -> void:
	var o: Dictionary = t.get("orbit", {})
	if not _a.has("oval_at"):
		var period := float(o.get("period", 0.0))
		var ttap = o.get("t_to_ap")
		if period <= 0.0 or ttap == null:
			phase = "round"
			return
		var best := INF
		var best_k := -1
		var n := clampi(int(3.0 * 86400.0 / period), 1, 12)
		for k in n:
			var at := float(t.t) + float(ttap) + period * k
			var ct := _oval_down(at)
			if ct.is_empty():
				continue
			var miss := _miss(ct).length()
			if miss < best:
				best = miss
				best_k = k
		if best_k < 0:
			_fail("can't work out a way down by %s from this orbit: no way down from its high point comes down" % _a.tname)
			return
		_a.oval_at = float(t.t) + float(ttap) + period * best_k
		_say("down from the high point %s from now: the run comes down %s from %s, to be trimmed on the way" % [span(float(_a.oval_at) - float(t.t)), km(best), _a.tname])
	var wait := float(_a.oval_at) - float(t.t)
	_sas("retrograde")
	if wait > 2.0 * LEAD:
		_thr(0.0)
		_warp_until(float(t.t) + wait - LEAD)
		return
	if wait > 0.3:
		_thr(0.0)
		return
	phase = "deorbit"


## On a copy of the world: warp to `at`, burn retrograde until the low point
## is deep in the air, fall to the hand-over; its telemetry there, or {}.
func _oval_down(at: float) -> Dictionary:
	var c := _world_copy()
	if c == null:
		return {}
	var id := _s.active
	var before := float(JSON.parse_string(c.telemetry(id)).t)
	c.warp(maxf(at - before, 0.0), id)
	c.vessel_set_mode(id, "physics")
	c.vessel_set_sas(id, "retrograde", PackedFloat64Array())
	var ct: Dictionary = {}
	var burning := true
	for sec in 60000:
		ct = JSON.parse_string(c.telemetry(id))
		if not ct.get("alive", false):
			return {}
		if burning:
			if float(ct.get("thrust_max", 0.0)) <= 0.0 and int(ct.get("next_stage", -1)) >= 0 \
					and not FlightPrograms.shield_press(c, id, int(ct.get("next_stage", -1)), false):
				c.vessel_stage(id)
			var aligned := v3(ct.nose).angle_to(-v3(ct.velocity)) < deg_to_rad(8.0)
			c.vessel_set_throttle(id, 1.0 if aligned else 0.0)
			if float(ct.get("orbit", {}).get("pe_alt", 1e12)) < _a.atm * 0.4:
				c.vessel_set_throttle(id, 0.0)
				burning = false
			c.step_physics(5)
			continue
		if ct.get("landed", false) or _handover(ct):
			return ct
		if not ct.get("in_atmosphere", false):
			var t0 := float(ct.t)
			c.warp(600.0, id)
			if float(JSON.parse_string(c.telemetry(id)).t) <= t0 + 1e-3:
				c.step_physics(50)
			continue
		c.vessel_set_sas(id, "retrograde" if float(ct.surface_speed) > 500.0 else "surface_retrograde", PackedFloat64Array())
		if FlightPrograms.shield_press(c, id, int(ct.get("next_stage", -1)), true):
			c.vessel_stage(id)
		c.step_physics(50)
	return {}


## "round": an oval orbit made a circle at its high point first.
func _land_round(t: Dictionary) -> void:
	var o: Dictionary = t.get("orbit", {})
	var ttap = o.get("t_to_ap")
	if not _a.get("rounding", false):
		if ttap != null and float(ttap) > LEAD + 10.0:
			_thr(0.0)
			_sas("prograde")
			_warp_until(float(t.t) + float(ttap) - LEAD)
			return
		_a.rounding = true
	var need := _circ_need(t)
	_sas("prograde" if need > 0.0 else "retrograde")
	if absf(need) < 1.0 or float(o.get("ecc", 0.0)) < 0.002:
		_thr(0.0)
		_a.rounding = false
		phase = "aim"
		_say("the orbit made round: %s" % _orbit_text(t))
		return
	if _dry(t):
		_fail("out of propellant making the orbit round")
		return
	_autostage(t)
	_thr(clampf(absf(need) / maxf(_accel(t) * 2.0, 0.01), 0.02, 1.0) if _aligned(t, dir_of(t, "prograde" if need > 0.0 else "retrograde"), 5.0) else 0.0)


## "aim": the orbit turned (a burn along its normal, best a quarter of the
## way round before the place) until it passes over the place.
func _land_aim(t: Dictionary) -> void:
	var w := _rate(t)
	var period := float(t.orbit.get("period", TAU / w))
	if _a.atm > 0.0 and not _a.has("drift"):
		# (the air will carry it across its track coming down: the orbit's
		# line is laid that far to the other side of the place)
		_a.drift = 0.0
		if _entry_lead(t) >= 0.0:
			_a.drift = float(_a.entry_drift)
			if absf(float(_a.drift)) > 1000.0:
				_say("the air will carry it %s across its track: the orbit is laid that far over" % km(absf(float(_a.drift))))
	var drift := float(_a.get("drift", 0.0)) / float(_a.R)
	if not _a.has("best_pass"):
		# the ground turns under the orbit: of the passes over the next few
		# days, the one nearest the place (the least turning to do)
		var best := INF
		var best_t := 0.0
		var th := _track(t, _pass_time(t)).x
		for k in mini(int(3.0 * 86400.0 / period), 120):
			var ahead := _time_round(t, th) + period * k
			for i in 3:
				ahead = _time_round(t, _track(t, ahead).x) + period * k
			var cross := absf(_track(t, ahead + period).y - drift)
			# (a later pass costs only the wait; a nearer one the propellant)
			if cross < best * 0.8:
				best = cross
				best_t = float(t.t) + ahead
		_a.best_pass = best_t
		if best_t - float(t.t) > period:
			_say("waiting %s for the pass nearest %s (%s off its line)" % [span(best_t - float(t.t) - period * 0.25), _a.tname, km(best * float(_a.R))])
	var to_best := float(_a.best_pass) - float(t.t)
	if to_best > period * 1.25 and not _a.get("aim_burning", false):
		_thr(0.0)
		_warp_until(float(t.t) + to_best - period)
		return
	# (the orbit is turned a quarter of the way round before one pass, and
	# the way down starts more than a quarter round before the place: so
	# it's the pass after that comes down, the world turned once more)
	var over := _pass_time(t)
	var th0 := _track(t, over).x
	if th0 < PI / 2.0 - 0.03 and not _a.get("aim_burning", false):
		# (the quarter round before this pass is behind already: the next
		# pass, the world turned once more meanwhile)
		over = _time_round(t, th0) + period
		for i in 3:
			over = _time_round(t, _track(t, over).x) + period
		th0 = _track(t, over).x + TAU
	var tr := Vector2(th0, _track(t, over + period).y - drift)
	var miss := tr.y * float(_a.R)
	var v := v3(t.velocity).length()
	if absf(miss) < maxf(200.0, float(_a.R) * 0.0004) and _a.get("aim_burning", false) == false or absf(miss) < 100.0:
		_thr(0.0)
		_a.aim_burning = false
		phase = "time"
		_say("the orbit passes over %s (%.0f m off)" % [_a.tname, miss])
		return
	var lead := _time_round(t, tr.x - PI / 2.0) if tr.x - PI / 2.0 > 0.0 else 0.0  # s until the place is a quarter round
	if not _a.get("aim_burning", false):
		if lead > 2.0 * LEAD:
			_thr(0.0)
			_point(dir_of(t, "normal") * signf(miss))
			_warp_until(float(t.t) + lead - LEAD)
			return
		if lead > 5.0:
			_thr(0.0)
			_point(dir_of(t, "normal") * signf(miss))
			return
		_a.aim_burning = true
		_a.aim_from = float(t.t)
	var d := dir_of(t, "normal") * signf(miss)
	_point(d)
	if _dry(t):
		_fail("out of propellant turning the orbit")
		return
	_autostage(t)
	var need := absf(tr.y) * v
	_thr(clampf(need / maxf(_accel(t), 0.01) * 0.5, 0.02, 1.0) if _aligned(t, d, 5.0) else 0.0)
	if absf(miss) < 100.0 or float(t.t) - float(_a.get("aim_from", float(t.t))) > period * 0.15:
		# (done, or gone too far round to keep at it: next time round)
		_thr(0.0)
		_a.aim_burning = false


## "time": warps to the moment for the burn down. Without air the low point
## goes over the place, half the way round from the burn; with air, the
## burn is when a run of the entry ahead (on a copy of the world) comes
## down by the place.
func _land_time(t: Dictionary) -> void:
	var w := _rate(t)
	var r := v3(t.position).length()
	var lead_ang := PI
	var low := maxf(maxf(8000.0, float(_a.R) * 0.03), float(_a.get("air_top", 0.0)) + 10000.0)
	var a_tr: float = (r + float(_a.R) + (low if _a.atm <= 0.0 else _a.atm * 0.4)) / 2.0
	var half := PI * sqrt(a_tr * a_tr * a_tr / float(_a.mu))
	if _a.atm > 0.0:
		lead_ang = float(_a.get("entry_lead", -1.0))
		if lead_ang < 0.0:
			lead_ang = _entry_lead(t)
			_a.entry_lead = lead_ang
			if lead_ang < 0.0:
				_fail("can't work out a way down by %s from this orbit: %s" % [_a.tname, _a.get("entry_why", "?")])
				return
			_say("the entry run says: start down %.0f degrees before %s" % [rad_to_deg(lead_ang), _a.tname])
		half = float(_a.get("entry_time", half))
	# (where the place will be when it comes down, the wait included: the
	# world turns on while it waits)
	var wait := 0.0
	for i in 6:
		wait = _time_round(t, fposmod(_track(t, wait + half).x - lead_ang, TAU))
	if wait > float(t.orbit.get("period", TAU / w)) - 2.0:
		wait = 0.0  # (just past it: now)
	_sas("retrograde")
	if wait > 2.0 * LEAD:
		_warp_until(float(t.t) + wait - LEAD)
		return
	if _a.atm > 0.0 and not _a.get("refined", false):
		# again from here, just before the burn: off a round orbit, where
		# round it the burn is made changes where it comes down
		_a.refined = true
		var again := _entry_lead(t)
		if again >= 0.0:
			# and to the side: a little tilt toward the orbit's normal moves
			# where it comes down across the track (the ground turns under
			# it while it falls): two runs give the tilt that brings it over
			var c0 := float(_a.entry_cross)
			var k := 0.02
			if absf(c0) > 50000.0 and int(_a.get("fixes", 0)) < 2:
				# too far to the side to trim on the way down: turn the
				# orbit by what the run says, and down on the next pass
				var v := v3(t.velocity).length()
				_a.fix_dv = -c0 / (float(_a.R) * maxf(absf(sin(again)), 0.3)) * v * signf(sin(again))
				_a.fix_start = float(t.get("dv_expended", 0.0))
				_a.fixes = int(_a.get("fixes", 0)) + 1
				_a.refined = false
				phase = "fix"
				_say("the entry run comes down %s off to the side: %.0f m/s to turn the orbit, then down next time round" % [km(absf(c0)), absf(float(_a.fix_dv))])
				return
			if absf(c0) > 500.0 and _entry_lead(t, k) >= 0.0 and absf(float(_a.entry_cross) - c0) > 1.0:
				var tilt := clampf(-c0 * k / (float(_a.entry_cross) - c0), -0.1, 0.1)
				var again2 := _entry_lead(t, tilt)
				if again2 >= 0.0:
					_a.tilt = tilt
					again = again2
					_say("the burn down tilted %.1f degrees off retrograde: %s across the track becomes %s" % [rad_to_deg(tilt), km(c0), km(float(_a.entry_cross))])
			_a.entry_lead = again
		return
	if wait > 0.3:
		return
	if _a.atm > 0.0:
		phase = "deorbit"
	else:
		_a.low = low
		phase = "lower"


## With air: how far round before the place to start the burn down, by
## running the burn and the fall on a copy of the world (rad), and how
## long from the burn to the hand-over low down (_a.entry_time). -1 if it
## never comes down.
func _entry_lead(t: Dictionary, tilt := 0.0) -> float:
	var c := TrsSim.new()
	c.load_system(ProjectSettings.globalize_path(Sim.SYSTEM_PATH))
	c.load_catalog(ProjectSettings.globalize_path(Sim.CATALOG_PATH))
	for id in Sim.nest_crafts():
		c.nest_craft(id, FileAccess.get_file_as_string(Sim.ALIEN_CRAFT_DIR + id + ".craft.json"))
	if not c.world_load(_s.core.world_save()):
		_a.entry_why = "the copy of the world wouldn't load"
		return -1.0
	var id := _s.active
	var t0 := float(t.t)
	var r0 := v3(t.position).normalized()
	var h := v3(t.position).cross(v3(t.velocity)).normalized()
	var h_burn := h
	c.vessel_set_mode(id, "physics")
	var ct: Dictionary = {}
	var chutes := false
	var burning := false
	var stage := "burn"
	for sec in 60000:
		c.step_physics(5 if stage == "burn" else 50)  # (the burn's end to a tenth of a second, as the real one's)
		ct = JSON.parse_string(c.telemetry(id))
		if not ct.get("alive", false):
			_a.entry_why = "the ship didn't come through it (%s)" % stage
			return -1.0
		var o: Dictionary = ct.get("orbit", {})
		match stage:
			"burn":
				if float(ct.get("thrust_max", 0.0)) <= 0.0 and int(ct.get("next_stage", -1)) >= 0:
					c.vessel_stage(id)  # (as _autostage lights the engine for the real burn)
				var bd := FlightPrograms.down_dir(ct, tilt)
				c.vessel_set_sas(id, "target", PackedFloat64Array([bd.x, bd.y, bd.z]))
				var aligned := v3(ct.nose).angle_to(bd) < deg_to_rad(8.0)
				c.vessel_set_throttle(id, 1.0 if aligned else 0.0)
				if aligned and not burning:
					# (from where the burn starts: the real one is lined up already)
					burning = true
					t0 = float(ct.t)
					r0 = v3(ct.position).normalized()
					h = v3(ct.position).cross(v3(ct.velocity)).normalized()
					h_burn = h
				if float(o.get("pe_alt", 1e12)) < _a.atm * 0.4:
					c.vessel_set_throttle(id, 0.0)
					stage = "coast"
				elif float(ct.get("thrust_max", 0.0)) <= 0.0 and float(ct.get("liquid", 0.0)) <= 0.0:
					_a.entry_why = "out of propellant for the burn down"
					return -1.0
			"coast":
				if ct.get("in_atmosphere", false):
					stage = "fall"
				else:
					c.warp(600.0, id)
					ct = JSON.parse_string(c.telemetry(id))
					if ct.get("in_atmosphere", false):
						stage = "fall"
			"fall":
				c.vessel_set_sas(id, "retrograde" if float(ct.surface_speed) > 500.0 else "surface_retrograde", PackedFloat64Array())
				if FlightPrograms.shield_press(c, id, int(ct.get("next_stage", -1)), true):
					c.vessel_stage(id)  # (as _clear_shield: the shield meets the air)
				if not _powered_finish(ct) and not chutes and float(ct.altitude) < _a.atm * 0.45 and float(ct.vertical_speed) < 0.0:
					c.vessel_deploy_chutes(id)
					chutes = true
				if ct.get("landed", false) or _handover(ct):
					break
	if ct.is_empty() or not (ct.get("landed", false) or _handover(ct)):
		_a.entry_why = "still %s, %s up at %.0f m/s when the run stopped" % [stage, km(float(ct.get("altitude", 0.0))), float(ct.get("surface_speed", 0.0))]
		return -1.0
	_a.entry_time = float(ct.t) - t0
	_say("the entry run: burn %s from now, down to the hand-over at %.2f, %.2f %s later" % [span(t0 - float(t.t)), float(ct.lat_deg), float(ct.lon_deg), span(float(ct.t) - t0)])
	# where it came down, round from the burn, in the orbit's plane
	var p := v3(ct.position).normalized()
	var la := deg_to_rad(float(ct.lat_deg))
	var lo := deg_to_rad(float(ct.lon_deg))
	# (the body-fixed place it came down, as the place would be then)
	var f := Vector3(cos(la) * cos(lo), cos(la) * sin(lo), sin(la))
	var rot := float(_a.rot0) + (TAU * (float(ct.t) - float(_a.rot_t)) / float(_a.period) if float(_a.period) != 0.0 else 0.0)
	var q := Vector3(cos(rot) * f.x - sin(rot) * f.y, sin(rot) * f.x + cos(rot) * f.y, f.z)
	# how far to the side of the place it comes down (m, + toward the orbit's normal)
	_a.entry_cross = (FlightPrograms.v3(ct.position).normalized() - _tdir(float(ct.t))).dot(h_burn) * float(_a.R)
	# and how far the air carried it off the orbit's plane: it turns with
	# the world, and on a tilted orbit it takes the ship with it, sideways
	_a.entry_drift = FlightPrograms.v3(ct.position).normalized().dot(h_burn) * float(_a.R)
	q = (q - h * q.dot(h)).normalized()
	return fposmod(atan2(r0.cross(q).dot(h), r0.dot(q)), TAU)


## The burn down's direction: retrograde, tilted `tilt` rad toward the
## orbit's normal (to come down to one side or the other of the track).
static func down_dir(t: Dictionary, tilt: float) -> Vector3:
	var v := v3(t.velocity)
	var n := v3(t.position).cross(v).normalized()
	return (-v.normalized() * cos(tilt) + n * sin(tilt)).normalized()


## A copy of the world to try a way down on (null if it won't load).
func _world_copy() -> TrsSim:
	var c := TrsSim.new()
	c.load_system(ProjectSettings.globalize_path(Sim.SYSTEM_PATH))
	c.load_catalog(ProjectSettings.globalize_path(Sim.CATALOG_PATH))
	for id in Sim.nest_crafts():
		c.nest_craft(id, FileAccess.get_file_as_string(Sim.ALIEN_CRAFT_DIR + id + ".craft.json"))
	return c if c.world_load(_s.core.world_save()) else null


## Coasting down to the air: where the ship would hand over to its engine
## (telemetry there, {} if it never does) with a kick of dv now (m/s, the
## telemetry's axes), on a copy of the world flown as the real one will be.
func _coast_down(dv: Vector3) -> Dictionary:
	var c := _world_copy()
	if c == null:
		return {}
	var id := _s.active
	c.vessel_set_mode(id, "physics")
	c.vessel_set_throttle(id, 0.0)
	c.vessel_nudge(id, PackedFloat64Array([dv.x, dv.y, dv.z]))
	var ct: Dictionary = {}
	for sec in 40000:
		ct = JSON.parse_string(c.telemetry(id))
		if not ct.get("alive", false):
			return {}
		if ct.get("landed", false) or _handover(ct):
			return ct
		if not ct.get("in_atmosphere", false):
			c.vessel_set_sas(id, "retrograde", PackedFloat64Array())
			var before := float(ct.t)
			c.warp(600.0, id)
			if float(JSON.parse_string(c.telemetry(id)).t) <= before + 1e-3:
				c.step_physics(50)
			continue
		c.vessel_set_sas(id, "retrograde" if float(ct.surface_speed) > 500.0 else "surface_retrograde", PackedFloat64Array())
		if FlightPrograms.shield_press(c, id, int(ct.get("next_stage", -1)), true):
			c.vessel_stage(id)
		c.step_physics(50)
	return {}


## Where a hand-over is from the place: (east, north) m on the ground.
func _miss(ct: Dictionary) -> Vector2:
	var R := float(_a.R)
	var dn := deg_to_rad(float(ct.lat_deg) - float(_a.tlat)) * R
	var de := deg_to_rad(wrapf(float(ct.lon_deg) - float(_a.tlon), -180.0, 180.0)) * R * cos(deg_to_rad(float(_a.tlat)))
	return Vector2(de, dn)


## "trim": on the way down to the air, a small burn (worked out on copies
## of the world: where it comes down as it is, and with a metre a second
## along the path and across it) that brings the hand-over over the place.
func _land_trim(t: Dictionary) -> void:
	if not _a.has("trim_dv"):
		_a.trim_dv = Vector3.ZERO
		var p0 := _coast_down(Vector3.ZERO)
		if p0.is_empty():
			_say("on the way down: the run on a copy of the world never came down (no trim)")
			phase = "entry"
			return
		var m0 := _miss(p0)
		if m0.length() < 800.0:
			_say("on the way down: coming down %s from %s, near enough" % [km(m0.length()), _a.tname])
			phase = "entry"
			return
		var pro := v3(t.velocity).normalized()
		var nrm := v3(t.position).cross(v3(t.velocity)).normalized()
		var pa := _coast_down(pro)
		var pb := _coast_down(nrm)
		if pa.is_empty() or pb.is_empty():
			phase = "entry"
			return
		var ja := _miss(pa) - m0
		var jb := _miss(pb) - m0
		var det := ja.x * jb.y - ja.y * jb.x
		if absf(det) < 1e-6:
			phase = "entry"
			return
		# (m0 + a ja + b jb = 0)
		var ka := (-m0.x * jb.y + m0.y * jb.x) / det
		var kb := (-ja.x * m0.y + ja.y * m0.x) / det
		var dv := pro * ka + nrm * kb
		# (far up the fall a little moves it a long way, and it's worth more)
		var ttp = t.get("orbit", {}).get("t_to_pe")
		var cap := 300.0 if ttp != null and float(ttp) > 1800.0 else 120.0
		if dv.length() > cap:
			dv = dv.normalized() * cap
		_a.trim_dv = dv
		_a.trim_start = float(t.get("dv_expended", 0.0))
		_say("on the way down: coming down %s from %s; a %.1f m/s trim" % [km(m0.length()), _a.tname, dv.length()])
	var want: Vector3 = _a.trim_dv
	var left := want.length() - (float(t.get("dv_expended", 0.0)) - float(_a.trim_start))
	if left <= 0.05 or float(t.get("thrust_max", 0.0)) <= 0.0:
		_thr(0.0)
		# (and look again: the path isn't quite a straight line in the kick)
		_a.trims = int(_a.get("trims", 0)) + 1
		if _a.trims < 3 and float(t.get("thrust_max", 0.0)) > 0.0 and not t.get("in_atmosphere", false):
			_a.erase("trim_dv")
			# (a long way to fall yet: the next look from half way there)
			var ttp = t.get("orbit", {}).get("t_to_pe")
			if ttp != null and float(ttp) > 1800.0:
				_warp_until(float(t.t) + float(ttp) * 0.5)
		else:
			phase = "entry"
		return
	_point(want)
	_thr(clampf(left / maxf(_accel(t) * 1.5, 0.01), 0.02, 1.0) if _aligned(t, want, 3.0) else 0.0)


## Coming down with air: whether the engine will do the last of it (so the
## chutes stay packed: under them there's no flying anywhere).
func _powered_finish(t: Dictionary) -> bool:
	return _a.get("target", false) and _finish_accel(t) > float(_a.g) * 1.6 or (_finish_accel(t) <= 0.0 and int(t.get("chutes", 0)) <= 0)


## The push (m/s2) of the engine that will fly the last of the way: the one
## lit, or the next stage's (a lander's engine waits under its heat shield).
func _finish_accel(t: Dictionary) -> float:
	if float(t.get("thrust_max", 0.0)) > 0.0:
		return _accel(t)
	for sg in t.get("stages", []):
		if float(sg.get("twr", 0.0)) > 0.0:
			return float(sg.twr) * float(_a.g)  # (its thrust to weight here, as it would start)
	return 0.0


## Out of the fire with a heat shield under the engine: on the chute, the
## shield dropped (slowed by the chute, the lander falls behind it and the
## shield away below), then the chute cut and the engine lit for the rest.
## A shield let go at speed would be caught up by the lander and hit.
func _land_shed(t: Dictionary) -> void:
	_thr(0.0)
	_sas("surface_retrograde")
	var now := float(t.t)
	match int(_a.shed):
		0:
			if int(t.get("chutes_stowed", 0)) > 0:
				_scene._deploy_chutes()
			_a.shed_t = now
			_a.shed = 1
		1:
			if float(t.surface_speed) < 30.0 or now - float(_a.shed_t) > 20.0 or int(t.get("chutes", 0)) == 0:
				_drop_shield(t)
				_a.shed_t = now
				_a.shed = 2
		2:
			if now - float(_a.shed_t) > 3.0:
				for p in JSON.parse_string(_s.core.vessel_part_states(_s.active)):
					if str(p.kind) == "parachute":
						_s.core.part_action(_s.active, int(p.uid), "cut", 0.0)
				phase = "home"
				_say("the chute cut: the rest on the engine")


## Through the air: a heat shield hung under the engine on a decoupler
## (with nobody aboard below it) has done its job, and goes.
func _drop_shield(t: Dictionary) -> void:
	if FlightPrograms.shield_press(_s.core, _s.active, int(t.get("next_stage", -1)), false):
		_scene._stage()
		_say("through the air: the heat shield dropped")


## Into the air: whatever hangs below a heat shield (the stage that made
## the burn down) goes first, so the shield meets the air.
func _clear_shield(t: Dictionary) -> void:
	if FlightPrograms.shield_press(_s.core, _s.active, int(t.get("next_stage", -1)), true):
		_scene._stage()
		_cool = STAGE_COOL
		_say("into the air: the stage under the heat shield dropped")


## Whether pressing stage `next` lights an engine with a heat shield still
## hung under it (on a decoupler below the engine, nobody aboard between).
static func fires_into_shield(core: TrsSim, id: int, next: int) -> bool:
	var parts: Array = JSON.parse_string(core.vessel_parts(id))
	var states: Array = JSON.parse_string(core.vessel_part_states(id))
	for i in parts.size():
		if not str(parts[i].part).begins_with("heat_shield"):
			continue
		var k := int(parts[i].parent)
		var held := false
		while k >= 0 and str(states[k].kind) != "pod":
			if str(states[k].kind) == "decoupler":
				held = true
			elif held and str(states[k].kind) == "engine" and int(states[k].stage) == next:
				return true
			k = int(parts[k].parent)
	return false


## Whether the next stage's press is a decoupler by a heat shield: the one
## under it (below: what hangs beneath the shield) or the one above it,
## with nobody aboard between (not below: the shield itself).
static func shield_press(core: TrsSim, id: int, next: int, below: bool) -> bool:
	if next < 0:
		return false
	var parts: Array = JSON.parse_string(core.vessel_parts(id))
	var states: Array = JSON.parse_string(core.vessel_part_states(id))
	for i in parts.size():
		if below:
			var k := int(parts[i].parent)
			if str(states[i].kind) == "decoupler" and int(states[i].stage) == next and k >= 0 and str(parts[k].part).begins_with("heat_shield"):
				return true
		elif str(parts[i].part).begins_with("heat_shield"):
			var k := int(parts[i].parent)
			while k >= 0 and str(states[k].kind) != "pod":
				if str(states[k].kind) == "decoupler":
					if int(states[k].stage) == next:
						return true
					break
				k = int(parts[k].parent)
	return false


## Low and slow enough through the air to fly the rest on the engine.
func _handover(t: Dictionary) -> bool:
	return float(t.altitude_terrain) < 6000.0 and float(t.surface_speed) < 300.0


## "home": flown on the engine to the place: braking from orbit when the
## stopping distance comes up, then across low, then down by it.
func _land_home(t: Dictionary) -> void:
	var up := v3(t.up)
	var vs := float(t.vertical_speed)
	var vh := v3(t.surface_velocity) - up * vs
	var off := _place_offset(t)
	var d := maxf(_ground_dist(t), 0.0)
	var dir := off.normalized() if off.length() > 0.01 else Vector3.ZERO
	var g := float(_a.g)
	var accel := _accel(t)
	var agl := float(t.altitude_terrain)
	var R := float(_a.R)
	var g_eff := maxf(g - vh.length_squared() / (R + float(t.altitude)), 0.0)
	var a_side := sqrt(maxf(accel * accel - g_eff * g_eff, 0.0)) * 0.8
	if not _a.get("braking", false):
		# coasting in: start braking when the run in (below) can only just
		# be flown in the time a steady brake would take from here
		_thr(0.0)
		_sas("surface_retrograde")
		var t_min := _run_in_time(t, v3(t.surface_velocity), up, g_eff, accel)
		var t_brake := 2.0 * d / maxf(vh.length(), 1.0)
		if (t_min > 0.0 and t_min >= t_brake * 0.9) or agl < 3000.0 or float(t.vertical_speed) < -150.0:
			_a.braking = true
			if not _a.get("legs", false):
				_scene.session.set_legs(true)
				_a.legs = true
		return
	if not _a.get("legs", false):
		_scene.session.set_legs(true)
		_a.legs = true
	if float(t.get("thrust_max", 0.0)) <= 0.0 and _cool <= 0.0 and int(t.get("next_stage", -1)) >= 0 and not _dry(t):
		_scene._stage()  # (the lander's engine, the shield gone)
		_cool = STAGE_COOL
		return
	if _dry(t):
		_fail("out of propellant %s from %s" % [km(d), _a.tname])
		return
	_autostage(t)
	if float(_a.standoff) > 0.0 and d < 3000.0 and not _a.get("stood_off", false):
		# (beside a relic, not on it: the spot short of it on the way in)
		var back := -dir * float(_a.standoff)
		_a.tlat = float(_a.tlat) + rad_to_deg(back.dot(v3(t.north)) / R)
		_a.tlon = float(_a.tlon) + rad_to_deg(back.dot(v3(t.east)) / (R * maxf(cos(deg_to_rad(float(t.lat_deg))), 0.01)))
		_a.stood_off = true
		return
	if d < 2500.0 and not _a.get("spot_aimed", false):
		# (the level ground for the legs found while still a way off, and
		# flown to: once down low over the place it is too late to go far)
		var lv: PackedFloat64Array = _s.core.level_spot(int(t.body_index), float(_a.tlat), float(_a.tlon), 300.0, LEVEL)
		_a.tlat = lv[0]
		_a.tlon = lv[1]
		_a.spot_aimed = true
		return
	if d < AT_PLACE and vh.length() < 3.0:
		var sp: PackedFloat64Array = _s.core.level_spot(int(t.body_index), float(_a.tlat), float(_a.tlon), 300.0, LEVEL)
		_a.spot = [sp[0], sp[1]]
		phase = "descent"
		_say("over the place: %s from %s, coming straight down" % [km(d), _a.tname])
		return
	# the rest by powered-descent guidance: the push that brings the ship
	# to rest HOME_OVER m over the place in T s (a straight run in, the
	# least fuel), with T the shortest the engine can fly at HOME_PUSH of
	# its thrust; again every frame as T shrinks
	var aim := _place_point(t)
	var vel := v3(t.surface_velocity)
	var T := _run_in_time(t, vel, up, g_eff, accel)
	var a_cmd := 6.0 * aim / (T * T) - 4.0 * vel / T + up * g_eff if T > 0.0 else Vector3.ZERO
	if T < 0.0:
		a_cmd = (-vel).normalized() * accel  # (none it can fly: brake hard)
	if (T > 0.0 and T < 2.5 and vel.length() < 6.0) or (d < AT_PLACE * 2.0 and vel.length() < 4.0):
		var sp2: PackedFloat64Array = _s.core.level_spot(int(t.body_index), float(_a.tlat), float(_a.tlon), 300.0, LEVEL)
		_a.spot = [sp2[0], sp2[1]]
		_clear_of_wreckage(t)
		phase = "descent"
		_say("over the place: %s from %s, coming straight down" % [km(d), _a.tname])
		return
	# the straight run in knows nothing of the hills on the way: low over
	# the ground under it, up first (the height comes before the distance)
	var floor_h := clampf(d * 0.03, 30.0, 600.0)
	# (and over the ground ahead: a hill coming up on the way in is seen
	# in time to climb it, not met at the speed of the run)
	var hs := vh.length()
	if hs > 5.0 and d > AT_PLACE * 2.0:
		for tau in [2.0, 4.0, 8.0, 15.0, 25.0]:
			var s_ahead := minf(hs * tau, d)
			var c: float = _s.core.ground_clearance(int(t.body_index), _s.active, PackedFloat64Array([vh.x / hs * s_ahead, vh.y / hs * s_ahead, vh.z / hs * s_ahead]))
			agl = minf(agl, c)
			if s_ahead >= d:
				break
	# (falling fast, the floor comes up sooner: in time to stop over it)
	var brake_up := maxf(accel - g_eff, 0.5) * 0.7
	var stop_h := vs * vs / (2.0 * brake_up) if vs < 0.0 else 0.0
	var low := agl < floor_h + stop_h and d > AT_PLACE * 2.0
	if low:
		var a_up_min := g_eff + 1.5 * ((floor_h - agl) * 0.3 - vs)
		if vs < 0.0:
			a_up_min = maxf(a_up_min, g_eff + vs * vs / (2.0 * maxf(agl - floor_h * 0.5, 1.0)))
		var a_up_now := a_cmd.dot(up)
		if a_up_now < a_up_min:
			a_cmd += up * (a_up_min - a_up_now)
		if a_cmd.length() > accel:
			var a_v := up * minf(a_cmd.dot(up), accel)
			var a_h2 := a_cmd - up * a_cmd.dot(up)
			var room2 := sqrt(maxf(accel * accel - a_v.length_squared(), 0.0))
			a_cmd = a_v + (a_h2.normalized() * minf(a_h2.length(), room2) if a_h2.length() > 0.01 else Vector3.ZERO)
	# (and low down, never leaning far over to push along: a tall ship
	# swung toward the horizon overshoots, noses down, and meets the ground;
	# the run in takes longer instead, planned again each frame)
	if float(t.altitude_terrain) < 600.0:
		var a_v := a_cmd.dot(up)
		var a_hz := a_cmd - up * a_v
		var most := maxf(a_v, g_eff * 0.5) * tan(deg_to_rad(35.0))
		if a_hz.length() > most:
			a_cmd = up * maxf(a_v, g_eff * 0.5) + a_hz.normalized() * most
	note = "%s from %s, there in %s" % [km(d), _a.tname, span(T) if T > 0.0 else "?"]
	_point(a_cmd)
	# (full push only once pointing the way: thrust the wrong way costs;
	# but low over the ground, whatever of the push the nose gives now,
	# while it turns: a big ship turns slowly, and falls meanwhile)
	# (and turning a long way round, as the push wanted swings over, the
	# part along the nose meanwhile: only the part across it is wasted)
	var nose := v3(t.nose)
	var a_dir := a_cmd.normalized() if a_cmd.length() > 0.01 else up
	if _aligned(t, a_cmd, 10.0):
		_thr(clampf(a_cmd.length() / maxf(accel, 0.01), 0.0, 1.0))
	elif nose.dot(a_dir) > 0.5 or (low and nose.dot(up) > 0.3):
		_thr(clampf(a_cmd.dot(nose) / maxf(accel, 0.01), 0.0, 1.0))
	elif float(t.altitude_terrain) < 600.0 and nose.dot(up) > 0.0:
		# (low down, never quite off while it turns: the engine's gimbal is
		# what turns a big ship, and with it off the turn runs on past)
		_thr(0.2)
	else:
		_thr(0.0)


## The spot for the legs moved off anything lying on the ground there
## (the heat shield it dropped falls about where it was let go).
func _clear_of_wreckage(t: Dictionary) -> void:
	var R := float(_a.R)
	var off := _spot_offset(t)
	for th in JSON.parse_string(_s.core.things_near(_s.active, 400.0)):
		if str(th.get("what", "")) != "ship":
			continue
		var b := deg_to_rad(float(th.bearing))
		var at := v3(t.east) * sin(b) * float(th.distance) + v3(t.north) * cos(b) * float(th.distance)
		var gap: Vector3 = off - at
		if gap.length() < 20.0:
			var away := gap.normalized() if gap.length() > 0.1 else v3(t.east)
			var move := away * (25.0 - gap.length())
			_a.spot = [float(_a.spot[0]) + rad_to_deg(move.dot(v3(t.north)) / R),
				float(_a.spot[1]) + rad_to_deg(move.dot(v3(t.east)) / (R * maxf(cos(deg_to_rad(float(t.lat_deg))), 0.01)))]
			_say("the spot moved %.0f m off %s lying there" % [move.length(), th.get("name", "wreckage")])
			return


const HOME_OVER := 40.0  # m over the place the run in ends, to come straight down from
const HOME_PUSH := 0.7   # of the engine's thrust the run in plans on (the rest steers)
const HOME_SLOPE := 0.15 # m of height over the place aimed for per m still to go


## The shortest run in (s) the engine can fly at HOME_PUSH of its thrust,
## or -1 if none (2 s to 50 min). The push of a run in T s, starting at
## a0, ramps evenly to a1 at the end: the least for coming to rest there.
func _run_in_time(t: Dictionary, vel: Vector3, up: Vector3, g_eff: float, accel: float) -> float:
	var aim := _place_point(t)
	var tt := 2.0
	while tt < 3000.0:
		# (the push at the start and at the end both within reach: it ramps
		# from one to the other)
		var a0 := 6.0 * aim / (tt * tt) - 4.0 * vel / tt + up * g_eff
		var a1 := -6.0 * aim / (tt * tt) + 2.0 * vel / tt + up * g_eff
		if a0.length() <= accel * HOME_PUSH and a1.length() <= accel * HOME_PUSH:
			return tt
		tt *= 1.06
	return -1.0


## From the ship to HOME_OVER m over the place (m, the telemetry's axes):
## the ground there by how far a point over it clears it.
func _place_point(t: Dictionary) -> Vector3:
	var R := float(_a.R)
	var dir := _tdir(float(t.t))
	var pos := v3(t.position)
	var probe := dir * (R + 20000.0) - pos
	var clear := float(_s.core.ground_clearance(int(t.body_index), _s.active, PackedFloat64Array([probe.x, probe.y, probe.z])))
	var ground := 20000.0 - clear
	# (over it on a glide slope: high while far, so the hills between stay below)
	return dir * (R + ground + HOME_OVER + _ground_dist(t) * HOME_SLOPE) - pos

# ---- the Stillwater Drive --------------------------------------------------------

const DRIVE_G := 9.80665  # the drive's most inside a heliopause (1 g)


func _begin_drive(w: PackedStringArray, t: Dictionary) -> String:
	if float(t.get("drive_max", 0.0)) <= 0.0:
		return "no Stillwater Drive aboard"
	if t.get("interstellar", false):
		return "between the stars the COURSE panel flies the drive: this is for the worlds inside a heliopause"
	if w.size() < 2:
		return "drive_to <body> [orbit height, m]"
	var to := _body_named(w[1])
	if to < 0:
		return "no body called '%s'" % w[1]
	var info: Dictionary = JSON.parse_string(_s.core.body_info(to))
	if int(info.get("star", -1)) != int(t.get("star", -2)):
		return "%s is round another star: set a course there first (COURSE, out past the heliopause)" % info.name
	if t.get("landed", false) or t.get("in_atmosphere", false):
		return "up out of the air first (launch)"
	if int(t.body_index) == to and t.get("orbit", {}).get("ap_alt") != null:
		return "already in orbit round %s" % info.name
	_a.to = to
	_a.to_parent = int(info.get("parent", -1))
	_a.to_name = str(info.name)
	_a.mu = float(info.mu)
	var R := float(info.radius)
	var atm := float(info.get("atmosphere_height", 0.0))
	# (low enough for a Signal Scanner's 3000 km, clear of the air)
	_a.alt = float(w[2]) if w.size() > 2 and w[2].is_valid_float() else clampf(R * 0.5, atm + 100000.0, 2000000.0)
	_a.r_stop = R + float(_a.alt)
	phase = "cruise"
	note = "on the drive to %s" % info.name
	_say("on the drive to %s, into a %s orbit" % [info.name, km(_a.alt)])
	return ""


## Ship and target in the galaxy's axes: [where the target is from the
## ship, the ship's velocity against the target's].
func _drive_rel(t: Dictionary) -> Array:
	# (from the star, not the galaxy: round another star galactic
	# coordinates are light-years long, and too coarse to fly by)
	var br: PackedFloat64Array = _s.core.body_rel(_s.active, int(_a.to))
	if br.size() < 6:
		return [Vector3.ZERO, Vector3.ZERO]
	return [Vector3(br[0], br[1], br[2]), -Vector3(br[3], br[4], br[5])]


## The drive pushing `a` (m/s2, the SOI axes), the engines quiet, for dt s
## of warp; the nose turned that way first (on rails it holds there).
func _drive_push(a: Vector3, dt: float) -> void:
	var amax := minf(float(_s.telemetry().get("drive_max", 0.0)), DRIVE_G)
	_scene._throttle = 0.0
	_s.core.vessel_set_throttle(_s.active, 0.0)
	if a.length() > 1e-6:
		_point(a)
		_s.core.vessel_set_sas(_s.active, "target", PackedFloat64Array([a.normalized().x, a.normalized().y, a.normalized().z]))
	_s.core.vessel_set_drive(_s.active, clampf(a.length() / maxf(amax, 1e-6), 0.0, 1.0))
	if dt < 2.0:
		_s.step_ticks(maxi(int(ceil(dt * FlightSession.PHYSICS_HZ)), 1))  # (too short to warp)
	else:
		_warp_until(_s.time() + dt)
	_s.core.vessel_set_drive(_s.active, 0.0)


func _drive(t: Dictionary) -> void:
	if t.get("interstellar", false):
		# (out past the heliopause the drive pushes 100 g: not this program's)
		_s.core.vessel_set_drive(_s.active, 0.0)
		_fail("out past %s's heliopause: too fast in to stop by %s" % [t.get("star_name", "the star"), _a.to_name])
		return
	var amax := minf(float(t.get("drive_max", 0.0)), DRIVE_G)
	if amax <= 0.0:
		_fail("the drive has stopped")
		return
	if not t.get("powered", true):
		_fail("no power for the drive (it wants 40 MW: a Hearth Core)")
		return
	var rel := _drive_rel(t)
	var d: Vector3 = rel[0]
	var vr: Vector3 = rel[1]
	var R_stop := float(_a.r_stop)
	if phase == "cruise" and int(t.body_index) != int(_a.to) and int(t.body_index) != int(_a.get("to_parent", -1)) \
			and int(_body(t).get("parent", -1)) >= 0 and _body(t).get("star", -1) != int(t.body_index):
		# out of this world's pull first (straight at the target could be
		# straight through it): along the orbit, round and out
		note = "out of %s's pull on the drive" % _body(t).get("name", "?")
		_drive_push(v3(t.velocity).normalized() * amax, clampf(v3(t.position).length() / maxf(v3(t.velocity).length(), 100.0) * 0.02, 5.0, 600.0))
		return
	if phase == "cruise":
		var dist := d.length() - R_stop
		if int(t.body_index) == int(_a.to) and dist < R_stop * 2.0 and vr.length() < sqrt(float(_a.mu) / R_stop) * 3.0:
			phase = "orbit"
			_say("at %s: %s out, %.0f m/s against it" % [_a.to_name, km(d.length()), vr.length()])
			return
		# a closing speed it can still brake from by the stopping point
		var v_des := d.normalized() * sqrt(2.0 * amax * 0.7 * maxf(dist, 0.0))
		var dv := v_des - vr
		var ttg := maxf(dist, 1.0) / maxf(vr.length(), 50.0)
		var dt := clampf(ttg * 0.01, 1.0, 3000.0)  # (the push eased to what the step needs: no overshoot)
		note = "%s to %s, closing at %s/s" % [km(dist), _a.to_name, km(vr.dot(d.normalized()))]
		_drive_push(dv.normalized() * minf(amax, dv.length() / dt), dt)
		return
	# "orbit": a circle at the stopping height, in the plane it came in on
	var r := v3(t.position)
	var v := v3(t.velocity)
	var rh := r.normalized()
	var h := r.cross(v)
	if h.length() < 1e-3 * r.length() * maxf(v.length(), 1.0):
		h = r.cross(Vector3(0, 0, 1)) if absf(rh.z) < 0.9 else r.cross(Vector3(1, 0, 0))
	var th := h.normalized().cross(rh)
	var v_des := th * sqrt(float(_a.mu) / r.length()) + rh * clampf((R_stop - r.length()) * 0.003, -300.0, 300.0)
	var dv := v_des - v
	if dv.length() < 1.0 and absf(r.length() - R_stop) < R_stop * 0.03:
		_s.core.vessel_set_drive(_s.active, 0.0)
		_done("in orbit round %s: %s" % [_a.to_name, _orbit_text(t)])
		return
	var dt := clampf(dv.length() / amax, 0.5, 20.0)
	note = "into orbit round %s, %.0f m/s to go" % [_a.to_name, dv.length()]
	_drive_push(dv.normalized() * minf(amax, dv.length() / dt), dt)


# ---- hover --------------------------------------------------------------------------------

func _begin_hover(w: PackedStringArray, t: Dictionary) -> String:
	if w.size() < 2 or not w[1].is_valid_float():
		return "hover <metres above the ground> [seconds]"
	_a.h = float(w[1])
	_a.hold = float(w[2]) if w.size() > 2 and w[2].is_valid_float() else 1e12  # s, counted once at the height
	_a.until = -1.0
	_a.g = float(_body(t).surface_gravity)
	if t.get("landed", false) and float(t.get("thrust_max", 0.0)) <= 0.0:
		_scene._stage()
	note = "%d m above the ground" % roundi(_a.h)
	return ""


func _hover(t: Dictionary) -> void:
	var dh := float(_a.h) - float(t.altitude_terrain)
	if float(_a.until) < 0.0 and absf(dh) < maxf(2.0, float(_a.h) * 0.02):
		_a.until = float(t.t) + float(_a.hold)
		note = "holding %d m above the ground" % roundi(_a.h)
	if float(_a.until) >= 0.0 and float(t.t) >= float(_a.until):
		_done("hovered at %d m" % roundi(float(t.altitude_terrain)))
		return
	if _dry(t):
		_fail("out of propellant hovering")
		return
	var accel := _accel(t)
	var up := v3(t.up)
	var vs := float(t.vertical_speed)
	# (up as fast as 40 m/s from far below; down never past 8)
	var vt := clampf(dh * 0.4, -8.0, 40.0)
	var vh := v3(t.surface_velocity) - up * vs
	var a_up: float = float(_a.g) + 1.5 * (vt - vs)
	var a_h := -vh * 0.5
	var want := up * maxf(a_up, 0.0) + a_h
	_point(want if want.length() > 0.01 else up)
	_thr(clampf(want.length() / maxf(accel, 0.01), 0.0, 1.0))
