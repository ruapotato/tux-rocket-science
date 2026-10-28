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
## Plants and animals on the ground near the viewer. trs_core decides where
## each one is (life.c: the same every visit, on the terrain's own height);
## this node draws them: each kind of plant in each cell as one MultiMesh
## of a small procedural model, each animal as its own little rig walking
## (or flying) its loop. A kind is drawn only out to its "range" from the
## data, so grass stays near and trees go far. Scenery: nothing collides.
class_name TerrainLife
extends Node3D

const BUILD_PER_FRAME := 2
const QUERY_EVERY := 0.4  # s between asking which cells are near
const NEAR_ANIM := 250.0  # m: animals nearer than this move every frame

var core: TrsSim
var body := -1
var _kinds: Array = []
var _max_range := 0.0
var _cells := {}  # key -> {node, center: [x, y, z] sim body-fixed, kinds: bit mask built, parts: {kind: node}, animals: []}
var _want := {}   # key -> [face, level, i, j]
var _since := 1e9
var _meshes := {}  # kind -> Mesh
var _mats := {}
var _time := 0.0
var _frame := 0
var _todo := []      # [dist, cell key, kind], farthest first
var _scan_eye: Array = [0.0, 0.0, 0.0]


func setup(c: TrsSim) -> void:
	core = c
	name = "Life"


func set_body(b: int, id: String) -> void:
	for k in _cells:
		_cells[k].node.queue_free()
	_cells.clear()
	_want.clear()
	_todo = []
	_meshes.clear()
	body = b
	# nothing is drawn without a screen: the headless gates skip it all
	_kinds = Sim.life(id) if DisplayServer.get_name() != "headless" else []
	_max_range = 0.0
	for k in _kinds:
		_max_range = maxf(_max_range, float(k.get("range", 1000.0)))
	_since = 1e9


## Same arguments as TerrainView.refresh.
func refresh(eye_fixed: PackedFloat64Array, body_rel: Array, rot: float, dt: float) -> void:
	_time += dt
	_frame += 1
	if body < 0 or _kinds.is_empty():
		return
	_since += dt
	var ex: float = eye_fixed[0]
	var ey: float = eye_fixed[1]
	var ez: float = eye_fixed[2]
	if _since > QUERY_EVERY:
		_since = 0.0
		var cells: PackedFloat64Array = core.life_cells(body, eye_fixed, _max_range)
		_want.clear()
		for k in cells.size() / 4:
			var o := k * 4
			_want["%d/%d/%d/%d" % [int(cells[o]), int(cells[o + 1]), int(cells[o + 2]), int(cells[o + 3])]] = [cells[o], cells[o + 1], cells[o + 2], cells[o + 3]]
		for key in _cells.keys():
			if not _want.has(key):
				_cells[key].node.queue_free()
				_cells.erase(key)
	# what is missing, nearest first: looked over again only after a query
	# or once the eye has moved a few metres (with five hundred cells near,
	# doing it every frame cost two milliseconds); built a little per frame
	var moved := Vector3(ex - _scan_eye[0], ey - _scan_eye[1], ez - _scan_eye[2]).length()
	if _since == 0.0 or moved > 4.0:
		_scan_eye = [ex, ey, ez]
		_todo = []
		for key in _want:
			if not _cells.has(key):
				_cells[key] = _new_cell(_want[key])
			var cell: Dictionary = _cells[key]
			var c: Array = cell.center
			var d := sqrt((c[0] - ex) * (c[0] - ex) + (c[1] - ey) * (c[1] - ey) + (c[2] - ez) * (c[2] - ez))
			cell["dist"] = d
			for k in _kinds.size():
				var reach := float(_kinds[k].get("range", 1000.0)) + float(cell.half)
				var bit := 1 << k
				if d < reach and not (int(cell.kinds) & bit):
					_todo.append([d, key, k])
				elif d > reach * 1.15 and (int(cell.kinds) & bit):
					_drop_kind(cell, k)
		_todo.sort_custom(func(a, b): return a[0] > b[0])   # (nearest last: popped first)
	var built := 0
	while built < BUILD_PER_FRAME and not _todo.is_empty():
		var t: Array = _todo.pop_back()
		if _cells.has(t[1]) and not (int(_cells[t[1]].kinds) & (1 << int(t[2]))):
			_build_kind(_cells[t[1]], int(t[2]))
			built += 1
	var basis := Frame.spin(rot)
	var cr := cos(rot)
	var sr := sin(rot)
	for key in _cells:
		var cell: Dictionary = _cells[key]
		if cell.node.get_child_count() == 0:
			continue   # (nothing drawn there: no need to place it)
		var c: Array = cell.center
		cell.node.transform = Transform3D(basis * TerrainView._sim_basis(), TerrainView._place(body_rel, cr, sr, c[0], c[2], -c[1]))
		if not cell.animals.is_empty() and float(cell.dist) < _max_range:
			# the far ones a frame in four, each cell on its own frame (a
			# hundred rigs posed every frame cost three milliseconds)
			if float(cell.dist) < NEAR_ANIM:
				_animate(cell, dt)
			elif (_frame + int(cell.half * 7.0 + float(c[0]))) % 4 == 0:
				_animate(cell, dt * 4.0)


func _new_cell(id: Array) -> Dictionary:
	var feed: PackedFloat64Array = core.life_cell(body, PackedFloat64Array(id), 0)
	var node := Node3D.new()
	add_child(node)
	var c := [feed[0], feed[1], feed[2]]
	var R := sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2])
	var up := Vector3(c[0] / R, c[1] / R, c[2] / R)  # sim axes, the cell's own frame
	var size: float = R * PI * 0.5 / float(1 << int(id[1]))
	return {"id": id, "node": node, "center": c, "up": up, "kinds": 0, "parts": {}, "animals": [], "half": size * 0.75, "dist": 1e9}


func _drop_kind(cell: Dictionary, k: int) -> void:
	cell.kinds = int(cell.kinds) & ~(1 << k)
	if cell.parts.has(k):
		cell.parts[k].queue_free()
		cell.parts.erase(k)
	var keep := []
	for a in cell.animals:
		if int(a.kind) == k:
			a.node.queue_free()
		else:
			keep.append(a)
	cell.animals = keep


## A frame whose Y is `up` (sim axes), turned `yaw` about it.
static func _upright(up: Vector3, yaw: float) -> Basis:
	var ref := Vector3(0, 0, 1) if absf(up.z) < 0.9 else Vector3(1, 0, 0)
	var x := ref.cross(up).normalized()
	var z := x.cross(up)
	return Basis(x, up, z) * Basis(Vector3.UP, yaw)


func _build_kind(cell: Dictionary, k: int) -> void:
	cell.kinds = int(cell.kinds) | (1 << k)
	var spec: Dictionary = _kinds[k]
	var feed: PackedFloat64Array = core.life_cell(body, PackedFloat64Array(cell.id), 1 << k)
	var n := int(feed[3])
	if n == 0:
		return
	var sz: Array = spec.get("size", [1.0, 1.0])
	var lean_on_ground: bool = spec.get("shape", "") in ["rock", "mat"]
	if spec.get("animal", false):
		var o := 4
		for i in n:
			var pos := Vector3(feed[o + 1], feed[o + 2], feed[o + 3])
			var s := lerpf(float(sz[0]), float(sz[1]), feed[o + 7])
			var yaw: float = feed[o + 8] * TAU
			var nl := int(feed[o + 9])
			var start: float = feed[o + 7] * nl
			var loop := PackedVector3Array()
			for e in nl:
				loop.append(Vector3(feed[o + 10 + e * 3], feed[o + 11 + e * 3], feed[o + 12 + e * 3]))
			o += 10 + nl * 3
			var rig := LifeModels.animal(spec, s)
			cell.node.add_child(rig)
			var a := {"kind": k, "node": rig, "pos": pos, "loop": loop, "s": start, "go": 0.0,
				"phase": yaw, "speed": float(spec.get("speed", 0.5)), "size": s, "fly": float(spec.get("fly", 0.0)) > 0.0,
				"shape": spec.get("shape", "")}
			rig.transform = Transform3D(_upright(cell.up, yaw).scaled(Vector3.ONE * s), pos)
			cell.animals.append(a)
		return
	if not _meshes.has(k):
		_meshes[k] = LifeModels.plant(spec)
	var mm := MultiMesh.new()
	mm.transform_format = MultiMesh.TRANSFORM_3D
	mm.use_colors = true
	mm.mesh = _meshes[k]
	mm.instance_count = n
	var o := 4
	for i in n:
		var pos := Vector3(feed[o + 1], feed[o + 2], feed[o + 3])
		var gu := Vector3(feed[o + 4], feed[o + 5], feed[o + 6])
		var s := lerpf(float(sz[0]), float(sz[1]), feed[o + 7])
		var yaw: float = feed[o + 8] * TAU
		# trees stand up straight; a rock or a mat lies on the slope
		var b := _upright(gu if lean_on_ground else cell.up, yaw).scaled(Vector3.ONE * s)
		mm.set_instance_transform(i, Transform3D(b, pos - cell.up * 0.05 * s))
		var v := 0.85 + 0.3 * fposmod(feed[o + 7] * 7.31 + feed[o + 8] * 3.7, 1.0)
		mm.set_instance_color(i, Color(v, v, v))
		o += 10 + int(feed[o + 9]) * 3
	var mi := MultiMeshInstance3D.new()
	mi.multimesh = mm
	mi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_ON if float(sz[1]) > 2.0 else GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	cell.node.add_child(mi)
	cell.parts[k] = mi


## Walks each animal round its loop (smoothly, with stops to graze), or
## flies it round, and poses it.
func _animate(cell: Dictionary, dt: float) -> void:
	for a in cell.animals:
		var loop: PackedVector3Array = a.loop
		var rig: Node3D = a.node
		if loop.size() < 2:
			LifeModels.pose(rig, a.shape, _time + a.phase, 0.0)
			continue
		# walk a while, stop a while (flyers never stop)
		var go := 1.0 if a.fly else clampf(sin(_time * 0.21 + a.phase * 3.0) * 2.5 + 0.6, 0.0, 1.0)
		a.go = lerpf(a.go, go, clampf(dt * 2.0, 0.0, 1.0))
		var seg := (loop[1] - loop[0]).length()
		a.s = fposmod(a.s + a.speed * a.go * dt / maxf(seg, 0.1), float(loop.size()))
		var i := int(a.s)
		var f: float = a.s - i
		var n := loop.size()
		var p0 := loop[(i - 1 + n) % n]
		var p1 := loop[i]
		var p2 := loop[(i + 1) % n]
		var p3 := loop[(i + 2) % n]
		var at := _catmull(p0, p1, p2, p3, f)
		var ahead := _catmull(p0, p1, p2, p3, minf(f + 0.05, 1.0)) - at
		if f + 0.05 > 1.0:
			ahead = p2 - p1
		var up: Vector3 = cell.up
		var fwd := (ahead - up * ahead.dot(up)).normalized()
		if fwd.length() < 0.5:
			fwd = Vector3(1, 0, 0)
		var right := up.cross(fwd).normalized()
		var b := Basis(right, up, fwd)
		if a.fly:
			b = Basis(fwd, 0.35) * b  # banking round the turn
		rig.transform = Transform3D(b.scaled(Vector3.ONE * float(a.size)), at)
		LifeModels.pose(rig, a.shape, _time + a.phase, a.go * a.speed)


static func _catmull(p0: Vector3, p1: Vector3, p2: Vector3, p3: Vector3, t: float) -> Vector3:
	var t2 := t * t
	var t3 := t2 * t
	return 0.5 * ((2.0 * p1) + (-p0 + p2) * t + (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t2 + (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * t3)
