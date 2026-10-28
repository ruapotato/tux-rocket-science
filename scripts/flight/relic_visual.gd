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
		"gloam_lamp":
			_lamp(root)
		"selkie_anchor":
			_anchor(root)
		"saffron_hourglass":
			_hourglass(root)
		"mere_garden":
			_garden(root)
		"cairn_beacon":
			_beacon(root)
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


## The Lamp: a tall lantern of glass on a stone foot, lit with the
## Hearth's warm white, four carved stones round it.
static func _lamp(root: Node3D) -> void:
	var warm := Color(1.0, 0.82, 0.55)
	var stone := _stone(Color(0.5, 0.36, 0.3), warm, 2.5)
	var foot := CylinderMesh.new()
	foot.top_radius = 4.0
	foot.bottom_radius = 6.0
	foot.height = 6.0
	_add(root, foot, stone, Transform3D(Basis.IDENTITY, Vector3(0, 1.0, 0)))
	var post := CylinderMesh.new()
	post.top_radius = 1.0
	post.bottom_radius = 1.4
	post.height = 18.0
	_add(root, post, stone, Transform3D(Basis.IDENTITY, Vector3(0, 13.0, 0)))
	var glass := CylinderMesh.new()
	glass.top_radius = 3.4
	glass.bottom_radius = 2.6
	glass.height = 8.0
	glass.radial_segments = 6
	_add(root, glass, _glow(warm, 0.18), Transform3D(Basis.IDENTITY, Vector3(0, 26.0, 0)))
	var cap := CylinderMesh.new()
	cap.top_radius = 0.4
	cap.bottom_radius = 4.2
	cap.height = 3.0
	cap.radial_segments = 6
	_add(root, cap, stone, Transform3D(Basis.IDENTITY, Vector3(0, 31.5, 0)))
	for k in 4:
		var a := k * TAU / 4.0 + 0.4
		_box(root, Vector3(1.4, 4.5, 0.8), stone, Transform3D(Basis(Vector3.UP, -a), Vector3(cos(a) * 14.0, 1.5, sin(a) * 14.0)))
	var light := OmniLight3D.new()
	light.light_color = warm
	light.light_energy = 5.0
	light.omni_range = 140.0
	light.position = Vector3(0, 26, 0)
	root.add_child(light)


## The Anchor: a ring of dark metal sunk in the rock, a chain of light
## running from it straight up into the sky.
static func _anchor(root: Node3D) -> void:
	var metal := _stone(Color(0.16, 0.17, 0.2), Color(0.55, 0.9, 1.0), 5.0)
	var ring := TorusMesh.new()
	ring.inner_radius = 9.0
	ring.outer_radius = 13.0
	ring.rings = 48
	_add(root, ring, metal, Transform3D(Basis(Vector3.RIGHT, PI / 2), Vector3(0, 6.0, 0)))
	var base := CylinderMesh.new()
	base.top_radius = 16.0
	base.bottom_radius = 18.0
	base.height = 3.0
	_add(root, base, metal, Transform3D(Basis.IDENTITY, Vector3(0, -1.0, 0)))
	var chain := CylinderMesh.new()
	chain.top_radius = 0.6
	chain.bottom_radius = 0.6
	chain.height = 2000.0
	chain.radial_segments = 8
	_add(root, chain, _glow(Color(0.55, 0.9, 1.0), 0.15), Transform3D(Basis.IDENTITY, Vector3(0, 1000.0, 0)))


## The Hourglass: a glass the height of a tower, half buried in the sand,
## its two bulbs bright and a stream of light running up between them.
static func _hourglass(root: Node3D) -> void:
	var gold := Color(1.0, 0.8, 0.45)
	var frame := _stone(Color(0.55, 0.42, 0.3), gold, 4.0)
	for half in [-1.0, 1.0]:
		var bulb := CylinderMesh.new()
		bulb.top_radius = 2.0 if half < 0 else 11.0
		bulb.bottom_radius = 11.0 if half < 0 else 2.0
		bulb.height = 22.0
		bulb.radial_segments = 24
		_add(root, bulb, _glow(gold.lerp(Color(0.6, 0.85, 1.0), 0.5), 0.12), Transform3D(Basis.IDENTITY, Vector3(0, 14.0 + half * 11.0 + 11.0, 0)))
	var neck := CylinderMesh.new()
	neck.top_radius = 0.5
	neck.bottom_radius = 0.5
	neck.height = 44.0
	_add(root, neck, _glow(gold, 0.5), Transform3D(Basis.IDENTITY, Vector3(0, 25.0, 0)))
	for k in 3:
		var a := k * TAU / 3.0
		_box(root, Vector3(1.6, 50.0, 1.6), frame, Transform3D(Basis.IDENTITY, Vector3(cos(a) * 12.5, 22.0, sin(a) * 12.5)))
	for y in [2.0, 47.0]:
		var plate := CylinderMesh.new()
		plate.top_radius = 14.0
		plate.bottom_radius = 14.0
		plate.height = 2.0
		plate.radial_segments = 3
		_add(root, plate, frame, Transform3D(Basis.IDENTITY, Vector3(0, y, 0)))


## The Garden: a ring of stones round a bed of light, gone wild: trees
## of the builders' kind grown up through it.
static func _garden(root: Node3D) -> void:
	var green := Color(0.6, 1.0, 0.65)
	var stone := _stone(Color(0.8, 0.8, 0.74), green, 2.0)
	for k in 12:
		var a := k * TAU / 12.0
		_box(root, Vector3(2.2, 6.0 + 2.0 * float(k % 3), 1.0), stone, Transform3D(Basis(Vector3.UP, -a), Vector3(cos(a) * 28.0, 2.5, sin(a) * 28.0)))
	var bed := CylinderMesh.new()
	bed.top_radius = 10.0
	bed.bottom_radius = 10.0
	bed.height = 1.2
	bed.radial_segments = 48
	_add(root, bed, _glow(green, 0.1), Transform3D(Basis.IDENTITY, Vector3(0, 0.2, 0)))
	var bark := _stone(Color(0.35, 0.28, 0.22), green, 50.0)
	for k in 7:
		var a := k * 2.4
		var r := 15.0 + 6.0 * sin(k * 1.7)
		var trunk := CylinderMesh.new()
		trunk.top_radius = 0.5
		trunk.bottom_radius = 0.9
		trunk.height = 12.0 + k
		_add(root, trunk, bark, Transform3D(Basis.IDENTITY, Vector3(cos(a) * r, trunk.height * 0.5, sin(a) * r)))
		var crown := SphereMesh.new()
		crown.radius = 4.0 + 0.4 * k
		crown.height = crown.radius * 1.6
		_add(root, crown, _stone(Color(0.22, 0.48, 0.24), green, 100.0), Transform3D(Basis.IDENTITY, Vector3(cos(a) * r, trunk.height + 2.0, sin(a) * r)))


## The Beacon: a needle of the Chime's pale stone a kilometre and a half
## tall, at the heart of the Great Ring, every groove of it alight.
static func _beacon(root: Node3D) -> void:
	var stone := _stone(Color(0.86, 0.85, 0.82), Color(0.6, 1.0, 0.9), 12.0)
	var needle := CylinderMesh.new()
	needle.top_radius = 1.0
	needle.bottom_radius = 30.0
	needle.height = 1500.0
	needle.radial_segments = 8
	_add(root, needle, stone, Transform3D(Basis.IDENTITY, Vector3(0, 745.0, 0)))
	var foot := CylinderMesh.new()
	foot.top_radius = 45.0
	foot.bottom_radius = 60.0
	foot.height = 12.0
	foot.radial_segments = 8
	_add(root, foot, stone, Transform3D(Basis.IDENTITY, Vector3(0, 2.0, 0)))
	var tip := SphereMesh.new()
	tip.radius = 8.0
	tip.height = 16.0
	_add(root, tip, _glow(Color(0.7, 1.0, 0.95), 0.1), Transform3D(Basis.IDENTITY, Vector3(0, 1500.0, 0)))
	var light := OmniLight3D.new()
	light.light_color = Color(0.6, 1.0, 0.9)
	light.light_energy = 8.0
	light.omni_range = 1200.0
	light.position = Vector3(0, 1500, 0)
	root.add_child(light)
