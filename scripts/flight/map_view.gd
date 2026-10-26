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
## Map view: the system at 1/SCALE in its own world, with the vessel's
## predicted patched-conic trajectory (from trs_core), apsis and SOI markers,
## and maneuver nodes you place by clicking the path and edit by dragging
## the prograde / normal / radial handles.
class_name MapView
extends Control

const SCALE := 1.0e-5
const MOON_MERGE_PX := 40.0  # a moon this close to its planet on screen is left out
const HANDLE_RATE := 0.6  # m/s per pixel dragged (grows with distance)
const AIR_PATH := Color(1.0, 0.45, 0.35, 0.55)  # drag-free continuation through the air
const TARGET_PATH := Color(0.95, 0.4, 1.0)  # the target's orbit and its closest-approach mark
const COLORS := [Color(0.35, 0.85, 1.0), Color(1.0, 0.62, 0.2), Color(0.6, 1.0, 0.45), Color(1.0, 0.45, 0.8), Color(0.9, 0.9, 0.4)]

var session: FlightSession
var focus_body := 1
## When set, the camera pivots on the vessel (the default); Tab moves the
## focus to the bodies and back.
var focus_vessel := true
var _target := Vector3.ZERO
var _ca_line: Array = []  # [our point, theirs] at the closest approach
var _vp: SubViewport
var _cam: Camera3D
var _yaw := 0.6
var _pitch := 0.9
var _dist := 30.0
var _bodies := []
var _lines: MeshInstance3D
var _imesh: ImmediateMesh
var _vessel_icon: MeshInstance3D
var _markers := []  # [{pos: Vector3 map world, label, color}]
var _samples := []  # [{pos (map), t, patch}] for picking
var _patches: Array = []
var _drag := {}     # {node, axis, start_mouse, start_value}
var _hover_handle := {}
var _orbiting := false
var _panel: RichTextLabel
var _warp_btn: Button
var _del_btn: Button
var _orbit_btns: Array[Button] = []
var _plan_btn: Button
var _plan_note := ""
var _plan_thread: Thread  ## the planner runs here so the game keeps going
var _plan_stale := false  ## the craft burned while it planned
var _shift_note := ""
var selected_node := -1
var _overlay: Control
const GLIDE_SECONDS := 2.5
var _glide_t := -1.0  # 0..1 through an SOI-change camera glide, -1 idle
var _glide_from := 1.0
var _glide_to := 1.0


func setup(s: FlightSession) -> void:
	session = s
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	var svc := SubViewportContainer.new()
	svc.stretch = true
	svc.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	svc.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(svc)
	_vp = SubViewport.new()
	_vp.own_world_3d = true
	_vp.msaa_3d = Viewport.MSAA_4X
	svc.add_child(_vp)
	var we := WorldEnvironment.new()
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.015, 0.02, 0.04)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.35, 0.38, 0.45)
	env.glow_enabled = true
	we.environment = env
	_vp.add_child(we)
	var sun := DirectionalLight3D.new()
	sun.name = "Sun"
	_vp.add_child(sun)
	_cam = Camera3D.new()
	_cam.near = 0.01
	_cam.far = 1.0e5
	_vp.add_child(_cam)
	var core := session.core
	for i in core.body_count():
		var info: Dictionary = JSON.parse_string(core.body_info(i))
		var mi := MeshInstance3D.new()
		var sm := SphereMesh.new()
		sm.radius = 1.0
		sm.height = 2.0
		mi.mesh = sm
		var m := StandardMaterial3D.new()
		match info.id:
			"austra":
				m.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
				m.albedo_color = Color(1.0, 0.85, 0.55)
			"floe":
				m.albedo_color = Color(0.78, 0.88, 1.0)
			"krill":
				m.albedo_color = Color(0.6, 0.58, 0.56)
		# real baked surfaces (shared cache with scaled space) for the planets
		mi.material_override = m if info.id == "austra" else PlanetBake.planet_material(core, i)
		sm.radial_segments = 96
		sm.rings = 48
		_vp.add_child(mi)
		var orbit_line := MeshInstance3D.new()
		var om := ImmediateMesh.new()
		orbit_line.mesh = om
		var lm := StandardMaterial3D.new()
		lm.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
		lm.albedo_color = Color(0.55, 0.55, 0.65, 0.6)
		lm.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
		orbit_line.material_override = lm
		_vp.add_child(orbit_line)
		_bodies.append({"node": mi, "info": info, "orbit": orbit_line, "orbit_mesh": om})
	_imesh = ImmediateMesh.new()
	_lines = MeshInstance3D.new()
	_lines.mesh = _imesh
	var lmat := StandardMaterial3D.new()
	lmat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	lmat.vertex_color_use_as_albedo = true
	_lines.material_override = lmat
	_vp.add_child(_lines)
	# the same path faintly through anything in front of it (a planet's
	# limb, the far side of an orbit): never lost, but clearly behind
	var ghost := MeshInstance3D.new()
	ghost.mesh = _imesh
	var gmat := StandardMaterial3D.new()
	gmat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	gmat.vertex_color_use_as_albedo = true
	gmat.no_depth_test = true
	gmat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	gmat.albedo_color = Color(1, 1, 1, 0.55)
	ghost.material_override = gmat
	_vp.add_child(ghost)
	_vessel_icon = MeshInstance3D.new()
	var vm := SphereMesh.new()
	vm.radius = 0.04
	vm.height = 0.08
	_vessel_icon.mesh = vm
	var vmat := StandardMaterial3D.new()
	vmat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	vmat.albedo_color = Color(1.0, 1.0, 1.0)
	vmat.no_depth_test = true
	_vessel_icon.material_override = vmat
	_vp.add_child(_vessel_icon)

	_overlay = Control.new()
	_overlay.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_overlay.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_overlay.draw.connect(_draw_overlay)
	add_child(_overlay)
	var p := PanelContainer.new()
	p.set_anchors_preset(Control.PRESET_CENTER_RIGHT)
	p.offset_left = -300
	p.offset_right = -10
	p.offset_top = -120
	p.offset_bottom = 120
	add_child(p)
	var v := VBoxContainer.new()
	p.add_child(v)
	_panel = RichTextLabel.new()
	_panel.bbcode_enabled = true
	_panel.fit_content = true
	_panel.custom_minimum_size = Vector2(270, 0)
	v.add_child(_panel)
	_warp_btn = Button.new()
	_warp_btn.text = "Skip to burn"
	_warp_btn.tooltip_text = "Fast-forward time until just before this maneuver node's burn starts,\nso you can point the ship and burn (MNV on the SAS row points it for you)."
	_warp_btn.focus_mode = Control.FOCUS_NONE
	_warp_btn.pressed.connect(_warp_to_node)
	v.add_child(_warp_btn)
	var orb := HBoxContainer.new()
	for dir in [-1, 1]:
		var ob := Button.new()
		ob.text = "◀ orbit earlier" if dir < 0 else "orbit later ▶"
		ob.tooltip_text = "Move this burn a whole orbit %s, to the same point on the path" % ("earlier" if dir < 0 else "later")
		ob.focus_mode = Control.FOCUS_NONE
		ob.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		var d: int = dir
		ob.pressed.connect(func():
			var ni := session.shift_node(selected_node, d)
			if ni >= 0:
				selected_node = ni
			_shift_note = session.shift_why)
		orb.add_child(ob)
		_orbit_btns.append(ob)
	v.add_child(orb)
	_plan_btn = Button.new()
	_plan_btn.focus_mode = Control.FOCUS_NONE
	_plan_btn.visible = false
	_plan_btn.pressed.connect(_plan)
	v.add_child(_plan_btn)
	_del_btn = Button.new()
	_del_btn.text = "Delete node"
	_del_btn.focus_mode = Control.FOCUS_NONE
	_del_btn.pressed.connect(func(): _delete_node(selected_node))
	v.add_child(_del_btn)
	var help := Label.new()
	help.text = "M back to flight · click your path to add a burn (maneuver node) · drag its handles · right-drag orbit · wheel zoom · Tab: focus vessel / planets"
	help.set_anchors_preset(Control.PRESET_TOP_WIDE)
	help.offset_top = 118
	help.offset_bottom = 136
	help.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	help.add_theme_font_size_override("font_size", 13)
	help.add_theme_color_override("font_color", Color(0.7, 0.78, 0.9))
	help.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(help)
	mouse_filter = Control.MOUSE_FILTER_STOP


# ---- coordinates ---------------------------------------------------------------

## Positions (Godot axes, metres) of every body relative to the focus body.
func _body_offsets() -> Array:
	var feed: PackedFloat64Array = session.core.body_render(session.active)
	var out := []
	if feed.size() < _bodies.size() * 5:
		# no live vessel to measure from (it just broke up): nothing moves
		for i in _bodies.size():
			out.append(Vector3.ZERO)
		return out
	var f := Frame.vec(feed, focus_body * 5)
	for i in _bodies.size():
		out.append(Frame.vec(feed, i * 5) - f)
	return out


func refresh() -> void:
	if session.active < 0 or not session.core.vessel_alive(session.active):
		return
	var t := session.telemetry()
	if focus_vessel:
		var nb := int(t.get("body_index", focus_body))
		if nb != focus_body:
			_glide_soi(focus_body, nb)
		focus_body = nb
	if _glide_t >= 0.0:
		# after a change of sphere of influence the camera eases out (or in)
		# to show the new neighbourhood instead of jumping
		_glide_t = minf(_glide_t + get_process_delta_time() / GLIDE_SECONDS, 1.0)
		_dist = exp(lerpf(log(_glide_from), log(_glide_to), smoothstep(0.0, 1.0, _glide_t)))
		if _glide_t >= 1.0:
			_glide_t = -1.0
	var offs := _body_offsets()
	# the vessel, from exact relative positions (it is the render origin)
	var vrel := Frame.vec(session.core.vessel_render(session.active, session.active), 1)
	var feed: PackedFloat64Array = session.core.body_render(session.active)
	var vpos := (-Frame.vec(feed, focus_body * 5) + vrel) * SCALE
	# camera around the vessel or the focus body
	var want := vpos if focus_vessel else Vector3.ZERO
	_target = want if _target.distance_to(want) > _dist * 2.0 else _target.lerp(want, 0.25)
	_cam.position = _target + Vector3(cos(_pitch) * sin(_yaw), sin(_pitch), cos(_pitch) * cos(_yaw)) * _dist
	# never inside a planet (it would vanish): lift the eye out over it
	for i in _bodies.size():
		var c: Vector3 = offs[i] * SCALE
		var rr: float = float(_bodies[i].info.radius) * SCALE * 1.03
		if _cam.position.distance_to(c) < rr:
			_cam.position = c + (_cam.position - c).normalized() * rr
	if _cam.position.distance_to(_target) > 1e-6:
		_cam.look_at(_target, Vector3.UP if absf((_target - _cam.position).normalized().y) < 0.99 else Vector3.RIGHT)
	var sun: DirectionalLight3D = _vp.get_node("Sun")
	var sd := (offs[0] as Vector3).normalized()
	if sd.length() > 0.5:
		sun.look_at(-sd, Vector3.UP if absf(sd.y) < 0.99 else Vector3.RIGHT)
	for i in _bodies.size():
		var b: Dictionary = _bodies[i]
		var r: float = b.info.radius
		var pos: Vector3 = offs[i] * SCALE
		# bodies past the far plane are drawn pulled in toward the camera's
		# pivot (true direction, smaller); near ones sit where they are
		var k := 1.0
		var rel := pos - _target
		if rel.length() > 5.0e4:
			k = 5.0e4 / rel.length()
		# never smaller than a dot on screen; a moon's dot is the smaller one
		var moon := int(b.info.parent) > 0
		b.node.transform = Transform3D(Frame.spin(feed[i * 5 + 3]).scaled(Vector3.ONE * maxf(r * SCALE * k, _dist * (0.006 if moon else 0.012))), _target + rel * k)
		b.node.visible = true
		_draw_body_orbit(i, offs)
	# zoomed out so far that a moon would sit on its planet on screen (dots
	# and names on top of each other): the moon and its orbit are left out
	# until you zoom back in
	for i in _bodies.size():
		var b: Dictionary = _bodies[i]
		var parent := int(b.info.parent)
		if parent <= 0 or i == focus_body:
			continue
		var pn: MeshInstance3D = _bodies[parent].node
		if _cam.is_position_behind(pn.position) or _cam.is_position_behind(b.node.position):
			continue
		var gap := _cam.unproject_position(b.node.position).distance_to(_cam.unproject_position(pn.position))
		if gap < MOON_MERGE_PX:
			b.node.visible = false
			(b.orbit_mesh as ImmediateMesh).clear_surfaces()
	# vessel trajectory with nodes
	var nodes := FlightSession.pack_nodes(session.nodes)
	_patches = JSON.parse_string(session.core.trajectory(session.active, nodes, 8, 3.0e6))
	var pts: PackedFloat64Array = session.core.trajectory_points(session.active, nodes, 8, 3.0e6, 256)
	_imesh.clear_surfaces()
	_samples.clear()
	_markers.clear()
	var n := pts.size() / 5
	if n > 1:
		_imesh.surface_begin(Mesh.PRIMITIVE_LINES)
		var prev := Vector3.ZERO
		var prev_patch := -1
		for k in n:
			var o := k * 5
			var pi := int(pts[o])
			var p: Dictionary = _patches[pi if pi >= 0 else -1 - pi]
			var world: Vector3 = (offs[int(p.body_index)] + Frame.vec(pts, o + 2)) * SCALE
			if pi >= 0:
				_samples.append({"pos": world, "t": pts[o + 1], "patch": pi})
			if pi == prev_patch:
				# current orbit cyan, after a node orange, later patches
				# green/pink/yellow; through the air to the ground a dim red
				var col: Color = AIR_PATH if pi < 0 else (COLORS[0] if pi == 0 else (COLORS[1] if int(p.node) >= 0 else COLORS[2 + (pi % 3)]))
				_imesh.surface_set_color(col)
				_imesh.surface_add_vertex(prev)
				_imesh.surface_add_vertex(world)
			prev = world
			prev_patch = pi
		_imesh.surface_end()
	# the target's path, and where we pass closest to it
	_ca_line = []
	var ti := session.target_info()
	if not ti.is_empty():
		var hz := session.approach_horizon()
		var tp: Array = JSON.parse_string(session.core.trajectory(session.target, PackedFloat64Array(), 8, hz))
		var tpts: PackedFloat64Array = session.core.trajectory_points(session.target, PackedFloat64Array(), 8, hz, 256)
		if tpts.size() >= 10:
			_imesh.surface_begin(Mesh.PRIMITIVE_LINES)
			var tprev := Vector3.ZERO
			var tpp := -999
			for k in tpts.size() / 5:
				var o := k * 5
				var pi := int(tpts[o])
				var p: Dictionary = tp[pi if pi >= 0 else -1 - pi]
				var world: Vector3 = (offs[int(p.body_index)] + Frame.vec(tpts, o + 2)) * SCALE
				if pi == tpp:
					_imesh.surface_set_color(TARGET_PATH)
					_imesh.surface_add_vertex(tprev)
					_imesh.surface_add_vertex(world)
				tprev = world
				tpp = pi
			_imesh.surface_end()
		var ca := session.closest_approach()
		if not ca.is_empty():
			var bo: Vector3 = offs[int(ca.body_index)]
			var pa: Vector3 = (bo + Frame.vec_arr(ca.pos)) * SCALE
			var pb: Vector3 = (bo + Frame.vec_arr(ca.target_pos)) * SCALE
			var lbl := "Closest %s in %s · %.1f m/s" % [FlightHUD.fmt_dist(ca.dist), FlightHUD.fmt_time(float(ca.t) - session.time()), ca.rel_speed]
			if ca.edge:
				lbl = "No pass closer than %s in the next %d orbits" % [FlightHUD.fmt_dist(ca.dist), int(FlightSession.APPROACH_ORBITS)]
			_markers.append({"pos": pa, "label": lbl, "color": COLORS[0]})
			_markers.append({"pos": pb, "label": str(ti.get("name", "target")), "color": TARGET_PATH})
			_ca_line = [pa, pb]
	# apses and SOI transitions, at exact positions from trs_core
	for pi in _patches.size():
		var p: Dictionary = _patches[pi]
		var bo: Vector3 = offs[int(p.body_index)]
		for kind in ["ap", "pe"]:
			if not p.has(kind + "_pos"):
				continue
			_markers.append({"pos": (bo + Frame.vec_arr(p[kind + "_pos"])) * SCALE,
				"label": "%s %s" % ["Ap" if kind == "ap" else "Pe", FlightHUD.fmt_dist(p[kind + "_alt"])], "color": Color(0.75, 0.9, 1.0)})
		var endp: Vector3 = (bo + Frame.vec_arr(p.end_pos)) * SCALE
		if p.end == "soi_enter" or p.end == "soi_exit":
			var nxt: String = _patches[pi + 1].body.capitalize() if pi + 1 < _patches.size() else "?"
			_markers.append({"pos": endp, "label": ("Encounter " if p.end == "soi_enter" else "Escape to ") + nxt, "color": COLORS[2]})
		elif p.end == "atmosphere" or p.end == "surface":
			_markers.append({"pos": endp, "label": "Impact" if p.end == "surface" else "Atmosphere", "color": Color(1, 0.4, 0.3)})
	# the vessel itself
	_vessel_icon.position = vpos
	_vessel_icon.scale = Vector3.ONE * _dist / 30.0
	_update_panel(t)
	_overlay.queue_redraw()


func _sample_at(patch: int, t: float) -> Dictionary:
	var best := {}
	var bd := INF
	for s in _samples:
		if s.patch != patch:
			continue
		var d := absf(s.t - t)
		if d < bd:
			bd = d
			best = s
	return best


func _draw_body_orbit(i: int, offs: Array) -> void:
	var b: Dictionary = _bodies[i]
	var om: ImmediateMesh = b.orbit_mesh
	om.clear_surfaces()
	var parent := int(b.info.parent)
	if parent < 0 or not b.info.has("sma") or i == focus_body:
		return
	var r: float = b.info.sma
	if parent != focus_body and i != 0:
		pass
	var c: Vector3 = offs[parent] * SCALE
	if c.length() > 5.0e4:
		return
	om.surface_begin(Mesh.PRIMITIVE_LINE_STRIP)
	for k in 129:
		var a := TAU * k / 128.0
		om.surface_add_vertex(c + Vector3(cos(a), 0, -sin(a)) * r * SCALE)
	om.surface_end()


## Transfer windows from the planet we orbit to the one in focus (or to
## every sibling planet when the star is in focus).
func _transfer_text(t: Dictionary) -> String:
	var home := int(t.get("body_index", -1))
	if home <= 0 or int(_bodies[home].info.parent) != 0:
		return ""
	var dests := []
	if focus_body > 0 and focus_body != home and int(_bodies[focus_body].info.parent) == 0:
		dests = [focus_body]
	elif focus_body == 0:
		for i in _bodies.size():
			if i != home and i > 0 and int(_bodies[i].info.parent) == 0:
				dests.append(i)
	var park: float = float(t.altitude) + float(_bodies[home].info.radius)
	var out := ""
	for d in dests:
		var w: Dictionary = JSON.parse_string(session.core.transfer_window(home, d, park))
		if w.is_empty():
			continue
		out += "\n[color=#9fe08a][b]To %s[/b][/color]: window in [b]%s[/b] (it should lead by %.1f°, now %.1f°)\n   eject %.0f m/s from here · %s flight · every %s" % [
			_bodies[d].info.name, FlightHUD.fmt_time(w.wait), w.phase_best_deg, w.phase_now_deg, w.dv_eject,
			FlightHUD.fmt_time(w.flight_time), FlightHUD.fmt_time(w.synodic)]
	return out


## What the plan button would do now: "transfer", "correction" or "".
func _plan_kind(t: Dictionary) -> String:
	var home := int(t.get("body_index", -1))
	if focus_body <= 0 or home < 0 or focus_body == home or int(_bodies[focus_body].info.parent) != 0:
		return ""
	if home == 0:
		return "correction"
	if int(_bodies[home].info.parent) == 0 and t.get("orbit", {}).get("ap_alt") != null:
		return "transfer"
	return ""


## The burn to the focused planet: from orbit at the next window, or (on
## the way round the star) a small correction to a pass just above its air.
func _plan() -> void:
	var t := session.telemetry()
	var kind := _plan_kind(t)
	if kind == "":
		return
	if _plan_thread != null:
		return
	var info: Dictionary = _bodies[focus_body].info
	var want: float = (float(info.atmosphere_height) + 40000.0) if float(info.get("atmosphere_height", 0.0)) > 0.0 else float(info.radius) * 0.15
	var core = session.core
	if not core.plan_start(session.active, focus_body, want, kind == "correction"):
		return
	var id := session.active
	var serial: int = core.vessel_serial(id)
	_plan_note = "Planning... (the game keeps running)"
	_plan_stale = false
	_plan_btn.disabled = true
	_plan_thread = Thread.new()
	_plan_thread.start(core.plan_run)
	while _plan_thread != null and _plan_thread.is_alive():
		await get_tree().process_frame
		if session.telemetry().get("thrust", 0.0) > 0.0:
			_plan_stale = true
	if _plan_thread == null:  # joined by _exit_tree
		return
	var r: PackedFloat64Array = _plan_thread.wait_to_finish()
	_plan_thread = null
	_plan_btn.disabled = false
	if r.size() < 6:
		_plan_note = "no window from here"
		return
	if _plan_stale or not session.still(id, serial) or session.active != id or r[0] < session.time() + 5.0:
		_plan_note = "the craft moved on while planning: plan again"
		return
	session.nodes.append([r[0], r[1], r[2], r[3]])
	session.nodes.sort_custom(func(a, b): return a[0] < b[0])
	selected_node = session.nodes.find(session.nodes.filter(func(n): return n[0] == r[0])[0])
	if r[4] > 0.5:
		_plan_note = "%s: into %s's sphere, passing %s up" % ["Transfer" if kind == "transfer" else "Correction", info.name, FlightHUD.fmt_dist(r[5])]
	else:
		_plan_note = "best found misses %s: try again nearer the window" % info.name
	Sfx.play("click")


func _update_panel(t: Dictionary) -> void:
	var txt := "[b]%s[/b] · focus [color=#8fd0ff]%s[/color]\n" % [t.get("name", ""), _bodies[focus_body].info.name]
	var pk := _plan_kind(t)
	_plan_btn.visible = pk != ""
	if pk != "":
		_plan_btn.text = ("Plan transfer to %s" if pk == "transfer" else "Plan correction to %s") % _bodies[focus_body].info.name
	if _plan_note != "":
		txt += "[color=#9fe08a]%s[/color]\n" % _plan_note
	if session.nodes.is_empty():
		txt += "[color=#9fb4cc]No maneuver nodes. Click the orbit line to add one.[/color]"
		_warp_btn.disabled = true
		_del_btn.disabled = true
		for b in _orbit_btns:
			b.disabled = true
		txt += _transfer_text(t)
	else:
		if selected_node < 0 or selected_node >= session.nodes.size():
			selected_node = 0
		var nd: Array = session.nodes[selected_node]
		var dv := Vector3(nd[1], nd[2], nd[3]).length()
		var bt: float = session.core.burn_time(session.active, dv)
		txt += "[color=#ff9a3a][b]Node %d[/b][/color] in %s\nΔv [b]%.1f m/s[/b]  (pro %.1f · nrm %.1f · rad %.1f)\nBurn time %s\n" % [
			selected_node + 1, FlightHUD.fmt_time(nd[0] - t.t), dv, nd[1], nd[2], nd[3], FlightHUD.fmt_time(bt) if bt >= 0 else "no engine"]
		for p in _patches:
			if int(p.node) == selected_node:
				txt += "After: Pe %s, Ap %s" % [FlightHUD.fmt_dist(p.pe_alt), "escape" if p.ap_alt == null else FlightHUD.fmt_dist(p.ap_alt)]
		for p in _patches:
			if p.body == "krill" and t.body != "krill":
				txt += "\n[color=#99ff77]Krill encounter: Pe %s[/color]" % FlightHUD.fmt_dist(p.pe_alt)
				break
		_warp_btn.disabled = false
		_del_btn.disabled = false
		for b in _orbit_btns:
			b.disabled = false
		if _shift_note != "":
			txt += "\n[color=#ff9a7a]Can't move it: %s[/color]" % _shift_note
	_panel.text = txt


# ---- nodes -----------------------------------------------------------------------

func _node_screen(i: int) -> Dictionary:
	var nd: Array = session.nodes[i]
	var offs := _body_offsets()
	for pi in _patches.size():
		var p: Dictionary = _patches[pi]
		if int(p.node) == i:
			var w: Vector3 = (offs[int(p.body_index)] + Frame.vec_arr(p.start_pos)) * SCALE
			if _cam.is_position_behind(w):
				return {}
			return {"pos": _cam.unproject_position(w), "world": w}
	return {}


## Screen-space handle directions for node i: prograde/normal/radial axes.
func _handles(i: int) -> Array:
	var ns := _node_screen(i)
	if ns.is_empty():
		return []
	var nd: Array = session.nodes[i]
	var v: PackedFloat64Array = session.core.node_vector(session.active, FlightSession.pack_nodes([[nd[0], 1.0, 0.0, 0.0]]), 0)
	var pro := Frame.vec(v, 0).normalized()
	var vn: PackedFloat64Array = session.core.node_vector(session.active, FlightSession.pack_nodes([[nd[0], 0.0, 1.0, 0.0]]), 0)
	var nrm := Frame.vec(vn, 0).normalized()
	var vr: PackedFloat64Array = session.core.node_vector(session.active, FlightSession.pack_nodes([[nd[0], 0.0, 0.0, 1.0]]), 0)
	var rad := Frame.vec(vr, 0).normalized()
	var out := []
	var base: Vector3 = ns.world
	for h in [[pro, 1, 1.0, Color(1, 0.9, 0.2)], [-pro, 1, -1.0, Color(1, 0.9, 0.2)], [nrm, 2, 1.0, Color(0.85, 0.4, 1)],
			[-nrm, 2, -1.0, Color(0.85, 0.4, 1)], [rad, 3, 1.0, Color(0.3, 0.95, 1)], [-rad, 3, -1.0, Color(0.3, 0.95, 1)]]:
		var tip: Vector3 = base + h[0] * _dist * 0.06
		if _cam.is_position_behind(tip):
			continue
		var sp := _cam.unproject_position(tip)
		var d: Vector2 = (sp - ns.pos)
		if d.length() < 1.0:
			continue
		out.append({"pos": ns.pos + d.normalized() * 46.0, "axis": h[1], "sign": h[2], "color": h[3], "dir": d.normalized()})
	return out


func _gui_input(event: InputEvent) -> void:
	if event is InputEventMouseButton:
		var mb := event as InputEventMouseButton
		match mb.button_index:
			MOUSE_BUTTON_RIGHT:
				_orbiting = mb.pressed
			MOUSE_BUTTON_WHEEL_UP:
				_dist = maxf(0.3, _dist * 0.88)
				_glide_t = -1.0
			MOUSE_BUTTON_WHEEL_DOWN:
				_dist = minf(4.0e4, _dist * 1.12)
				_glide_t = -1.0
			MOUSE_BUTTON_LEFT:
				if mb.pressed:
					_press(mb.position)
				else:
					_drag = {}
		accept_event()
	elif event is InputEventMouseMotion:
		var mm := event as InputEventMouseMotion
		if _orbiting:
			_yaw -= mm.relative.x * 0.008
			_pitch = clampf(_pitch + mm.relative.y * 0.006, -1.5, 1.5)
		elif not _drag.is_empty():
			var along: float = mm.relative.dot(_drag.dir) * _drag.sign
			var nd: Array = session.nodes[_drag.node]
			var rate := HANDLE_RATE * (1.0 + absf(nd[_drag.axis]) * 0.01)
			nd[_drag.axis] = nd[_drag.axis] + along * rate * (0.1 if Input.is_key_pressed(KEY_SHIFT) else 1.0)
		accept_event()


func _press(pos: Vector2) -> void:
	# handles first
	for i in session.nodes.size():
		for h in _handles(i):
			if (h.pos as Vector2).distance_to(pos) < 14.0:
				selected_node = i
				_drag = {"node": i, "axis": h.axis, "sign": h.sign, "dir": h.dir}
				return
		var ns := _node_screen(i)
		if not ns.is_empty() and (ns.pos as Vector2).distance_to(pos) < 14.0:
			selected_node = i
			return
	# otherwise add a node at the nearest point on the path
	var best := {}
	var bd := 12.0
	var now := session.time()
	for s in _samples:
		if _cam.is_position_behind(s.pos) or s.t <= now + 1.0:
			continue
		var d := _cam.unproject_position(s.pos).distance_to(pos)
		if d < bd:
			bd = d
			best = s
	if not best.is_empty():
		session.nodes.append([best.t, 0.0, 0.0, 0.0])
		session.nodes.sort_custom(func(a, b): return a[0] < b[0])
		for i in session.nodes.size():
			if session.nodes[i][0] == best.t:
				selected_node = i


func _delete_node(i: int) -> void:
	if i >= 0 and i < session.nodes.size():
		session.nodes.remove_at(i)
		selected_node = -1


func _warp_to_node() -> void:
	if session.nodes.is_empty():
		return
	var nd: Array = session.nodes[clampi(selected_node, 0, session.nodes.size() - 1)]
	var dv := Vector3(nd[1], nd[2], nd[3]).length()
	var bt: float = maxf(session.core.burn_time(session.active, dv), 0.0)
	var reason := session.warp_to(nd[0] - bt * 0.5 - 30.0)
	if reason != "" and get_parent().has_method("toast"):
		get_parent().toast("Warp stopped: " + reason)


## Starts the camera's glide after the vessel passes from body `from` to
## `to`: out to the new parent's view with the old planet's sphere of
## influence in frame, or in toward the moon it just met.
func _glide_soi(from: int, to: int) -> void:
	if from < 0 or from >= _bodies.size() or to < 0 or to >= _bodies.size():
		return
	var want := _dist
	if int(_bodies[from].info.parent) == to:
		want = maxf(_dist, float(_bodies[from].info.soi) * SCALE * 2.5)
	else:
		want = minf(_dist, float(_bodies[to].info.radius) * SCALE * 12.0)
	if absf(want - _dist) < 1e-6:
		return
	_glide_from = _dist
	_glide_to = clampf(want, 0.3, 4.0e4)
	_glide_t = 0.0


## vessel -> Floe -> Krill -> ... -> vessel
func cycle_focus() -> void:
	if focus_vessel:
		focus_vessel = false
		focus_body = 1
	else:
		focus_body += 1
		if focus_body >= _bodies.size():
			focus_vessel = true
			focus_body = int(session.telemetry().get("body_index", 1))
	_dist = 6.0 if focus_vessel else (30.0 if focus_body == 1 else 8.0)


## Re-centres on the vessel (when the map opens).
func focus_on_vessel() -> void:
	focus_vessel = true
	focus_body = int(session.telemetry().get("body_index", 1))
	_dist = clampf(_dist, 2.0, 40.0)


func _draw_overlay() -> void:
	var ov := _overlay
	var font := get_theme_default_font()
	# you are here
	if not _cam.is_position_behind(_vessel_icon.position):
		var vp := _cam.unproject_position(_vessel_icon.position)
		var pulse := 0.6 + 0.4 * sin(Time.get_ticks_msec() * 0.006)
		ov.draw_arc(vp, 9.0 + 3.0 * pulse, 0, TAU, 24, Color(1, 1, 1, 0.8), 2.0, true)
		ov.draw_circle(vp, 3.5, Color(1, 1, 1))
		# name on the left: Ap / Pe / node labels go to the right of their marks
		var vname := str(session.telemetry().get("name", "vessel"))
		var vw := font.get_string_size(vname, HORIZONTAL_ALIGNMENT_LEFT, -1, 14).x
		ov.draw_string(font, vp + Vector2(-14 - vw, 5), vname, HORIZONTAL_ALIGNMENT_LEFT, -1, 14, Color(1, 1, 1, 0.9))
	if _ca_line.size() == 2 and not _cam.is_position_behind(_ca_line[0]) and not _cam.is_position_behind(_ca_line[1]):
		ov.draw_dashed_line(_cam.unproject_position(_ca_line[0]), _cam.unproject_position(_ca_line[1]), Color(TARGET_PATH, 0.8), 1.5, 6.0)
	for m in _markers:
		if _cam.is_position_behind(m.pos):
			continue
		var p := _cam.unproject_position(m.pos)
		ov.draw_circle(p, 4.0, m.color)
		ov.draw_string(font, p + Vector2(8, -6), m.label, HORIZONTAL_ALIGNMENT_LEFT, -1, 13, m.color)
	for i in (session.nodes.size() if session.core.vessel_alive(session.active) else 0):
		var ns := _node_screen(i)
		if ns.is_empty():
			continue
		var c: Vector2 = ns.pos
		ov.draw_arc(c, 10, 0, TAU, 24, Color(0.35, 0.6, 1.0), 3.0, true)
		ov.draw_circle(c, 4, Color(0.35, 0.6, 1.0) if i != selected_node else Color(1, 0.6, 0.2))
		if i == selected_node:
			for h in _handles(i):
				ov.draw_line(c, h.pos, Color(h.color, 0.6), 2.0)
				ov.draw_circle(h.pos, 8.0, h.color)
	for b in _bodies:
		var p3: Vector3 = b.node.position
		if not b.node.visible or _cam.is_position_behind(p3):
			continue
		var p := _cam.unproject_position(p3)
		ov.draw_string(font, p + Vector2(10, 18), b.info.name, HORIZONTAL_ALIGNMENT_LEFT, -1, 14, Color(0.85, 0.9, 1.0, 0.85))


## A plan still running when the map goes away finishes first: it reads
## only its own copy of the bodies, but the Thread must be joined.
func _exit_tree() -> void:
	if _plan_thread != null:
		_plan_thread.wait_to_finish()
		_plan_thread = null
