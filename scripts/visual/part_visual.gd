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
## Procedural visuals for parts, built from each part's "shape" data so
## new parts in the JSON catalog get a body without any art pipeline.
## Used by the hangar and by flight. Views only: no gameplay state here.
class_name PartVisual
extends RefCounted

const PICK_LAYER := 1 << 1

static var _mats := {}


static func mat(key: String, color: Color, metal := 0.0, rough := 0.6, emission := Color.BLACK) -> StandardMaterial3D:
	if _mats.has(key):
		return _mats[key]
	var m := StandardMaterial3D.new()
	m.albedo_color = color
	m.metallic = metal
	m.roughness = rough
	if emission != Color.BLACK:
		m.emission_enabled = true
		m.emission = emission
	_mats[key] = m
	return m


static func ghost_mat(ok: bool) -> StandardMaterial3D:
	var key := "ghost_ok" if ok else "ghost_bad"
	if _mats.has(key):
		return _mats[key]
	var m := StandardMaterial3D.new()
	m.albedo_color = Color(0.45, 0.9, 1.0, 0.45) if ok else Color(1.0, 0.4, 0.3, 0.45)
	m.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	m.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	m.cull_mode = BaseMaterial3D.CULL_DISABLED
	_mats[key] = m
	return m


static func highlight_mat() -> StandardMaterial3D:
	return mat("highlight", Color(1.0, 0.75, 0.2), 0.0, 0.4, Color(0.6, 0.35, 0.0))


# Palette: PASA white, ice blue, signal orange, graphite.
const WHITE := Color(0.93, 0.94, 0.96)
const ICE := Color(0.55, 0.78, 0.95)
const ORANGE := Color(1.0, 0.52, 0.12)
const GRAPHITE := Color(0.16, 0.17, 0.2)
const STEEL := Color(0.55, 0.57, 0.6)
const YELLOW := Color(0.98, 0.8, 0.15)
const RED := Color(0.86, 0.2, 0.18)


static func build(part_id: String, with_pick := true) -> Node3D:
	var d: Dictionary = Sim.part(part_id)
	var sh: Dictionary = d.get("shape", {})
	var root := Node3D.new()
	root.name = part_id
	match sh.get("type", "cylinder"):
		"capsule":
			_capsule(root, sh, d.get("crew", 1))
		"cylinder":
			_cylinder_part(root, d, sh)
		"engine":
			_engine(root, sh)
		"booster":
			_booster(root, sh)
		"cone":
			_cone(root, d, sh)
		"pylon":
			_pylon(root, sh)
		"fin":
			_fin(root, sh)
		"leg":
			_leg(root, sh)
		_:
			_cyl(root, sh.get("radius", 0.5), sh.get("radius", 0.5), sh.get("height", 1.0), 0.0, mat("white", WHITE))
	if with_pick:
		_add_pick(root, sh)
	return root


static func _mesh(parent: Node3D, mesh: Mesh, m: Material, xf := Transform3D.IDENTITY) -> MeshInstance3D:
	var mi := MeshInstance3D.new()
	mi.mesh = mesh
	mi.material_override = m
	mi.transform = xf
	parent.add_child(mi)
	return mi


static func _cyl(parent: Node3D, r_bottom: float, r_top: float, h: float, y: float, m: Material, segs := 32) -> MeshInstance3D:
	var c := CylinderMesh.new()
	c.bottom_radius = r_bottom
	c.top_radius = r_top
	c.height = h
	c.radial_segments = segs
	c.rings = 1
	return _mesh(parent, c, m, Transform3D(Basis.IDENTITY, Vector3(0, y, 0)))


static func _capsule(root: Node3D, sh: Dictionary, crew: int) -> void:
	var rb: float = sh.radius_bottom
	var rt: float = sh.radius_top
	var h: float = sh.height
	_cyl(root, rb, rt, h, 0.0, mat("white", WHITE))
	_cyl(root, rb * 1.02, rb * 1.02, h * 0.08, -h * 0.46, mat("graphite", GRAPHITE))  # heat shield rim
	_cyl(root, rt * 1.05, rt * 1.05, h * 0.06, h * 0.47, mat("steel", STEEL, 0.7, 0.3))
	# windows: one per seat, facing +Z
	var win := mat("window", Color(0.1, 0.25, 0.4), 0.2, 0.1, Color(0.05, 0.12, 0.2))
	for i in crew:
		var a := (float(i) - (crew - 1) * 0.5) * 0.55
		var y := 0.08 * h
		var rr := lerpf(rb, rt, (y + h * 0.5) / h)
		var box := BoxMesh.new()
		box.size = Vector3(0.22, 0.2, 0.05)
		var b := Basis(Vector3.UP, a)
		var tilt := atan2(rb - rt, h)
		b = b * Basis(Vector3.RIGHT, -tilt)
		_mesh(root, box, win, Transform3D(b, Basis(Vector3.UP, a) * Vector3(0, y, rr)))
	# PASA band
	_cyl(root, rb * 0.97 + 0.012, rb * 0.97 + 0.012, h * 0.05, -h * 0.32, mat("orange", ORANGE))


static func _cylinder_part(root: Node3D, d: Dictionary, sh: Dictionary) -> void:
	var r: float = sh.radius
	var h: float = sh.height
	if d.has("decoupler"):
		_cyl(root, r, r, h * 0.5, h * 0.25, mat("yellow", YELLOW))
		_cyl(root, r, r, h * 0.5, -h * 0.25, mat("graphite", GRAPHITE))
	elif d.has("fuel_liquid"):
		_cyl(root, r, r, h, 0.0, mat("white", WHITE, 0.0, 0.5))
		var bands := maxi(1, int(round(h / 0.95)))
		for i in bands:
			var y := -h * 0.5 + (i + 0.5) * h / bands
			_cyl(root, r * 1.01, r * 1.01, minf(0.12, h * 0.1), y, mat("ice", ICE))
		_cyl(root, r * 1.012, r * 1.012, 0.04, h * 0.5 - 0.02, mat("steel", STEEL, 0.7, 0.3))
		_cyl(root, r * 1.012, r * 1.012, 0.04, -h * 0.5 + 0.02, mat("steel", STEEL, 0.7, 0.3))
	else:  # reaction wheel and other short cylinders
		_cyl(root, r, r, h, 0.0, mat("graphite", GRAPHITE, 0.3, 0.5))
		_cyl(root, r * 1.01, r * 1.01, h * 0.25, 0.0, mat("orange", ORANGE))


static func _engine(root: Node3D, sh: Dictionary) -> void:
	var r: float = sh.radius
	var rb: float = sh.radius_bell
	var h: float = sh.height
	var mount_h := h * 0.35
	_cyl(root, r * 0.9, r, mount_h, h * 0.5 - mount_h * 0.5, mat("graphite", GRAPHITE, 0.4, 0.5))
	_cyl(root, r * 0.35, r * 0.6, h * 0.2, h * 0.5 - mount_h - h * 0.1, mat("steel", STEEL, 0.8, 0.3))
	var bell_h := h * 0.5
	var bell := _cyl(root, rb, rb * 0.35, bell_h, -h * 0.5 + bell_h * 0.5, mat("bell", Color(0.32, 0.3, 0.3), 0.9, 0.35))
	bell.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_ON


static func _booster(root: Node3D, sh: Dictionary) -> void:
	var r: float = sh.radius
	var h: float = sh.height
	_cyl(root, r, r, h, 0.0, mat("white", WHITE))
	for i in 4:
		_cyl(root, r * 1.01, r * 1.01, 0.16, -h * 0.4 + i * h * 0.27, mat("red", RED))
	_cyl(root, r * 0.75, r * 0.45, 0.5, -h * 0.5 - 0.15, mat("bell", Color(0.32, 0.3, 0.3), 0.9, 0.35))


static func _cone(root: Node3D, d: Dictionary, sh: Dictionary) -> void:
	var rb: float = sh.radius_bottom
	var rt: float = sh.radius_top
	var h: float = sh.height
	if d.has("parachute"):
		_cyl(root, rb, rt, h, 0.0, mat("chute", Color(0.95, 0.35, 0.25)))
		_cyl(root, rb * 1.02, rb * 1.02, h * 0.2, -h * 0.4, mat("white", WHITE))
	else:
		_cyl(root, rb, maxf(rt, 0.01), h, 0.0, mat("white", WHITE), 32)
		_cyl(root, rb * 1.01, rb * 0.82, h * 0.18, -h * 0.41, mat("orange", ORANGE))


static func _pylon(root: Node3D, sh: Dictionary) -> void:
	var box := BoxMesh.new()
	var depth: float = sh.get("depth", 0.3)
	box.size = Vector3(sh.radius * 2.0, sh.height, depth)
	_mesh(root, box, mat("yellow", YELLOW))
	var stripe := BoxMesh.new()
	stripe.size = Vector3(sh.radius * 2.02, sh.height * 0.25, depth * 1.02)
	_mesh(root, stripe, mat("graphite", GRAPHITE))


static func _fin(root: Node3D, sh: Dictionary) -> void:
	# trapezoid in the local YZ plane: root chord along Y at z=0, span along +Z
	var span: float = sh.span
	var rc: float = sh.root
	var tc: float = sh.tip
	var t: float = sh.thickness * 0.5
	var st := SurfaceTool.new()
	st.begin(Mesh.PRIMITIVE_TRIANGLES)
	var p := [
		Vector3(0, rc * 0.5, 0), Vector3(0, -rc * 0.5, 0),
		Vector3(0, -rc * 0.5, span), Vector3(0, -rc * 0.5 + tc, span),
	]
	for side in [-1.0, 1.0]:
		var o := Vector3(t * side, 0, 0)
		var tri := [p[0], p[1], p[2], p[0], p[2], p[3]]
		if side < 0:
			tri = [p[0], p[2], p[1], p[0], p[3], p[2]]
		for v in tri:
			st.set_normal(Vector3(side, 0, 0))
			st.add_vertex(v + o)
	# edges
	for e in 4:
		var a: Vector3 = p[e]
		var b: Vector3 = p[(e + 1) % 4]
		var n := (b - a).cross(Vector3.RIGHT).normalized()
		for v in [a + Vector3(t, 0, 0), b + Vector3(t, 0, 0), b - Vector3(t, 0, 0), a + Vector3(t, 0, 0), b - Vector3(t, 0, 0), a - Vector3(t, 0, 0)]:
			st.set_normal(n)
			st.add_vertex(v)
	var m := st.commit()
	var mi := _mesh(root, m, mat("fin", ORANGE))
	mi.material_override.cull_mode = BaseMaterial3D.CULL_DISABLED


static func _leg(root: Node3D, sh: Dictionary) -> void:
	var l: float = sh.length
	var r: float = sh.radius
	# strut angled down and out from the attach point
	var strut := CylinderMesh.new()
	strut.top_radius = r
	strut.bottom_radius = r * 0.8
	strut.height = l
	var ang := deg_to_rad(20)
	var b := Basis(Vector3.RIGHT, -ang)
	var center := b * Vector3(0, -l * 0.5, 0)
	_mesh(root, strut, mat("steel", STEEL, 0.7, 0.3), Transform3D(b, center + Vector3(0, 0, r)))
	var foot := CylinderMesh.new()
	foot.top_radius = 0.18
	foot.bottom_radius = 0.22
	foot.height = 0.06
	_mesh(root, foot, mat("graphite", GRAPHITE), Transform3D(Basis.IDENTITY, b * Vector3(0, -l, 0) + Vector3(0, 0, r)))
	var box := BoxMesh.new()
	box.size = Vector3(0.18, 0.3, 0.12)
	_mesh(root, box, mat("yellow", YELLOW), Transform3D(Basis.IDENTITY, Vector3(0, -0.1, 0.06)))


static func _add_pick(root: Node3D, sh: Dictionary) -> void:
	var body := StaticBody3D.new()
	body.name = "Pick"
	body.collision_layer = PICK_LAYER
	body.collision_mask = 0
	var cs := CollisionShape3D.new()
	match sh.get("type", ""):
		"fin":
			var b := BoxShape3D.new()
			b.size = Vector3(0.08, sh.root, sh.span)
			cs.shape = b
			cs.position = Vector3(0, 0, sh.span * 0.5)
		"leg":
			var b := BoxShape3D.new()
			b.size = Vector3(0.3, sh.length, 0.5)
			cs.shape = b
			cs.position = Vector3(0, -sh.length * 0.5, 0.2)
		"pylon":
			var b := BoxShape3D.new()
			b.size = Vector3(sh.radius * 2.0, sh.height, sh.get("depth", 0.3))
			cs.shape = b
		_:
			var c := CylinderShape3D.new()
			c.radius = sh.get("radius", sh.get("radius_bottom", 0.5))
			c.height = sh.get("height", 1.0)
			cs.shape = c
	body.add_child(cs)
	root.add_child(body)


## Applies a material to every mesh under a node (ghosts, highlights).
static func paint(n: Node, m: Material) -> void:
	for c in n.get_children():
		if c is MeshInstance3D:
			(c as MeshInstance3D).material_override = m
		paint(c, m)


static func xform(pos: Array, rot: Array) -> Transform3D:
	return Transform3D(Basis(DMath.to_quat(rot)), DMath.to_v3(pos))
