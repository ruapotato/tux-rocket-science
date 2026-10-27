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
## The relics: what is left of whoever came before, standing on the
## ground where trs_core's body data puts them. Built in a local frame
## whose +Y is up and whose origin is the ground at the relic; each sinks
## a few metres into it so a sloping patch shows no gap. The light in them
## breathes slowly (the hum the scanner hears).
class_name RelicVisual
extends RefCounted

const GLOW_SHADER := """
shader_type spatial;
render_mode unshaded;
uniform vec4 tint : source_color = vec4(0.45, 1.0, 0.8, 1.0);
uniform float rate = 0.6;
void fragment() {
	float pulse = 0.65 + 0.35 * sin(TIME * rate * 6.2831);
	ALBEDO = tint.rgb * pulse * 1.6;
}
"""

const STONE_SHADER := """
shader_type spatial;
uniform vec4 base : source_color = vec4(0.82, 0.8, 0.74, 1.0);
uniform vec4 line : source_color = vec4(0.45, 1.0, 0.8, 1.0);
uniform float band = 3.0;
varying vec3 wp;
float hash(vec3 p) { return fract(sin(dot(p, vec3(127.1, 311.7, 74.7))) * 43758.5453); }
void vertex() { wp = VERTEX; }
void fragment() {
	float tone = 0.9 + 0.1 * hash(floor(wp * 0.7));
	// thin glowing grooves at even heights, like writing nobody can read
	float g = abs(fract(wp.y / band) - 0.5);
	float groove = 1.0 - smoothstep(0.015, 0.03, g);
	float pulse = 0.6 + 0.4 * sin(TIME * 3.77 + wp.y * 0.15);
	ALBEDO = base.rgb * tone;
	ROUGHNESS = 0.7;
	EMISSION = line.rgb * groove * pulse * 2.0;
}
"""


static func _stone(col := Color(0.82, 0.8, 0.74), tint := Color(0.45, 1.0, 0.8), band := 3.0) -> ShaderMaterial:
	var sh := Shader.new()
	sh.code = STONE_SHADER
	var m := ShaderMaterial.new()
	m.shader = sh
	m.set_shader_parameter("base", col)
	m.set_shader_parameter("line", tint)
	m.set_shader_parameter("band", band)
	return m


static func _glow(tint := Color(0.45, 1.0, 0.8), rate := 0.6) -> ShaderMaterial:
	var sh := Shader.new()
	sh.code = GLOW_SHADER
	var m := ShaderMaterial.new()
	m.shader = sh
	m.set_shader_parameter("tint", tint)
	m.set_shader_parameter("rate", rate)
	return m


static func _add(root: Node3D, mesh: Mesh, m: Material, xf: Transform3D) -> MeshInstance3D:
	var mi := MeshInstance3D.new()
	mi.mesh = mesh
	mi.material_override = m
	mi.transform = xf
	root.add_child(mi)
	return mi


static func _box(root: Node3D, size: Vector3, m: Material, xf: Transform3D) -> void:
	var b := BoxMesh.new()
	b.size = size
	_add(root, b, m, xf)


## The relic `id` (a node in its local frame; a generic standing stone for
## an id it doesn't know).
static func build(id: String) -> Node3D:
	var root := Node3D.new()
	root.name = "Relic_" + id
	match id:
		"krill_chime":
			_chime(root)
		"kelp_hearth":
			_hearth(root)
		"narwhal_keel":
			_keel(root)
		"haven_archive":
			_archive(root)
		_:
			_box(root, Vector3(3, 14, 3), _stone(), Transform3D(Basis.IDENTITY, Vector3(0, 5, 0)))
	var light := OmniLight3D.new()
	light.light_color = Color(0.5, 1.0, 0.85)
	light.light_energy = 2.0
	light.omni_range = 60.0
	light.position = Vector3(0, 8, 0)
	root.add_child(light)
	return root


## The Chime: a standing ring of pale stone 36 m across, its foot sunk in
## a low plinth, a thread of light across its middle and stones hanging
## still around it.
static func _chime(root: Node3D) -> void:
	var stone := _stone()
	var ring := TorusMesh.new()
	ring.inner_radius = 15.0
	ring.outer_radius = 18.0
	ring.rings = 64
	ring.ring_segments = 16
	_add(root, ring, stone, Transform3D(Basis(Vector3.RIGHT, PI / 2), Vector3(0, 15.0, 0)))
	var plinth := CylinderMesh.new()
	plinth.top_radius = 9.0
	plinth.bottom_radius = 11.0
	plinth.height = 5.0
	_add(root, plinth, _stone(Color(0.7, 0.68, 0.64)), Transform3D(Basis.IDENTITY, Vector3(0, -1.5, 0)))
	var thread := CylinderMesh.new()
	thread.top_radius = 0.25
	thread.bottom_radius = 0.25
	thread.height = 30.0
	_add(root, thread, _glow(), Transform3D(Basis(Vector3.FORWARD, PI / 2), Vector3(0, 15.0, 0)))
	var core := SphereMesh.new()
	core.radius = 1.6
	core.height = 3.2
	_add(root, core, _glow(Color(0.6, 1.0, 0.9), 0.3), Transform3D(Basis.IDENTITY, Vector3(0, 15.0, 0)))
	for k in 7:
		var a := k * TAU / 7.0
		var rock := BoxMesh.new()
		rock.size = Vector3(1.6, 2.4, 1.2)
		var at := Vector3(cos(a) * 24.0, 6.0 + 4.0 * sin(a * 3.0), sin(a) * 24.0)
		_add(root, rock, stone, Transform3D(Basis(Vector3(0.3, 1, 0.2).normalized(), a), at))


## The Hearth: a low dome 40 m across with five tall doorways, warm light
## pouring out of them, and a heart of light above its crown.
static func _hearth(root: Node3D) -> void:
	var warm := Color(1.0, 0.75, 0.4)
	var dome := SphereMesh.new()
	dome.radius = 20.0
	dome.height = 24.0
	dome.is_hemisphere = true
	_add(root, dome, _stone(Color(0.62, 0.66, 0.58), warm, 4.0), Transform3D(Basis.IDENTITY, Vector3(0, -2.0, 0)))
	for k in 5:
		var a := k * TAU / 5.0
		var b := Basis(Vector3.UP, -a)
		_box(root, Vector3(3.0, 7.0, 0.6), _glow(warm, 0.4), Transform3D(b, b * Vector3(0, 1.5, 19.3)))
	var heart := SphereMesh.new()
	heart.radius = 2.4
	heart.height = 4.8
	_add(root, heart, _glow(Color(1.0, 0.85, 0.55), 0.25), Transform3D(Basis.IDENTITY, Vector3(0, 15.0, 0)))
	var halo := TorusMesh.new()
	halo.inner_radius = 3.6
	halo.outer_radius = 4.0
	_add(root, halo, _glow(warm, 0.25), Transform3D(Basis(Vector3.RIGHT, 0.3), Vector3(0, 15.0, 0)))


## The Keel: the spine of a ship far bigger than anything PASA could lift,
## 90 m long and half buried, its ribs arching out of the ice, and the
## dark housing of its engine at the stern with a seam of light.
static func _keel(top: Node3D) -> void:
	var root := Node3D.new()
	root.rotation.y = PI / 2  # lying east to west
	top.add_child(root)
	var hull := _stone(Color(0.32, 0.34, 0.4), Color(0.55, 0.75, 1.0), 6.0)
	_box(root, Vector3(4.0, 3.0, 90.0), hull, Transform3D(Basis(Vector3.RIGHT, 0.04), Vector3(0, 0.0, 0)))
	for k in 9:
		var z := -36.0 + k * 9.0
		var rib := TorusMesh.new()
		var rr := 14.0 - absf(z) * 0.12
		rib.inner_radius = rr - 1.0
		rib.outer_radius = rr
		rib.rings = 40
		_add(root, rib, hull, Transform3D(Basis(Vector3.RIGHT, PI / 2), Vector3(0, -rr * 0.35, z)))
	var housing := CylinderMesh.new()
	housing.top_radius = 7.0
	housing.bottom_radius = 9.0
	housing.height = 12.0
	_add(root, housing, hull, Transform3D(Basis(Vector3.RIGHT, PI / 2), Vector3(0, 2.0, 50.0)))
	var seam := TorusMesh.new()
	seam.inner_radius = 8.0
	seam.outer_radius = 8.5
	_add(root, seam, _glow(Color(0.55, 0.8, 1.0), 0.2), Transform3D(Basis(Vector3.RIGHT, PI / 2), Vector3(0, 2.0, 50.0)))


## The Archive: three rows of standing stones on a paved oval, each with
## the same band of light, and a taller stone at the head.
static func _archive(root: Node3D) -> void:
	var stone := _stone(Color(0.86, 0.84, 0.8), Color(0.75, 0.85, 1.0), 2.0)
	var pave := CylinderMesh.new()
	pave.top_radius = 26.0
	pave.bottom_radius = 26.0
	pave.height = 4.0
	_add(root, pave, _stone(Color(0.6, 0.6, 0.58), Color(0.75, 0.85, 1.0), 100.0), Transform3D(Basis.IDENTITY, Vector3(0, -1.6, 0)))
	for row in 3:
		for k in 6:
			var x := -15.0 + k * 6.0
			var z := -10.0 + row * 10.0
			_box(root, Vector3(1.6, 7.0, 0.7), stone, Transform3D(Basis(Vector3.UP, 0.05 * (k - row)), Vector3(x, 3.0, z)))
	_box(root, Vector3(3.0, 16.0, 1.2), stone, Transform3D(Basis.IDENTITY, Vector3(0, 7.5, -20.0)))
	_box(root, Vector3(2.0, 0.5, 0.2), _glow(Color(0.75, 0.85, 1.0), 0.2), Transform3D(Basis.IDENTITY, Vector3(0, 12.0, -19.35)))
