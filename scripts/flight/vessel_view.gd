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
## Renders one vessel from trs_core's render feed. Parts are keyed by uid so
## splits and crashes just move visuals between vessels or hide them.
class_name VesselView
extends Node3D

const RPP := 11

var core: TrsSim
var vessel_id := -1
var _parts := {}   # uid -> {node, part, plume, canopy}


func setup(c: TrsSim, id: int) -> void:
	core = c
	vessel_id = id
	name = "Vessel%d" % id


func _make_part(uid: int, part_id: String) -> Dictionary:
	var v := PartVisual.build(part_id, false)
	add_child(v)
	var d: Dictionary = Sim.part(part_id)
	var entry := {"node": v, "part": part_id, "plume": null, "canopy": null}
	if d.has("engine"):
		entry.plume = _plume(d)
		v.add_child(entry.plume)
	if d.has("parachute"):
		entry.canopy = _canopy()
		v.add_child(entry.canopy)
	return entry


func _plume(d: Dictionary) -> Node3D:
	var sh: Dictionary = d.shape
	var h: float = sh.get("height", 1.0)
	var r: float = sh.get("radius_bell", sh.get("radius", 0.5))
	var root := Node3D.new()
	root.position = Vector3(0, -h * 0.5 - (0.15 if sh.type == "booster" else 0.0), 0)
	var solid: bool = d.engine.propellant == "solid"
	for k in 2:
		var c := CylinderMesh.new()
		c.top_radius = r * (0.85 if k == 0 else 0.5)
		c.bottom_radius = r * (0.35 if k == 0 else 0.15)
		c.height = 1.0
		c.radial_segments = 16
		var mi := MeshInstance3D.new()
		mi.mesh = c
		var m := StandardMaterial3D.new()
		m.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
		m.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
		m.blend_mode = BaseMaterial3D.BLEND_MODE_ADD
		m.cull_mode = BaseMaterial3D.CULL_DISABLED
		var col := Color(1.0, 0.62, 0.25, 0.55) if solid else Color(0.55, 0.75, 1.0, 0.45)
		if k == 1:
			col = Color(1.0, 0.95, 0.8, 0.8)
		m.albedo_color = col
		mi.material_override = m
		mi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
		mi.name = "Core" if k == 1 else "Outer"
		root.add_child(mi)
	var light := OmniLight3D.new()
	light.light_color = Color(1.0, 0.7, 0.4)
	light.omni_range = 12.0
	light.light_energy = 0.0
	light.name = "Glow"
	root.add_child(light)
	root.visible = false
	return root


func _canopy() -> Node3D:
	var root := Node3D.new()
	var dome := SphereMesh.new()
	dome.radius = 1.0
	dome.height = 1.0
	dome.is_hemisphere = true
	var mi := MeshInstance3D.new()
	mi.mesh = dome
	var m := StandardMaterial3D.new()
	m.albedo_color = Color(0.95, 0.35, 0.25)
	m.cull_mode = BaseMaterial3D.CULL_DISABLED
	mi.material_override = m
	root.add_child(mi)
	for k in 6:
		var line := CylinderMesh.new()
		line.top_radius = 0.01
		line.bottom_radius = 0.01
		line.height = 1.0
		var li := MeshInstance3D.new()
		li.mesh = line
		li.material_override = PartVisual.mat("steel", PartVisual.STEEL)
		li.name = "Line%d" % k
		root.add_child(li)
	root.visible = false
	return root


## Places every part relative to the floating origin (the active vessel).
## `pressure` (Pa) shapes the plumes: tight in air, wide in vacuum.
## uid -> last world position of parts that left this view this frame
var vanished := {}


func refresh(origin_id: int, pressure: float, flicker: float) -> void:
	vanished.clear()
	var feed: PackedFloat64Array = core.vessel_render(vessel_id, origin_id)
	var n := feed.size() / RPP
	if n == 0:
		visible = false
		for uid in _parts.keys():
			vanished[uid] = _parts[uid].node.global_position
			_parts[uid].node.queue_free()
		_parts.clear()
		return
	visible = true
	var seen := {}
	var parts_json: Array = []
	for k in n:
		var o := k * RPP
		var uid := int(feed[o])
		seen[uid] = true
		if not _parts.has(uid):
			if parts_json.is_empty():
				parts_json = JSON.parse_string(core.vessel_parts(vessel_id))
			_parts[uid] = _make_part(uid, parts_json[k].part)
		var e: Dictionary = _parts[uid]
		var node: Node3D = e.node
		node.transform = Transform3D(Frame.local_basis(feed, o + 4), Frame.vec(feed, o + 1))
		node.visible = true
		if e.plume:
			_update_plume(e.plume, feed[o + 8], pressure, flicker)
		if e.canopy:
			_update_canopy(e.canopy, int(feed[o + 9]))
	for uid in _parts.keys():
		if not seen.has(uid):
			vanished[uid] = _parts[uid].node.global_position
			_parts[uid].node.queue_free()
			_parts.erase(uid)


func has_uid(uid: int) -> bool:
	return _parts.has(uid)


func _update_plume(p: Node3D, thr: float, pressure: float, flicker: float) -> void:
	p.visible = thr > 0.01
	if not p.visible:
		return
	var vac := clampf(1.0 - pressure / 101325.0, 0.0, 1.0)
	var length := (3.0 + 9.0 * vac) * (0.35 + 0.65 * thr) * (0.92 + 0.16 * flicker)
	var width := 1.0 + 1.6 * vac
	for c in p.get_children():
		if c is MeshInstance3D:
			var core_len := length * (0.45 if c.name == "Core" else 1.0)
			c.scale = Vector3(width, core_len, width)
			c.position = Vector3(0, -core_len * 0.5, 0)
	var glow: OmniLight3D = p.get_node("Glow")
	glow.light_energy = 3.0 * thr


func _update_canopy(c: Node3D, state: int) -> void:
	# 2 = semi-deployed, 3 = fully deployed (trs_chute_state)
	c.visible = state == 2 or state == 3
	if not c.visible:
		return
	var r := 1.4 if state == 2 else 6.5
	var lift := 3.0 if state == 2 else 9.0
	var dome: MeshInstance3D = c.get_child(0)
	dome.scale = Vector3(r, r * 0.7, r)
	dome.position = Vector3(0, lift, 0)
	for k in 6:
		var a := TAU * k / 6.0
		var top := Vector3(cos(a) * r * 0.95, lift, sin(a) * r * 0.95)
		var line: MeshInstance3D = c.get_node("Line%d" % k)
		var mid := top * 0.5
		line.position = mid
		var len := top.length()
		line.scale = Vector3(1, len, 1)
		line.basis = Basis(Quaternion(Vector3.UP, top.normalized())).scaled(Vector3(1, len, 1))
