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
## Flight: renders a FlightSession. The active vessel's centre of mass is
## always the origin (floating origin); everything else is placed relative
## to it from double-precision trs_core feeds every frame.
extends Node3D

const SKY_DAY := Color(0.38, 0.6, 0.92)
const THROTTLE_RATE := 0.6  # per second with Shift/Ctrl

var session: FlightSession
var hud: FlightHUD
var mission_start := 0.0
var _cam: Camera3D
var _cam_yaw := 0.4
var _cam_pitch := 0.08
var _cam_dist := 30.0
var _orbiting := false
var _terrain: TerrainView
var _scaled: ScaledSpace
var _views := {}  # vessel id -> VesselView
var _env: Environment
var _sun: DirectionalLight3D
var _throttle := 0.0
var _launched := false
var _flicker := 0.0
var _map: MapView
var _sas_maneuver := false
var crew_names: Array = []
var _portraits: Array[PenguinPortrait] = []
var _mission := {"max_altitude": 0.0, "max_g": 0.0, "orbits": 0, "krill_landings": 0, "visited": ["floe"], "pod_lost": false}
var _was_orbiting := false
var _recover_btn: Button
var _recorded := false


func _ready() -> void:
	session = FlightSession.new()
	var text := Sim.pending_craft_text
	if text == "":
		text = FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "orbiter.craft.json")
	Sim.pending_craft_text = text
	session.launch(text, "floe")
	session.sim_event.connect(_on_sim_event)
	var cs := get_node_or_null("/root/ControlSocket")
	if cs:
		cs.attach(session)
	_build()
	_assign_crew()
	_cam_dist = 18.0 + _craft_height() * 1.2
	hud.toast("PASA, %s is on the pad. Z for full throttle, SPACE to launch." % session.telemetry().get("name", "vessel"), 5.0)


func _assign_crew() -> void:
	for p in _portraits:
		p.queue_free()
	_portraits.clear()
	var seats := int(session.telemetry().get("crew_capacity", 0))
	crew_names = Crew.assign(seats, Sim.pending_crew)
	var row := HBoxContainer.new()
	row.name = "Crew"
	row.add_theme_constant_override("separation", 6)
	row.set_anchors_and_offsets_preset(Control.PRESET_BOTTOM_LEFT)
	for c in hud.portrait_slot.get_children():
		c.queue_free()
	hud.portrait_slot.add_child(row)
	for n in crew_names:
		var pp := PenguinPortrait.new()
		pp.set_member(Crew.find(n))
		if crew_names.size() > 1:
			pp.custom_minimum_size = Vector2(118, 150)
		else:
			pp.custom_minimum_size = Vector2(200, 220)
		row.add_child(pp)
		_portraits.append(pp)
	row.position = Vector2(0, 240 - (220 if crew_names.size() <= 1 else 150))
	_recover_btn = Button.new()
	_recover_btn.text = "  Recover vessel  "
	_recover_btn.visible = false
	_recover_btn.focus_mode = Control.FOCUS_NONE
	_recover_btn.set_anchors_and_offsets_preset(Control.PRESET_CENTER_TOP)
	_recover_btn.position.y = 90
	_recover_btn.pressed.connect(_recover)
	hud.add_child(_recover_btn)


func _react(kind: String, secs := 3.0) -> void:
	for p in _portraits:
		p.react(kind, secs)


## Writes the flight into the crew roster (not for reverts).
func _record(outcome: String) -> void:
	if _recorded or not _launched or crew_names.is_empty():
		return
	_recorded = true
	Crew.record_flight(crew_names, outcome, _mission)


func _recover() -> void:
	_record("recovered")
	var names := ", ".join(crew_names)
	Sim.last_mission_note = "Recovered %s. Max altitude %s, peak %.1f g%s." % [names, FlightHUD.fmt_dist(_mission.max_altitude),
		_mission.max_g, ", landed on Krill!" if _mission.krill_landings > 0 else ""]
	get_tree().change_scene_to_file("res://scenes/hangar.tscn")


func _craft_height() -> float:
	var r: PackedFloat64Array = session.core.vessel_render(session.active, session.active)
	var lo := INF
	var hi := -INF
	for k in r.size() / VesselView.RPP:
		var p := Frame.vec(r, k * VesselView.RPP + 1)
		lo = minf(lo, p.y)
		hi = maxf(hi, p.y)
	return hi - lo if hi > lo else 5.0


func _build() -> void:
	_env = Environment.new()
	_env.background_mode = Environment.BG_CANVAS
	_env.background_canvas_max_layer = -1
	_env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	_env.ambient_light_color = Color(0.5, 0.58, 0.7)
	_env.ambient_light_energy = 0.45
	# the canvas background is not a sky: never reflect it
	_env.reflected_light_source = Environment.REFLECTION_SOURCE_DISABLED
	_env.tonemap_mode = Environment.TONE_MAPPER_FILMIC
	_env.glow_enabled = true
	_env.fog_enabled = true
	_env.fog_light_color = SKY_DAY
	_env.fog_density = 0.0
	_env.fog_sky_affect = 0.0
	var we := WorldEnvironment.new()
	we.environment = _env
	add_child(we)

	_sun = DirectionalLight3D.new()
	_sun.light_energy = 1.3
	_sun.shadow_enabled = true
	_sun.directional_shadow_max_distance = 400.0
	add_child(_sun)

	_cam = Camera3D.new()
	_cam.near = 0.2
	_cam.far = 3.0e6
	_cam.fov = 60
	add_child(_cam)

	_terrain = TerrainView.new()
	_terrain.setup(session.core)
	add_child(_terrain)

	# scaled space as the background canvas
	var bg := CanvasLayer.new()
	bg.layer = -1
	add_child(bg)
	_scaled = ScaledSpace.new()
	_scaled.setup(session.core)
	add_child(_scaled)
	var tr := TextureRect.new()
	tr.set_anchors_preset(Control.PRESET_FULL_RECT)
	tr.texture = _scaled.get_texture()
	tr.stretch_mode = TextureRect.STRETCH_SCALE
	tr.mouse_filter = Control.MOUSE_FILTER_IGNORE
	bg.add_child(tr)

	var ui := CanvasLayer.new()
	ui.layer = 5
	add_child(ui)
	var map_layer := CanvasLayer.new()
	map_layer.layer = 3
	add_child(map_layer)
	_map = MapView.new()
	map_layer.add_child(_map)
	_map.setup(session)
	_map.visible = false
	hud = FlightHUD.new()
	ui.add_child(hud)
	hud.sas_pressed.connect(_on_sas)
	hud.menu_action.connect(_on_menu)


# ---- input --------------------------------------------------------------------

func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventMouseButton:
		var mb := event as InputEventMouseButton
		if mb.button_index == MOUSE_BUTTON_RIGHT:
			_orbiting = mb.pressed
		elif mb.button_index == MOUSE_BUTTON_WHEEL_UP and mb.pressed:
			_cam_dist = maxf(4.0, _cam_dist * 0.9)
		elif mb.button_index == MOUSE_BUTTON_WHEEL_DOWN and mb.pressed:
			_cam_dist = minf(5.0e4, _cam_dist * 1.1)
	elif event is InputEventMouseMotion and _orbiting:
		var mm := event as InputEventMouseMotion
		_cam_yaw -= mm.relative.x * 0.008
		_cam_pitch = clampf(_cam_pitch + mm.relative.y * 0.006, -1.45, 1.45)
	elif event is InputEventKey and event.pressed and not event.echo:
		_key(event as InputEventKey)


func _key(k: InputEventKey) -> void:
	if hud.menu_visible() and k.keycode != KEY_ESCAPE:
		return
	match k.keycode:
		KEY_ESCAPE:
			hud.show_menu(not hud.menu_visible())
		KEY_SPACE:
			_stage()
		KEY_Z:
			_throttle = 1.0
		KEY_X:
			_throttle = 0.0
		KEY_T:
			_on_sas("off" if session.telemetry().get("sas", "off") != "off" else "stability")
		KEY_G:
			_legs_down = not _legs_down
			session.set_legs(_legs_down)
			hud.toast("Legs " + ("down" if _legs_down else "up"), 1.5)
		KEY_V:
			hud.speed_mode = "orbit" if hud.speed_mode == "surface" else "surface"
		KEY_M:
			_map.visible = not _map.visible
			if _map.visible:
				_map.focus_body = int(session.telemetry().get("body_index", 1))
		KEY_TAB:
			if _map.visible:
				_map.cycle_focus()
		KEY_PERIOD:
			_change_warp(1)
		KEY_COMMA:
			_change_warp(-1)
		KEY_SLASH, KEY_BACKSLASH:
			session.set_warp("physics", 0)
		KEY_BRACKETLEFT:
			_cycle_vessel(-1)
		KEY_BRACKETRIGHT:
			_cycle_vessel(1)

var _legs_down := false


func _stage() -> void:
	var created := session.stage()
	_react("stage", 2.0)
	if not _launched:
		_launched = true
		mission_start = session.time()
		hud.toast("Liftoff! The penguins are going to space.", 3.0)
	elif not created.is_empty():
		hud.toast("Staging: separation confirmed", 1.5)


func _change_warp(dir: int) -> void:
	var t := session.telemetry()
	var kind := session.warp_kind
	var idx := session.warp_index + dir
	if session.warp_index == 0 and dir > 0:
		kind = "rails" if session.rails_refusal() == "" else "physics"
		idx = 1
	if kind == "physics":
		idx = clampi(idx, 0, FlightSession.PHYSICS_WARP.size() - 1)
	var reason := session.set_warp(kind, idx)
	if reason != "":
		hud.toast("Warp refused: " + reason, 2.0)


func _cycle_vessel(dir: int) -> void:
	var vessels: Array = JSON.parse_string(session.core.vessel_list())
	if vessels.size() < 2:
		return
	var ids := []
	for v in vessels:
		ids.append(int(v.id))
	ids.sort()
	var k := ids.find(session.active)
	session.switch_to(ids[(k + dir + ids.size()) % ids.size()])


func _on_sas(mode: String) -> void:
	_sas_maneuver = mode == "maneuver"
	if _sas_maneuver:
		if session.nodes.is_empty():
			_sas_maneuver = false
			hud.toast("No maneuver node", 1.5)
		return
	if not session.set_sas(mode):
		hud.toast("SAS mode unavailable", 1.5)


## Direction (sim axes) and size of the first upcoming node's burn.
func _node_burn() -> Dictionary:
	if session.nodes.is_empty():
		return {}
	var v: PackedFloat64Array = session.core.node_vector(session.active, FlightSession.pack_nodes(session.nodes), 0)
	var vec := Vector3(v[0], v[1], v[2])
	if vec.length() < 1e-6:
		return {}
	var nd: Array = session.nodes[0]
	return {"vec": vec, "dv": vec.length(), "t": nd[0]}


func _on_menu(action: String) -> void:
	hud.show_menu(false)
	match action:
		"revert_launch":
			_clear_views()
			session.revert_to_launch()
			_launched = false
			_recorded = false
			_throttle = 0.0
			_mission = {"max_altitude": 0.0, "max_g": 0.0, "orbits": 0, "krill_landings": 0, "visited": ["floe"], "pod_lost": false}
			_assign_crew()
			hud.toast("Reverted to launch. The penguins pretend nothing happened.", 2.5)
		"revert_hangar":
			get_tree().change_scene_to_file("res://scenes/hangar.tscn")
		"main_menu":
			_record("lost" if _mission.pod_lost else "recovered")
			get_tree().change_scene_to_file("res://scenes/main_menu.tscn")


func _on_sim_event(e: Dictionary) -> void:
	match e.kind:
		"part_destroyed":
			_react("boom", 3.5)
			if Sim.part(e.part).get("crew", 0) > 0:
				_mission.pod_lost = true
				hud.toast("PASA, we have a problem. (%s lost at %.0f m/s)" % [Sim.part(e.part).name, e.speed], 5.0)
			else:
				hud.toast("%s destroyed (%.0f m/s impact)" % [Sim.part(e.part).name, e.speed], 2.5)
		"soi_change":
			_react("orbit", 3.0)
			if not _mission.visited.has(str(e.get("body", ""))):
				_mission.visited.append(str(e.get("body", "")))
			hud.toast("Entering the sphere of influence of %s" % str(e.get("body", "?")).capitalize(), 4.0)
		"landed":
			if e.vessel == session.active and _launched:
				_react("landed", 5.0)
				var t := session.telemetry()
				if t.get("body", "") == "krill":
					_mission.krill_landings += 1
					hud.toast("The penguins have landed on Krill! One small waddle for a penguin...", 5.0)
				else:
					hud.toast("Touchdown! The crew is doing a little waddle.", 3.5)
		"warp_stopped":
			hud.toast("Warp stopped: %s ahead" % e.reason, 2.5)


# ---- frame -------------------------------------------------------------------

func _process(delta: float) -> void:
	if hud.menu_visible():
		return
	_controls(delta)
	session.advance(delta)
	_render(delta)


func _controls(delta: float) -> void:
	if Input.is_key_pressed(KEY_SHIFT):
		_throttle = minf(1.0, _throttle + THROTTLE_RATE * delta)
	if Input.is_key_pressed(KEY_CTRL):
		_throttle = maxf(0.0, _throttle - THROTTLE_RATE * delta)
	session.set_throttle(_throttle)
	var pitch := float(Input.is_key_pressed(KEY_S)) - float(Input.is_key_pressed(KEY_W))
	var yaw := float(Input.is_key_pressed(KEY_D)) - float(Input.is_key_pressed(KEY_A))
	var roll := float(Input.is_key_pressed(KEY_E)) - float(Input.is_key_pressed(KEY_Q))
	session.set_input(pitch, yaw, roll)
	if _sas_maneuver:
		var nb := _node_burn()
		if nb.is_empty():
			_sas_maneuver = false
		else:
			session.set_sas("target", PackedFloat64Array([nb.vec.x, nb.vec.y, nb.vec.z]))


func _clear_views() -> void:
	for id in _views:
		_views[id].queue_free()
	_views.clear()


func _render(delta: float) -> void:
	var t := session.telemetry()
	if not t.get("alive", false):
		hud.update_from(t, 0.0)
		return
	var core := session.core
	var origin := session.active
	_flicker = randf()
	# vessels
	var vessels: Array = JSON.parse_string(core.vessel_list())
	var alive := {}
	for v in vessels:
		var id := int(v.id)
		alive[id] = true
		if not _views.has(id):
			var vv := VesselView.new()
			vv.setup(core, id)
			add_child(vv)
			_views[id] = vv
		_views[id].refresh(origin, t.pressure, _flicker)
	for id in _views.keys():
		if not alive.has(id):
			_views[id].refresh(origin, 0.0, 0.0)  # flushes its parts into `vanished`
			_explode_vanished(_views[id].vanished)
			_views[id].queue_free()
			_views.erase(id)
	for id in _views:
		_explode_vanished(_views[id].vanished)

	# camera rig in the local horizon frame
	var up := Frame.vec_arr(t.up)
	var north := Frame.vec_arr(t.north)
	var east := Frame.vec_arr(t.east)
	# positive pitch puts the camera above the vessel, looking down at it
	var horiz := north * cos(_cam_yaw) + east * sin(_cam_yaw)
	_cam.position = (-horiz * cos(_cam_pitch) + up * sin(_cam_pitch)) * _cam_dist
	_cam.look_at(Vector3.ZERO, up)

	# bodies: sun direction, terrain for the current body
	var bodies: PackedFloat64Array = core.body_render(origin)
	var sun_rel := Frame.vec(bodies, 0)
	var sun_dir := sun_rel.normalized()
	_sun.look_at(-sun_dir * 1000.0, up if absf(sun_dir.dot(up)) < 0.99 else north)
	_sun.position = Vector3.ZERO
	var bi := int(t.body_index)
	_terrain.set_body(bi)
	var body_rel := Frame.vec(bodies, bi * 5)
	var eye: PackedFloat64Array = core.to_body_fixed(bi, origin, PackedFloat64Array(Frame.to_sim(_cam.position)))
	_terrain.refresh(eye, body_rel, bodies[bi * 5 + 3])

	# sky: atmosphere colour fading to space, dimmed on the night side
	var sky := Color(0, 0, 0)
	var atm_h: float = t.atmosphere_height
	var day := clampf(up.dot(sun_dir) * 3.0 + 0.4, 0.0, 1.0)
	if atm_h > 0.0 and t.altitude < atm_h:
		var k := pow(1.0 - clampf(t.altitude / atm_h, 0.0, 1.0), 1.6)
		sky = Color(0.01, 0.01, 0.03).lerp(SKY_DAY, k * day)
		_env.fog_light_color = sky
		_env.fog_density = 0.00006 * k
		_env.ambient_light_energy = 0.12 + 0.35 * k * day
	else:
		_env.fog_density = 0.0
		_env.ambient_light_energy = 0.08  # airless: hard shadows
	_sun.light_energy = 0.15 + 1.2 * clampf(up.dot(sun_dir) * 4.0 + 0.6, 0.0, 1.0) if t.altitude < 20000.0 else 1.3
	_scaled.sync(_cam, origin, _cam.position, sun_dir, sky)

	# HUD
	var nose := Frame.vec_arr(t.nose)
	var q_top := _vessel_top(origin)
	var markers := []
	var modes := ["prograde", "retrograde", "normal", "antinormal", "radial_out", "radial_in"] if hud.speed_mode == "orbit" \
		else ["surface_prograde", "surface_retrograde", "normal", "antinormal", "radial_out", "radial_in"]
	for m in modes:
		var d: PackedFloat64Array = core.sas_direction(origin, m)
		markers.append([Frame.vec(d, 0), m.replace("surface_", "")])
	var nb := _node_burn()
	if not nb.is_empty():
		markers.append([Frame.vec_arr([nb.vec.x, nb.vec.y, nb.vec.z]), "maneuver"])
		var bt: float = core.burn_time(origin, nb.dv)
		hud.set_node_text("NODE in %s · Δv %.1f m/s · burn %s" % [FlightHUD.fmt_time(nb.t - session.time()), nb.dv,
			FlightHUD.fmt_time(bt) if bt >= 0 else "—"])
	else:
		hud.set_node_text("")
	hud.navball.update_attitude(up, north, east, nose, q_top, markers)
	hud.maneuver_active = _sas_maneuver
	# crew and mission bookkeeping
	_mission.max_altitude = maxf(_mission.max_altitude, t.altitude)
	_mission.max_g = maxf(_mission.max_g, t.g_force)
	if t.situation == "orbiting" and not _was_orbiting and _launched:
		_mission.orbits += 1
		_react("orbit", 4.0)
		hud.toast("Orbit achieved! The crew presses their beaks to the window.", 4.0)
	_was_orbiting = t.situation == "orbiting"
	for p in _portraits:
		p.update_state(t, t.get("spin", 0.0), delta)
	_recover_btn.visible = _launched and t.landed and t.body == "floe" and t.surface_speed < 0.5 and _mission.max_altitude > 500.0
	if _map.visible:
		_map.refresh()
	var warp := ""
	if session.warp_index > 0:
		warp = "WARP %s%dx" % ["" if session.warp_kind == "rails" else "physics ",
			int(FlightSession.RAILS_WARP[session.warp_index]) if session.warp_kind == "rails" else FlightSession.PHYSICS_WARP[session.warp_index]]
	hud.set_warp_text(warp)
	hud.update_from(t, session.time() - mission_start if _launched else 0.0)


## The vessel's "top" axis (local -Z) for the navball's roll reference.
func _vessel_top(id: int) -> Vector3:
	var r: PackedFloat64Array = session.core.vessel_render(id, id)
	if r.size() < VesselView.RPP:
		return Vector3.UP
	# part 0 is the root; its rotation relative to the craft is usually identity
	return Frame.local_basis(r, 4) * Vector3(0, 0, -1)


## Parts that left a view without reappearing in another were destroyed.
func _explode_vanished(gone: Dictionary) -> void:
	for uid in gone:
		var moved := false
		for id in _views:
			if _views[id].has_uid(uid):
				moved = true
				break
		if not moved:
			_explosion(gone[uid])


func _explosion(at: Vector3) -> void:
	var root := Node3D.new()
	root.position = at
	add_child(root)
	var ball := MeshInstance3D.new()
	var sm := SphereMesh.new()
	sm.radius = 1.0
	sm.height = 2.0
	ball.mesh = sm
	var m := StandardMaterial3D.new()
	m.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	m.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	m.blend_mode = BaseMaterial3D.BLEND_MODE_ADD
	m.albedo_color = Color(1.0, 0.6, 0.2, 0.9)
	ball.material_override = m
	root.add_child(ball)
	var light := OmniLight3D.new()
	light.light_color = Color(1.0, 0.6, 0.3)
	light.omni_range = 40.0
	light.light_energy = 8.0
	root.add_child(light)
	var tw := create_tween().set_parallel(true)
	tw.tween_property(ball, "scale", Vector3.ONE * 7.0, 0.7)
	tw.tween_property(m, "albedo_color:a", 0.0, 0.7)
	tw.tween_property(light, "light_energy", 0.0, 0.7)
	tw.chain().tween_callback(root.queue_free)
