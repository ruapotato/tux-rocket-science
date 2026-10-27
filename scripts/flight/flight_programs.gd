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
##   hover <height> [seconds]   holds a height above the ground
##   node                       flies the next maneuver node on the map
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
		"land": why = _begin_land(t)
		"hover": why = _begin_hover(words, t)
		"node": why = _begin_node(t)
		_: why = "no such program: " + name
	if why != "":
		_end("failed", why)


## WORKED OUT, NOT FLOWN: what a script's when_to_leave("krill") asks. The
## burn to that body from this orbit: {wait (s until it), at (when), dv,
## prograde, normal, radial (m/s), travel (s from the burn into its
## sphere), pass (m up, as planned), body}, or {error}.
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
	var node := []
	if moon:
		node = _plan_moon(t, str(info.id), want)
	else:
		var r: PackedFloat64Array = _s.core.plan_transfer(_s.active, to, want, false)
		if r.size() >= 6 and r[4] > 0.5:
			node = [r[0], r[1], r[2], r[3]]
	if node.is_empty():
		return {"error": "no way to %s found from this orbit" % info.name}
	var pp := _patch_in(node, str(info.id), 6.0e5 if moon else 1.0e8)
	return {"body": str(info.name), "wait": float(node[0]) - float(t.t), "at": float(node[0]),
		"dv": Vector3(node[1], node[2], node[3]).length(), "prograde": node[1], "normal": node[2], "radial": node[3],
		"travel": float(pp.get("t_start", node[0])) - float(node[0]), "pass": float(pp.get("pe_alt", -1.0))}


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
	_end("failed", what)


# ---- each frame ---------------------------------------------------------------

func step(scene: Node, t: Dictionary) -> void:
	if not running():
		return
	_scene = scene
	_s = scene.session
	if not t.get("alive", false):
		_fail("the ship was lost")
		return
	var now := float(t.t)
	var dt := maxf(now - _last_t, 0.0)
	_last_t = now
	_cool -= dt
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


# ---- controls -------------------------------------------------------------------

func _thr(x: float) -> void:
	_scene._throttle = x
	_s.set_throttle(x)


func _point(d: Vector3) -> void:
	var n := d.normalized()
	_scene._sas_follow = ""
	_s.set_sas("target", PackedFloat64Array([n.x, n.y, n.z]))


func _sas(mode: String) -> void:
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
	if not (t.get("landed", false) or str(t.situation) in ["prelaunch", "landed", "splashed"]):
		return "launch is from the ground (the ship is %s)" % t.situation
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
	if _a.moon:
		node = _plan_moon(t, str(info.id), want)
	else:
		var r: PackedFloat64Array = _s.core.plan_transfer(_s.active, to, want, false)
		if r.size() >= 6 and r[4] > 0.5:
			node = [r[0], r[1], r[2], r[3]]
	if node.is_empty():
		return "no way to %s found from this orbit" % info.name
	_a.node = node
	_s.nodes = [node]
	var pp := _patch_in(node, str(info.id), 6.0e5 if _a.moon else 1.0e8)
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


## A small three-axis correction a while out, if the pass has drifted.
func _plan_correction(t: Dictionary) -> Array:
	var body: String = _a.to_id
	var want: float = _a.want
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
	if phase in ["warp", "align", "burn"]:
		if _fly_node(t):
			_s.nodes = []
			if int(_a.get("corrections", 0)) >= 2 or not _a.get("moon", false):
				_finish_transfer()
				return
			phase = "coast"
		return
	if phase == "coast":
		# corrections: a little way out, and again halfway, if the pass has
		# drifted (a long burn is never quite the planned instant)
		var first := int(_a.get("corrections", 0)) == 0
		if first:
			_warp_until(float(t.t) + 600.0)
		else:
			var p := _patch_in([_s.time() + 1.0, 0.0, 0.0, 0.0], _a.to_id)
			if not p.is_empty():
				_warp_until(_s.time() + (float(p.t_start) - _s.time()) * 0.5)
		_a.corrections = int(_a.get("corrections", 0)) + 1
		var c := _plan_correction(_s.telemetry())
		if not c.is_empty():
			_say("correction: %.1f m/s, then passing %s up" % [Vector3(c[1], c[2], c[3]).length(), km(float(_patch_in(c, _a.to_id).get("pe_alt", -1.0)))])
		if c.is_empty():
			_finish_transfer()
			return
		_a.node = c
		phase = "warp"


func _finish_transfer() -> void:
	var p := _patch_in([_s.time() + 1.0, 0.0, 0.0, 0.0], _a.to_id, 6.0e5 if _a.get("moon", false) else 1.0e8)
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
	if _a.want_ap < float(o.get("pe_alt", 0.0)):
		_a.want_ap = float(o.pe_alt) + 1000.0
	phase = "warp"
	note = "into orbit of %s" % _body(t).get("name", "?")
	return ""


func _capture(t: Dictionary) -> void:
	var o: Dictionary = t.get("orbit", {})
	var b := _body(t)
	match phase:
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

func _begin_land(t: Dictionary) -> String:
	if t.get("landed", false) and float(t.surface_speed) < 1.0:
		return "the ship is on the ground already"
	var b := _body(t)
	_a.atm = float(b.get("atmosphere_height", 0.0))
	_a.g = float(b.surface_gravity)
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
	# the last few metres at a walking pace, legs first
	var vt := -minf(1.0 + sqrt(2.0 * brake * maxf(agl - 6.0, 0.0)), 250.0)
	var up := v3(t.up)
	var vs := float(t.vertical_speed)
	var vh := v3(t.surface_velocity) - up * vs
	var a_up := g + 1.2 * (vt - vs)
	var a_h := -vh * 0.6
	var tilt := maxf(a_up, 0.0) * tan(deg_to_rad(35.0))
	if a_h.length() > tilt:
		a_h = a_h.normalized() * tilt
	if a_up <= 0.0:
		_point(up)
		return 0.0
	var want := up * a_up + a_h
	_point(want)
	if agl < 2.0 and absf(vs) < 1.5:
		return 0.0
	return clampf(want.length() / accel, 0.0, 1.0)


func _land(t: Dictionary) -> void:
	var o: Dictionary = t.get("orbit", {})
	var agl := float(t.altitude_terrain)
	if phase != "settle" and t.get("landed", false) and float(t.surface_speed) < 2.0:
		phase = "settle"
	match phase:
		"deorbit":
			# drop the low point deep into the air, then let it do the work
			_sas("retrograde")
			if float(o.get("pe_alt", 0.0)) < _a.atm * 0.4:
				_thr(0.0)
				phase = "entry"
				_warp_until(float(t.t) + 1.0e6)  # (stops at the air)
				return
			if _dry(t):
				_fail("out of propellant coming down: %s" % _orbit_text(t))
				return
			_autostage(t)
			_thr(1.0 if _aligned(t, dir_of(t, "retrograde"), 8.0) else 0.0)
		"entry":
			_thr(0.0)
			_sas("retrograde" if t.get("in_atmosphere", false) and float(t.surface_speed) > 500.0 else "surface_retrograde")
			if float(t.altitude) < _a.atm * 0.45 and float(t.vertical_speed) < 0.0 and not _a.get("chutes", false):
				if int(t.get("chutes", 0)) > 0:
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
			phase = "brake"
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
			_autostage(t)
			_thr(_descend(t))
		"settle":
			_thr(0.0)
			_sas("stability")
			if float(t.surface_speed) < 0.3:
				_a.still = float(_a.get("still", 0.0)) + 1.0
			if t.get("landed", false) and float(_a.get("still", 0.0)) > 20.0:
				_done("down on %s at %.2f, %.2f, %s, %.1f m/s at touchdown" % [
					_body(t).get("name", "?"), float(t.lat_deg), float(t.lon_deg), "upright" if float(t.pitch_deg) > 60.0 else "NOT upright",
					float(t.get("max_impact", 0.0))])


# ---- hover --------------------------------------------------------------------------------

func _begin_hover(w: PackedStringArray, t: Dictionary) -> String:
	if w.size() < 2 or not w[1].is_valid_float():
		return "hover <metres above the ground> [seconds]"
	_a.h = float(w[1])
	_a.until = float(t.t) + (float(w[2]) if w.size() > 2 and w[2].is_valid_float() else 1e12)
	_a.g = float(_body(t).surface_gravity)
	if t.get("landed", false) and float(t.get("thrust_max", 0.0)) <= 0.0:
		_scene._stage()
	note = "%d m above the ground" % roundi(_a.h)
	return ""


func _hover(t: Dictionary) -> void:
	if float(t.t) >= float(_a.until):
		_done("hovered at %d m" % roundi(float(t.altitude_terrain)))
		return
	if _dry(t):
		_fail("out of propellant hovering")
		return
	var accel := _accel(t)
	var up := v3(t.up)
	var vs := float(t.vertical_speed)
	var vt := clampf((float(_a.h) - float(t.altitude_terrain)) * 0.4, -8.0, 8.0)
	var vh := v3(t.surface_velocity) - up * vs
	var a_up: float = float(_a.g) + 1.5 * (vt - vs)
	var a_h := -vh * 0.5
	var want := up * maxf(a_up, 0.0) + a_h
	_point(want if want.length() > 0.01 else up)
	_thr(clampf(want.length() / maxf(accel, 0.01), 0.0, 1.0))
