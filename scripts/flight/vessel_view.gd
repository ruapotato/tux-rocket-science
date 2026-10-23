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
var _parts := {}   # uid -> {node, part, plume, canopy, legs, shroud}
var _shrouds := {}
var _last_count := -1
var _dt := 1.0 / 60.0


func setup(c: TrsSim, id: int) -> void:
	core = c
	vessel_id = id
	name = "Vessel%d" % id


func _make_part(uid: int, part_id: String, opts: Dictionary) -> Dictionary:
	var v := PartVisual.build(part_id, false, opts)
	add_child(v)
	var d: Dictionary = Sim.part(part_id)
	var entry := {"node": v, "part": part_id, "plume": null, "canopy": null, "legs": float(opts.get("legs", 0.0)),
		"shroud": float(opts.get("shroud", 0.0))}
	if d.has("engine"):
		entry.plume = _plume(d)
		v.add_child(entry.plume)
	if d.has("parachute"):
		entry.canopy = _canopy()
		v.add_child(entry.canopy)
	return entry


const PLUME_SHADER := """
shader_type spatial;
render_mode unshaded, blend_add, cull_disabled, depth_draw_never, shadows_disabled;
uniform vec4 core_color : source_color = vec4(1.0, 0.95, 0.8, 1.0);
uniform vec4 edge_color : source_color = vec4(0.35, 0.55, 1.0, 1.0);
uniform float intensity = 1.0;
uniform float diamonds = 0.0;   // 1 at sea level (shock diamonds), 0 in vacuum
uniform float smoke = 0.0;      // solid motors: orange, turbulent
varying float t;                // 0 at the nozzle .. 1 at the tail
varying vec3 lp;
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
void vertex() { lp = VERTEX; t = clamp(-VERTEX.y + 0.5, 0.0, 1.0); }
void fragment() {
	float facing = abs(dot(NORMAL, VIEW));
	float body = pow(facing, 1.5);                       // soft, see-through edges
	float fade = pow(1.0 - t, 1.6);
	float flick = 0.85 + 0.15 * sin(TIME * 60.0 + lp.y * 9.0) * hash(vec2(floor(TIME * 30.0), 1.0));
	float shock = 1.0 + diamonds * 0.8 * pow(max(sin(t * 34.0 - TIME * 3.0), 0.0), 6.0) * (1.0 - t);
	float turb = 1.0 + smoke * 0.4 * (hash(floor(vec2(atan(lp.x, lp.z) * 6.0, t * 18.0 - TIME * 20.0))) - 0.5);
	vec3 col = mix(core_color.rgb, edge_color.rgb, smoothstep(0.0, 0.7, t + (1.0 - facing) * 0.5));
	ALBEDO = col * body * fade * flick * shock * turb * intensity;
}
"""

static var _plume_shader: Shader


func _plume(d: Dictionary) -> Node3D:
	var sh: Dictionary = d.shape
	var h: float = sh.get("height", 1.0)
	var r: float = sh.get("radius_bell", sh.get("radius", 0.5))
	var root := Node3D.new()
	root.position = Vector3(0, -h * 0.5 - (0.45 if sh.type == "booster" else 0.0), 0)
	var solid: bool = d.engine.propellant == "solid"
	if _plume_shader == null:
		_plume_shader = Shader.new()
		_plume_shader.code = PLUME_SHADER
	for k in 2:
		var c := CylinderMesh.new()
		c.top_radius = r * (0.9 if k == 0 else 0.45)
		c.bottom_radius = r * (1.3 if k == 0 else 0.25)
		c.height = 1.0
		c.radial_segments = 24
		c.rings = 8
		c.cap_top = false
		c.cap_bottom = false
		var mi := MeshInstance3D.new()
		mi.mesh = c
		var m := ShaderMaterial.new()
		m.shader = _plume_shader
		if solid:
			m.set_shader_parameter("core_color", Color(1.0, 0.85, 0.55))
			m.set_shader_parameter("edge_color", Color(1.0, 0.42, 0.12))
			m.set_shader_parameter("smoke", 1.0)
		else:
			m.set_shader_parameter("core_color", Color(1.0, 0.96, 0.85) if k == 1 else Color(0.75, 0.82, 1.0))
			m.set_shader_parameter("edge_color", Color(0.3, 0.5, 1.0))
		m.set_shader_parameter("intensity", 1.6 if k == 1 else 0.8)
		mi.material_override = m
		mi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
		mi.name = "Core" if k == 1 else "Outer"
		root.add_child(mi)
	var light := OmniLight3D.new()
	light.light_color = Color(1.0, 0.6, 0.3) if solid else Color(0.9, 0.85, 1.0)
	light.omni_range = 18.0
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


func _structure_dirty(n: int) -> bool:
	if n != _last_count:
		_last_count = n
		return true
	return false


func refresh(origin_id: int, pressure: float, flicker: float, dt := 1.0 / 60.0) -> void:
	_dt = dt
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
		if parts_json.is_empty() and (not _parts.has(uid) or _structure_dirty(n)):
			parts_json = JSON.parse_string(core.vessel_parts(vessel_id))
			_shrouds = PartVisual.shroud_heights(parts_json)
		if _parts.has(uid) and not parts_json.is_empty():
			# a separation can remove an interstage fairing: rebuild that part
			if float(_shrouds.get(k, 0.0)) != _parts[uid].shroud:
				_parts[uid].node.queue_free()
				_parts.erase(uid)
		if not _parts.has(uid):
			_parts[uid] = _make_part(uid, parts_json[k].part, {"shroud": _shrouds.get(k, 0.0), "legs": feed[o + 10]})
		var e: Dictionary = _parts[uid]
		var node: Node3D = e.node
		node.transform = Transform3D(Frame.local_basis(feed, o + 4), Frame.vec(feed, o + 1))
		node.visible = true
		if e.plume:
			_update_plume(e.plume, feed[o + 8], pressure, flicker)
		if e.canopy:
			_update_canopy(e.canopy, int(feed[o + 9]))
		if Sim.part(e.part).has("leg"):
			var target := feed[o + 10]
			var kk: float = move_toward(e.legs, target, _dt / 1.4)
			if kk != e.legs:
				e.legs = kk
				PartVisual.pose_leg(node, Sim.part(e.part), smoothstep(0.0, 1.0, kk))
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
	var length := (6.0 + 12.0 * vac) * (0.35 + 0.65 * thr) * (0.92 + 0.16 * flicker)
	var width := 1.0 + 1.6 * vac
	for c in p.get_children():
		if c is MeshInstance3D:
			var core_len := length * (0.45 if c.name == "Core" else 1.0)
			c.scale = Vector3(width, core_len, width)
			c.position = Vector3(0, -core_len * 0.5, 0)
			var m: ShaderMaterial = c.material_override
			m.set_shader_parameter("diamonds", 1.0 - vac)
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
