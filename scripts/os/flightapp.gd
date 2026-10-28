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
# flightapp.gd -- the ship, from the ship's computer.
#
# Three things in one window: what the flight panels say (read off the
# machine, the same table `ship status` prints), a button for each thing the
# ship can be told, and the scripts on the disk that fly it.
#
# EVERY BUTTON IS A LINE OF TEXT. Pressing THROTTLE 100% runs `ship throttle
# 1` on the machine's own shell, through /bin/ship, exactly as typing it in a
# terminal would, and the console strip at the bottom prints the line and
# the answer. So the line is always there to copy into a script.

extends Control

var mono: Font
var machine: Object = null
var ship: Node = null            # the flight scene, for the scripts' log

const CHROME := Color("#d6d3ce")
const FACE := Color("#e4e1dc")
const WHITE := Color("#ffffff")
const EDGE_L := Color("#f4f2ef")
const EDGE_D := Color("#8e8b86")
const INK := Color("#1b1b1b")
const DIM := Color("#5f6469")
const SEL := Color("#3465a4")
const GOOD := Color("#2e7d32")
const BAD := Color("#b0281a")
const TERM_BG := Color("#12161c")
const TERM_FG := Color("#d7dee6")
const PAD := 8.0
const FS := 13

const SHOWN := [
	["situation", "", "situation"], ["body", "", "over"], ["altitude", "m", "altitude"],
	["altitude_terrain", "m", "above ground"], ["apoapsis", "m", "apoapsis"], ["periapsis", "m", "periapsis"],
	["time_to_apoapsis", "s", "to apoapsis"], ["vertical_speed", "m/s", "vertical speed"],
	["orbital_speed", "m/s", "orbital speed"], ["pitch", "deg", "pitch"], ["heading", "deg", "heading"],
	["throttle_pct", "%", "throttle"], ["thrust_kn", "kN", "thrust"], ["g_force", "g", "felt"],
	["mass", "kg", "mass"], ["liquid", "kg", "propellant"], ["dv_left", "m/s", "delta-v left"],
	["ec", "kJ", "battery"], ["sas", "", "SAS"], ["rcs", "", "RCS"],
	["encounter", "", "path meets"], ["program", "", "program"], ["program_status", "", "  status"],
]

## label, the line it runs
const BUTTONS := [
	["Throttle 0", "ship throttle 0"], ["50%", "ship throttle 0.5"], ["100%", "ship throttle 1"],
	["STAGE", "ship stage"], ["Chutes", "ship chutes"], ["Legs", "ship legs"],
	["SAS hold", "ship sas stability"], ["Prograde", "ship sas prograde"], ["Retrograde", "ship sas retrograde"],
	["Radial out", "ship sas radial_out"], ["Normal", "ship sas normal"], ["SAS off", "ship sas off"],
	["Point up", "ship point 90 0"], ["Point 45 E", "ship point 45 90"], ["RCS on", "ship rcs on"],
	["RCS off", "ship rcs off"], ["Spin on", "ship spin on"], ["Spin off", "ship spin off"],
	# the programs: the computer flies these itself (any flying key takes over)
	["LAUNCH", "ship launch"], ["Circularize", "ship circularize"], ["Warp to Ap", "ship warp apoapsis"],
	["To Krill", "ship transfer krill"], ["Capture", "ship capture"], ["LAND", "ship land"],
	["Hover 20 m", "ship hover 20 20"], ["Burn node", "ship node"], ["ABORT", "ship abort"],
]

const PROGRAMS := ["launch", "circularize", "warp", "transfer", "capture", "land", "hover", "node"]

var _state := {}
var _console: Array[String] = []
var _scripts: Array = []      # [path, ...] on the disk
var _running: Array = []      # [pid, path]
var _hot := -1
var _hot_script := -1
var _since := 0.0
var _seen_log := 0
var _chart: ChartView      # what scripts chart(), under the readings


func _ready() -> void:
	focus_mode = Control.FOCUS_ALL
	mouse_filter = Control.MOUSE_FILTER_STOP
	if mono == null:
		mono = preload("res://scripts/os/uifont.gd").mono()
	_chart = ChartView.new()
	_chart.dark = false
	_chart.font = mono
	_chart.visible = false
	add_child(_chart)
	_refresh()


func _process(dt: float) -> void:
	_since += dt
	if _since > 0.25:
		_since = 0.0
		_refresh()
	# what the scripts have said (and done), from the flight
	if ship and ship.get("computer") != null:
		var lg: Array = ship.computer.log
		var fresh := mini(int(ship.computer.log_count) - _seen_log, lg.size())
		for k in range(lg.size() - maxi(fresh, 0), lg.size()):
			_say(str(lg[k]))
		_seen_log = int(ship.computer.log_count)
		# the charts, in the left column under the readings
		var ch: Dictionary = ship.computer.charts
		_chart.visible = not ch.is_empty()
		if _chart.visible:
			var c := _cols()
			var cr := _console_rect()
			_chart.position = Vector2(c[0], cr.position.y - 128)
			_chart.size = Vector2(c[1] - c[0], 118)
			_chart.set_series(ch)
	queue_redraw()


func _refresh() -> void:
	if machine == null:
		return
	var env: String = machine.api("ship")
	var lines := env.split("\n")
	if lines.size() > 1:
		var d = JSON.parse_string(lines[1])
		if d is Dictionary:
			_state = d
	_scripts.clear()
	for dir in ["/root/examples", "/root/scripts"]:
		for n in str(machine.list_dir(dir)).split("\n", false):
			if n.ends_with(".py") or n.ends_with(".sh"):
				_scripts.append(dir + "/" + n)
	_running.clear()
	# (the machine's list of scripts: ps shows /bin/py, not the script, so the
	# RUN marks were never lit)
	for row in str(machine.scripts()).split("\n", false):
		var f := row.split("\t")
		if f.size() >= 4 and f[1] == "1":
			_running.append([int(f[0]), f[3]])


func _say(line: String) -> void:
	_console.append(line)
	if _console.size() > 60:
		_console = _console.slice(_console.size() - 60)


func _run(line: String) -> void:
	_say("$ " + line)
	var out: String = machine.sh_on(0, line) if machine else "the computer is off\n"
	for l in out.split("\n", false):
		_say(l)
	_refresh()


# ---- layout -------------------------------------------------------------

func _cols() -> Array:
	var w := size.x
	return [PAD, w * 0.4, w * 0.4 + PAD, w - PAD]


func _button_rect(i: int) -> Rect2:
	var c := _cols()
	var x0: float = c[2]
	var bw: float = (c[3] - x0 - 2 * 4.0) / 3.0
	var bh := 24.0
	return Rect2(x0 + (i % 3) * (bw + 4.0), PAD + 20.0 + (i / 3) * (bh + 4.0), bw, bh)


func _scripts_top() -> float:
	return _button_rect(BUTTONS.size() - 1).end.y + 26.0


func _script_rect(i: int) -> Rect2:
	var c := _cols()
	return Rect2(c[2], _scripts_top() + i * 19.0, c[3] - c[2], 18.0)


func _console_rect() -> Rect2:
	var h := maxf(100.0, size.y * 0.17)
	return Rect2(PAD, size.y - h - PAD, size.x - 2 * PAD, h)


# ---- drawing ------------------------------------------------------------

func _draw() -> void:
	draw_rect(Rect2(Vector2.ZERO, size), CHROME)
	var c := _cols()
	# readings
	draw_string(mono, Vector2(c[0], PAD + 12), "THE SHIP", HORIZONTAL_ALIGNMENT_LEFT, -1, FS, SEL)
	var y := PAD + 32.0
	var lim := _console_rect().position.y - (136.0 if _chart and _chart.visible else 8.0)
	for row in SHOWN:
		if y > lim:
			break
		var v := str(_state.get(row[0], "-"))
		draw_string(mono, Vector2(c[0], y), row[2], HORIZONTAL_ALIGNMENT_LEFT, -1, FS, DIM)
		draw_string(mono, Vector2(c[0] + 130, y), v + (" " + row[1] if row[1] != "" and v != "-" else ""),
			HORIZONTAL_ALIGNMENT_LEFT, -1, FS, INK)
		y += 17.0
	var pn := str(_state.get("program_note", "-"))
	if pn != "-" and y < lim:
		draw_string(mono, Vector2(c[0], y), pn, HORIZONTAL_ALIGNMENT_LEFT, c[1] - c[0], FS - 1, SEL)
	# controls
	draw_string(mono, Vector2(c[2], PAD + 12), "TELL IT", HORIZONTAL_ALIGNMENT_LEFT, -1, FS, SEL)
	for i in BUTTONS.size():
		var r := _button_rect(i)
		_bevel(r, i == _hot)
		draw_string(mono, Vector2(r.position.x + 4, r.position.y + 16), BUTTONS[i][0], HORIZONTAL_ALIGNMENT_CENTER,
			r.size.x - 8, FS, BAD if BUTTONS[i][0] in ["STAGE", "ABORT"] else (SEL if BUTTONS[i][1].split(" ")[1] in PROGRAMS else INK))
	# scripts
	var st := _scripts_top()
	draw_string(mono, Vector2(c[2], st - 6), "SCRIPTS  (click to run, again to stop)", HORIZONTAL_ALIGNMENT_LEFT, -1, FS, SEL)
	for i in _scripts.size():
		var r := _script_rect(i)
		if r.end.y > lim:
			break
		var pid := _pid_of(_scripts[i])
		draw_rect(r, WHITE if i != _hot_script else Color("#dbe6f4"))
		draw_string(mono, Vector2(r.position.x + 4, r.position.y + 13), ("RUN %d  " % pid if pid >= 0 else "") + str(_scripts[i]),
			HORIZONTAL_ALIGNMENT_LEFT, r.size.x - 8, FS - 1, GOOD if pid >= 0 else INK)
	# the console: every click a line
	var cr := _console_rect()
	draw_rect(cr, TERM_BG)
	var lh := 15.0
	var n := int((cr.size.y - 6) / lh)
	var first := maxi(0, _console.size() - n)
	for k in range(first, _console.size()):
		var line := _console[k]
		draw_string(mono, Vector2(cr.position.x + 6, cr.position.y + 14 + (k - first) * lh), line, HORIZONTAL_ALIGNMENT_LEFT,
			cr.size.x - 12, FS - 1, Color("#8fd18f") if line.begins_with("$ ") else TERM_FG)


func _bevel(r: Rect2, hot: bool) -> void:
	draw_rect(r, FACE.darkened(0.08) if hot else FACE)
	draw_line(r.position, Vector2(r.end.x, r.position.y), EDGE_L)
	draw_line(r.position, Vector2(r.position.x, r.end.y), EDGE_L)
	draw_line(Vector2(r.position.x, r.end.y), r.end, EDGE_D)
	draw_line(Vector2(r.end.x, r.position.y), r.end, EDGE_D)


func _pid_of(path: String) -> int:
	for r in _running:
		if r[1] == path:
			return r[0]
	return -1


# ---- input --------------------------------------------------------------

func _gui_input(e: InputEvent) -> void:
	if e is InputEventMouseMotion:
		var p: Vector2 = (e as InputEventMouseMotion).position
		_hot = -1
		_hot_script = -1
		for i in BUTTONS.size():
			if _button_rect(i).has_point(p):
				_hot = i
		for i in _scripts.size():
			if _script_rect(i).has_point(p):
				_hot_script = i
	elif e is InputEventMouseButton and (e as InputEventMouseButton).pressed and (e as InputEventMouseButton).button_index == MOUSE_BUTTON_LEFT:
		var p: Vector2 = (e as InputEventMouseButton).position
		for i in BUTTONS.size():
			if _button_rect(i).has_point(p):
				_run(BUTTONS[i][1])
				accept_event()
				return
		for i in _scripts.size():
			if _script_rect(i).has_point(p):
				var pid := _pid_of(_scripts[i])
				_run("kill %d" % pid if pid >= 0 else "ship run " + str(_scripts[i]))
				accept_event()
				return
