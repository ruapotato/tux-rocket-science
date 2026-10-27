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
## Procedural plants and animals for the ground, built from a body's "life"
## data (shape, color, color2): no art pipeline, like the parts. Plants are
## one small mesh each (1 m tall, standing on y = 0, scaled per instance),
## coloured by vertex; alpha below 1 marks parts that glow. Animals are
## little rigs facing +Z, 1 m to a size unit, posed every frame.
class_name LifeModels
extends RefCounted

const PLANT_SHADER := """
shader_type spatial;
render_mode cull_disabled;
uniform float sway = 0.0;      // metres of lean at the top in the wind
uniform float glow = 1.6;
varying vec4 col;
void vertex() {
	col = COLOR;
	vec3 wp = MODEL_MATRIX[3].xyz;
	float ph = dot(wp, vec3(0.071, 0.113, 0.093));
	float k = VERTEX.y * VERTEX.y;
	VERTEX.x += sway * k * sin(TIME * 1.3 + ph) * 0.6;
	VERTEX.z += sway * k * sin(TIME * 0.9 + ph * 1.7) * 0.4;
}
void fragment() {
	ALBEDO = col.rgb;
	ROUGHNESS = 0.85;
	EMISSION = col.rgb * (1.0 - col.a) * glow;
}
"""

static var _shader: Shader
static var _mats := {}


static func _color(spec: Dictionary, key: String, fallback: Color) -> Color:
	return Sim.col(spec.get(key), fallback)


static func _material(sway: float, crystal := false) -> Material:
	var key := "%s:%.2f" % ["crystal" if crystal else "plant", sway]
	if _mats.has(key):
		return _mats[key]
	var m: Material
	if crystal:
		var sm := StandardMaterial3D.new()
		sm.vertex_color_use_as_albedo = true
		sm.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
		sm.albedo_color = Color(1, 1, 1, 0.75)
		sm.roughness = 0.08
		sm.metallic_specular = 1.0
		sm.rim_enabled = true
		sm.rim = 0.5
		sm.cull_mode = BaseMaterial3D.CULL_DISABLED
		m = sm
	else:
		if _shader == null:
			_shader = Shader.new()
			_shader.code = PLANT_SHADER
		var shm := ShaderMaterial.new()
		shm.shader = _shader
		shm.set_shader_parameter("sway", sway)
		m = shm
	_mats[key] = m
	return m


# ---- mesh pieces -------------------------------------------------------------

static func _tri(st: SurfaceTool, a: Vector3, b: Vector3, c: Vector3, ca: Color, cb: Color, cc: Color) -> void:
	var n := (b - a).cross(c - a).normalized()
	st.set_normal(n)
	st.set_color(ca)
	st.add_vertex(a)
	st.set_color(cb)
	st.add_vertex(b)
	st.set_color(cc)
	st.add_vertex(c)


## A frustum (or a cone with r1 = 0) from y0 to y1 round the Y axis at `at`.
static func _frustum(st: SurfaceTool, at: Vector3, y0: float, y1: float, r0: float, r1: float, segs: int, c0: Color, c1: Color) -> void:
	for k in segs:
		var a0 := TAU * k / segs
		var a1 := TAU * (k + 1) / segs
		var p00 := at + Vector3(cos(a0) * r0, y0, sin(a0) * r0)
		var p01 := at + Vector3(cos(a1) * r0, y0, sin(a1) * r0)
		var p10 := at + Vector3(cos(a0) * r1, y1, sin(a0) * r1)
		var p11 := at + Vector3(cos(a1) * r1, y1, sin(a1) * r1)
		_tri(st, p00, p10, p01, c0, c1, c0)
		if r1 > 0.0:
			_tri(st, p01, p10, p11, c0, c1, c1)
		_tri(st, at + Vector3(0, y0, 0), p00, p01, c0, c0, c0)


## A lumpy ball: radii `r`, `jag` of random bumps, `top` colour on the
## upward faces (snow, moss), the rest `col`.
static func _blob(st: SurfaceTool, at: Vector3, r: Vector3, col: Color, top: Color, jag: float, seed: int, segs := 7) -> void:
	var rng := RandomNumberGenerator.new()
	rng.seed = seed
	var rows := maxi(3, segs - 2)
	var grid := []
	for y in rows + 1:
		var row := []
		var th := PI * y / rows
		for x in segs:
			var ph := TAU * x / segs
			var d := Vector3(sin(th) * cos(ph), cos(th), sin(th) * sin(ph))
			var j := 1.0 + (rng.randf() - 0.5) * 2.0 * jag if y > 0 and y < rows else 1.0
			row.append(at + d * r * j)
		grid.append(row)
	for y in rows:
		for x in segs:
			var a: Vector3 = grid[y][x]
			var b: Vector3 = grid[y][(x + 1) % segs]
			var c: Vector3 = grid[y + 1][x]
			var d: Vector3 = grid[y + 1][(x + 1) % segs]
			var up_a := clampf(((a + b + c) / 3.0 - at).normalized().y * 2.0 - 0.6, 0.0, 1.0)
			var ct := col.lerp(top, up_a)
			_tri(st, a, c, b, ct, ct, ct)
			_tri(st, b, c, d, ct, ct, ct)


## A ribbon (blade of grass, frond) through `pts`, `w` wide, flat across `side`.
static func _ribbon(st: SurfaceTool, pts: Array, w: float, side: Vector3, c0: Color, c1: Color) -> void:
	for k in pts.size() - 1:
		var f0 := 1.0 - float(k) / (pts.size() - 1)
		var f1 := 1.0 - float(k + 1) / (pts.size() - 1)
		var p0: Vector3 = pts[k]
		var p1: Vector3 = pts[k + 1]
		var a := p0 - side * w * f0
		var b := p0 + side * w * f0
		var c := p1 - side * w * f1
		var d := p1 + side * w * f1
		var ca := c0.lerp(c1, float(k) / (pts.size() - 1))
		var cb := c0.lerp(c1, float(k + 1) / (pts.size() - 1))
		_tri(st, a, c, b, ca, cb, ca)
		_tri(st, b, c, d, ca, cb, cb)


# ---- plants ------------------------------------------------------------------

## The mesh for one kind of plant (or rock, crystal, mat).
static func plant(spec: Dictionary) -> Mesh:
	var c1 := _color(spec, "color", Color(0.3, 0.5, 0.25))
	var c2 := _color(spec, "color2", c1.lightened(0.3))
	var st := SurfaceTool.new()
	st.begin(Mesh.PRIMITIVE_TRIANGLES)
	var sway := 0.0
	var crystal := false
	var bark := Color(0.3, 0.2, 0.13)
	match spec.get("shape", "rock"):
		"pine":
			_frustum(st, Vector3.ZERO, -0.1, 0.25, 0.045, 0.03, 6, bark, bark)
			# tiers of needles, each with a cap of color2 (snow on Floe)
			for k in 4:
				var y0 := 0.16 + k * 0.19
				var r := 0.33 - k * 0.065
				_frustum(st, Vector3.ZERO, y0, y0 + 0.36, r, 0.0, 9, c1, c1.lightened(0.1))
				_frustum(st, Vector3.ZERO, y0 + 0.12, y0 + 0.4, r * 0.78, 0.0, 9, c2.lerp(c1, 0.25), c2)
			sway = 0.04
		"broadleaf":
			_frustum(st, Vector3.ZERO, -0.1, 0.55, 0.06, 0.035, 7, c1, c1)
			var leaf := c2
			for k in 5:
				var a := TAU * k / 5.0
				var at := Vector3(cos(a) * 0.17, 0.62 + 0.06 * sin(a * 2.0), sin(a) * 0.17)
				_blob(st, at, Vector3(0.24, 0.2, 0.24), leaf.darkened(0.08 * (k % 2)), leaf.lightened(0.12), 0.18, 31 + k)
			_blob(st, Vector3(0, 0.78, 0), Vector3(0.22, 0.2, 0.22), leaf, leaf.lightened(0.15), 0.15, 77)
			sway = 0.05
		"frond":
			# a tall curved stalk, a crown of long drooping fronds
			var pts := []
			for k in 7:
				var y := k / 6.0 * 0.8
				pts.append(Vector3(0.06 * sin(y * 3.0), y, 0.0))
			for k in 6:
				var p0: Vector3 = pts[k]
				var p1: Vector3 = pts[k + 1]
				var r0 := 0.035 * (1.0 - k / 7.0) + 0.012
				var r1 := 0.035 * (1.0 - (k + 1) / 7.0) + 0.012
				_frustum(st, p0, 0.0, p1.y - p0.y, r0, r1, 6, c1, c1)
			var top: Vector3 = pts[6]
			for k in 7:
				var a := TAU * k / 7.0 + 0.3
				var dir := Vector3(cos(a), 0, sin(a))
				var fr := []
				for e in 6:
					var t := e / 5.0
					fr.append(top + dir * (0.32 * t) + Vector3(0, 0.08 * t - 0.34 * t * t, 0))
				_ribbon(st, fr, 0.05, dir.cross(Vector3.UP).normalized(), c2.darkened(0.15), c2.lightened(0.1))
			sway = 0.12
		"shrub":
			for k in 4:
				var a := TAU * k / 4.0 + 0.4
				_blob(st, Vector3(cos(a) * 0.22, 0.28, sin(a) * 0.22), Vector3(0.3, 0.3, 0.3), c1.darkened(0.1 * (k % 2)), c1.lightened(0.1), 0.25, 11 + k)
			for k in 7:
				var a := TAU * k / 7.0
				_blob(st, Vector3(cos(a) * 0.38, 0.4 + 0.1 * sin(a * 3.0), sin(a) * 0.38), Vector3(0.05, 0.05, 0.05), c2, c2, 0.0, k, 5)
			sway = 0.02
		"grass":
			for k in 7:
				var a := TAU * k / 7.0 + 0.2 * k
				var dir := Vector3(cos(a), 0, sin(a))
				var lean := 0.15 + 0.1 * (k % 3)
				var pts := []
				for e in 4:
					var t := e / 3.0
					pts.append(dir * (0.04 + lean * t * t) + Vector3(0, t * (0.75 + 0.25 * ((k * 5) % 3) / 2.0), 0))
				_ribbon(st, pts, 0.035, dir.cross(Vector3.UP).normalized(), c1.darkened(0.25), c1.lightened(0.15))
			sway = 0.15
		"flower":
			var pts := [Vector3.ZERO, Vector3(0.02, 0.45, 0), Vector3(0, 0.9, 0.02)]
			_ribbon(st, pts, 0.012, Vector3.RIGHT, c1, c1)
			_ribbon(st, pts, 0.012, Vector3.BACK, c1, c1)
			for k in 5:
				var a := TAU * k / 5.0
				_blob(st, Vector3(cos(a) * 0.06, 0.9, sin(a) * 0.06 + 0.02), Vector3(0.06, 0.025, 0.06), c2, c2, 0.0, k, 5)
			_blob(st, Vector3(0, 0.91, 0.02), Vector3(0.03, 0.03, 0.03), Color(0.95, 0.6, 0.1), Color(0.95, 0.6, 0.1), 0.0, 9, 5)
			sway = 0.1
		"puffball":
			_frustum(st, Vector3.ZERO, -0.05, 0.62, 0.05, 0.035, 6, c1, c1)
			var g := Color(c2.r, c2.g, c2.b, 0.0)  # (alpha 0: it glows)
			_blob(st, Vector3(0, 0.75, 0), Vector3(0.22, 0.2, 0.22), g, g, 0.06, 5, 9)
			for k in 5:
				var a := TAU * k / 5.0
				_blob(st, Vector3(cos(a) * 0.12, 0.25 + k * 0.06, sin(a) * 0.12), Vector3(0.07, 0.035, 0.07), c1.lightened(0.2), c1, 0.0, k, 5)
			sway = 0.03
		"crystal":
			crystal = true
			var rng := RandomNumberGenerator.new()
			rng.seed = 7
			for k in 4:
				var a := TAU * k / 4.0 + rng.randf()
				var tilt := Basis(Vector3(cos(a + PI / 2), 0, sin(a + PI / 2)), 0.25 + rng.randf() * 0.25) if k > 0 else Basis.IDENTITY
				var r := 0.09 if k == 0 else 0.05 + rng.randf() * 0.03
				var h := 1.0 if k == 0 else 0.45 + rng.randf() * 0.3
				var base := Vector3(cos(a) * 0.08, -0.05, sin(a) * 0.08) if k > 0 else Vector3(0, -0.05, 0)
				var ring := []
				for e in 6:
					var ang := TAU * e / 6.0
					ring.append(Vector3(cos(ang) * r, 0, sin(ang) * r))
				for e in 6:
					var a0: Vector3 = base + tilt * ring[e]
					var a1: Vector3 = base + tilt * ring[(e + 1) % 6]
					var b0: Vector3 = base + tilt * (ring[e] + Vector3(0, h * 0.82, 0))
					var b1: Vector3 = base + tilt * (ring[(e + 1) % 6] + Vector3(0, h * 0.82, 0))
					var tip: Vector3 = base + tilt * Vector3(0, h, 0)
					_tri(st, a0, b0, a1, c1.darkened(0.15), c1, c1.darkened(0.15))
					_tri(st, a1, b0, b1, c1.darkened(0.15), c1, c1)
					_tri(st, b0, tip, b1, c1, c1.lightened(0.3), c1)
		"mat":
			var rng := RandomNumberGenerator.new()
			rng.seed = 3
			var segs := 14
			var rad := []
			for k in segs:
				rad.append(0.5 * (0.75 + 0.25 * rng.randf()))
			for ring in 3:
				var f0 := ring / 3.0
				var f1 := (ring + 1) / 3.0
				var cc := c1 if ring % 2 == 0 else c2
				for k in segs:
					var a0 := TAU * k / segs
					var a1 := TAU * (k + 1) / segs
					var r0: float = rad[k]
					var r1: float = rad[(k + 1) % segs]
					var p00 := Vector3(cos(a0) * r0 * f0, 0.02, sin(a0) * r0 * f0)
					var p01 := Vector3(cos(a1) * r1 * f0, 0.02, sin(a1) * r1 * f0)
					var p10 := Vector3(cos(a0) * r0 * f1, 0.015, sin(a0) * r0 * f1)
					var p11 := Vector3(cos(a1) * r1 * f1, 0.015, sin(a1) * r1 * f1)
					_tri(st, p00, p10, p01, cc, cc, cc)
					_tri(st, p01, p10, p11, cc, cc, cc)
		_:  # a boulder (size is its height): lumpy, half sunk, snow or moss on top
			_blob(st, Vector3(0, 0.3, 0), Vector3(0.7, 0.55, 0.6), c1, c2, 0.22, 19, 9)
	st.index()
	var mesh := st.commit()
	mesh.surface_set_material(0, _material(sway, crystal))
	return mesh


# ---- animals -------------------------------------------------------------------

static func _m(c: Color, glow := 0.0, alpha := 1.0) -> StandardMaterial3D:
	var key := "%s:%.2f:%.2f" % [c.to_html(), glow, alpha]
	if _mats.has(key):
		return _mats[key]
	var m := StandardMaterial3D.new()
	m.albedo_color = Color(c.r, c.g, c.b, alpha)
	m.roughness = 0.7
	if alpha < 1.0:
		m.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
		m.cull_mode = BaseMaterial3D.CULL_DISABLED
	if glow > 0.0:
		m.emission_enabled = true
		m.emission = c
		m.emission_energy_multiplier = glow
	_mats[key] = m
	return m


static func _part(parent: Node3D, mesh: Mesh, m: Material, at: Vector3, scale := Vector3.ONE, name := "") -> MeshInstance3D:
	var mi := MeshInstance3D.new()
	mi.mesh = mesh
	mi.material_override = m
	mi.position = at
	mi.scale = scale
	if name != "":
		mi.name = name
	parent.add_child(mi)
	return mi


static func _sphere(r: float) -> SphereMesh:
	var s := SphereMesh.new()
	s.radius = r
	s.height = r * 2.0
	s.radial_segments = 12
	s.rings = 6
	return s


static func _rod(r: float, h: float) -> CylinderMesh:
	var c := CylinderMesh.new()
	c.top_radius = r
	c.bottom_radius = r
	c.height = h
	c.radial_segments = 6
	c.rings = 1
	return c


static func _pivot(parent: Node3D, name: String, at: Vector3) -> Node3D:
	var p := Node3D.new()
	p.name = name
	p.position = at
	parent.add_child(p)
	return p


## An animal's rig: the root (placed by the caller), a "Rig" child that the
## pose moves, the parts under it.
static func animal(spec: Dictionary, _size: float) -> Node3D:
	var c1 := _color(spec, "color", Color(0.1, 0.1, 0.12))
	var c2 := _color(spec, "color2", Color(0.95, 0.95, 0.95))
	var root := Node3D.new()
	var rig := Node3D.new()
	rig.name = "Rig"
	root.add_child(rig)
	var orange := _m(Color(1.0, 0.55, 0.12))
	match spec.get("shape", ""):
		"penguin":
			var black := _m(Color(0.07, 0.07, 0.09))
			_part(rig, _sphere(0.28), black, Vector3(0, 0.42, 0), Vector3(1, 1.45, 0.95), "Body")
			_part(rig, _sphere(0.24), _m(Color(0.96, 0.96, 0.94)), Vector3(0, 0.4, 0.07), Vector3(1, 1.4, 0.9))
			_part(rig, _sphere(0.17), black, Vector3(0, 0.86, 0.02), Vector3.ONE, "Head")
			var beak := CylinderMesh.new()
			beak.top_radius = 0.0
			beak.bottom_radius = 0.045
			beak.height = 0.14
			var bk := _part(rig, beak, orange, Vector3(0, 0.84, 0.2))
			bk.rotation.x = PI / 2
			for s in [-1.0, 1.0]:
				_part(rig, BoxMesh.new(), orange, Vector3(s * 0.09, 0.015, 0.08), Vector3(0.1, 0.03, 0.16))
				var w := _pivot(rig, "WingL" if s < 0 else "WingR", Vector3(s * 0.26, 0.6, 0))
				_part(w, BoxMesh.new(), black, Vector3(s * 0.03, -0.17, 0), Vector3(0.04, 0.36, 0.14))
		"seal":
			var body := _m(c1)
			_part(rig, _sphere(0.2), body, Vector3(0, 0.18, 0), Vector3(1, 0.85, 2.6), "Body")
			_part(rig, _sphere(0.13), body, Vector3(0, 0.3, 0.48), Vector3.ONE, "Head")
			_part(rig, _sphere(0.05), _m(Color(0.05, 0.05, 0.06)), Vector3(0, 0.3, 0.6))
			for s in [-1.0, 1.0]:
				_part(rig, BoxMesh.new(), _m(c2), Vector3(s * 0.2, 0.05, 0.18), Vector3(0.14, 0.03, 0.12))
			_part(rig, BoxMesh.new(), _m(c2), Vector3(0, 0.1, -0.52), Vector3(0.28, 0.03, 0.12), "Tail")
		"bird":
			var body := _m(c1)
			_part(rig, _sphere(0.12), body, Vector3.ZERO, Vector3(0.9, 0.9, 1.9), "Body")
			_part(rig, _sphere(0.1), _m(Color(0.95, 0.95, 0.95)), Vector3(0, -0.03, 0.05), Vector3(0.8, 0.8, 1.5))
			_part(rig, _sphere(0.09), body, Vector3(0, 0.05, 0.22), Vector3.ONE, "Head")
			var beak := CylinderMesh.new()
			beak.top_radius = 0.0
			beak.bottom_radius = 0.04
			beak.height = 0.12
			var bk := _part(rig, beak, _m(c2), Vector3(0, 0.04, 0.33))
			bk.rotation.x = PI / 2
			for s in [-1.0, 1.0]:
				var w := _pivot(rig, "WingL" if s < 0 else "WingR", Vector3(s * 0.08, 0.04, 0))
				_part(w, BoxMesh.new(), body, Vector3(s * 0.28, 0, 0), Vector3(0.56, 0.02, 0.2))
		"grazer":
			var body := _m(c1)
			var legs := int(spec.get("legs", 4))
			_part(rig, _sphere(0.4), body, Vector3(0, 0.68, 0), Vector3(0.95, 0.85, 1.5), "Body")
			_part(rig, _sphere(0.3), _m(c2), Vector3(0, 0.56, 0.02), Vector3(0.85, 0.55, 1.35))
			var neck := _pivot(rig, "Neck", Vector3(0, 0.86, 0.5))
			_part(neck, _rod(0.11, 0.35), body, Vector3(0, 0.08, 0.1)).rotation.x = 0.9
			_part(neck, _sphere(0.18), body, Vector3(0, 0.2, 0.28), Vector3(0.9, 0.9, 1.3), "Head")
			for s in [-1.0, 1.0]:
				_part(neck, _sphere(0.03), _m(Color(0.05, 0.05, 0.06)), Vector3(s * 0.12, 0.26, 0.38))
			if spec.get("horns", true):
				for s in [-1.0, 1.0]:
					_part(neck, _rod(0.02, 0.28), _m(c2.darkened(0.2)), Vector3(s * 0.09, 0.38, 0.22)).rotation.z = -s * 0.4
			for k in legs:
				var z := lerpf(0.4, -0.4, float(k / 2) / maxf(1.0, legs / 2 - 1))
				var s := -1.0 if k % 2 == 0 else 1.0
				var lg := _pivot(rig, "Leg%d" % k, Vector3(s * 0.22, 0.5, z))
				_part(lg, _rod(0.075, 0.5), body, Vector3(0, -0.25, 0))
				_part(lg, _sphere(0.08), _m(c1.darkened(0.4)), Vector3(0, -0.5, 0.02), Vector3(1, 0.6, 1.3))
		"drifter":
			var bell := SphereMesh.new()
			bell.radius = 0.5
			bell.height = 0.5
			bell.is_hemisphere = true
			bell.radial_segments = 16
			bell.rings = 6
			_part(rig, bell, _m(c1, 1.2, 0.55), Vector3(0, 0, 0), Vector3.ONE, "Body")
			for k in 7:
				var a := TAU * k / 7.0
				var t := _pivot(rig, "Tent%d" % k, Vector3(cos(a) * 0.3, 0, sin(a) * 0.3))
				_part(t, _rod(0.015, 0.9), _m(c2, 1.5, 0.8), Vector3(0, -0.45, 0))
		_:
			_part(rig, _sphere(0.3), _m(c1), Vector3(0, 0.3, 0))
	return root


## Poses a rig at clock t moving at `speed` m/s (0: standing).
static func pose(root: Node3D, shape: String, t: float, speed: float) -> void:
	var rig := root.get_node_or_null("Rig") as Node3D
	if rig == null:
		return
	var moving := clampf(speed / 0.3, 0.0, 1.0)
	match shape:
		"penguin":
			var w := t * 9.0
			rig.rotation.z = sin(w) * 0.16 * moving
			rig.position.y = absf(sin(w)) * 0.03 * moving
			for n in ["WingL", "WingR"]:
				var f := rig.get_node(n) as Node3D
				f.rotation.z = (0.25 + 0.15 * sin(t * 3.0)) * (1.0 if n == "WingR" else -1.0)
			var head := rig.get_node("Head") as Node3D
			head.rotation.y = sin(t * 0.7) * 0.5 * (1.0 - moving)
		"seal":
			rig.rotation.x = sin(t * 5.0) * 0.08 * moving
			var head := rig.get_node("Head") as Node3D
			head.position.y = 0.3 + 0.05 * sin(t * 0.8) * (1.0 - moving) + 0.03 * moving
		"bird":
			var flap := sin(t * 13.0) * 0.75
			(rig.get_node("WingL") as Node3D).rotation.z = flap
			(rig.get_node("WingR") as Node3D).rotation.z = -flap
			rig.position.y = sin(t * 13.0 + 1.0) * 0.05
		"grazer":
			var w := t * 5.0
			for k in 6:
				var lg := rig.get_node_or_null("Leg%d" % k) as Node3D
				if lg:
					lg.rotation.x = sin(w + (PI if (k + k / 2) % 2 == 1 else 0.0)) * 0.5 * moving
			var neck := rig.get_node("Neck") as Node3D
			neck.rotation.x = lerpf(0.9 + 0.1 * sin(t * 2.0), 0.0, moving)  # head down to graze when it stops
			rig.position.y = absf(sin(w)) * 0.03 * moving
		"drifter":
			var p := sin(t * 1.6)
			var body := rig.get_node("Body") as Node3D
			body.scale = Vector3(1.0 - 0.08 * p, 1.0 + 0.12 * p, 1.0 - 0.08 * p)
			rig.position.y = sin(t * 0.5) * 0.4
			for k in 7:
				var tn := rig.get_node_or_null("Tent%d" % k) as Node3D
				if tn:
					tn.rotation.x = sin(t * 1.2 + k) * 0.25
					tn.rotation.z = cos(t * 1.1 + k * 1.3) * 0.25
