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
## A crew penguin's live portrait, drawn procedurally (our own penguins, no
## sprites). The face is a blend of continuous parameters that ease toward
## the current mood, so reactions flow instead of snapping:
##   calm · thrilled · squished · panicking · dizzy · weightless · cheering
class_name PenguinPortrait
extends Control

var member: Dictionary = {}
var mood := "calm"
var mood_text := ""
var g := 1.0

# eased face parameters
var _eye_open := 1.0     # 0 shut .. 1 open .. 1.5 wide
var _pupil := 1.0        # pupil size
var _beak_open := 0.0
var _squash := 0.0       # +squished by G, -stretched
var _flipper := 0.0      # -1 pressed down .. 1 raised
var _flap := 0.0         # flapping amplitude
var _blush := 0.0
var _sweat := 0.0
var _tremble := 0.0
var _float := 0.0        # weightless drift
var _spin := 0.0         # dizzy swirl
var _happy_eyes := 0.0   # ^ ^ eyes
var _brow := 0.0         # -1 worried .. 1 determined
var _time := 0.0
var _blink := 0.0
var _next_blink := 2.0
var _event := ""
var _event_t := 0.0
var _sky := Color(0.4, 0.6, 0.9)
var _rng := RandomNumberGenerator.new()


func _ready() -> void:
	if custom_minimum_size == Vector2.ZERO:
		custom_minimum_size = Vector2(150, 170)
	mouse_filter = Control.MOUSE_FILTER_IGNORE


func set_member(m: Dictionary) -> void:
	member = m
	_rng.seed = int(m.get("seed", 1))


## Short reactions on top of the continuous mood: "stage", "boom", "landed",
## "orbit", "soi".
func react(kind: String, secs := 3.0) -> void:
	_event = kind
	_event_t = secs


## Feeds flight state. t: telemetry dictionary; spin: rad/s of tumbling.
func update_state(t: Dictionary, spin_rate: float, delta: float) -> void:
	_time += delta
	g = t.get("g_force", 0.0)
	var nerve: float = member.get("nerve", 0.5)
	var glee: float = member.get("glee", 0.5)
	var landed: bool = t.get("landed", false)
	var freefall: bool = not landed and g < 0.08
	var agl: float = t.get("altitude_terrain", 1e9)
	var vs: float = t.get("vertical_speed", 0.0)
	var danger: bool = not landed and vs < -45.0 and agl < 2500.0 and agl > 0.0
	var tolerance := 2.0 + 3.5 * nerve
	var stress := maxf(0.0, (g - tolerance) / 2.0) + (1.5 if danger else 0.0) + maxf(0.0, spin_rate - 2.5) * 0.5
	var speed: float = t.get("surface_speed", 0.0)
	var in_air: bool = t.get("altitude", 0.0) < t.get("atmosphere_height", 0.0)
	var thrilling: bool = (t.get("thrust", 0.0) > 0.0 and g > 1.2) or (speed > 900.0 and in_air)
	_sky = _sky_color(t)
	_event_t -= delta
	if _event_t <= 0.0:
		_event = ""

	var m := "calm"
	if _event == "boom" or stress > 1.0:
		m = "panicking"
	elif spin_rate > 1.6:
		m = "dizzy"
	elif g > tolerance * 0.75:
		m = "squished"
	elif _event == "landed" or _event == "orbit":
		m = "cheering"
	elif _event == "stage" or (thrilling and glee > 0.35):
		m = "thrilled"
	elif freefall and not landed:
		m = "weightless"
	mood = m
	mood_text = {"calm": "calm", "thrilled": "thrilled", "squished": "squished (%.1f g)" % g, "panicking": "PANICKING",
		"dizzy": "dizzy", "weightless": "weightless", "cheering": "cheering!"}[m]

	# targets for each mood
	var tgt := {"eye": 1.0, "pupil": 1.0, "beak": 0.0, "squash": 0.0, "flip": 0.0, "flap": 0.0, "blush": 0.0, "sweat": 0.0,
		"tremble": 0.0, "float": 0.0, "spin": 0.0, "happy": 0.0, "brow": 0.0}
	match m:
		"thrilled":
			tgt.merge({"eye": 1.15, "beak": 0.5, "flip": 0.6, "flap": 0.6 * glee + 0.2, "blush": 0.8, "happy": 0.7 * glee, "brow": 0.3}, true)
		"squished":
			var k := clampf((g - 1.0) / 5.0, 0.0, 1.0)
			tgt.merge({"eye": 0.25, "pupil": 0.8, "squash": 0.35 * k + 0.1, "flip": -1.0, "brow": 0.9, "beak": 0.05}, true)
		"panicking":
			tgt.merge({"eye": 1.5, "pupil": 0.45, "beak": 1.0, "flip": 1.0, "flap": 1.0, "sweat": 1.0, "tremble": 1.0, "brow": -1.0}, true)
		"dizzy":
			tgt.merge({"eye": 1.1, "spin": 1.0, "beak": 0.3, "flip": 0.3, "brow": -0.4}, true)
		"weightless":
			tgt.merge({"eye": 1.1, "float": 1.0, "beak": 0.25 * glee, "flip": 0.8, "happy": 0.4 * glee, "blush": 0.3 * glee}, true)
		"cheering":
			tgt.merge({"eye": 1.0, "beak": 0.8, "flip": 1.0, "flap": 0.8, "blush": 1.0, "happy": 1.0, "brow": 0.5}, true)
	var e := 1.0 - exp(-delta * 6.0)
	_eye_open = lerpf(_eye_open, tgt.eye, e)
	_pupil = lerpf(_pupil, tgt.pupil, e)
	_beak_open = lerpf(_beak_open, tgt.beak, e)
	_squash = lerpf(_squash, tgt.squash, e)
	_flipper = lerpf(_flipper, tgt.flip, e)
	_flap = lerpf(_flap, tgt.flap, e)
	_blush = lerpf(_blush, tgt.blush, e)
	_sweat = lerpf(_sweat, tgt.sweat, e)
	_tremble = lerpf(_tremble, tgt.tremble, e)
	_float = lerpf(_float, tgt.float, 1.0 - exp(-delta * 1.5))
	_spin = lerpf(_spin, tgt.spin, e)
	_happy_eyes = lerpf(_happy_eyes, tgt.happy, e)
	_brow = lerpf(_brow, tgt.brow, e)
	# blinking (not while panicking: too scared to blink)
	_next_blink -= delta
	if _next_blink <= 0.0:
		_blink = 1.0
		_next_blink = _rng.randf_range(1.8, 5.0)
	_blink = maxf(0.0, _blink - delta * 7.0)
	queue_redraw()


func _sky_color(t: Dictionary) -> Color:
	var atm: float = t.get("atmosphere_height", 0.0)
	var alt: float = t.get("altitude", 0.0)
	if atm > 0.0 and alt < atm:
		return Color(0.02, 0.02, 0.06).lerp(Color(0.45, 0.68, 0.95), pow(1.0 - alt / atm, 1.5))
	return Color(0.02, 0.02, 0.06)


# ---- drawing --------------------------------------------------------------------

func _draw() -> void:
	var w := size.x
	var h := size.y - 26.0
	var c := Vector2(w * 0.5, h * 0.53)
	# pod interior: padded wall, porthole with sky, seat
	draw_rect(Rect2(0, 0, w, h), Color(0.1, 0.16, 0.2))
	for i in 6:
		draw_line(Vector2(0, i * h / 6.0), Vector2(w, i * h / 6.0 + 6), Color(0.13, 0.2, 0.25), 3.0)
	var port := Vector2(w * 0.84, h * 0.17)
	draw_circle(port, 22, Color(0.55, 0.58, 0.62))
	draw_circle(port, 18, _sky)
	if _sky.v < 0.2:
		for k in 5:
			draw_circle(port + Vector2(sin(k * 2.3) * 11, cos(k * 1.7) * 11), 1.2, Color(1, 1, 1, 0.8))
	draw_rect(Rect2(w * 0.18, h * 0.62, w * 0.64, h * 0.4), Color(0.75, 0.38, 0.12))  # seat back

	# body transform: tremble, float, spin, squash
	var off := Vector2.ZERO
	if _tremble > 0.01:
		off += Vector2(sin(_time * 60.0), cos(_time * 53.0)) * 2.5 * _tremble
	off += Vector2(sin(_time * 0.9) * 8.0, -12.0 + sin(_time * 1.3) * 6.0) * _float
	off.y += sin(_time * 2.0) * 1.5  # breathing
	var rot := sin(_time * 0.7) * 0.25 * _float + sin(_time * 9.0) * 0.08 * _spin
	var sc := clampf(minf(w, h) / 150.0, 0.6, 2.2) * 1.2
	var sx := (1.0 + _squash * 0.35) * sc
	var sy := (1.0 - _squash) * sc
	draw_set_transform(c + off * sc + Vector2(0, h * 0.02), rot, Vector2(sx, sy) * 0.94)
	_draw_penguin()
	draw_set_transform(Vector2.ZERO, 0.0, Vector2.ONE)
	# sweat drops
	if _sweat > 0.1:
		for k in 2:
			var p := c + off + Vector2(30 + k * 8, -40 + fmod(_time * 40.0 + k * 13.0, 30.0))
			draw_circle(p, 3.0, Color(0.6, 0.85, 1.0, _sweat))
	# speech
	var bubble := ""
	match mood:
		"panicking": bubble = "AAAAH!" if fmod(_time, 2.0) < 1.4 else "PASA?!"
		"thrilled": bubble = "Wheee!" if member.get("glee", 0.5) > 0.6 else ""
		"cheering": bubble = "We did it!"
		"dizzy": bubble = "@_@"
	if bubble != "":
		var font := get_theme_default_font()
		draw_rect(Rect2(6, 6, 74, 22), Color(1, 1, 1, 0.92))
		draw_string(font, Vector2(10, 22), bubble, HORIZONTAL_ALIGNMENT_LEFT, -1, 13, Color(0.1, 0.1, 0.15))
	# name plate
	var font := get_theme_default_font()
	draw_rect(Rect2(0, h, w, 26), Color(0.05, 0.07, 0.11, 0.95))
	draw_string(font, Vector2(6, h + 12), str(member.get("name", "")), HORIZONTAL_ALIGNMENT_LEFT, w - 8, 12, Color(1, 1, 1))
	var mood_col := Color(1, 0.4, 0.3) if mood == "panicking" else Color(1.0, 0.75, 0.3) if mood in ["thrilled", "cheering"] else Color(0.7, 0.85, 1.0)
	draw_string(font, Vector2(6, h + 24), mood_text, HORIZONTAL_ALIGNMENT_LEFT, w - 8, 11, mood_col)


func _draw_penguin() -> void:
	var dark := Color(0.08, 0.09, 0.14)
	var belly := Color(0.97, 0.97, 0.95)
	var beak := Color(1.0, 0.62, 0.12)
	var acc_col := Color(str(member.get("color", "#d84a3a")))
	# feet
	draw_colored_polygon(_ellipse(Vector2(-14, 50), 11, 5), beak)
	draw_colored_polygon(_ellipse(Vector2(14, 50), 11, 5), beak)
	# flippers (raised = negative angle), with flapping
	var flap := sin(_time * 18.0) * 0.5 * _flap
	for side in [-1.0, 1.0]:
		# hangs down at rest; _flipper > 0 swings it out and up
		var a: float = (_flipper * 1.5 + flap) * side
		var base := Vector2(34 * side, -4)
		var tip := base + Vector2(8 * side, 34).rotated(-a)
		var pts := PackedVector2Array([base + Vector2(0, -12), base + Vector2(-4 * side, 14), tip])
		draw_colored_polygon(pts, dark)
	# body and belly
	draw_colored_polygon(_ellipse(Vector2(0, 8), 40, 46), dark)
	draw_colored_polygon(_ellipse(Vector2(0, 14), 29, 36), belly)
	# head
	draw_colored_polygon(_ellipse(Vector2(0, -34), 30, 27), dark)
	draw_colored_polygon(_ellipse(Vector2(0, -28), 22, 18), belly)
	# highlight
	draw_arc(Vector2(-12, -44), 10, PI * 1.1, PI * 1.6, 8, Color(1, 1, 1, 0.25), 3.0)
	# cheeks
	if _blush > 0.05:
		draw_circle(Vector2(-17, -22), 5, Color(1.0, 0.5, 0.55, 0.5 * _blush))
		draw_circle(Vector2(17, -22), 5, Color(1.0, 0.5, 0.55, 0.5 * _blush))
	_draw_eyes()
	# beak (opens downward)
	var bo := _beak_open * 9.0
	draw_colored_polygon(PackedVector2Array([Vector2(-8, -24), Vector2(8, -24), Vector2(0, -17 - bo * 0.2)]), beak)
	if bo > 0.5:
		draw_colored_polygon(PackedVector2Array([Vector2(-7, -21), Vector2(7, -21), Vector2(0, -14 + bo)]), beak.darkened(0.15))
		draw_colored_polygon(PackedVector2Array([Vector2(-4, -20), Vector2(4, -20), Vector2(0, -16 + bo * 0.7)]), Color(0.6, 0.15, 0.2))
	_draw_accessory(acc_col)


func _draw_eyes() -> void:
	var open := _eye_open * (1.0 - _blink) if _sweat < 0.5 else _eye_open
	for side in [-1.0, 1.0]:
		var p := Vector2(10 * side, -36)
		# brows
		var tilt: float = -_brow * 4.0 * side
		draw_line(p + Vector2(-6, -9 - tilt), p + Vector2(6, -9 + tilt), Color(0.02, 0.02, 0.04), 2.5)
		if _spin > 0.4:
			# dizzy swirls
			var pts := PackedVector2Array()
			for k in 24:
				var a: float = k * 0.55 + _time * 8.0 * side
				pts.append(p + Vector2.from_angle(a) * (k * 0.28))
			draw_polyline(pts, Color(0.1, 0.1, 0.15), 1.6)
			continue
		if _happy_eyes > 0.5:
			draw_arc(p + Vector2(0, 2), 5, PI * 1.1, PI * 1.9, 10, Color(0.05, 0.05, 0.08), 2.5)
			continue
		var eh := maxf(0.6, 7.0 * open)
		draw_colored_polygon(_ellipse(p, 6.0 * maxf(open, 0.9), eh), Color(1, 1, 1))
		if open > 0.12:
			var look := Vector2(sin(_time * 0.6) * 1.5, 0) * (1.0 - _tremble)
			draw_circle(p + look + Vector2(0, 1), 3.2 * _pupil * clampf(open, 0.3, 1.0), Color(0.04, 0.04, 0.07))
			draw_circle(p + look + Vector2(-1, -1), 1.0, Color(1, 1, 1, 0.9))


func _draw_accessory(col: Color) -> void:
	match member.get("accessory", "none"):
		"scarf":
			draw_colored_polygon(_ellipse(Vector2(0, -12), 28, 6), col)
			var wave := sin(_time * 5.0) * 4.0
			draw_colored_polygon(PackedVector2Array([Vector2(12, -10), Vector2(22, -10), Vector2(26 + wave, 14), Vector2(16 + wave, 14)]), col.darkened(0.1))
		"bowtie":
			draw_colored_polygon(PackedVector2Array([Vector2(0, -12), Vector2(-11, -18), Vector2(-11, -6)]), col)
			draw_colored_polygon(PackedVector2Array([Vector2(0, -12), Vector2(11, -18), Vector2(11, -6)]), col)
			draw_circle(Vector2(0, -12), 2.5, col.darkened(0.3))
		"goggles":
			var y := -52.0 if mood != "panicking" else -38.0
			draw_line(Vector2(-28, y), Vector2(28, y), col.darkened(0.4), 4.0)
			for side in [-1.0, 1.0]:
				draw_circle(Vector2(10 * side, y), 7, Color(0.5, 0.6, 0.7))
				draw_circle(Vector2(10 * side, y), 5, Color(0.6, 0.85, 1.0, 0.7))
		"beanie":
			draw_colored_polygon(_ellipse(Vector2(0, -56), 26, 12), col)
			draw_rect(Rect2(-27, -54, 54, 6), col.lightened(0.3))
			draw_circle(Vector2(0, -70), 5, Color(1, 1, 1))
		"headset":
			draw_arc(Vector2(0, -36), 30, PI, TAU, 16, Color(0.2, 0.2, 0.22), 3.0)
			draw_circle(Vector2(-29, -34), 6, Color(0.25, 0.25, 0.28))
			draw_circle(Vector2(29, -34), 6, Color(0.25, 0.25, 0.28))
			draw_line(Vector2(-29, -30), Vector2(-12, -18), Color(0.2, 0.2, 0.22), 2.0)
			draw_circle(Vector2(-12, -18), 2.5, col)
		"monocle":
			draw_arc(Vector2(10, -36), 9, 0, TAU, 16, Color(0.85, 0.7, 0.3), 2.0)
			draw_line(Vector2(18, -32), Vector2(24, -4), Color(0.85, 0.7, 0.3), 1.0)
		"bandana":
			draw_colored_polygon(_ellipse(Vector2(0, -52), 29, 7), col)
			draw_colored_polygon(PackedVector2Array([Vector2(24, -52), Vector2(36, -60 + sin(_time * 6.0) * 3.0), Vector2(36, -46)]), col)


static func _ellipse(c: Vector2, rx: float, ry: float, n := 24) -> PackedVector2Array:
	var p := PackedVector2Array()
	for k in n:
		var a := TAU * k / n
		p.append(c + Vector2(cos(a) * rx, sin(a) * ry))
	return p
