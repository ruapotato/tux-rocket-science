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
## The PASA insignia: a fish swimming through an orbit of spaghetti, on a
## navy badge. Drawn procedurally at any size; entirely our own design.
class_name PasaLogo
extends Control

@export var animate := true
var _t := 0.0


func _ready() -> void:
	if custom_minimum_size == Vector2.ZERO:
		custom_minimum_size = Vector2(220, 220)
	mouse_filter = Control.MOUSE_FILTER_IGNORE


func _process(delta: float) -> void:
	if animate:
		_t += delta
		queue_redraw()


func _draw() -> void:
	var s := minf(size.x, size.y)
	var c := size * 0.5
	var R := s * 0.48
	# badge
	draw_circle(c, R, Color(0.95, 0.52, 0.12))
	draw_circle(c, R * 0.93, Color(0.07, 0.11, 0.24))
	# stars
	var rng := RandomNumberGenerator.new()
	rng.seed = 7
	for i in 28:
		var p := c + Vector2.from_angle(rng.randf() * TAU) * R * sqrt(rng.randf()) * 0.88
		var tw := 0.6 + 0.4 * sin(_t * 2.0 + i)
		draw_circle(p, s * 0.006 * tw + s * 0.002, Color(1, 1, 1, 0.8))
	# spaghetti: three noodles looping round like orbits, slightly wobbly
	var noodle := Color(0.99, 0.86, 0.45)
	for k in 3:
		var pts := PackedVector2Array()
		var tilt := -0.45 + k * 0.42
		for i in 97:
			var a := TAU * i / 96.0
			var wob := 1.0 + 0.05 * sin(a * 7.0 + k * 2.0 + _t * 1.5)
			var e := Vector2(cos(a) * R * 0.74 * wob, sin(a) * R * 0.3 * wob)
			pts.append(c + e.rotated(tilt))
		draw_polyline(pts, noodle.darkened(0.18), s * 0.022, true)
		draw_polyline(pts, noodle, s * 0.013, true)
	# the fish (heading up and to the right, like a launch)
	var f := c + Vector2(0, -s * 0.02)
	var ang := -0.5 + sin(_t * 1.2) * 0.05
	var body := PackedVector2Array()
	for i in 40:
		var a := TAU * i / 40.0
		body.append(f + Vector2(cos(a) * s * 0.2, sin(a) * s * 0.085).rotated(ang))
	var tail_base := f + Vector2(-s * 0.18, 0).rotated(ang)
	var flick := sin(_t * 6.0) * s * 0.02
	var tail := PackedVector2Array([tail_base, tail_base + Vector2(-s * 0.12, -s * 0.09 + flick).rotated(ang),
		tail_base + Vector2(-s * 0.12, s * 0.09 + flick).rotated(ang)])
	var fish := Color(0.55, 0.8, 0.98)
	draw_colored_polygon(tail, fish.darkened(0.2))
	draw_colored_polygon(body, fish)
	var belly := PackedVector2Array()
	for i in 21:
		var a := PI * i / 20.0
		belly.append(f + Vector2(cos(a) * s * 0.17, sin(a) * s * 0.06 + s * 0.012).rotated(ang))
	draw_colored_polygon(belly, Color(0.85, 0.94, 1.0))
	draw_circle(f + Vector2(s * 0.12, -s * 0.025).rotated(ang), s * 0.022, Color(1, 1, 1))
	draw_circle(f + Vector2(s * 0.125, -s * 0.025).rotated(ang), s * 0.011, Color(0.05, 0.05, 0.1))
	draw_line(f + Vector2(s * 0.02, -s * 0.07).rotated(ang), f + Vector2(-s * 0.04, -s * 0.12).rotated(ang), fish.darkened(0.25), s * 0.02)
	# lettering
	var font := get_theme_default_font()
	var fs := int(s * 0.16)
	var txt := "PASA"
	var tw2 := font.get_string_size(txt, HORIZONTAL_ALIGNMENT_LEFT, -1, fs).x
	draw_string(font, c + Vector2(-tw2 * 0.5, R * 0.78), txt, HORIZONTAL_ALIGNMENT_LEFT, -1, fs, Color(1, 1, 1))
