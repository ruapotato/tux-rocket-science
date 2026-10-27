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
## Real-scale terrain for the body the active vessel is in: trs_core picks
## the cube-sphere LOD leaves and generates each patch's vertices; this node
## only turns them into meshes and places them relative to the floating origin.
class_name TerrainView
extends Node3D

const RES := 25
const MAX_LEVEL := 14
const SPLIT := 2.6
const BUILD_PER_FRAME := 6

var core: TrsSim
var body := -1
var _style := {}
var _patches := {}  # key -> {node, center: [x, y, z] sim body-fixed, doubles}
var _indices := PackedInt32Array()
var _material: ShaderMaterial
var _edge_list: Array = []
var _noise := FastNoiseLite.new()
var _pending: Array = []
var _site_node: Node3D
var _site_pos := Vector3.ZERO
var _site_d: Array = [0.0, 0.0, 0.0]  # the same, Godot axes, doubles
var _relics: Array = []  # [{id, node, pos (Godot, body-fixed), d (doubles)}]


func setup(c: TrsSim) -> void:
	core = c
	_noise.frequency = 1.0 / 18000.0
	_noise.fractal_octaves = 3
	_build_indices()


func set_body(b: int) -> void:
	if b == body:
		return
	for k in _patches.keys():
		_patches[k].node.queue_free()
	_patches.clear()
	_pending.clear()
	body = b
	var info: Dictionary = JSON.parse_string(core.body_info(b))
	_style = _styles().get(info.id, _styles()["default"])
	var lk: Dictionary = Sim.look(info.id)
	if lk.has("low"):
		_style = {"kind": 0 if lk.get("style", "") == "ice" else 1, "low": Sim.col(lk.low), "high": Sim.col(lk.high),
			"slope": Sim.col(lk.slope), "rock": Sim.col(lk.rock), "range": float(lk.get("range", 1500.0))}
	_style["radius"] = info.radius
	_material = TerrainShader.make(_style, PlanetBake.textures(core, b))
	_noise.seed = hash(info.id)
	if _site_node:
		_site_node.queue_free()
		_site_node = null
	for r in _relics:
		r.node.queue_free()
	_relics.clear()
	for r in info.get("relics", []):
		var f: Array = r.fixed
		var d := [float(f[0]), float(f[2]), -float(f[1])]  # sim -> Godot axes
		var node := RelicVisual.build(r.id)
		add_child(node)
		_relics.append({"id": r.id, "node": node, "pos": Vector3(d[0], d[1], d[2]), "d": d})
	if info.has("site"):
		_site_node = LaunchSite.build(info.radius)
		_site_pos = LaunchSite.site_fixed_godot(core, b)
		_site_d = LaunchSite.site_fixed_d(core, b)
		add_child(_site_node)


static func _styles() -> Dictionary:
	return {
		"floe": {"kind": 0, "low": Color(0.70, 0.76, 0.85), "high": Color(0.86, 0.88, 0.92), "slope": Color(0.55, 0.72, 0.88),
			"rock": Color(0.24, 0.24, 0.27), "range": 1600.0},
		"krill": {"kind": 1, "low": Color(0.30, 0.295, 0.30), "high": Color(0.55, 0.53, 0.5), "slope": Color(0.22, 0.21, 0.22),
			"rock": Color(0.17, 0.165, 0.17), "range": 1800.0},
		"default": {"kind": 1, "low": Color(0.4, 0.4, 0.4), "high": Color(0.8, 0.8, 0.8), "slope": Color(0.3, 0.3, 0.3),
			"rock": Color(0.2, 0.2, 0.2), "range": 1000.0},
	}


func _build_indices() -> void:
	var idx := PackedInt32Array()
	for y in RES - 1:
		for x in RES - 1:
			var a := y * RES + x
			var b := a + 1
			var c := a + RES
			var d := c + 1
			idx.append_array([a, c, b, b, c, d])  # clockwise seen from outside = front
	# skirt: edge vertices follow the grid in scan order after RES*RES
	var edge := []
	for y in RES:
		for x in RES:
			if x == 0 or y == 0 or x == RES - 1 or y == RES - 1:
				edge.append(y * RES + x)
	_edge_list = edge
	var skirt_of := {}
	for k in edge.size():
		skirt_of[edge[k]] = RES * RES + k
	var ring := []
	for x in RES:
		ring.append(x)
	for y in range(1, RES):
		ring.append(y * RES + RES - 1)
	for x in range(RES - 2, -1, -1):
		ring.append((RES - 1) * RES + x)
	for y in range(RES - 2, 0, -1):
		ring.append(y * RES)
	for k in ring.size():
		var a: int = ring[k]
		var b: int = ring[(k + 1) % ring.size()]
		var sa: int = skirt_of[a]
		var sb: int = skirt_of[b]
		# skirts are thin walls seen from either side: emit both windings
		idx.append_array([a, sa, b, b, sa, sb, a, b, sa, b, sb, sa])
	_indices = idx


func _key(f: int, l: int, i: int, j: int) -> String:
	return "%d/%d/%d/%d" % [f, l, i, j]


## eye_fixed: camera position in the body-fixed frame (sim axes, doubles).
## body_rel: body centre relative to the floating origin (Godot axes, as
## doubles: it is ~R away, and float32 there jitters by centimetres).
## rot: body rotation angle about sim +Z.
func refresh(eye_fixed: PackedFloat64Array, body_rel: Array, rot: float) -> void:
	if body < 0:
		return
	var leaves: PackedFloat64Array = core.terrain_lod(body, eye_fixed, MAX_LEVEL, SPLIT)
	var want := {}
	for k in leaves.size() / 4:
		var o := k * 4
		var key := _key(int(leaves[o]), int(leaves[o + 1]), int(leaves[o + 2]), int(leaves[o + 3]))
		want[key] = [leaves[o], leaves[o + 1], leaves[o + 2], leaves[o + 3]]
	# build missing patches, nearest-first
	var built := 0
	for key in want.keys():
		if not _patches.has(key):
			if built >= BUILD_PER_FRAME:
				continue
			_patches[key] = _build(want[key])
			built += 1
	# Drop unwanted patches only once the area they cover is fully drawn by
	# wanted patches; otherwise keep them, but never draw two levels at once.
	for key in _patches.keys():
		if want.has(key):
			_patches[key].node.visible = true
			continue
		if _covered(key, want):
			_patches[key].node.queue_free()
			_patches.erase(key)
		else:
			_patches[key].node.visible = true
	# a wanted patch hides while an old ancestor still covers its area
	for key in want.keys():
		if _patches.has(key):
			_patches[key].node.visible = not _ancestor_present(key, want)
	var basis := Frame.spin(rot)
	var cr := cos(rot)
	var sr := sin(rot)
	for key in _patches:
		var p: Dictionary = _patches[key]
		var c: Array = p.center
		p.node.transform = Transform3D(basis * _sim_basis(), _place(body_rel, cr, sr, c[0], c[2], -c[1]))
	for r in _relics:
		r.node.transform = Transform3D(basis * LaunchSite.site_basis(r.pos), _place(body_rel, cr, sr, r.d[0], r.d[1], r.d[2]))
	if _site_node:
		_site_node.transform = Transform3D(basis * LaunchSite.site_basis(_site_pos),
			_place(body_rel, cr, sr, _site_d[0], _site_d[1], _site_d[2]))


## Relic `id`'s node (null if it isn't on this body).
func relic_node(id: String) -> Node3D:
	for r in _relics:
		if r.id == id:
			return r.node
	return null


## body_rel + spin(rot) * (x, y, z), all in doubles; only the small result
## becomes a float32 Vector3.
static func _place(body_rel: Array, cr: float, sr: float, x: float, y: float, z: float) -> Vector3:
	return Vector3(body_rel[0] + cr * x + sr * z, body_rel[1] + y, body_rel[2] - sr * x + cr * z)


func set_cloud_shadows(on: bool, sun_cloud_obj: Vector3, offset: float) -> void:
	if _material == null:
		return
	_material.set_shader_parameter("has_clouds", 1.0 if on else 0.0)
	_material.set_shader_parameter("sun_cloud", sun_cloud_obj)
	_material.set_shader_parameter("cloud_offset", offset)


static func _parse(key: String) -> Array:
	var p := key.split("/")
	return [int(p[0]), int(p[1]), int(p[2]), int(p[3])]


## True when every wanted patch overlapping `key`'s area is built.
func _covered(key: String, want: Dictionary) -> bool:
	var a := _parse(key)
	for wk in want:
		var b: Array = want[wk]
		var f := int(b[0])
		var l := int(b[1])
		if f != a[0]:
			continue
		# overlap test in a common (finer) grid
		var L := maxi(l, a[1])
		var ai: int = a[2] << (L - a[1])
		var aj: int = a[3] << (L - a[1])
		var an: int = 1 << (L - a[1])
		var bi := int(b[2]) << (L - l)
		var bj := int(b[3]) << (L - l)
		var bn := 1 << (L - l)
		if bi < ai + an and ai < bi + bn and bj < aj + an and aj < bj + bn:
			if not _patches.has(wk):
				return false
	return true


## True while a built, unwanted patch at a coarser level still covers `key`.
func _ancestor_present(key: String, want: Dictionary) -> bool:
	var a := _parse(key)
	var f: int = a[0]
	var l: int = a[1]
	var i: int = a[2]
	var j: int = a[3]
	while l > 0:
		l -= 1
		i >>= 1
		j >>= 1
		var k := _key(f, l, i, j)
		if _patches.has(k) and not want.has(k):
			return true
	return false


## Patch vertices arrive in sim axes; this basis maps sim -> Godot inside
## the node so meshes keep the raw (double-derived) local coordinates.
static func _sim_basis() -> Basis:
	return Basis(Vector3(1, 0, 0), Vector3(0, 0, -1), Vector3(0, 1, 0))


func _build(id: Array) -> Dictionary:
	var data: PackedFloat64Array = core.terrain_patch(body, PackedFloat64Array(id), RES)
	var nv := (data.size() - 3) / 6
	var cen := [data[0], data[1], data[2]]
	var verts := PackedVector3Array()
	var norms := PackedVector3Array()
	var cols := PackedColorArray()
	var uv := PackedVector2Array()
	var uv2 := PackedVector2Array()
	var ups := PackedFloat32Array()
	verts.resize(nv)
	norms.resize(nv)
	cols.resize(nv)
	uv.resize(nv)
	uv2.resize(nv)
	ups.resize(nv * 4)
	var R: float = _style.radius
	# snapped local origin (doubles) keeps shader detail coordinates small
	var ox := snappedf(data[0], 1024.0)
	var oy := snappedf(data[1], 1024.0)
	var oz := snappedf(data[2], 1024.0)
	for k in nv:
		var o := 3 + k * 3
		var px: float = data[o]
		var py: float = data[o + 1]
		var pz: float = data[o + 2]
		verts[k] = Vector3(px, py, pz)
		norms[k] = Vector3(data[3 + nv * 3 + k * 3], data[4 + nv * 3 + k * 3], data[5 + nv * 3 + k * 3])
		var wx: float = px + data[0]
		var wy: float = py + data[1]
		var wz: float = pz + data[2]
		var wl := sqrt(wx * wx + wy * wy + wz * wz)
		uv[k] = Vector2(wx - ox, wy - oy)
		uv2[k] = Vector2(wz - oz, wl - R)
		ups[k * 4] = wx / wl
		ups[k * 4 + 1] = wy / wl
		ups[k * 4 + 2] = wz / wl
		ups[k * 4 + 3] = 0.0
		var big := _noise.get_noise_3d(wx, wy, wz) * 0.5 + 0.5
		cols[k] = Color(0.0 if k >= RES * RES else 1.0, 1, 1, big)  # r = 0 marks skirt vertices
	# skirt vertices hang below the edge; shade them as the edge vertex they
	# belong to, or their lowered height reads as a blue stripe at the horizon
	for k in range(RES * RES, nv):
		var src: int = _edge_list[k - RES * RES]
		uv2[k] = Vector2(uv2[k].x, uv2[src].y)
		cols[k] = Color(0.0, 1.0, 1.0, cols[src].a)
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = verts
	arrays[Mesh.ARRAY_NORMAL] = norms
	arrays[Mesh.ARRAY_COLOR] = cols
	arrays[Mesh.ARRAY_TEX_UV] = uv
	arrays[Mesh.ARRAY_TEX_UV2] = uv2
	arrays[Mesh.ARRAY_CUSTOM0] = ups
	arrays[Mesh.ARRAY_INDEX] = _indices
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays, [], {},
		Mesh.ARRAY_CUSTOM_RGBA_FLOAT << Mesh.ARRAY_FORMAT_CUSTOM0_SHIFT)
	var mi := MeshInstance3D.new()
	mi.mesh = mesh
	mi.material_override = _material
	# terrain self-shadowing at grazing sun angles shows up as acne stripes;
	# vessels and the pad still cast onto it
	mi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	mi.set_instance_shader_parameter("detail_origin", Vector3(fposmod(ox, TerrainShader.PERIOD),
		fposmod(oy, TerrainShader.PERIOD), fposmod(oz, TerrainShader.PERIOD)))
	add_child(mi)
	return {"node": mi, "center": cen}
