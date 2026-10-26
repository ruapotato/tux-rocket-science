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

const RPP := 21

var core: TrsSim
var vessel_id := -1
var _parts := {}   # uid -> {node, part, plume, canopy, legs, shroud}
var air_vel := Vector3.ZERO  # render-space velocity through the air; canopies stream behind it (zero: hang along the part)
var _shrouds := {}
var _last_count := -1
var _dt := 1.0 / 60.0
var anchor := Vector3.ZERO  # render position of the first part, to track origin jumps
## A penguin on EVA: set each frame by the flight scene from its telemetry:
## {"ground": feet down, "vel": render-space velocity over the ground,
## "space": out of the air and off the ground (floats)}
var eva := {}


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
	var jets := []
	if d.has("rcs"):
		for k in d.rcs.get("nozzles", []).size():
			jets.append(v.get_node("Jet%d" % k))
	entry["jets"] = jets
	entry["flap"] = v.get_node_or_null("Flap")
	entry["tux"] = null
	var facing := v.get_node_or_null("Facing")
	if facing:
		var anim := TuxAnimator.new()
		anim.setup(facing)
		entry.tux = {"anim": anim, "facing": facing, "air": false}
	# reentry sheath: a copy of each hull mesh drawn in plasma (ReentryFX),
	# a child of the mesh so it follows legs and flaps as they move
	var sheath := []
	for mi in v.find_children("*", "MeshInstance3D", true, false):
		if _not_hull(entry, mi):
			continue
		var sm := MeshInstance3D.new()
		sm.name = "Sheath"
		sm.mesh = (mi as MeshInstance3D).mesh
		sm.material_override = ReentryFX.sheath_material()
		sm.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
		sm.visible = false
		mi.add_child(sm)
		sheath.append(sm)
	entry["sheath"] = sheath
	return entry


const PLUME_SHADER := """
shader_type spatial;
render_mode unshaded, blend_add, cull_disabled, depth_draw_never, shadows_disabled;
uniform vec4 core_color : source_color = vec4(1.0, 0.95, 0.8, 1.0);
uniform vec4 edge_color : source_color = vec4(0.35, 0.55, 1.0, 1.0);
uniform float intensity = 1.0;
uniform float exit_r = 0.5;     // nozzle exit radius (m): the plume starts exactly there
uniform float len = 6.0;        // plume length (m)
uniform float spread = 0.4;     // how far it balloons by the tail (x exit radius)
uniform float diamonds = 0.0;   // 1 at sea level (shock diamonds), 0 in vacuum
uniform float smoke = 0.0;      // solid motors: orange, turbulent
varying float t;                // 0 at the nozzle .. 1 at the tail
varying vec3 lp;
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
void vertex() {
	t = clamp(0.5 - VERTEX.y, 0.0, 1.0);
	vec2 dir = length(VERTEX.xz) > 1e-5 ? normalize(VERTEX.xz) : vec2(0.0);
	// a sea-level plume necks in after the nozzle, a vacuum one flares out
	float neck = 1.0 - diamonds * 0.18 * sin(min(t * 6.0, 3.14159));
	float r = exit_r * neck * (1.0 + spread * pow(t, 0.75));
	VERTEX = vec3(dir.x * r, -t * len, dir.y * r);
	lp = VERTEX;
}
void fragment() {
	float facing = abs(dot(NORMAL, VIEW));
	float body = pow(facing, 1.5);                       // soft, see-through edges
	float fade = pow(1.0 - t, 1.4) * smoothstep(0.0, 0.04, t + 0.02);
	float flick = 0.85 + 0.15 * sin(TIME * 60.0 + lp.y * 9.0) * hash(vec2(floor(TIME * 30.0), 1.0));
	float shock = 1.0 + diamonds * 0.9 * pow(max(sin(t * 30.0 - TIME * 2.0), 0.0), 6.0) * (1.0 - t);
	float turb = 1.0 + smoke * 0.45 * (hash(floor(vec2(atan(lp.x, lp.z) * 6.0, t * 18.0 - TIME * 20.0))) - 0.5);
	vec3 col = mix(core_color.rgb, edge_color.rgb, smoothstep(0.0, 0.7, t + (1.0 - facing) * 0.5));
	ALBEDO = col * body * fade * flick * shock * turb * intensity;
}
"""

static var _plume_shader: Shader

## Incandescence for hot skins: added over a part's own surfaces, coloured
## by an approximate blackbody ramp of its skin temperature.
const HEAT_SHADER := """
shader_type spatial;
render_mode unshaded, blend_add, depth_draw_never, shadows_disabled;
instance uniform float temp_k = 288.0;
void fragment() {
	float t = clamp((temp_k - 750.0) / 1800.0, 0.0, 1.0);
	vec3 c = mix(vec3(0.55, 0.04, 0.0), vec3(1.0, 0.35, 0.05), smoothstep(0.0, 0.35, t));
	c = mix(c, vec3(1.0, 0.85, 0.6), smoothstep(0.35, 1.0, t));
	float on = smoothstep(750.0, 1100.0, temp_k);
	float rim = 0.6 + 0.4 * pow(1.0 - abs(dot(NORMAL, VIEW)), 2.0);
	ALBEDO = c * on * rim * (0.6 + 2.4 * t);
}
"""
static var _heat_mat: ShaderMaterial


static func heat_material() -> ShaderMaterial:
	if _heat_mat == null:
		var sh := Shader.new()
		sh.code = HEAT_SHADER
		_heat_mat = ShaderMaterial.new()
		_heat_mat.shader = sh
	return _heat_mat


## Sets a part's glow from its skin temperature (K).
func _update_heat(e: Dictionary, temp: float) -> void:
	var glowing := temp > 760.0
	if not glowing and not e.get("hot", false):
		return
	if absf(temp - float(e.get("temp", 0.0))) < 4.0 and glowing == e.get("hot", false):
		return
	e.temp = temp
	e.hot = glowing
	for mi in (e.node as Node3D).find_children("*", "MeshInstance3D", true, false):
		if _not_hull(e, mi) or mi.name == "Sheath":
			continue  # the exhaust, the canopy and the RCS puffs are not hull
		var g := mi as GeometryInstance3D
		g.material_overlay = heat_material() if glowing else null
		if glowing:
			g.set_instance_shader_parameter("temp_k", temp)


func _not_hull(e: Dictionary, mi: Node) -> bool:
	if (e.plume and (e.plume as Node).is_ancestor_of(mi)) or (e.canopy and (e.canopy as Node).is_ancestor_of(mi)):
		return true
	for j in e.get("jets", []):
		if j == mi or (j as Node).is_ancestor_of(mi):
			return true
	return false


## Reentry sheaths: shown on the parts in `exposure` (uid -> 0 shielded ..
## 1 meeting the air first), hidden everywhere when it's empty.
func set_sheath(exposure: Dictionary) -> void:
	for uid in _parts:
		var e: Dictionary = _parts[uid]
		var on := exposure.has(uid)
		for sm in e.get("sheath", []):
			(sm as MeshInstance3D).visible = on
			if on:
				(sm as GeometryInstance3D).set_instance_shader_parameter("exposure", float(exposure[uid]))


## Nozzle exit radius and where it sits below the part centre.
static func nozzle(d: Dictionary) -> Dictionary:
	var sh: Dictionary = d.shape
	var h: float = sh.get("height", 1.0)
	if sh.type == "booster":
		return {"r": float(sh.radius) * 0.72, "y": -h * 0.5 - 0.475}
	return {"r": float(sh.get("radius_bell", sh.get("radius", 0.5))), "y": -h * 0.5}


func _plume(d: Dictionary) -> Node3D:
	var nz := nozzle(d)
	var r: float = nz.r
	var root := Node3D.new()
	root.position = Vector3(0, nz.y, 0)
	var solid: bool = d.engine.propellant == "solid"
	root.set_meta("exit_r", r)
	root.set_meta("solid", solid)
	if _plume_shader == null:
		_plume_shader = Shader.new()
		_plume_shader.code = PLUME_SHADER
	for k in 2:
		var c := CylinderMesh.new()
		c.top_radius = 1.0
		c.bottom_radius = 1.0
		c.height = 1.0
		c.radial_segments = 24
		c.rings = 12
		c.cap_top = false
		c.cap_bottom = false
		var mi := MeshInstance3D.new()
		mi.mesh = c
		var m := ShaderMaterial.new()
		m.shader = _plume_shader
		if solid:
			m.set_shader_parameter("core_color", Color(1.0, 0.88, 0.6))
			m.set_shader_parameter("edge_color", Color(1.0, 0.45, 0.12))
			m.set_shader_parameter("smoke", 1.0)
		else:
			m.set_shader_parameter("core_color", Color(1.0, 0.96, 0.85) if k == 1 else Color(0.75, 0.82, 1.0))
			m.set_shader_parameter("edge_color", Color(0.3, 0.5, 1.0))
		m.set_shader_parameter("intensity", 2.6 if k == 1 else 1.3)
		mi.material_override = m
		mi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
		mi.name = "Core" if k == 1 else "Outer"
		root.add_child(mi)
	var light := OmniLight3D.new()
	light.light_color = Color(1.0, 0.6, 0.3) if solid else Color(0.9, 0.85, 1.0)
	light.omni_range = 8.0 + r * 20.0
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
## uid -> {pos, part} of parts that left this view this frame
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
			vanished[uid] = {"pos": _parts[uid].node.global_position, "part": _parts[uid].part}
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
		if k == 0:
			anchor = node.position
		node.visible = true
		if e.plume:
			_update_plume(e.plume, feed[o + 8], pressure, flicker)
		if e.canopy:
			_update_canopy(e.canopy, int(feed[o + 9]), feed[o + 13])
		_update_heat(e, feed[o + 11])
		for j in e.jets.size():
			var f := feed[o + 14 + j]
			var jet: Node3D = e.jets[j]
			jet.visible = f > 0.02
			if jet.visible:
				var s := (0.45 + 0.55 * f) * (0.85 + 0.3 * randf())
				jet.scale = Vector3(s, s, s)
		if e.flap:
			# a real flap can't jump: it slews at 60 degrees a second
			var fl: Node3D = e.flap
			fl.rotation.z = move_toward(fl.rotation.z, feed[o + 20], _dt * deg_to_rad(60.0))
		if e.tux:
			_animate_tux(e.tux, node)
		if Sim.part(e.part).has("leg"):
			var target := feed[o + 10]
			var kk: float = move_toward(e.legs, target, _dt / 1.4)
			if kk != e.legs:
				e.legs = kk
				PartVisual.pose_leg(node, Sim.part(e.part), smoothstep(0.0, 1.0, kk))
	for uid in _parts.keys():
		if not seen.has(uid):
			vanished[uid] = {"pos": _parts[uid].node.global_position, "part": _parts[uid].part}
			_parts[uid].node.queue_free()
			_parts.erase(uid)


## Picks the penguin's pose from how it moves, and turns it to face the
## way it walks (the physics keeps it upright; which way it looks is free).
func _animate_tux(tx: Dictionary, node: Node3D) -> void:
	var anim: TuxAnimator = tx.anim
	var vel: Vector3 = eva.get("vel", Vector3.ZERO)
	var local := node.global_transform.basis.inverse() * vel
	var flat := Vector2(local.x, local.z)
	var ground: bool = eva.get("ground", false)
	if ground and flat.length() > 0.25:
		var want := atan2(-local.x, -local.z)
		tx.facing.rotation.y = lerp_angle(tx.facing.rotation.y, want, clampf(_dt * 8.0, 0.0, 1.0))
	if ground:
		if tx.air:
			anim.play("land")
		elif not anim.busy():
			var sp := flat.length()
			anim.play("run" if sp > 1.2 else ("walk" if sp > 0.15 else "idle"), clampf(sp / 1.2, 0.6, 1.4) if sp > 0.15 else 1.0)
	elif eva.get("space", false):
		anim.play("float")
	else:
		anim.play("jump" if local.y > 0.3 else "fall")
	tx.air = not ground
	anim.tick(_dt)


## Lit nozzles for the exhaust smoke: [{key, pos, dir, thr, r, solid}].
func nozzles() -> Array:
	var out := []
	for uid in _parts:
		var p: Node3D = _parts[uid].plume
		if p == null or not p.visible or not p.is_inside_tree():
			continue
		out.append({"key": "%d/%d" % [vessel_id, uid], "pos": p.global_position, "dir": -p.global_basis.y.normalized(),
			"thr": float(p.get_meta("thr", 0.0)), "r": float(p.get_meta("exit_r")), "solid": bool(p.get_meta("solid"))})
	return out


func has_uid(uid: int) -> bool:
	return _parts.has(uid)


func _update_plume(p: Node3D, thr: float, pressure: float, flicker: float) -> void:
	var on := thr > 0.01
	p.set_meta("thr", thr)
	p.visible = on
	if not on:
		return
	var air := clampf(pressure / 101325.0, 0.0, 1.0)
	var r: float = p.get_meta("exit_r")
	var solid: bool = p.get_meta("solid")
	var vac := 1.0 - air
	var length := r * (14.0 if solid else 11.0) * (1.0 + 0.9 * vac) * (0.4 + 0.6 * thr) * (0.94 + 0.12 * flicker)
	for c in p.get_children():
		if c is MeshInstance3D:
			var mi := c as MeshInstance3D
			var core := mi.name == "Core"
			var m: ShaderMaterial = mi.material_override
			var L := length * (0.42 if core else 1.0)
			var rr := r * (0.55 if core else 0.97)
			m.set_shader_parameter("exit_r", rr)
			m.set_shader_parameter("len", L)
			m.set_shader_parameter("spread", lerpf(0.25, 1.9, vac) * (0.6 if core else 1.0))
			m.set_shader_parameter("diamonds", air)
			var ext := maxf(rr * 4.0, L)
			mi.custom_aabb = AABB(Vector3(-ext, -L - 1.0, -ext), Vector3(ext * 2.0, L + 2.0, ext * 2.0))
	var glow: OmniLight3D = p.get_node("Glow")
	glow.light_energy = 3.0 * thr


## open: canopy radius as a fraction of fully open (the core inflates it
## over a few seconds), so the drogue and the main grow as their drag does.
func _update_canopy(c: Node3D, state: int, open: float) -> void:
	# 2 = semi-deployed, 3 = fully deployed (trs_chute_state)
	c.visible = (state == 2 or state == 3) and open > 0.0
	if not c.visible:
		return
	var r := maxf(6.5 * open, 0.9)
	var lift := lerpf(3.0, 9.0, clampf((open - 0.11) / 0.89, 0.0, 1.0))
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
		line.basis = Basis(Quaternion(Vector3.UP, top.normalized())) * Basis.from_scale(Vector3(1, len, 1))
	# an open canopy trails straight back along the airflow, whatever the capsule's attitude
	if air_vel.length() > 2.0 and c.is_inside_tree():
		var want := Basis(Quaternion(Vector3.UP, -air_vel.normalized()))
		var gb := c.global_basis.orthonormalized()
		c.global_basis = Basis(gb.get_rotation_quaternion().slerp(want.get_rotation_quaternion(), 0.25))
	else:
		c.basis = Basis.IDENTITY
