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
var _clouds: MeshInstance3D  # real-scale cloud deck, drawn over the near terrain
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
	_roster_snapshot = Crew.crew.duplicate(true)
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
	_env.tonemap_mode = Environment.TONE_MAPPER_AGX
	_env.tonemap_exposure = 0.95
	_env.ssao_enabled = true
	_env.ssao_radius = 0.8
	_env.ssao_intensity = 1.2
	_env.ssao_detail = 0.3
	_env.ssao_horizon = 0.12
	_env.ssao_sharpness = 0.98
	# only near contact shading: SSAO on distant grazing terrain makes stripes
	_env.ssao_light_affect = 0.0
	_env.ssao_ao_channel_affect = 0.0
	_env.glow_enabled = true
	_env.fog_enabled = true
	_env.fog_light_color = SKY_DAY
	_env.fog_density = 0.0
	_env.fog_sky_affect = 0.0
	# far terrain takes on the haze, like real air does
	_env.fog_height_density = 0.0
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

	_clouds = MeshInstance3D.new()
	var cm := SphereMesh.new()
	cm.radius = 1.0
	cm.height = 2.0
	cm.radial_segments = 256
	cm.rings = 128
	_clouds.mesh = cm
	_clouds.material_override = PlanetBake.cloud_material()
	_clouds.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	_clouds.visible = false
	add_child(_clouds)

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
	hud.warp_pressed.connect(func(dir: int):
		if dir == 0:
			session.set_warp("physics", 0)
		else:
			_change_warp(dir))
	hud.throttle_set.connect(func(x: float): _throttle = x)
	hud.action_pressed.connect(_on_action)


func _on_action(a: String) -> void:
	match a:
		"stage":
			_stage()
		"chute":
			_deploy_chutes()
		"legs":
			_toggle_legs()


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
		_cam_yaw += mm.relative.x * 0.008
		_cam_pitch = clampf(_cam_pitch + mm.relative.y * 0.006, -1.45, 1.45)
	elif event is InputEventKey and event.pressed and not event.echo:
		_key(event as InputEventKey)


func _key(k: InputEventKey) -> void:
	if hud.menu_visible() and k.keycode != KEY_ESCAPE:
		return
	if k.keycode != KEY_F1:
		hud.hide_help()
	match k.keycode:
		KEY_F1:
			hud.toggle_help()
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
		KEY_P:
			_deploy_chutes()
		KEY_G:
			_toggle_legs()
		KEY_V:
			hud.speed_mode = "orbit" if hud.speed_mode == "surface" else "surface"
		KEY_M:
			_map.visible = not _map.visible
			if _map.visible:
				_map.focus_on_vessel()
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


func _toggle_legs() -> void:
	_legs_down = not _legs_down
	session.set_legs(_legs_down)
	hud.toast("Legs " + ("down" if _legs_down else "up"), 1.5)


func _stage() -> void:
	var t0 := session.telemetry()
	if not t0.get("alive", false):
		return
	if int(t0.next_stage) < 0:
		hud.toast("No stages left. (P deploys parachutes)", 2.0)
		Sfx.play("click")
		return
	var lit0 := int(t0.get("engines_lit", 0))
	var was_clamped: bool = t0.get("clamped", false)
	var created := session.stage()
	var t1 := session.telemetry()
	var lit := int(t1.get("engines_lit", 0)) > lit0
	_react("stage", 2.0)
	# each sound is something that actually happened
	if not created.is_empty():
		Sfx.play("stage")
	if lit:
		Sfx.play("ignite")
	if created.is_empty() and not lit:
		Sfx.play("decouple" if was_clamped else "click")
	if not _launched:
		_launched = true
		mission_start = session.time()
		if t1.get("thrust", 0.0) > 0.0:
			hud.toast("Liftoff! The penguins are going to space.", 3.0)
		else:
			hud.toast("Clamps released, but no engine is burning! (Z for full throttle)", 3.5)
	elif not created.is_empty():
		hud.toast("Staging: separation confirmed", 1.5)
	elif lit:
		hud.toast("Ignition", 1.2)


func _deploy_chutes() -> void:
	var t := session.telemetry()
	if int(t.get("chutes", 0)) == 0:
		hud.toast("This vessel has no parachute", 2.0)
		return
	var n := session.deploy_chutes()
	if n == 0:
		hud.toast("Parachutes already deployed", 1.5)
		return
	Sfx.play("click")
	hud.toast("Parachute armed: it opens once the air is thick enough and you are slow enough", 3.0)


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
			_restore_crew()
			_fail_in = 0.0
			hud.hide_failure()
			_clear_views()
			session.revert_to_launch()
			_launched = false
			_recorded = false
			_throttle = 0.0
			_wreck_set = false
			_mission = {"max_altitude": 0.0, "max_g": 0.0, "orbits": 0, "krill_landings": 0, "visited": ["floe"], "pod_lost": false}
			_assign_crew()
			hud.toast("Reverted to launch. The penguins pretend nothing happened.", 2.5)
		"revert_hangar":
			_restore_crew()
			get_tree().change_scene_to_file("res://scenes/hangar.tscn")
		"main_menu":
			_record("lost" if _mission.pod_lost else "recovered")
			get_tree().change_scene_to_file("res://scenes/main_menu.tscn")


func _on_sim_event(e: Dictionary) -> void:
	match e.kind:
		"part_destroyed":
			_react("boom", 3.5)
			var how: String = "by overheating" if float(e.speed) <= -2.0 else "in the blast" if float(e.speed) < 0.0 else "at %.0f m/s" % float(e.speed)
			if Sim.part(e.part).get("crew", 0) > 0:
				_crew_lost(Sim.part(e.part).name, how)
			elif not _mission.pod_lost:
				hud.toast("%s destroyed %s" % [Sim.part(e.part).name, how], 2.5)
		"soi_change":
			_react("orbit", 3.0)
			if not _mission.visited.has(str(e.get("body", ""))):
				_mission.visited.append(str(e.get("body", "")))
			hud.toast("Entering the sphere of influence of %s" % str(e.get("body", "?")).capitalize(), 4.0)
		"landed":
			if e.vessel == session.active and _launched:
				_react("landed", 5.0)
				Sfx.play("touchdown")
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
	# steering is relative to the view: W/Up tips the nose away from the
	# camera (or down the screen), D/Right swings it to the screen's right
	var away := float(Input.is_key_pressed(KEY_W) or Input.is_key_pressed(KEY_UP)) - float(Input.is_key_pressed(KEY_S) or Input.is_key_pressed(KEY_DOWN))
	var right := float(Input.is_key_pressed(KEY_D) or Input.is_key_pressed(KEY_RIGHT)) - float(Input.is_key_pressed(KEY_A) or Input.is_key_pressed(KEY_LEFT))
	var roll := float(Input.is_key_pressed(KEY_E)) - float(Input.is_key_pressed(KEY_Q))
	var pr := _view_steer(away, right)
	session.set_input(pr.x, pr.y, roll)
	if _sas_maneuver:
		var nb := _node_burn()
		if nb.is_empty():
			_sas_maneuver = false
		else:
			session.set_sas("target", PackedFloat64Array([nb.vec.x, nb.vec.y, nb.vec.z]))


## Turns screen-relative steering into the vessel's pitch (local X torque)
## and yaw (local Z torque) inputs.
func _view_steer(away: float, right: float) -> Vector2:
	if away == 0.0 and right == 0.0:
		return Vector2.ZERO
	var t := session.telemetry()
	if not t.get("alive", false) or not t.has("side"):
		return Vector2.ZERO
	var n := Frame.vec_arr(t.nose).normalized()
	var x := Frame.vec_arr(t.side).normalized()
	var z := x.cross(n)  # vessel local +Z (right-handed, Y = nose)
	var cr := _cam.global_basis.x.normalized()
	var cu := _cam.global_basis.y.normalized()
	var cf := -_cam.global_basis.z.normalized()
	var perp := func(v: Vector3) -> Vector3: return v - n * v.dot(n)
	# where each key wants the nose to go: "away" heads into the screen,
	# blending to down-screen as the nose already points away (and to
	# up-screen when it points at the camera); "right" is screen-right
	var fd := n.dot(cf)
	var m_away: Vector3 = perp.call(cf) + perp.call(-cu) * smoothstep(0.5, 0.95, fd) * 2.0 \
		+ perp.call(cu) * smoothstep(0.5, 0.95, -fd) * 2.0
	var m_right: Vector3 = perp.call(cr)
	# a torque about axis A moves the nose along A x nose, so A = nose x m
	var a_away := n.cross(m_away)
	var a_right := n.cross(m_right)
	a_away /= maxf(a_away.length(), 0.3)
	a_right /= maxf(a_right.length(), 0.3)
	var axis := a_away * away + a_right * right
	return Vector2(clampf(axis.dot(x), -1.0, 1.0), clampf(axis.dot(z), -1.0, 1.0))


## Drops every vessel visual (the scene is about to show a new situation),
## and any toast about the old one, like "on the pad" after a respawn.
func _clear_views() -> void:
	hud.clear_toast()
	for id in _views:
		_views[id].queue_free()
	_views.clear()


func _render(delta: float) -> void:
	var t := session.telemetry()
	if not t.get("alive", false):
		# nothing left to follow: every remaining visual goes up where it was
		for id in _views.keys():
			_views[id].refresh(-1, 0.0, 0.0)
			_explode_vanished(_views[id].vanished)
			_views[id].queue_free()
		_views.clear()
		_update_booms(delta, Vector3.ZERO)
		_update_failure(delta)
		hud.update_from(t, 0.0)
		return
	var core := session.core
	var origin := session.active
	_flicker = randf()
	# vessels
	var vessels: Array = JSON.parse_string(core.vessel_list())
	var prev_anchor := {}
	for id in _views:
		prev_anchor[id] = _views[id].anchor
	var alive := {}
	for v in vessels:
		var id := int(v.id)
		alive[id] = true
		if not _views.has(id):
			var vv := VesselView.new()
			vv.setup(core, id)
			add_child(vv)
			_views[id] = vv
		_views[id].air_vel = Frame.vec_arr(t.surface_velocity) if id == session.active else Vector3.ZERO
		_views[id].refresh(origin, t.pressure, _flicker, delta)
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
	_last_up = up
	var north := Frame.vec_arr(t.north)
	var east := Frame.vec_arr(t.east)
	# positive pitch puts the camera above the vessel, looking down at it
	var horiz := north * cos(_cam_yaw) + east * sin(_cam_yaw)
	var drift := -Frame.vec_arr(t.surface_velocity) * delta
	var focus := Vector3.ZERO
	if _wreck_set:
		_wreck += drift
		focus = _wreck
	_cam.position = focus + (-horiz * cos(_cam_pitch) + up * sin(_cam_pitch)) * _cam_dist
	_cam.look_at(focus, up)
	_update_booms(delta, drift)
	_update_failure(delta)

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
	# the body centre stays in doubles: the ground near us must not jitter
	_terrain.refresh(eye, [bodies[bi * 5], bodies[bi * 5 + 2], -bodies[bi * 5 + 1]], bodies[bi * 5 + 3])
	# cloud deck 7 km up on bodies that have weather; from below it would be
	# an overcast we don't model yet, so it shows once you're above it
	var deck: float = 7000.0
	var has_clouds: bool = t.body == "floe"
	var above: float = t.altitude - deck
	_clouds.visible = has_clouds and above > 2000.0
	_terrain.set_cloud_shadows(has_clouds, Frame.spin(bodies[bi * 5 + 3]).inverse() * sun_dir, deck / bodies[bi * 5 + 4])
	if _clouds.visible:
		var R: float = bodies[bi * 5 + 4]
		var spin := Frame.spin(bodies[bi * 5 + 3])
		var op := clampf((above - 2000.0) / 15000.0, 0.0, 1.0)
		_clouds.transform = Transform3D(spin.scaled(Vector3.ONE * (R + deck)), body_rel)
		(_clouds.material_override as ShaderMaterial).set_shader_parameter("opacity", op)


	# sky: atmosphere density fades to space; the sky shader handles day/night
	var density := 0.0
	var atm_h: float = t.atmosphere_height
	var day := clampf(up.dot(sun_dir) * 3.0 + 0.4, 0.0, 1.0)
	if atm_h > 0.0 and t.altitude < atm_h:
		density = pow(1.0 - clampf(t.altitude / atm_h, 0.0, 1.0), 1.6)
		_env.fog_light_color = Color(0.62, 0.76, 0.93).lerp(Color(0.02, 0.02, 0.05), 1.0 - day)
		_env.fog_density = 0.00003 * density
		_env.ambient_light_color = Color(0.55, 0.65, 0.85)
		_env.ambient_light_energy = 0.1 + 0.4 * density * day
	else:
		_env.fog_density = 0.0
		_env.ambient_light_energy = 0.08  # airless: hard shadows
	_sun.light_energy = (0.15 + 1.2 * clampf(up.dot(sun_dir) * 4.0 + 0.6, 0.0, 1.0)) if t.altitude < 20000.0 else 1.3
	_scaled.sync(_cam, origin, _cam.position, sun_dir, density, up)

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
		Sfx.play("chime")
		_react("orbit", 4.0)
		hud.toast("Orbit achieved! The crew presses their beaks to the window.", 4.0)
	_was_orbiting = t.situation == "orbiting"
	for p in _portraits:
		p.update_state(t, t.get("spin", 0.0), delta)
	_update_audio(t)
	if _smoke == null:
		_smoke = SmokeTrail.new()
		add_child(_smoke)
	if origin != _smoke_origin:
		# the floating origin moved to another vessel: carry what hangs in
		# the air across by how far that vessel's render position jumped
		if prev_anchor.has(origin) and _views.has(origin):
			var jump: Vector3 = _views[origin].anchor - prev_anchor[origin]
			_smoke.shift(jump)
			for b in _booms:
				if is_instance_valid(b):
					b.position += jump
			_wreck += jump
		else:
			_smoke.clear()
		_smoke_origin = origin
	var nozzles := []
	for id in _views:
		nozzles.append_array(_views[id].nozzles())
	var day_light := clampf(up.dot(sun_dir) * 3.0 + 0.35, 0.12, 1.0)
	# smoke lives in sim time: under physics warp the rocket covers more air
	# per frame, and the trail must still be laid down continuously
	var sim_dt := clampf(session.time() - _smoke_t, 0.0, 0.5)
	_smoke_t = session.time()
	_smoke.step(sim_dt, nozzles, Frame.vec_arr(t.surface_velocity), t.pressure / 101325.0, day_light, up, t.altitude_terrain)
	if _reentry == null:
		_reentry = ReentryFX.new()
		add_child(_reentry)
	if session.ticks % 50 == 1 or _craft_r <= 0.0:
		_craft_r = maxf(1.5, _craft_height() * 0.5)
	_reentry.update_fx(Frame.vec_arr(t.surface_velocity), t.density, _craft_r, delta)
	Sfx.wind_level = maxf(Sfx.wind_level, _reentry.level)
	var hint := ""
	if int(t.get("chutes_stowed", 0)) > 0 and _launched and t.in_atmosphere and not t.landed \
			and t.vertical_speed < -20.0 and t.altitude_terrain < 12000.0:
		hint = "Falling with the parachute packed: press P to deploy it"
	var hf: float = t.get("heat_frac", 0.0)
	if hf > 0.8 and not t.landed:
		hint = "%s at %.0f K of %.0f K: overheating! Turn the heat shield (or the strongest end) into the flow, or climb" % [
			Sim.part(t.heat_part).get("name", "A part"), t.heat_temp, t.heat_limit]
		if hf > 0.9 and session.time() - _heat_alarm_t > 1.2:
			_heat_alarm_t = session.time()
			Sfx.play("alarm", 0.6)
	hud.set_hint(hint)
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
			_explosion(gone[uid].pos, gone[uid].part)


func _explosion(at: Vector3, part_id: String) -> void:
	var d: Dictionary = Sim.part(part_id)
	if int(d.get("crew", 0)) > 0 and not _wreck_set:
		# the camera stays with where the crew was lost, not with debris
		_wreck = at
		_wreck_set = true
	# propellant makes the bang: a full tank is a bomb, a fin is a pop
	var mass: float = d.get("mass", 100.0) + d.get("fuel_liquid", 0.0) + d.get("fuel_solid", 0.0)
	var size := clampf(1.2 * pow(mass / 100.0, 1.0 / 3.0), 1.5, 9.0)
	for b in _booms:
		if is_instance_valid(b) and b.young() and b.position.distance_to(at) < b.size * 1.6:
			b.merge(size)
			if _smoke:
				_smoke.burst(at, size * 0.7, _last_up)
			return
	var up := Frame.vec_arr(session.telemetry().get("up", [0.0, 0.0, 1.0])) if session.telemetry().get("alive", false) else _last_up
	var ex := Explosion.new(size, up)
	ex.position = at
	add_child(ex)
	_booms.append(ex)
	if _smoke:
		_smoke.burst(at, size, up)
	var dist := at.distance_to(_cam.position if _cam else Vector3.ZERO)
	_shake = maxf(_shake, clampf(size * 6.0 / (dist + 8.0), 0.0, 2.5))
	Sfx.play("boom", clampf(size / 5.0 * 30.0 / (dist * 0.05 + 30.0), 0.3, 1.8))


## The crew pod is gone. PASA does not get this one back.
func _crew_lost(part_name: String, how: String) -> void:
	if _mission.pod_lost:
		return
	_mission.pod_lost = true
	var names := ", ".join(crew_names) if not crew_names.is_empty() else "the crew"
	if _launched and not crew_names.is_empty():
		Crew.record_loss(crew_names, _mission)
		_recorded = true
	for p in _portraits:
		p.set_lost()
	Sfx.play("alarm")
	hud.toast("PASA, we have a problem...", 2.5)
	_fail_text = ["MISSION LOST", "%s was destroyed %s.\n%s did not survive." % [part_name, how, names],
		"Revert to launch to try again: it will be as if it never happened."]
	_fail_in = 3.0


func _restore_crew() -> void:
	if not _roster_snapshot.is_empty():
		Crew.crew = _roster_snapshot.duplicate(true)
		Crew.save_roster()
	_recorded = false


## Explosions hang in the air (or sit on the ground) while we move on;
## big ones shake the camera.
func _update_booms(delta: float, drift: Vector3) -> void:
	for k in range(_booms.size() - 1, -1, -1):
		if not is_instance_valid(_booms[k]):
			_booms.remove_at(k)
		else:
			_booms[k].position += drift
	if _shake > 0.01:
		_cam.position += Vector3(randf_range(-1, 1), randf_range(-1, 1), randf_range(-1, 1)) * _shake * 0.35
		_shake *= exp(-delta * 2.5)


## The mission-lost panel waits a moment so the player sees what happened.
func _update_failure(delta: float) -> void:
	if _fail_in > 0.0:
		_fail_in -= delta
		if _fail_in <= 0.0:
			hud.show_failure(_fail_text[0], _fail_text[1], _fail_text[2])


var _last_up := Vector3.UP
var _fail_in := 0.0
var _heat_alarm_t := -100.0
var _smoke_t := 0.0
var _wreck := Vector3.ZERO
var _wreck_set := false
var _fail_text: Array = []
var _booms: Array[Explosion] = []
var _shake := 0.0
var _roster_snapshot: Array = []


var _smoke: SmokeTrail
var _smoke_origin := -1
var _chutes_open := 0
var _reentry: ReentryFX
var _craft_r := 5.0


func _update_audio(t: Dictionary) -> void:
	if hud.menu_visible():
		Sfx.silence_beds()
		return
	var tmax: float = t.get("thrust_max", 0.0)
	var frac: float = t.get("thrust", 0.0) / tmax if tmax > 0.0 else 0.0
	var solid_frac := 0.0
	if t.get("solid", 0.0) > 0.0 and frac > 0.0:
		solid_frac = 0.6
	# the rumble is about thrust, scaled by how big the engines are
	var size := clampf(tmax / 400000.0, 0.35, 1.0)
	Sfx.set_flight(frac * size, solid_frac, t.pressure / 101325.0, t.dynamic_pressure / 1000.0)
	# a canopy opening is the moment the drag-feed shows a new chute state
	var open := 0
	var feed: PackedFloat64Array = session.core.vessel_render(session.active, session.active)
	for k in feed.size() / VesselView.RPP:
		if int(feed[k * VesselView.RPP + 9]) >= 2:
			open += 1
	if open > _chutes_open:
		Sfx.play("chute")
	_chutes_open = open
