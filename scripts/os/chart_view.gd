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
# chart_view.gd -- what a script's chart("height", ship.height) draws: each
# series a line against game time, on its own scale, labelled with its name
# and its latest value. On the flight screen and in the Flight app.
class_name ChartView
extends Control

const COLORS := [Color("#ffcb6b"), Color("#82aaff"), Color("#c3e88d"), Color("#f78c6c")]

var series := {}          # name -> [[t, value], ...] (ShipComputer.charts)
var font: Font
var dark := true          # light ink on dark (the HUD), or dark on light (the Flight app)


func _ready() -> void:
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	if font == null:
		font = ThemeDB.fallback_font


func set_series(s: Dictionary) -> void:
	series = s
	queue_redraw()


func _draw() -> void:
	var r := Rect2(Vector2.ZERO, size)
	var ink := Color(0.85, 0.9, 1.0) if dark else Color("#1b1b1b")
	var grid := Color(1, 1, 1, 0.08) if dark else Color(0, 0, 0, 0.1)
	draw_rect(r, Color(0, 0, 0, 0.25) if dark else Color("#ffffff"))
	for k in 4:
		var y := r.size.y * (k + 1) / 5.0
		draw_line(Vector2(0, y), Vector2(r.size.x, y), grid)
	if series.is_empty():
		draw_string(font, Vector2(8, r.size.y / 2), "chart(\"height\", ship.height) draws here", HORIZONTAL_ALIGNMENT_LEFT, -1, 12, ink.darkened(0.3))
		return
	var names := series.keys()
	names.sort()
	# one time axis for all of them: the last minute or so of what came in
	var t1 := -INF
	var t0 := INF
	for n in names:
		var pts: Array = series[n]
		if pts.is_empty():
			continue
		t1 = maxf(t1, float(pts[-1][0]))
		t0 = minf(t0, float(pts[0][0]))
	if t1 <= t0:
		t1 = t0 + 1.0
	var row := 0
	for i in mini(names.size(), COLORS.size()):
		var pts: Array = series[names[i]]
		if pts.size() < 1:
			continue
		var lo := INF
		var hi := -INF
		for p in pts:
			lo = minf(lo, float(p[1]))
			hi = maxf(hi, float(p[1]))
		if hi - lo < 1e-9:
			hi = lo + 1.0
		var line := PackedVector2Array()
		for p in pts:
			var x := (float(p[0]) - t0) / (t1 - t0) * (r.size.x - 4) + 2
			var y := r.size.y - 4 - (float(p[1]) - lo) / (hi - lo) * (r.size.y - 22)
			line.append(Vector2(x, y))
		if line.size() >= 2:
			draw_polyline(line, COLORS[i], 1.6, true)
		var last := float(pts[-1][1])
		draw_string(font, Vector2(6 + row * (r.size.x / 2.0), 13), "%s %s" % [names[i], _short(last)], HORIZONTAL_ALIGNMENT_LEFT,
			r.size.x / 2.0 - 8, 12, COLORS[i])
		row += 1


static func _short(x: float) -> String:
	var a := absf(x)
	if a >= 1.0e6:
		return "%.2fM" % (x / 1.0e6)
	if a >= 1.0e4:
		return "%.1fk" % (x / 1.0e3)
	if a >= 100.0:
		return "%.0f" % x
	return "%.2f" % x
