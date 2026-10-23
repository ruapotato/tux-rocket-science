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
## Navball: a shaded sphere for the local horizon frame (blue sky, brown
## ground, pitch/heading grid drawn in the shader from object-space normals)
## plus 2D markers for prograde, retrograde, normal, radial and target.
## Object frame of the ball: +Y up, -Z north, +X east.
class_name Navball
extends Control

const SIZE := 220

var _vp: SubViewport
var _ball: MeshInstance3D
var _basis := Basis()  # ball object -> view
var _markers := []     # [{dir_obj: Vector3, kind}]
var heading := 0.0
var pitch := 0.0


func _ready() -> void:
	custom_minimum_size = Vector2(SIZE, SIZE)
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	var svc := SubViewportContainer.new()
	svc.stretch = true
	svc.size = Vector2(SIZE, SIZE)
	svc.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(svc)
	_vp = SubViewport.new()
	_vp.size = Vector2i(SIZE, SIZE)
	_vp.own_world_3d = true
	_vp.transparent_bg = true
	_vp.msaa_3d = Viewport.MSAA_4X
	svc.add_child(_vp)
	var cam := Camera3D.new()
	cam.projection = Camera3D.PROJECTION_ORTHOGONAL
	cam.size = 2.08
	cam.position = Vector3(0, 0, 3)
	_vp.add_child(cam)
	var light := DirectionalLight3D.new()
	light.rotation = Vector3(-0.5, 0.4, 0)
	light.light_energy = 0.9
	_vp.add_child(light)
	var env := WorldEnvironment.new()
	var e := Environment.new()
	e.background_mode = Environment.BG_CLEAR_COLOR
	e.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	e.ambient_light_color = Color(0.7, 0.7, 0.7)
	env.environment = e
	_vp.add_child(env)
	_ball = MeshInstance3D.new()
	var sm := SphereMesh.new()
	sm.radius = 1.0
	sm.height = 2.0
	sm.radial_segments = 96
	sm.rings = 48
	_ball.mesh = sm
	var mat := ShaderMaterial.new()
	var sh := Shader.new()
	sh.code = """
shader_type spatial;
varying vec3 objn;
void vertex() { objn = VERTEX; }
float grid(float v, float spacing, float width) {
	float d = abs(fract(v / spacing + 0.5) - 0.5) * spacing;
	return 1.0 - smoothstep(width * 0.5, width, d);
}
void fragment() {
	vec3 n = normalize(objn);
	float lat = degrees(asin(clamp(n.y, -1.0, 1.0)));
	float lon = degrees(atan(n.x, -n.z));
	vec3 sky = mix(vec3(0.16, 0.42, 0.78), vec3(0.42, 0.68, 0.95), clamp(lat / 90.0, 0.0, 1.0));
	vec3 ground = mix(vec3(0.66, 0.42, 0.2), vec3(0.42, 0.25, 0.12), clamp(-lat / 90.0, 0.0, 1.0));
	vec3 col = lat >= 0.0 ? sky : ground;
	float wl = fwidth(lat) * 1.4;
	float minor = grid(lat, 10.0, wl);
	float lon_w = fwidth(lon) * 1.4;
	float merid = abs(lat) < 80.0 ? grid(lon, 30.0, lon_w) : 0.0;
	float horizon = 1.0 - smoothstep(wl * 1.2, wl * 2.4, abs(lat));
	col = mix(col, vec3(0.95), max(minor * 0.55, merid * 0.45));
	col = mix(col, vec3(1.0, 1.0, 1.0), horizon);
	ALBEDO = col;
	ROUGHNESS = 0.4;
	SPECULAR = 0.3;
}
"""
	mat.shader = sh
	_ball.material_override = mat
	_vp.add_child(_ball)


## frame: {up, north, east} SOI vectors; nose/top: vessel axes (SOI);
## markers: [[dir (SOI), kind], ...]
func update_attitude(up: Vector3, north: Vector3, east: Vector3, nose: Vector3, top: Vector3, markers: Array) -> void:
	var to_obj := func(v: Vector3) -> Vector3: return Vector3(v.dot(east), v.dot(up), -v.dot(north))
	var f: Vector3 = to_obj.call(nose).normalized()
	var u: Vector3 = to_obj.call(top)
	u = (u - f * u.dot(f)).normalized()
	var r := u.cross(f)
	_basis = Basis(r, u, f).transposed()
	_ball.basis = _basis
	_markers.clear()
	for m in markers:
		_markers.append({"view": _basis * to_obj.call(m[0]).normalized(), "kind": m[1]})
	# cardinal labels along the horizon
	for k in 4:
		var a := k * PI / 2.0
		_markers.append({"view": _basis * Vector3(sin(a), 0, -cos(a)), "kind": "NESW"[k]})
	heading = fposmod(rad_to_deg(atan2(nose.dot(east), nose.dot(north))), 360.0)
	pitch = rad_to_deg(asin(clampf(nose.dot(up), -1.0, 1.0)))
	queue_redraw()


func _draw() -> void:
	var c := Vector2(SIZE, SIZE) * 0.5
	var R := SIZE * 0.5 / 1.04
	draw_arc(c, R + 2, 0, TAU, 96, Color(0.08, 0.1, 0.14), 6.0, true)
	for m in _markers:
		var v: Vector3 = m.view
		if v.z <= 0.05:
			continue
		var p := c + Vector2(v.x, -v.y) * R
		match m.kind:
			"prograde":
				draw_arc(p, 9, 0, TAU, 24, Color(1, 0.9, 0.1), 2.5, true)
				draw_line(p + Vector2(9, 0), p + Vector2(16, 0), Color(1, 0.9, 0.1), 2.5)
				draw_line(p - Vector2(9, 0), p - Vector2(16, 0), Color(1, 0.9, 0.1), 2.5)
				draw_line(p - Vector2(0, 9), p - Vector2(0, 16), Color(1, 0.9, 0.1), 2.5)
			"retrograde":
				draw_arc(p, 9, 0, TAU, 24, Color(1, 0.9, 0.1), 2.5, true)
				draw_line(p + Vector2(-6, -6), p + Vector2(6, 6), Color(1, 0.9, 0.1), 2.5)
				draw_line(p + Vector2(-6, 6), p + Vector2(6, -6), Color(1, 0.9, 0.1), 2.5)
			"normal", "antinormal":
				var col := Color(0.85, 0.35, 1.0)
				draw_polyline(PackedVector2Array([p + Vector2(0, -10), p + Vector2(9, 7), p + Vector2(-9, 7), p + Vector2(0, -10)]), col, 2.5, true)
				if m.kind == "antinormal":
					draw_circle(p, 2.5, col)
			"radial_out", "radial_in":
				var col := Color(0.3, 0.95, 1.0)
				draw_arc(p, 8, 0, TAU, 24, col, 2.5, true)
				if m.kind == "radial_out":
					for k in 4:
						var d := Vector2.from_angle(k * PI / 2 + PI / 4)
						draw_line(p + d * 8, p + d * 14, col, 2.5)
				else:
					for k in 4:
						var d := Vector2.from_angle(k * PI / 2 + PI / 4)
						draw_line(p + d * 3, p + d * 8, col, 2.5)
			"maneuver":
				var col := Color(0.35, 0.6, 1.0)
				draw_arc(p, 11, 0, TAU, 24, col, 3.0, true)
				draw_line(p + Vector2(0, -11), p + Vector2(0, -18), col, 3.0)
				draw_line(p + Vector2(-10, 6), p + Vector2(-16, 10), col, 3.0)
				draw_line(p + Vector2(10, 6), p + Vector2(16, 10), col, 3.0)
			"target":
				draw_arc(p, 10, 0, TAU, 24, Color(1, 0.35, 0.7), 2.5, true)
				draw_circle(p, 3, Color(1, 0.35, 0.7))
			"N", "E", "S", "W":
				draw_string(get_theme_default_font(), p + Vector2(-5, 6), m.kind, HORIZONTAL_ALIGNMENT_LEFT, -1, 15, Color(1, 1, 1, 0.9))
	# fixed nose marker
	var nose := Color(1.0, 0.55, 0.12)
	draw_line(c + Vector2(-30, 0), c + Vector2(-10, 0), nose, 3.0)
	draw_line(c + Vector2(10, 0), c + Vector2(30, 0), nose, 3.0)
	draw_line(c + Vector2(-10, 0), c + Vector2(0, 8), nose, 3.0)
	draw_line(c + Vector2(10, 0), c + Vector2(0, 8), nose, 3.0)
	draw_circle(c, 2.5, nose)
	var txt := "HDG %03d°   PITCH %+03d°" % [int(round(heading)) % 360, int(round(pitch))]
	draw_string(get_theme_default_font(), Vector2(c.x - 70, SIZE + 18), txt, HORIZONTAL_ALIGNMENT_LEFT, -1, 14, Color(0.85, 0.9, 1.0))
