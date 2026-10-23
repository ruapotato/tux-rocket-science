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
## Glacier Shelf Launch Complex: pad with flame trench, braced service tower
## with umbilical arms, water tower, the PASA operations building, lights.
## Built in a local frame whose +Y is the site's up direction; the pad top
## is at y = 0 (the flattened terrain height).
class_name LaunchSite
extends RefCounted

const CONCRETE_SHADER := """
shader_type spatial;
uniform vec4 base : source_color = vec4(0.56, 0.57, 0.6, 1.0);
uniform float slab = 4.0;
varying vec3 wp;
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
void vertex() { wp = VERTEX; }
void fragment() {
	vec2 g = wp.xz / slab;
	vec2 f = abs(fract(g) - 0.5) * 2.0;
	float joint = smoothstep(0.96, 1.0, max(f.x, f.y));
	float tone = 0.92 + 0.08 * hash(floor(g));
	float stain = hash(floor(wp.xz * 1.7)) * 0.05;
	// scorch around the flame trench
	float scorch = 1.0 - smoothstep(4.0, 16.0, length(wp.xz));
	vec3 col = base.rgb * tone * (1.0 - joint * 0.4) * (1.0 - stain) * (1.0 - scorch * 0.55);
	// yellow safety ring and the orange PASA circle
	float r = length(wp.xz);
	if (abs(r - 9.0) < 0.25) col = vec3(0.95, 0.75, 0.15);
	if (abs(r - 6.3) < 0.2) col = vec3(1.0, 0.5, 0.12);
	ALBEDO = col;
	ROUGHNESS = 0.85;
}
"""


static func build() -> Node3D:
	var root := Node3D.new()
	root.name = "LaunchSite"
	var cm := ShaderMaterial.new()
	var sh := Shader.new()
	sh.code = CONCRETE_SHADER
	cm.shader = sh
	var steel := PartVisual.mat("tower", Color(0.72, 0.28, 0.16), 0.35, 0.55)
	var grey := PartVisual.mat("tower_grey", Color(0.55, 0.57, 0.6), 0.5, 0.45)
	var dark := PartVisual.mat("graphite", PartVisual.GRAPHITE, 0.4, 0.5)
	var white := PartVisual.hull(PartVisual.WHITE, {"panels": 1.0, "ring_spacing": 0.0, "grime": 0.3})

	# pad deck, apron and a stepped plinth so the edge doesn't float
	# deck top sits 5 cm proud of the (equally flat) terrain to avoid z-fighting
	_box(root, Vector3(48, 3.0, 48), Vector3(0, -1.45, 0), cm)
	_box(root, Vector3(54, 2.0, 54), Vector3(0, -2.55, 0), PartVisual.mat("plinth", Color(0.45, 0.47, 0.5), 0.0, 0.9))
	var apron := CylinderMesh.new()
	apron.top_radius = 90
	apron.bottom_radius = 96
	apron.height = 2.0
	apron.radial_segments = 64
	_mesh(root, apron, PartVisual.mat("apron", Color(0.5, 0.53, 0.58), 0.0, 0.95), Vector3(0, -0.99, 0))
	# flame trench: a dark slot running out to one side
	_box(root, Vector3(7, 0.2, 30), Vector3(0, 0.07, -15), PartVisual.mat("trench", Color(0.06, 0.06, 0.07), 0.0, 0.9))
	_box(root, Vector3(9, 0.6, 1.0), Vector3(0, 0.3, -30.5), dark)
	# hold-down posts around the vehicle
	for k in 4:
		var a := TAU * k / 4.0 + PI / 4
		_box(root, Vector3(0.7, 0.9, 0.7), Vector3(cos(a) * 3.8, 0.45, sin(a) * 3.8), PartVisual.hull(PartVisual.YELLOW, {"hazard": 1.0, "stripe_color": Color(0.08, 0.08, 0.1), "panels": 1.0, "ring_spacing": 0.0}))

	# service tower: four legs, cross bracing, decks, umbilical arms
	var tx := 22.0
	var levels := 14
	for lvl in levels:
		var y := lvl * 4.0
		for corner in 4:
			var cx := tx + (corner % 2) * 5.0
			var cz := -2.5 + (corner / 2) * 5.0
			_box(root, Vector3(0.4, 4.0, 0.4), Vector3(cx, y + 2.0, cz), steel)
		for side in 2:
			# diagonal braces on two faces
			_strut(root, Vector3(tx, y, -2.5 + side * 5.0), Vector3(tx + 5.0, y + 4.0, -2.5 + side * 5.0), 0.12, steel)
			_strut(root, Vector3(tx + side * 5.0, y, -2.5), Vector3(tx + side * 5.0, y + 4.0, 2.5), 0.12, steel)
		_box(root, Vector3(5.6, 0.25, 5.6), Vector3(tx + 2.5, y + 4.0, 0), grey)
	for arm_y in [12.0, 28.0, 44.0]:
		_box(root, Vector3(tx - 2.2, 0.6, 1.2), Vector3((tx + 2.2) * 0.5, arm_y, 0), steel)
		_box(root, Vector3(1.2, 1.2, 1.6), Vector3(2.2, arm_y, 0), dark)
	# lightning mast and warning beacon
	_strut(root, Vector3(tx + 2.5, levels * 4.0, 0), Vector3(tx + 2.5, levels * 4.0 + 12.0, 0), 0.15, grey)
	var beacon := OmniLight3D.new()
	beacon.light_color = Color(1.0, 0.2, 0.1)
	beacon.light_energy = 2.0
	beacon.omni_range = 30.0
	beacon.position = Vector3(tx + 2.5, levels * 4.0 + 12.5, 0)
	root.add_child(beacon)
	var bulb := SphereMesh.new()
	bulb.radius = 0.4
	bulb.height = 0.8
	_mesh(root, bulb, PartVisual.mat("beacon", Color(1, 0.2, 0.1), 0.0, 0.3, Color(1, 0.15, 0.05)), beacon.position)

	# water tower (sound suppression) on tall legs
	var wt := Vector3(-70, 0, 55)
	for k in 4:
		var a := TAU * k / 4.0
		_strut(root, wt + Vector3(cos(a) * 5.0, 0, sin(a) * 5.0), wt + Vector3(cos(a) * 3.0, 26.0, sin(a) * 3.0), 0.25, grey)
	var tank := CylinderMesh.new()
	tank.top_radius = 5.5
	tank.bottom_radius = 5.5
	tank.height = 8.0
	_mesh(root, tank, white, wt + Vector3(0, 30, 0))
	var dome := SphereMesh.new()
	dome.radius = 5.5
	dome.height = 5.5
	dome.is_hemisphere = true
	_mesh(root, dome, white, wt + Vector3(0, 34, 0))
	var band := CylinderMesh.new()
	band.top_radius = 5.56
	band.bottom_radius = 5.56
	band.height = 1.2
	_mesh(root, band, PartVisual.mat("orange", PartVisual.ORANGE, 0.0, 0.45), wt + Vector3(0, 30, 0))

	# PASA operations building with the insignia
	var bpos := Vector3(-95, 0, -70)
	_box(root, Vector3(30, 12, 18), bpos + Vector3(0, 6, 0), white)
	_box(root, Vector3(31, 1.0, 19), bpos + Vector3(0, 12.4, 0), dark)
	_box(root, Vector3(30.2, 1.6, 18.2), bpos + Vector3(0, 9.5, 0), PartVisual.mat("orange", PartVisual.ORANGE, 0.0, 0.45))
	for k in 6:
		_box(root, Vector3(3.0, 2.0, 0.1), bpos + Vector3(-11.5 + k * 4.6, 4.5, 9.05), PartVisual.mat("window", Color(0.06, 0.16, 0.28), 0.3, 0.05, Color(0.03, 0.08, 0.14)))
	var logo := Sprite3D.new()
	logo.texture = PasaLogo.decal_texture()
	logo.pixel_size = 0.03
	logo.position = bpos + Vector3(0, 7.0, 9.12)
	root.add_child(logo)
	var sign := Label3D.new()
	sign.text = "PASA"
	sign.font_size = 640
	sign.pixel_size = 0.02
	sign.outline_size = 32
	sign.modulate = Color(1, 1, 1)
	sign.position = Vector3(tx + 2.5, levels * 4.0 - 6.0, 2.9)
	root.add_child(sign)
	var sub := Label3D.new()
	sub.text = "GLACIER SHELF LAUNCH COMPLEX"
	sub.font_size = 120
	sub.pixel_size = 0.02
	sub.position = Vector3(0, 0.1, 20)
	sub.rotation = Vector3(-PI / 2, 0, 0)
	sub.modulate = Color(0.15, 0.15, 0.18)
	root.add_child(sub)
	# floodlight masts at the pad corners
	for k in 4:
		var a := TAU * k / 4.0 + PI / 4
		var base := Vector3(cos(a) * 30.0, 0, sin(a) * 30.0)
		_strut(root, base, base + Vector3(0, 18, 0), 0.2, grey)
		var head := Vector3(cos(a) * 29.0, 18.5, sin(a) * 29.0)
		_box(root, Vector3(2.0, 1.0, 0.5), head, dark)
		var spot := SpotLight3D.new()
		spot.light_energy = 1.2
		spot.spot_range = 80.0
		spot.spot_angle = 35.0
		spot.light_color = Color(1.0, 0.95, 0.85)
		spot.position = head
		root.add_child(spot)
		spot.look_at_from_position(head, Vector3(0, 10, 0), Vector3.UP)
	return root


static func _mesh(root: Node3D, m: Mesh, mat: Material, at: Vector3) -> MeshInstance3D:
	var mi := MeshInstance3D.new()
	mi.mesh = m
	mi.material_override = mat
	mi.position = at
	root.add_child(mi)
	return mi


static func _box(root: Node3D, size: Vector3, at: Vector3, mat: Material) -> MeshInstance3D:
	var b := BoxMesh.new()
	b.size = size
	return _mesh(root, b, mat, at)


static func _strut(root: Node3D, a: Vector3, b: Vector3, r: float, mat: Material) -> void:
	var c := CylinderMesh.new()
	c.top_radius = r
	c.bottom_radius = r
	c.height = 1.0
	c.radial_segments = 8
	var d := b - a
	var mi := _mesh(root, c, mat, (a + b) * 0.5)
	mi.basis = Basis(Quaternion(Vector3.UP, d.normalized())).scaled(Vector3(1, d.length(), 1))


## Site centre in the body-fixed frame, Godot axes.
static func site_fixed_godot(core: TrsSim, body: int) -> Vector3:
	var info: Dictionary = JSON.parse_string(core.body_info(body))
	var s: Dictionary = info.get("site", {})
	if s.is_empty():
		return Vector3.ZERO
	var lat := deg_to_rad(s.lat_deg)
	var lon := deg_to_rad(s.lon_deg)
	var r: float = info.radius + s.altitude
	return Frame.vec_arr([r * cos(lat) * cos(lon), r * cos(lat) * sin(lon), r * sin(lat)])


## Local frame at the site: +Y up, +X east (Godot axes, body-fixed).
static func site_basis(site_pos: Vector3) -> Basis:
	var up := site_pos.normalized()
	var east := Vector3.UP.cross(up).normalized()  # sim +Z is Godot +Y
	var north := up.cross(east)
	return Basis(east, up, -north)
