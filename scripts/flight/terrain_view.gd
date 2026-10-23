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

const RES := 17
const MAX_LEVEL := 13
const SPLIT := 2.6
const BUILD_PER_FRAME := 6

var core: TrsSim
var body := -1
var _style := {}
var _patches := {}  # key -> {node, center: Vector3 (sim fixed, as doubles in array)}
var _indices := PackedInt32Array()
var _material: StandardMaterial3D
var _pending: Array = []
var _site_node: Node3D
var _site_pos := Vector3.ZERO


func setup(c: TrsSim) -> void:
	core = c
	_material = StandardMaterial3D.new()
	_material.vertex_color_use_as_albedo = true
	_material.vertex_color_is_srgb = true
	_material.roughness = 0.8
	_material.metallic_specular = 0.3
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
	_style["radius"] = info.radius
	if _site_node:
		_site_node.queue_free()
		_site_node = null
	if info.has("site"):
		_site_node = LaunchSite.build()
		_site_pos = LaunchSite.site_fixed_godot(core, b)
		add_child(_site_node)


static func _styles() -> Dictionary:
	return {
		"floe": {"low": Color(0.72, 0.8, 0.9), "high": Color(0.97, 0.98, 1.0), "slope": Color(0.45, 0.62, 0.8), "range": 900.0},
		"krill": {"low": Color(0.26, 0.255, 0.27), "high": Color(0.5, 0.48, 0.46), "slope": Color(0.2, 0.19, 0.2), "range": 1800.0},
		"default": {"low": Color(0.4, 0.4, 0.4), "high": Color(0.8, 0.8, 0.8), "slope": Color(0.3, 0.3, 0.3), "range": 1000.0},
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
## body_rel: body centre relative to the floating origin (Godot axes).
## rot: body rotation angle about sim +Z.
func refresh(eye_fixed: PackedFloat64Array, body_rel: Vector3, rot: float) -> void:
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
	for key in _patches:
		var p: Dictionary = _patches[key]
		var c: Vector3 = p.center
		p.node.transform = Transform3D(basis * _sim_basis(), body_rel + basis * Vector3(c.x, c.z, -c.y))
	if _site_node:
		_site_node.transform = Transform3D(basis * LaunchSite.site_basis(_site_pos), body_rel + basis * _site_pos)


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
	var cen := Vector3(data[0], data[1], data[2])
	var verts := PackedVector3Array()
	var norms := PackedVector3Array()
	var cols := PackedColorArray()
	verts.resize(nv)
	norms.resize(nv)
	cols.resize(nv)
	var R: float = _style.radius
	var low: Color = _style.low
	var high: Color = _style.high
	var slope_c: Color = _style.slope
	var hr: float = _style.range
	for k in nv:
		var o := 3 + k * 3
		var p := Vector3(data[o], data[o + 1], data[o + 2])
		var n := Vector3(data[3 + nv * 3 + k * 3], data[4 + nv * 3 + k * 3], data[5 + nv * 3 + k * 3])
		verts[k] = p
		norms[k] = n
		var world := (p + cen)
		var up := world.normalized()
		var h := world.length() - R
		var c := low.lerp(high, clampf(h / hr * 0.5 + 0.5, 0.0, 1.0))
		var flat := clampf((n.dot(up) - 0.82) / 0.16, 0.0, 1.0)
		cols[k] = slope_c.lerp(c, flat)
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = verts
	arrays[Mesh.ARRAY_NORMAL] = norms
	arrays[Mesh.ARRAY_COLOR] = cols
	arrays[Mesh.ARRAY_INDEX] = _indices
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	var mi := MeshInstance3D.new()
	mi.mesh = mesh
	mi.material_override = _material
	add_child(mi)
	return {"node": mi, "center": cen}
