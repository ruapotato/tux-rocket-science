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
const CAM_CLEARANCE := 2.5  # m the camera keeps above the ground
const CAM_MAX_DIST := 1000.0  # m: further out a rocket is a speck (the map shows the big picture)
const LOW_WARP_ALT := 3000.0  # m above the ground where a descent drops out of warp
var _was_in_air := true
var _was_low := false
var _slowing_t := 0.0
var _orbiting := false
var _terrain: TerrainView
var _sky_body := -1   # the body whose sky colours are set
var _sun_star := -2   # the star lighting the scene (its colour is set when it changes)
var _clouds: MeshInstance3D  # real-scale cloud deck, drawn over the near terrain
var _scaled: ScaledSpace
var _views := {}  # vessel id -> VesselView
var _env: Environment
var _sun: DirectionalLight3D
var _throttle := 0.0
var _launched := false
## Did the pilot fly it (a flying key, a SAS button, a stage by hand) since
## the launch? A hands-off contract wants the computer to have flown it.
var hands_used := false
var _pending_toast := ""
var _flicker := 0.0
var _map: MapView
var _sas_follow := ""  # SAS chasing a moving direction: maneuver, target, antirel, dock
var crew_names: Array = []
var _portraits: Array[PenguinPortrait] = []
var _mission := {"max_altitude": 0.0, "max_g": 0.0, "orbits": 0, "krill_landings": 0, "visited": ["floe"], "pod_lost": false}
var _was_orbiting := false
var _recover_btn: Button
var _recorded := false
var tutorial: FirstFlight


func _ready() -> void:
	# a new flight: the last one's scripts stop (they'd fly this ship
	# otherwise); the console keeps its variables
	var stopped := ShipComputer.get_shared().stop_scripts()
	if stopped > 0:
		_pending_toast = "The ship's computer stopped %d script%s from the last flight" % [stopped, "" if stopped == 1 else "s"]
	var text := Sim.pending_craft_text
	if text == "":
		text = FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "orbiter.craft.json")
	Sim.pending_craft_text = text
	_roster_snapshot = Crew.crew.duplicate(true)
	var cleared := []
	if Sim.tutorial:
		# the walkthrough flies alone, in a world of its own
		session = FlightSession.new()
		session.launch_site = "pad"
		session.launch(text, "floe")
	else:
		persistent = true
		session = Sim.game_session()
		if Sim.pending_vessel >= 0 and session.core.vessel_alive(Sim.pending_vessel):
			session.switch_to(Sim.pending_vessel)
			_resumed = true
			_launched = true
		else:
			session.launch_site = Sim.pending_site
			steer_craft = session.launch_site == "runway"  # planes fly by their own axes
			Sim.pending_site = "pad"  # the next launch from elsewhere starts on the pad
			cleared = _clear_pad()
			_seats_snapshot = Sim.seats.duplicate(true)
			_roster_snapshot = Crew.crew.duplicate(true)
			_career_snapshot = Career.state().duplicate(true)
			_launch_cost = Career.craft_cost(Career.craft_parts(text)) if Career.is_career() else 0.0
			Career.charge_launch(Career.craft_parts(text))
			session.launch(text, "floe", false)
		Sim.pending_vessel = -1
	session.sim_event.connect(_on_sim_event)
	session.vessel_switched.connect(func(_id):
		# each craft keeps its own throttle and steering mode
		_throttle = float(session.telemetry().get("throttle", 0.0))
		_sas_follow = ""
		_cam_dist = _framing_dist()
		_refresh_crew())
	var cs := get_node_or_null("/root/ControlSocket")
	if cs:
		cs.attach(session)
	_build()
	_assign_crew()
	_cam_dist = _framing_dist()
	if persistent:
		Sim.save_game()
	if Sim.tutorial:
		_start_tutorial()
	elif _resumed:
		var tr := session.telemetry()
		# the mission clock runs from its liftoff, however long ago
		mission_start = float(tr.liftoff_t) if tr.get("liftoff_t") != null else session.time()
		hud.toast("Back aboard %s: %s over %s." % [tr.get("name", "the vessel"), str(tr.get("situation", "")).replace("_", " "), str(tr.get("body", "")).capitalize()], 4.0)
	elif session.launch_site == "orbit":
		_launched = true
		mission_start = session.time()
		hud.toast("PASA, %s is in the Orbital Yard, %d km over Floe. Enter takes you inside%s." % [session.telemetry().get("name", "vessel"),
			int(FlightSession.YARD_ALT / 1000.0), ", Home spins it up" if session.telemetry().has("spin_design") else ""], 6.0)
	elif session.launch_site == "runway":
		hud.toast("%sPASA, %s is on the runway, brakes on. SPACE starts the engine, Z full throttle, B lets the brakes off." % [
			("Cleared the runway first: recovered %s. " % ", ".join(cleared)) if not cleared.is_empty() else "", session.telemetry().get("name", "vessel")], 6.0)
	else:
		hud.toast("%sPASA, %s is on the pad. Z for full throttle, SPACE to launch." % [
			("Cleared the pad first: recovered %s. " % ", ".join(cleared)) if not cleared.is_empty() else "", session.telemetry().get("name", "vessel")], 5.0)


## Anything sitting on the pad is recovered before a launch.
func _clear_pad() -> Array:
	var names := []
	for v in session.pad_blockers("floe"):
		names.append(v.name)
		Sim.recover_vessel(int(v.id), {})
	return names


## The First Flight walkthrough, above the map so it stays readable there.
func _start_tutorial() -> void:
	if tutorial:
		tutorial.get_parent().queue_free()
	var layer := CanvasLayer.new()
	layer.layer = 4
	add_child(layer)
	tutorial = FirstFlight.new()
	layer.add_child(tutorial)
	tutorial.setup(self)
	hud.hide_help()


func _assign_crew() -> void:
	for p in _portraits:
		p.queue_free()
	_portraits.clear()
	var seats := int(session.telemetry().get("crew_capacity", 0))
	if _resumed:
		crew_names = Sim.crew_of(session.active)
	else:
		crew_names = Crew.assign(seats, Sim.pending_crew)
		if persistent:
			Sim.seat_crew(session.active, crew_names)
	_show_crew()
	if _recover_btn == null:
		_recover_btn = Button.new()
		_recover_btn.text = "  Recover vessel  "
		_recover_btn.visible = false
		_recover_btn.focus_mode = Control.FOCUS_NONE
		_recover_btn.set_anchors_and_offsets_preset(Control.PRESET_CENTER_TOP)
		_recover_btn.position.y = 112  # just under the clock panel
		_recover_btn.pressed.connect(_recover)
		hud.add_child(_recover_btn)


## Portraits of whoever is aboard the vessel we fly now.
func _show_crew() -> void:
	for p in _portraits:
		p.queue_free()
	_portraits.clear()
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


## In the shared world the crew shown are the ones aboard the vessel now
## flown: after switching, docking or undocking.
func _refresh_crew() -> void:
	if persistent and not _mission.pod_lost:
		var now := Sim.crew_of(session.active)
		if now != crew_names:
			crew_names = now
			_show_crew()


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
	if persistent:
		crew_names = Sim.recover_vessel(session.active, _mission)
		_recorded = true
		Sim.save_game()
	else:
		_record("recovered")
	var names := ", ".join(crew_names) if not crew_names.is_empty() else "the uncrewed craft"
	var lr: Dictionary = Sim.last_recovery if persistent else {}
	var gain := ""
	if int(lr.get("results", 0)) > 0:
		gain += " %d results: +%.1f research." % [int(lr.results), float(lr.research)]
	if float(lr.get("funds", 0.0)) > 0.0:
		gain += " Parts refunded %s." % Career.money(float(lr.funds))
	Sim.last_mission_note = "Recovered %s. Max altitude %s, peak %.1f g%s.%s" % [names, FlightHUD.fmt_dist(_mission.max_altitude),
		_mission.max_g, ", landed on Krill!" if _mission.krill_landings > 0 else "", gain]
	get_tree().change_scene_to_file("res://scenes/hangar.tscn")


## Why the pilot can't leave this flight for the Space Center now ("" = can).
## Time stands still while nobody watches, but the next launch runs the
## clock, and a craft left falling through the air would be lost.
func leave_refusal() -> String:
	if not persistent:
		return "The tutorial flight can't be left running."
	var t := session.telemetry()
	if not t.get("alive", false):
		return ""
	if t.get("in_atmosphere", false) and not t.get("landed", false) and t.situation != "prelaunch":
		return "Still flying through the air: land, splash down or reach space first."
	return ""


## Leaves the vessel where it is (it stays in the world) and goes to the
## Space Center.
func _leave() -> void:
	session.set_throttle(0.0)
	session.set_translate(0.0, 0.0, 0.0)
	session.set_input(0.0, 0.0, 0.0)
	session.set_warp("physics", 0)
	Sim.sync_crew(_mission)
	Sim.save_game()
	var t := session.telemetry()
	Sim.last_mission_note = "%s left %s over %s." % [t.get("name", "The vessel"), str(t.get("situation", "")).replace("_", " "), str(t.get("body", "")).capitalize()] if t.get("alive", false) else ""
	get_tree().change_scene_to_file("res://scenes/main_menu.tscn")


## Where the active vessel meets the oncoming air. Each part's leading
## point (its cylinder's furthest reach into the air) counts as exposed
## unless another part sits in front of it, covering it: a nose meets the
## air first and shields the tanks behind it, while a side booster's cone
## stands out in the stream on its own. Returns {"fronts": [[point,
## radius], ...] exposed tips, most windward first, "exposure": {uid: 0 or
## 1}}. flow: the direction the air moves past the vessel (render space).
func _windward_fronts(flow: Vector3) -> Dictionary:
	var r: PackedFloat64Array = session.core.vessel_render(session.active, session.active)
	var n := r.size() / VesselView.RPP
	var key := "%d/%d/%d" % [session.active, session._active_serial, n]
	if _front_ids.size() != n or key != _front_key:
		_front_key = key
		_front_ids = []
		var ids: Array = JSON.parse_string(session.core.vessel_parts(session.active))
		for d in ids:
			var sh: Dictionary = Sim.part(d.part).get("shape", {})
			var rad: float = sh.get("radius", sh.get("radius_bottom", 0.5 if sh.get("type", "") != "fin" else 0.05))
			var sz: Array = d.get("size", [1.0, 1.0, 1.0])
			_front_ids.append([rad * (float(sz[0]) + float(sz[2])) * 0.5, CraftModel.part_height(d.part, sz) * 0.5])
	var into := -flow
	var pos := []
	var reach := []
	var side := []  # how far each part spreads across the flow
	for k in n:
		var p := Frame.vec(r, k * VesselView.RPP + 1)
		var axis := Frame.local_basis(r, k * VesselView.RPP + 4) * Vector3.UP
		var rad: float = _front_ids[k][0]
		var hh: float = _front_ids[k][1]
		var c := absf(axis.dot(into))
		var s := sqrt(maxf(0.0, 1.0 - c * c))
		pos.append(p)
		reach.append(hh * c + rad * s)
		side.append(maxf(rad, hh * s))
	var fronts := []
	var exposure := {}
	for k in n:
		var f: Vector3 = pos[k] + into * float(reach[k])
		var covered := false
		for j in n:
			if j == k:
				continue
			var rel: Vector3 = f - pos[j]
			var ahead: float = -rel.dot(into) + float(reach[j])  # j's front beyond f
			if ahead > 0.02 and (rel - into * rel.dot(into)).length() < float(side[j]) * 0.95:
				covered = true
				break
		exposure[int(r[k * VesselView.RPP])] = 0.0 if covered else 1.0
		if not covered and float(_front_ids[k][0]) >= 0.2:
			fronts.append([f.dot(into), f, float(_front_ids[k][0])])
	fronts.sort_custom(func(a, b): return a[0] > b[0])
	# one trail per tip: drop tips within a hull radius of a stronger one
	var out := []
	for fr in fronts:
		var near := false
		for o in out:
			if (o[0] as Vector3).distance_to(fr[1]) < maxf(o[1], fr[2]):
				near = true
				break
		if not near:
			out.append([fr[1], fr[2]])
	return {"fronts": out, "exposure": exposure}


## Far enough that the whole craft fits between the top and bottom HUD;
## close in on a penguin.
func _framing_dist() -> float:
	if session.telemetry().get("eva", false):
		return 3.5
	var ch := _craft_height()
	return minf(CAM_MAX_DIST, maxf(18.0 + ch * 1.2, 2.0 * (ch + 3.0)))


## The craft's size: the widest span between two of its parts' centres
## (the feed is in the world frame, which is not lined up with the craft).
func _craft_height() -> float:
	var r: PackedFloat64Array = session.core.vessel_render(session.active, session.active)
	var pts: Array[Vector3] = []
	for k in r.size() / VesselView.RPP:
		pts.append(Frame.vec(r, k * VesselView.RPP + 1))
	var span := 0.0
	for i in pts.size():
		for j in range(i + 1, pts.size()):
			span = maxf(span, pts[i].distance_to(pts[j]))
	return span if span > 0.0 else 5.0


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
	_sun.light_cull_mask = 0xFFFFF & ~InteriorView.LAYER  # rooms are lit by their own lamps
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
	hud.sas_pressed.connect(func(mode): hands_used = true; _on_sas(mode))
	hud.menu_action.connect(_on_menu)
	hud.warp_pressed.connect(func(dir: int):
		if dir == 0:
			session.set_warp("physics", 0)
		else:
			_change_warp(dir))
	hud.throttle_set.connect(func(x: float): _throttle = x)
	hud.action_pressed.connect(func(a): hands_used = true; _on_action(a))
	Career.mission_done.connect(_on_mission_done)
	Career.relic_heard.connect(func(r: Dictionary):
		Sfx.play("click")
		hud.toast("Signal! The scanner hears %s, %s away, heading %03d°" % [r.name, FlightHUD.fmt_dist(r.distance), int(roundf(float(r.bearing_deg))) % 360], 4.0))
	Career.landmark_sighted.connect(func(lm: Dictionary):
		Sfx.play("click")
		hud.toast("Discovered: %s. %s  (+%.0f research)" % [lm.name, lm.get("text", ""), float(lm.research)], 9.0))
	Career.landmark_over.connect(func(lm: Dictionary): hud.toast("Below: %s" % lm.name, 3.0))
	Career.survey_done.connect(func(sv: Dictionary): hud.toast("Survey done: %s! +%.1f research%s" % [Career.describe_survey(sv), sv.research,
		" · +%s" % Career.money(float(sv.funds)) if Career.is_career() else ""], 4.0))


func _on_action(a: String) -> void:
	match a:
		"stage":
			_stage()
		"chute":
			_deploy_chutes()
		"science":
			_toggle_science()
		"legs":
			_toggle_legs()
		"rcs":
			_toggle_rcs()


# ---- input --------------------------------------------------------------------

## ~ (the key under Esc, without Shift) goes round the views from
## anywhere, the desktop too: it comes here before the desktop's own keys.
## With Shift it is a tilde, typed at the terminal.
func _input(event: InputEvent) -> void:
	if event is InputEventKey and event.pressed and not event.echo and (event as InputEventKey).keycode == KEY_QUOTELEFT \
			and not (event as InputEventKey).shift_pressed and not hud.menu_visible():
		_cycle_view()
		get_viewport().set_input_as_handled()


func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventMouseButton:
		var mb := event as InputEventMouseButton
		if mb.button_index == MOUSE_BUTTON_RIGHT:
			_orbiting = mb.pressed
		elif _inside and mb.pressed and mb.button_index == MOUSE_BUTTON_LEFT and _inside.screen_hit(_cam, mb.position):
			_toggle_desk()
		elif _inside and mb.pressed and mb.button_index in [MOUSE_BUTTON_WHEEL_UP, MOUSE_BUTTON_WHEEL_DOWN]:
			_in_dist = clampf(_in_dist * (0.9 if mb.button_index == MOUSE_BUTTON_WHEEL_UP else 1.1), 0.6, 8.0)
		elif mb.button_index == MOUSE_BUTTON_WHEEL_UP and mb.pressed:
			_cam_dist = maxf(1.5 if session.telemetry().get("eva", false) else 4.0, _cam_dist * 0.9)
		elif mb.button_index == MOUSE_BUTTON_WHEEL_DOWN and mb.pressed:
			_cam_dist = minf(CAM_MAX_DIST, _cam_dist * 1.1)
	elif event is InputEventMouseMotion and _orbiting and _inside:
		var mm := event as InputEventMouseMotion
		_in_yaw += mm.relative.x * 0.008
		_in_pitch = clampf(_in_pitch + mm.relative.y * 0.006, -1.4, 1.4)
		_in_looked = true
	elif event is InputEventMouseMotion and _orbiting:
		var mm := event as InputEventMouseMotion
		_cam_yaw += mm.relative.x * 0.008
		_cam_pitch = clampf(_cam_pitch + mm.relative.y * 0.006, -1.45, 1.45)
	elif event is InputEventKey and event.pressed and not event.echo:
		_key(event as InputEventKey)


func _key(k: InputEventKey) -> void:
	if k.keycode == KEY_F2:
		_toggle_desk()
		return
	if _desk_open:
		return  # the keyboard is the computer's while its desktop is up
	if hud.menu_visible() and k.keycode != KEY_ESCAPE:
		return
	if k.keycode != KEY_F1:
		hud.hide_help()
	if k.keycode in [KEY_Z, KEY_X, KEY_T, KEY_R, KEY_G, KEY_P, KEY_B, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9] \
			and not _inside and not session.telemetry().get("eva", false):
		hands_used = true   # (the pilot flying it: a hands-off contract no longer counts)
	match k.keycode:
		KEY_F1:
			hud.toggle_help()
		KEY_ESCAPE:
			hud.show_menu(not hud.menu_visible())
		KEY_ENTER, KEY_KP_ENTER:
			_toggle_inside()
		KEY_HOME:
			_toggle_spin()
		KEY_SPACE:
			if _inside:
				session.core.walker_command(session.active, PackedFloat64Array([0, 0, 0]), true)
			elif session.telemetry().get("eva", false):
				session.set_walk(0.0, 0.0, true)  # jump (the walk is set again next frame)
			elif computer != null and computer.wants_space():
				# a script is waiting for this press (wait_user_trigger): it's
				# the script's, not a stage
				computer.take_space()
				hud.toast("Space: the script goes on", 1.2)
				Sfx.play("click")
			else:
				hands_used = true
				_stage()
		KEY_F:
			_eva_key()
		KEY_B:
			_toggle_brakes()
		KEY_O:
			_toggle_science()
		KEY_C:
			_toggle_mining()
		KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9:
			_action_group(k.keycode - KEY_0)
		KEY_F3:
			steer_craft = not steer_craft
			hud.toast("Steering %s" % ("by the craft: W/S nose down / up, A/D yaw, Q/E roll" if steer_craft else "by the view: W tips the nose away from the camera"), 2.5)
		KEY_F5:
			quicksave()
		KEY_F9:
			quickload()
		KEY_Z:
			_hands_on()
			_throttle = 1.0
		KEY_X:
			_hands_on()
			_throttle = 0.0
		KEY_T:
			_on_sas("off" if session.telemetry().get("sas", "off") != "off" else "stability")
		KEY_P:
			_deploy_chutes()
		KEY_G:
			if session.telemetry().get("eva", false):
				_plant_flag()
			else:
				_toggle_legs()
		KEY_R:
			_toggle_rcs()
		KEY_Y:
			var tg := session.cycle_target()
			hud.toast("Target: %s" % session.target_info().get("name", "?") if tg >= 0 else "No target", 1.5)
		KEY_U:
			var nid := session.undock()
			hud.toast("Undocked: %s floats free" % JSON.parse_string(session.core.telemetry(nid)).get("name", "it") if nid >= 0 else "Nothing docked here", 2.0)
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



## R: arms or disarms the RCS, saying why it won't do anything if so.
func _toggle_rcs() -> void:
	var t := session.telemetry()
	var on: bool = not t.get("rcs", false)
	session.set_rcs(on)
	if not on:
		hud.toast("RCS off", 1.5)
	elif int(t.get("rcs_blocks", 0)) == 0:
		hud.toast("RCS on, but this craft has no RCS thrusters", 2.5)
	elif t.get("mono", 0.0) <= 0.0:
		hud.toast("RCS on, but there is no monopropellant left", 2.5)
	else:
		hud.toast("RCS on: I J K L slide, H N forward / back", 2.5)


## G: legs down if any are up, else up; asks the craft, not a memory (so
## it is right after switching craft or coming back aboard).
func _toggle_legs() -> void:
	var r: PackedFloat64Array = session.core.vessel_render(session.active, session.active)
	var parts: Array = JSON.parse_string(session.core.vessel_parts(session.active))
	var legs := 0
	var down := 0
	for k in mini(parts.size(), r.size() / VesselView.RPP):
		if Sim.part(parts[k].part).has("leg"):
			legs += 1
			down += 1 if r[k * VesselView.RPP + 10] > 0.5 else 0
	if legs == 0:
		hud.toast("This craft has no landing legs", 1.5)
		return
	var want := down < legs
	session.set_legs(want)
	hud.toast("Legs " + ("down" if want else "up"), 1.5)


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
	session.set_throttle(_throttle)  # a throttle set this frame counts for the engines lit now
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
		if session.launch_site == "runway":
			hud.toast("Engine running. B lets the brakes off; pull up (S) once you're fast enough." if t1.get("thrust", 0.0) > 0.0 else "Engine started: Z for full throttle", 3.5)
		elif t1.get("thrust", 0.0) > 0.0:
			hud.toast("Liftoff! The penguins are going to space." if not crew_names.is_empty() else "Liftoff!", 3.0)
		else:
			hud.toast("Clamps released, but no engine is burning! (Z for full throttle)", 3.5)
	elif not created.is_empty():
		hud.toast("Staging: separation confirmed", 1.5)
	elif lit:
		hud.toast("Ignition" if _throttle > 0.0 or int(t1.get("engines_lit", 0)) == _solids_lit(t1) else "Ignition, but the throttle is at 0 (Z for full, Shift/Ctrl to adjust)", 1.2 if _throttle > 0.0 else 3.5)
	# an engine this press switched on that no tank can reach
	if _no_tank(t1).size() > _no_tank(t0).size():
		var name: String = Sim.part(_no_tank(t1)[0]).get("name", "An engine")
		hud.toast("%s lit, but no tank reaches it: no fuel, no thrust" % name, 4.0)
		Sfx.play("alarm", 0.4)


func _no_tank(t: Dictionary) -> Array:
	var out := []
	for e in t.get("starved", []):
		if not e.tank:
			out.append(e.part)
	return out


## Lit solid boosters (they ignore the throttle).
func _solids_lit(t: Dictionary) -> int:
	return int(t.get("solids_lit", 0))


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
	var reason := session.step_warp(dir)
	if reason != "":
		hud.toast(reason, 2.0)


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
	if computer != null:
		computer._let_go_aim()  # (a SAS mode picked by hand: the script's aim is let go)
	_sas_follow = mode if mode in ["maneuver", "target", "antirel", "dock"] else ""
	if _sas_follow == "maneuver" and session.nodes.is_empty():
		_sas_follow = ""
		hud.toast("No maneuver node", 1.5)
		return
	if _sas_follow in ["target", "antirel", "dock"] and session.target_info().is_empty():
		_sas_follow = ""
		hud.toast("No target: Y picks one", 2.0)
		return
	if _sas_follow != "":
		return
	if not session.set_sas(mode):
		hud.toast("SAS mode unavailable", 1.5)


## The direction (sim axes) SAS should hold for the mode it follows, or
## Vector3.ZERO when there is none any more.
func _follow_dir() -> Vector3:
	match _sas_follow:
		"maneuver":
			var nb := _node_burn()
			return nb.vec if not nb.is_empty() else Vector3.ZERO
		"target", "antirel", "dock":
			var ti := session.target_info()
			if ti.is_empty():
				return Vector3.ZERO
			if _sas_follow == "target":
				return Vector3(ti.dir[0], ti.dir[1], ti.dir[2])
			if _sas_follow == "antirel":
				var rv := Vector3(ti.rel_v[0], ti.rel_v[1], ti.rel_v[2])
				return rv if rv.length() > 0.01 else Vector3.ZERO
			if ti.has("port_axis"):
				return Vector3(ti.port_axis[0], ti.port_axis[1], ti.port_axis[2])
	return Vector3.ZERO


## The target panel: range and closing speed, and once a pair of free
## docking ports is near, how well they line up (green when it would latch).
func _target_text() -> String:
	var ti := session.target_info()
	if ti.is_empty():
		return ""
	var s := "[color=#ff8a2a][b]TARGET[/b][/color]  %s\n[color=#9fb4cc]Range[/color] [b]%s[/b]   [color=#9fb4cc]closing[/color] %s m/s\n[color=#9fb4cc]Relative speed[/color] %.2f m/s" % [
		ti.get("name", "?"), FlightHUD.fmt_dist(ti.distance), FlightHUD.signed(ti.closing, 2), ti.rel_speed]
	var ca := session.closest_approach()
	if not ca.is_empty() and float(ca.t) - session.time() > 1.0:
		if ca.edge:
			s += "\n[color=#9fb4cc]Closest[/color] none under %s in %d orbits" % [FlightHUD.fmt_dist(ca.dist), int(FlightSession.APPROACH_ORBITS)]
		else:
			s += "\n[color=#9fb4cc]Closest[/color] [b]%s[/b] in %s at %.1f m/s" % [FlightHUD.fmt_dist(ca.dist), FlightHUD.fmt_time(float(ca.t) - session.time()), ca.rel_speed]
	if ti.has("port_distance") and ti.port_distance < 200.0:
		var d: Dictionary = Sim.part("dock_port").get("docking", {})
		var good: bool = ti.port_angle_deg < d.get("max_angle_deg", 10.0) and ti.port_closing < d.get("max_speed", 0.8)
		s += "\n[color=%s]PORT[/color] %.2f m · %.1f° off · %.2f m aside · closing %s m/s" % [
			"#7fe08a" if good else "#ffcf5a", ti.port_distance, ti.port_angle_deg, ti.port_lateral, FlightHUD.signed(ti.port_closing, 2)]
	return s


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
			if _resumed:
				hud.toast("This flight began before you came aboard: there is no launch to go back to.", 3.0)
				return
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
			if persistent:
				Career.charge_launch(Career.craft_parts(session.launch_text))  # the same rocket, bought again
			_assign_crew()
			hud.toast("Reverted to launch. The penguins pretend nothing happened.", 2.5)
			if tutorial:
				_start_tutorial()
		"revert_hangar":
			if _resumed:
				hud.toast("This flight began before you came aboard: there is no launch to go back to.", 3.0)
				return
			_restore_crew()
			if persistent:
				session.revert_before_launch()
				Sim.save_game()
			get_tree().change_scene_to_file("res://scenes/hangar.tscn")
		"main_menu":
			if not persistent:
				_record("lost" if _mission.pod_lost else "recovered")
				get_tree().change_scene_to_file("res://scenes/main_menu.tscn")
				return
			var why := leave_refusal()
			if why != "":
				hud.toast(why, 3.5)
				return
			_leave()


func _on_sim_event(e: Dictionary) -> void:
	match e.kind:
		"part_destroyed":
			_react("boom", 3.5)
			var how: String = "by overheating" if float(e.speed) <= -2.0 else "in the blast" if float(e.speed) < 0.0 else "at %.0f m/s" % float(e.speed)
			if Sim.part(e.part).has("eva"):
				# a penguin out walking: just that one is lost
				var lost: Array = Sim.sync_crew(_mission) if persistent else []
				hud.toast("%s was lost %s" % [", ".join(lost) if not lost.is_empty() else "A penguin on EVA", how], 4.0)
				if persistent:
					Sim.save_game()
			elif Sim.part(e.part).get("crew", 0) > 0:
				_crew_lost(Sim.part(e.part).name, how)
			elif not _mission.pod_lost:
				hud.toast("%s destroyed %s" % [Sim.part(e.part).name, how], 2.5)
		"docked":
			_docked_seen = true
			_refresh_crew()
			if int(e.vessel) == session.active:
				Sfx.play("stage")
				_react("orbit", 3.0)
				hud.toast("Docked! %s is one craft now (U to undock)" % session.telemetry().get("name", ""), 3.0)
		"undocked":
			Sfx.play("decouple")
			_refresh_crew()
		"soi_change":
			_react("orbit", 3.0)
			if not _mission.visited.has(str(e.get("body", ""))):
				_mission.visited.append(str(e.get("body", "")))
			var bi: int = session.core.body_find(str(e.get("body", "")))
			var star := bi >= 0 and int(JSON.parse_string(session.core.body_info(bi)).parent) < 0
			hud.toast(("In through %s's heliopause: back among a star's worlds" if star else "Entering the sphere of influence of %s") % str(e.get("body", "?")).capitalize(), 4.0)
		"arrived":
			Sfx.play("click")
			hud.toast("Arrived: stopped short of %s's heliopause, drive off. Ease in with a little drive" % str(e.get("body", "?")), 6.0)
		"interstellar":
			if e.vessel == session.active:
				_react("orbit", 3.0)
				hud.toast("Out past %s's heliopause: between the stars. The Stillwater Drive runs at full strength out here, and warp keeps the ship's clock" % str(e.get("body", "?")).capitalize(), 6.0)
		"landed":
			if e.vessel == session.active and _launched:
				_react("landed", 5.0)
				Sfx.play("touchdown")
				var t := session.telemetry()
				if t.get("body", "") == "krill":
					_mission.krill_landings += 1
				if t.get("body", "floe") != "floe":
					var where := str(t.body).capitalize()
					hud.toast(("The penguins have landed on %s! One small waddle for a penguin..." % where) if not crew_names.is_empty() else "Touchdown on %s!" % where, 5.0)
				else:
					hud.toast("Touchdown! The crew is doing a little waddle.", 3.5)
		"warp_stopped":
			if e.reason == "engines firing":
				hud.toast("Warp stopped: engines firing (they only burn at normal time or physics warp)", 3.0)
			else:
				hud.toast("Warp stopped: %s ahead" % e.reason, 2.5)
		"warp_eased":
			hud.toast("Warp eased to %dx: closing on %s" % [int(e.rate), String(session.telemetry().get("body", "the ground")).capitalize()], 2.0)


# ---- frame -------------------------------------------------------------------

func _process(delta: float) -> void:
	if _pending_toast != "" and hud != null:
		hud.toast(_pending_toast, 3.0)
		_pending_toast = ""
	if hud.menu_visible():
		Sfx.silence_beds()  # paused: the engines and wind go quiet too
		return
	if not _desk_open:
		_controls(delta)
	else:
		session.set_input(0.0, 0.0, 0.0)  # (typing at the computer is not flying)
		session.set_translate(0.0, 0.0, 0.0)
	# a script asleep until a moment: time warp eases off before it, so the
	# moment isn't warped past (one frame at 1000x is a quarter of a minute)
	if computer != null and session.warp_kind == "rails" and session.warp_index > 0:
		var wake := computer.wake_at()
		if wake > 0.0:
			var left := wake - session.time()
			var k := session.warp_index
			while k > 0 and FlightSession.RAILS_WARP[k] * delta > maxf(left * 0.5, 0.0):
				k -= 1
			if k < session.warp_index:
				session.set_warp("rails", k)
	session.advance(delta)
	if computer == null:
		computer = ShipComputer.get_shared()
	computer.update(self, session.telemetry())
	for line in computer.take_said():
		hud.toast(line, 4.0)
	_render(delta)
	_update_course_panel()
	_mission_tick += delta
	if persistent and _mission_tick > 0.25:
		_mission_tick = 0.0
		_check_missions()
		if _science and _science.visible:
			_fill_science()


## Lifted off without a press of space (a script lit the engine): the
## mission clock starts all the same.
func _note_liftoff(t: Dictionary) -> void:
	if not _launched and t.get("liftoff_t") != null and not t.get("clamped", false):
		_launched = true
		mission_start = float(t.liftoff_t)


## PASA's missions, against the craft being flown.
func _check_missions() -> void:
	var t := session.telemetry()
	if not t.get("alive", false):
		return
	_note_liftoff(t)
	var ti := session.target_info()
	Career.check(t, session.core.vessel_serial(session.active), {"eva": t.get("eva", false),
		"target_distance": ti.get("distance", INF), "docked": _docked_seen, "hands_off": _launched and not hands_used})
	_docked_seen = false


func _on_mission_done(m: Dictionary) -> void:
	Sfx.play("stage")
	_react("orbit", 3.0)
	hud.toast("Mission complete: %s! +%d research%s" % [m.title, int(m.research), " · +%s" % Career.money(float(m.funds)) if Career.is_career() else ""], 4.0)
	if m.has("story"):
		hud.show_story(m.title.to_upper(), m.story)


## O: the science panel: each experiment aboard, what it would bring home
## from here, and a button to run it.
## Between the stars: a button per star sets a course there (drive flat
## out, turning round halfway, stopping just short of its heliopause);
## shown only out there and only for a ship with a Stillwater Drive.
func _update_course_panel() -> void:
	var t := session.telemetry()
	var show: bool = t.get("interstellar", false) and float(t.get("drive_max", 0.0)) > 0.0
	if _course == null:
		if not show:
			return
		_course = PanelContainer.new()
		_course.add_theme_stylebox_override("panel", HangarTheme.box(Color(0.06, 0.1, 0.18, 0.92), 8, Color(0.5, 0.95, 0.9), 2))
		_course.set_anchors_and_offsets_preset(Control.PRESET_TOP_RIGHT)
		_course.offset_left = -330
		_course.offset_right = -10
		_course.offset_top = 150
		hud.add_child(_course)
		_course.add_child(VBoxContainer.new())
	_course.visible = show
	if not show:
		return
	var box: VBoxContainer = _course.get_child(0)
	var sig := str(t.get("course", ""))
	if box.get_meta("sig", "-") == sig:
		if t.has("course"):
			(box.get_child(1) as Label).text = "turn round in %s, there in %s (ship time)" % [FlightHUD.fmt_time(t.course_to_turn) if float(t.course_to_turn) > 0.0 else "(braking)", FlightHUD.fmt_time(t.course_left)]
		return
	box.set_meta("sig", sig)
	for c in box.get_children():
		c.queue_free()
	var head := Label.new()
	head.text = "COURSE · %s" % (str(t.course_name) if t.has("course") else "none")
	head.add_theme_color_override("font_color", Color(0.5, 0.95, 0.9))
	box.add_child(head)
	var info := Label.new()
	info.text = "pick a star: the drive does the rest" if not t.has("course") else ""
	info.add_theme_font_size_override("font_size", 12)
	box.add_child(info)
	# the stars nearest first, with how far each is
	var feed: PackedFloat64Array = session.core.body_render(session.active)
	var by_dist := []
	for si in _stars_idx():
		by_dist.append([float(Frame.dist_dir(feed, si * 5)[0]), si])
	by_dist.sort_custom(func(x, y): return x[0] < y[0])
	for e in by_dist:
		var si: int = e[1]
		var b := Button.new()
		b.focus_mode = Control.FOCUS_NONE
		b.text = "Set course to %s · %.1f ly" % [JSON.parse_string(session.core.body_info(si)).name, float(e[0]) / 9.4607304725808e15]
		b.pressed.connect(func():
			var r: PackedFloat64Array = session.core.set_course(session.active, si)
			session._telemetry_tick = -1
			if r.size() == 2:
				Sfx.play("click")
				hud.toast("Course set: %s of ship time, %s at home. Warp (.) to fly it" % [FlightHUD.fmt_time(r[0]), FlightHUD.fmt_time(r[1])], 5.0)
			else:
				hud.toast("Too close to set a course there", 2.0))
		box.add_child(b)
	if t.has("course"):
		var cancel := Button.new()
		cancel.focus_mode = Control.FOCUS_NONE
		cancel.text = "Cancel course (drive off)"
		cancel.pressed.connect(func():
			session.core.set_course(session.active, -1)
			session._telemetry_tick = -1
			_throttle = 0.0)
		box.add_child(cancel)


func _toggle_science() -> void:
	if _science == null:
		_science = PanelContainer.new()
		_science.add_theme_stylebox_override("panel", HangarTheme.box(Color(0.06, 0.1, 0.18, 0.94), 8, Color(0.4, 0.8, 1.0), 2))
		_science.set_anchors_and_offsets_preset(Control.PRESET_CENTER_TOP)
		_science.offset_left = -230
		_science.offset_right = 230
		_science.offset_top = 112  # under the clock
		_science.grow_horizontal = Control.GROW_DIRECTION_BOTH
		hud.add_child(_science)
		_science.add_child(VBoxContainer.new())
		_science.visible = false
	_science.visible = not _science.visible
	if _science.visible:
		_fill_science()


func _fill_science() -> void:
	var box: VBoxContainer = _science.get_child(0)
	var t := session.telemetry()
	var sig := JSON.stringify([Career.situation(t), t.get("body", ""), t.get("biome", ""), Career.results_on(session.active).size(), session.active, int(float(t.get("ec", 0.0)) / 10.0)])
	if box.get_meta("sig", "") == sig:
		return
	box.set_meta("sig", sig)
	for c in box.get_children():
		c.queue_free()
	var head := Label.new()
	var sit := Career.situation(t)
	head.text = "SCIENCE · %s" % Career.describe(sit, t.get("body", ""), Career.biome(t, sit))
	head.add_theme_color_override("font_color", Color(0.45, 0.85, 1.0))
	box.add_child(head)
	var exps := Career.experiments_on(session.active, t) if persistent else []
	if exps.is_empty():
		var none := Label.new()
		none.text = "No experiments aboard. Pods (crew reports) and the Science parts carry them."
		none.autowrap_mode = TextServer.AUTOWRAP_WORD
		box.add_child(none)
	for x in exps:
		var row := HBoxContainer.new()
		var b := Button.new()
		b.text = "Run"
		b.focus_mode = Control.FOCUS_NONE
		b.disabled = x.why != ""
		var uid: String = x.uid
		var exp: String = x.exp
		b.pressed.connect(func():
			var why := Career.run(session.active, uid, exp, session.telemetry())
			Sfx.play("click")
			hud.toast("%s: %s" % [Career.science.experiments[exp].name, "recorded. Bring it home to Floe to count it" if why == "" else why], 2.5)
			box.set_meta("sig", "")
			_fill_science())
		row.add_child(b)
		var l := Label.new()
		l.text = "%s · %s" % [x.name, x.why if x.why != "" else "%.1f research" % x.value]
		l.add_theme_color_override("font_color", Color(0.85, 0.9, 1.0) if x.why == "" else Color(0.55, 0.62, 0.72))
		row.add_child(l)
		box.add_child(row)
	var res := Career.results_on(session.active)
	var worth := 0.0
	for r in res:
		worth += Career.value_of(r)
	var frac: float = t.get("antenna_fraction", 0.0)
	if frac > 0.0 and persistent:
		for r in res:
			var row := HBoxContainer.new()
			var b := Button.new()
			b.text = "Send"
			b.focus_mode = Control.FOCUS_NONE
			var pts := Career.value_of(r) * frac
			var kj: float = pts * float(t.get("antenna_kj_per_point", 0.0))
			b.disabled = kj > float(t.get("ec", 0.0))
			var subj: String = r.subject
			b.pressed.connect(func():
				var res2 := Career.transmit(session.active, subj, session.telemetry())
				Sfx.play("click")
				hud.toast("Sent home: +%.1f research for %.0f kJ" % [res2.research, res2.kj] if res2.ok else "Can't send: " + res2.why, 2.5)
				box.set_meta("sig", "")
				_fill_science())
			row.add_child(b)
			var l := Label.new()
			l.text = "%s, %s · %.1f research by antenna (%.0f kJ)" % [Career.science.experiments[r.exp].name, Career.describe(r.situation, r.body, r.get("biome", "")).to_lower(), pts, kj]
			l.add_theme_font_size_override("font_size", 12)
			row.add_child(l)
			box.add_child(row)
	var foot := Label.new()
	foot.text = "Aboard: %d results, worth %.1f research once recovered at Floe (O to close)" % [res.size(), worth]
	foot.add_theme_font_size_override("font_size", 12)
	foot.add_theme_color_override("font_color", Color(0.6, 0.7, 0.85))
	box.add_child(foot)


## Leaving flight (hangar, menu, revert) takes its engine roar with it.
func _exit_tree() -> void:
	if computer:
		computer.save()
	Sfx.silence_beds()


## A key that flies the ship stops the computer's program.
func _hands_on() -> void:
	if computer != null:
		ShipComputer.engines_follow(session)  # the throttle keys move every engine
	if computer != null and computer.programs.running():
		computer.programs.stop("you took the controls")
		hud.toast("The computer's %s program stopped: you have the controls" % computer.programs.name, 2.5)


func _controls(delta: float) -> void:
	if _inside:
		_inside_controls()
		return
	if session.telemetry().get("eva", false):
		_eva_controls()
		return
	# a program of the ship's computer is flying: hands on the keys take over
	if computer != null and computer.programs.running():
		var hands := false
		for key in [KEY_SHIFT, KEY_CTRL, KEY_W, KEY_A, KEY_S, KEY_D, KEY_Q, KEY_E, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT]:
			hands = hands or Input.is_key_pressed(key)
		if hands:
			computer.programs.stop("you took the controls")
			hud.toast("The computer's %s program stopped: you have the controls" % computer.programs.name, 2.5)
		else:
			_throttle = session.throttle_set
	elif computer != null:
		if Input.is_key_pressed(KEY_SHIFT) or Input.is_key_pressed(KEY_CTRL):
			ShipComputer.engines_follow(session)
			hands_used = true
		for key in [KEY_W, KEY_A, KEY_S, KEY_D, KEY_Q, KEY_E, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT]:
			if Input.is_key_pressed(key):
				computer._let_go_aim()  # steering by hand
				hands_used = true
				break
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
	# craft-relative (planes): W/S pitch the nose down / up about the craft's
	# own wing axis (local X), A/D yaw it left / right (local Z), whatever
	# the camera does; view-relative (rockets) turns the keys by the view
	var pr := Vector2(-away, -right) if steer_craft else _view_steer(away, right)
	session.set_input(pr.x, pr.y, roll)
	# RCS translation, like steering, as you see it: J/L slide left/right
	# on screen, I/K up/down, H/N along the nose
	var sl := float(Input.is_key_pressed(KEY_L)) - float(Input.is_key_pressed(KEY_J))
	var su := float(Input.is_key_pressed(KEY_I)) - float(Input.is_key_pressed(KEY_K))
	var sf := float(Input.is_key_pressed(KEY_H)) - float(Input.is_key_pressed(KEY_N))
	var tr := _view_translate(sl, su, sf)
	session.set_translate(tr.x, tr.y, tr.z)
	if _sas_follow != "":
		var fd := _follow_dir()
		if fd == Vector3.ZERO:
			if _sas_follow == "antirel":
				session.set_sas("stability")  # matched: hold here
			_sas_follow = ""
		else:
			session.set_sas("target", PackedFloat64Array([fd.x, fd.y, fd.z]))


## A penguin out walking: WASD walks it (relative to the view, over the
## ground), and with the jetpack on (R) WASD and Shift / Ctrl fly it.
## SAS keeps it standing near the ground and steady further out.
func _eva_controls() -> void:
	var t := session.telemetry()
	session.set_input(0.0, 0.0, 0.0)
	_throttle = 0.0  # penguins have no throttle
	session.set_throttle(0.0)
	var fwd := float(Input.is_key_pressed(KEY_W) or Input.is_key_pressed(KEY_UP)) - float(Input.is_key_pressed(KEY_S) or Input.is_key_pressed(KEY_DOWN))
	var right := float(Input.is_key_pressed(KEY_D) or Input.is_key_pressed(KEY_RIGHT)) - float(Input.is_key_pressed(KEY_A) or Input.is_key_pressed(KEY_LEFT))
	var lift := float(Input.is_key_pressed(KEY_SHIFT)) - float(Input.is_key_pressed(KEY_CTRL))
	var up := Frame.vec_arr(t.up).normalized()
	var cf := -_cam.global_basis.z
	cf = (cf - up * cf.dot(up)).normalized() if absf(cf.dot(up)) < 0.99 else (_cam.global_basis.y - up * _cam.global_basis.y.dot(up)).normalized()
	var cr := cf.cross(up)
	var want := cf * fwd + cr * right
	if want.length() > 1.0:
		want = want.normalized()
	var top: float = Sim.part("eva_penguin").get("eva", {}).get("walk_speed", 1.5)
	var w := want * top
	session.set_walk(w.dot(Frame.vec_arr(t.east)), w.dot(Frame.vec_arr(t.north)))
	# the jetpack: the same keys as a push in the world, turned into the
	# penguin's own frame for its thrusters
	if t.get("rcs", false) and t.has("side"):
		var push := want + up * lift
		var n := Frame.vec_arr(t.nose).normalized()
		var x := Frame.vec_arr(t.side).normalized()
		var z := x.cross(n)
		session.set_translate(clampf(push.dot(x), -1, 1), clampf(push.dot(n), -1, 1), clampf(push.dot(z), -1, 1))
	else:
		session.set_translate(0.0, 0.0, 0.0)
	if t.altitude_terrain < 30.0 and t.sas != "upright":
		session.set_sas("upright")
	elif t.altitude_terrain > 40.0 and t.sas == "upright":
		session.set_sas("stability")


## F5: everything as it is now (the world, who sits where, the program,
## the roster, the craft flown, its planned burns and target, the mission
## clock) into the quicksave; F9 puts it all back.
func quicksave() -> bool:
	if not persistent:
		hud.toast("No quicksave on this flight", 2.0)
		return false
	var d := {"version": 1, "world": session.core.world_save(), "seats": Sim.seats, "career": Career.state(), "crew": Crew.crew,
		"active": session.active, "serial": session.core.vessel_serial(session.active), "target": session.target,
		"nodes": session.nodes, "mission_start": mission_start, "launched": _launched, "mission": _mission,
		"launch_text": session.launch_text, "launch_site": session.launch_site, "pre_launch": session._pre_launch, "t": session.time()}
	var f := FileAccess.open(Sim.save_dir + "quicksave.json", FileAccess.WRITE)
	if f == null:
		hud.toast("Quicksave failed: can't write the save", 2.5)
		return false
	f.store_string(JSON.stringify(d))
	f.close()
	Sfx.play("click")
	hud.toast("Quicksaved at T+%s (F9 to load)" % FlightHUD.fmt_time(session.time() - mission_start), 2.0)
	return true


func quickload() -> bool:
	if not persistent:
		hud.toast("No quickload on this flight", 2.0)
		return false
	var text := FileAccess.get_file_as_string(Sim.save_dir + "quicksave.json")
	var d = JSON.parse_string(text) if text != "" else null
	if not d is Dictionary:
		hud.toast("No quicksave yet (F5 makes one)", 2.0)
		return false
	session.set_warp("physics", 0)
	if not session.core.world_load(d.world):
		hud.toast("Quicksave unreadable: " + session.core.last_error(), 3.0)
		return false
	Sim.seats = d.seats
	Career.restore(d.career)
	Crew.crew = d.crew
	Crew.save_roster()
	session.launch_text = d.launch_text
	session.launch_site = d.launch_site
	session._pre_launch = d.pre_launch
	var id := int(d.active)
	if session.core.vessel_alive(id) and session.core.vessel_serial(id) == int(d.serial):
		session.switch_to(id)
	session.target = int(d.target) if session.core.vessel_alive(int(d.target)) else -1
	session.nodes = d.nodes
	mission_start = float(d.mission_start)
	_launched = d.launched
	_mission = d.mission
	_wreck_set = false
	_fail_in = 0.0
	hud.hide_failure()
	_clear_views()
	_refresh_crew()
	Sim.save_game()
	Sfx.play("click")
	hud.toast("Quickloaded: back to T+%s" % FlightHUD.fmt_time(session.time() - mission_start), 2.5)
	return true


## 1-9: the craft's action groups (set up in the hangar).
func _action_group(g: int) -> void:
	var n: int = session.core.vessel_action_group(session.active, g)
	session._telemetry_tick = -1
	Sfx.play("click" if n == 0 else "ignite")
	hud.toast("Action group %d: %s" % [g, "%d thing%s happened" % [n, "" if n == 1 else "s"] if n > 0 else "nothing in it (hover a part in the hangar and press %d)" % g], 2.0)


## G on EVA: the penguin plants a PASA flag where it stands.
func _plant_flag() -> void:
	var t := session.telemetry()
	var who: String = t.get("name", "A penguin")
	var fid: int = session.core.eva_plant_flag(session.active)
	if fid < 0:
		hud.toast("Feet on the ground first: a flag needs something to stand in", 2.0)
		return
	var body := Career.body_name(str(t.body))
	session.core.vessel_rename(fid, "%s's flag on %s" % [who, body])
	Sfx.play("stage")
	_react("orbit", 3.0)
	hud.toast("%s plants a PASA flag on %s!" % [who, body], 3.5)
	if persistent:
		Career.flag_planted(str(t.body))
		Sim.save_game()


## B: wheel brakes on / off.
func _toggle_brakes() -> void:
	var t := session.telemetry()
	if int(t.get("wheels", 0)) == 0:
		hud.toast("No wheels to brake", 1.5)
		return
	var on: bool = not t.get("brakes", false)
	session.core.vessel_set_brakes(session.active, on)
	session._telemetry_tick = -1
	Sfx.play("click")
	hud.toast("Brakes on" if on else "Brakes off: free to roll", 1.5)


var _star_cache: Array[int] = []


func _stars_idx() -> Array[int]:
	if _star_cache.is_empty():
		for i in session.core.body_count():
			if int(JSON.parse_string(session.core.body_info(i)).parent) < 0:
				_star_cache.append(i)
	return _star_cache


## C: every drill and refinery aboard on (or all off).
func _toggle_mining() -> void:
	var t := session.telemetry()
	if int(t.get("drills", 0)) + int(t.get("refineries", 0)) == 0:
		hud.toast("No drill or refinery aboard", 1.5)
		return
	var on: bool = int(t.get("drills_on", 0)) + int(t.get("refineries_on", 0)) == 0
	session.core.vessel_set_mining(session.active, on)
	session._telemetry_tick = -1
	Sfx.play("click")
	hud.toast("Mining on: drills dig while landed, refineries fill the tanks" if on else "Mining off", 2.0)


## F: a penguin climbs out of the pod we fly, or (on EVA) back into the
## nearest crewed part within reach that has a free seat.
func _eva_key() -> void:
	var t := session.telemetry()
	if t.get("eva", false):
		_board()
		return
	if not persistent:
		hud.toast("No going outside on this flight", 2.0)
		return
	if not t.get("alive", false):
		return
	if float(t.get("dynamic_pressure", 0.0)) > 2000.0:
		hud.toast("Too fast to open the hatch: slow down first", 2.5)
		return
	var parts: Array = JSON.parse_string(session.core.vessel_parts(session.active))
	for k in parts.size():
		var uid := str(int(parts[k].uid))
		var aboard: Array = Sim.seats.get(uid, [])
		if int(Sim.part(parts[k].part).get("crew", 0)) <= 0 or aboard.is_empty():
			continue
		var nid: int = session.core.vessel_eva_exit(session.active, k)
		if nid < 0:
			break
		var who: String = aboard[0]
		aboard.remove_at(0)
		if aboard.is_empty():
			Sim.seats.erase(uid)
		var pen: Array = JSON.parse_string(session.core.vessel_parts(nid))
		Sim.seats[str(int(pen[0].uid))] = [who]
		session.core.vessel_rename(nid, who)
		var from := session.active
		session.switch_to(nid)
		# the camera beyond the penguin, looking back at the craft past it
		var out := -Frame.vec(session.core.vessel_render(from, nid), 1)
		var tp := session.telemetry()
		var u := Frame.vec_arr(tp.up)
		out -= u * out.dot(u)
		if out.length() > 0.1:
			_cam_yaw = atan2(-out.dot(Frame.vec_arr(tp.east)), -out.dot(Frame.vec_arr(tp.north)))
			_cam_pitch = 0.25
		Sim.save_game()
		Sfx.play("click")
		hud.toast("%s steps outside. WASD walk, Space jump, R jetpack, F climbs back in" % who, 4.0)
		return
	hud.toast("Nobody aboard to go outside", 2.0)


## F2: the ship's computer, its desktop over the flight. The ship flies on
## underneath; scripts started from it keep running when it closes.
func _toggle_desk() -> void:
	if _desk_open:
		_desk_open = false
		_desk.visible = false
		_desk_box.visible = false
		_screen_live()
		if computer:
			computer.save()
		return
	if not _ensure_desk():
		return
	_desk_open = true
	_desk.visible = true
	_desk_box.visible = true
	_screen_live()
	Sfx.play("click")


## The ship's desktop, made once a flight: NOMINAL's DE in a viewport of
## its own the size of the window. Full screen (F2, ~, a click on the
## cabin's screen) it is shown over everything and takes the keys; in the
## penguins' view the same picture is on the screen in the cabin.
func _ensure_desk() -> bool:
	if _desk != null:
		return true
	if computer == null:
		computer = ShipComputer.get_shared()
	if computer.machine == null:
		hud.toast("The ship's computer did not start (no TrsComputer in this build)", 3.0)
		return false
	_desk = CanvasLayer.new()
	_desk.layer = 120  # over the HUD and the map
	_desk.visible = false
	_desk_box = SubViewportContainer.new()
	_desk_box.name = "DeskHolder"
	_desk_box.stretch = true
	_desk_box.visible = false
	_desk_vp = SubViewport.new()
	_desk_vp.name = "Desk"
	_desk_vp.transparent_bg = false
	_desk_vp.handle_input_locally = true
	_desk_vp.render_target_update_mode = SubViewport.UPDATE_DISABLED
	_desk_box.add_child(_desk_vp)
	_desk.add_child(_desk_box)
	add_child(_desk)
	_desk_box.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_de = load("res://scripts/os/de.gd").new()
	_de.machine = computer.machine
	_de.ship = self
	_desk_vp.add_child(_de)
	_de.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	return true


## The desktop is drawn while someone can see it: full screen, or on the
## cabin's screen in the penguins' view.
func _screen_live() -> void:
	if hud and hud.instruments:
		hud.instruments.visible = _inside == null  # (they would cover the cabin's screen)
	if _desk_vp == null:
		return
	var seen := _desk_open or (_inside != null and _inside.screen != null)
	_desk_vp.render_target_update_mode = SubViewport.UPDATE_ALWAYS if seen else SubViewport.UPDATE_DISABLED


## ~ goes round the views: the rocket, the penguins inside it, the ship's
## computer, and back to the rocket.
func _cycle_view() -> void:
	if _desk_open:
		_toggle_desk()
		if _inside:
			_leave_inside()
		hud.toast("Rocket view (~: the penguins' view)", 2.0)
		return
	if _inside:
		_toggle_desk()
		return
	_toggle_inside()
	if _inside == null:
		_toggle_desk()  # nobody aboard to look through: straight to the computer
		return
	hud.toast("Penguin view. Click the ship's computer screen (or ~) for the computer; ~ again for the rocket view.", 5.0)


## Enter: one of the crew gets up and walks about inside (a room of a
## crewed part, or any room aboard), or goes back to the controls.
func _toggle_inside() -> void:
	if _inside:
		_leave_inside()
		hud.toast("%s is back at the controls" % _in_who, 2.0)
		return
	var t := session.telemetry()
	if not t.get("alive", false) or t.get("eva", false):
		return
	var parts: Array = JSON.parse_string(session.core.vessel_parts(session.active))
	var room := -1
	var who := ""
	for k in parts.size():
		var d: Dictionary = Sim.part(parts[k].part)
		if d.get("interior", {}).get("volumes", []).is_empty():
			continue
		var aboard: Array = Sim.seats.get(str(int(parts[k].uid)), [])
		if room < 0 or (who == "" and not aboard.is_empty()):
			room = k
			who = aboard[0] if not aboard.is_empty() else ""
	if room < 0:
		hud.toast("No room aboard to walk about in: build in a hab (Burrow, Rookery) or a spin ring", 3.0)
		return
	if who == "":
		for uid in Sim.seats:
			for k in parts.size():
				if str(int(parts[k].uid)) == uid and not Sim.seats[uid].is_empty():
					who = Sim.seats[uid][0]
	if who == "" and int(t.get("crew_capacity", 0)) > 0 and Sim.seats.is_empty():
		who = "A penguin"
	if who == "":
		hud.toast("Nobody aboard to get up and walk about", 2.5)
		return
	if not session.core.walker_enter(session.active, room):
		return
	_in_who = who
	_inside = InteriorView.new()
	_inside.setup(session.core, session.active)
	add_child(_inside)
	_inside.rebuild(parts)
	if _ensure_desk():
		_inside.set_screen_texture(_desk_vp.get_texture())
	_screen_live()
	_in_dist = 3.0
	_in_reframe = true
	_in_looked = false
	_in_cam_up = Vector3.UP
	_in_framed_up = Vector3.UP
	Sfx.play("click")
	hud.toast("%s floats out of the seat into the %s. WASD move, Space jump or push off, Enter back to the controls" % [who, Sim.part(parts[room].part).get("name", "room")], 4.0)


func _leave_inside() -> void:
	if _inside:
		session.core.walker_leave(_inside.vessel_id)
		_inside.queue_free()
	_inside = null
	_screen_live()


## Home: the spin hold, turning the craft about its long axis at the rate
## its spin ring was built for, with SAS keeping the nose where it is.
func _toggle_spin() -> void:
	var t := session.telemetry()
	if not t.has("spin_design"):
		hud.toast("No spin ring aboard: nothing to spin for", 2.0)
		return
	if float(t.get("spin_hold", 0.0)) != 0.0:
		session.core.vessel_set_spin(session.active, 0.0)
		hud.toast("Spin hold off: the craft keeps turning at %.2f rpm until something stops it (SAS holds still)" % (absf(float(t.spin_rate)) * 60.0 / TAU), 3.0)
		return
	var w: float = t.spin_design
	session.core.vessel_set_spin(session.active, w)
	if t.get("sas", "off") == "off":
		_on_sas("stability")
	hud.toast("Spinning up to %.2f rpm: %.2f g on the deck %.1f m out, once it gets there" % [w * 60.0 / TAU, w * w * _deck_radius() / 9.80665, _deck_radius()], 4.0)


## How far a spin ring's deck is from the axis (the widest one aboard).
func _deck_radius() -> float:
	var best := 0.0
	for p in JSON.parse_string(session.core.vessel_parts(session.active)):
		for v in Sim.part(p.part).get("interior", {}).get("volumes", []):
			if v.get("type", "") == "annulus":
				best = maxf(best, float(v.get("r1", 0.0)))
	return best


## Walking indoors: WASD by the view (over the floor when there is weight
## to stand on, anywhere when floating), Shift / Ctrl up and down when
## floating. The ship keeps flying as it was: throttle, SAS and all.
func _inside_controls() -> void:
	session.set_input(0.0, 0.0, 0.0)
	session.set_translate(0.0, 0.0, 0.0)
	session.set_throttle(_throttle)
	var t := session.telemetry()
	var wk: Dictionary = t.get("walker", {})
	if wk.is_empty():
		return
	var fwd := float(Input.is_key_pressed(KEY_W) or Input.is_key_pressed(KEY_UP)) - float(Input.is_key_pressed(KEY_S) or Input.is_key_pressed(KEY_DOWN))
	var right := float(Input.is_key_pressed(KEY_D) or Input.is_key_pressed(KEY_RIGHT)) - float(Input.is_key_pressed(KEY_A) or Input.is_key_pressed(KEY_LEFT))
	var lift := float(Input.is_key_pressed(KEY_SHIFT)) - float(Input.is_key_pressed(KEY_CTRL))
	var inv := _inside.global_basis.inverse()
	var cf := inv * -_cam.global_basis.z
	var cr := inv * _cam.global_basis.x
	var cu := inv * _cam.global_basis.y
	var want: Vector3
	if wk.get("standing", false):
		var u := _inside.up
		cf = (cf - u * cf.dot(u)).normalized() if absf(cf.dot(u)) < 0.98 else (cu - u * cu.dot(u)).normalized()
		cr = (cr - u * cr.dot(u)).normalized()
		want = cf * fwd + cr * right
	else:
		want = cf * fwd + cr * right + cu * lift
	if want.length() > 1.0:
		want = want.normalized()
	var top: float = Sim.part("eva_penguin").get("eva", {}).get("walk_speed", 1.5)
	var w := want * top
	session.core.walker_command(session.active, PackedFloat64Array([w.x, w.y, w.z]), false)


## The camera indoors: behind the penguin, its up away from the weight it
## feels (or the way it already was, weightless), kept inside the room. It
## picks the longest view in the room when the penguin comes in or its
## weight swings round; in a tube too tight to stand back in it looks out
## through the penguin's eyes.
func _inside_view(t: Dictionary, delta: float) -> void:
	if _inside == null:
		return
	var wk: Dictionary = t.get("walker", {})
	if wk.is_empty() or _inside.vessel_id != session.active or not t.get("alive", false):
		_leave_inside()
		return
	var frame: PackedFloat64Array = session.core.vessel_frame(session.active, session.active)
	_inside.rebuild(JSON.parse_string(session.core.vessel_parts(session.active)))
	if _desk_vp != null:
		_inside.set_screen_texture(_desk_vp.get_texture())
	_inside.update(frame, wk, delta)
	var felt := DMath.to_v3(wk.felt)
	var want_up := -felt.normalized() if felt.length() > 0.3 else _in_cam_up
	if want_up.dot(_in_framed_up) < 0.85:
		_in_framed_up = want_up
		_in_reframe = true
	_in_cam_up = _in_cam_up.slerp(want_up, clampf(delta * 3.0, 0.0, 1.0)).normalized()
	var u := _in_cam_up
	var target := DMath.to_v3(wk.x) + u * 0.25
	if _in_reframe and not _orbiting:
		_in_reframe = false
		var best := -1.0
		for yi in 12:
			for pi in [0.15, 0.5, 0.95]:
				var dd := _in_free(DMath.to_v3(wk.x) + want_up * 0.25, _in_dir(want_up, TAU * yi / 12.0, pi))
				if dd > best + 0.05:
					best = dd
					_in_yaw = TAU * yi / 12.0
					_in_pitch = pi
	var dir := _in_dir(u, _in_yaw, _in_pitch)
	var d := _in_free(target, dir)
	var g := _inside.global_transform
	var first := d < 1.1
	_inside.penguin.visible = not first
	if first:
		var head := DMath.to_v3(wk.x) + _inside.up * 0.2
		_cam.global_position = g * head
		# in a capsule, out of its window (until the mouse turns the view)
		var look := head - dir
		if not _inside.windows.is_empty() and not _in_looked:
			look = _inside.windows[0]
			# and the computer's screen under it, in the same view
			if _inside.screen != null:
				look = look.lerp(_inside.screen.position, 0.55)
		_cam.look_at(g * look, (g.basis * u).normalized())
	else:
		_cam.global_position = g * (target + dir * d)
		_cam.look_at(g * target, (g.basis * u).normalized())
	_cam.v_offset = 0.0


## Which way the camera stands off the penguin: yaw about its up, pitch above.
func _in_dir(u: Vector3, yaw: float, pitch: float) -> Vector3:
	var ref := Vector3.UP if absf(u.dot(Vector3.UP)) < 0.9 else Vector3.BACK
	var e1 := (ref - u * ref.dot(u)).normalized()
	var e2 := u.cross(e1)
	return -(e1 * cos(yaw) + e2 * sin(yaw)) * cos(pitch) + u * sin(pitch)


## How far the camera can stand back from `from` along `dir` (up to the
## chosen distance) and stay a little way off the walls.
func _in_free(from: Vector3, dir: Vector3) -> float:
	var d := 0.3
	while d < _in_dist:
		var p := from + dir * (d + 0.1)
		if float(session.core.room_depth(session.active, PackedFloat64Array([p.x, p.y, p.z]))) < 0.12:
			break
		d += 0.1
	return minf(d, _in_dist)


func _inside_hint(t: Dictionary) -> String:
	var wk: Dictionary = t.get("walker", {})
	if wk.is_empty():
		return ""
	var g: float = wk.get("felt_g", 0.0)
	var why := ""
	if float(t.get("drive_accel", 0.0)) > 0.0 and g < 0.01:
		why = "the Stillwater Drive pushes %.2f g and nobody feels it" % (float(t.drive_accel) / 9.80665)
	elif absf(float(t.get("spin_rate", 0.0))) > 0.01 and g > 0.01:
		why = "spinning at %.2f rpm" % (absf(float(t.spin_rate)) * 60.0 / TAU)
	elif float(t.get("thrust", 0.0)) > 0.0:
		why = "the engines push"
	elif t.get("landed", false):
		why = "the ground holds the ship up"
	var feel := ("weighs %.2f g" % g) if g >= 0.005 else "weightless"
	return "%s in the %s: %s%s · WASD %s · Space %s · Enter back to the controls" % [_in_who, wk.get("room_name", "room"), feel,
		(" (" + why + ")") if why != "" else "", "walk" if wk.get("standing", false) else "paddle, Shift / Ctrl up / down",
		"jump" if wk.get("standing", false) else "push off a wall"]


## The name of the craft whose hatch the penguin can reach now ("" if none).
func _hatch_near() -> String:
	for v in JSON.parse_string(session.core.vessel_list()):
		if int(v.id) != session.active and not v.get("eva", false):
			var h: PackedFloat64Array = session.core.eva_hatch(session.active, int(v.id))
			if h[0] >= 0:
				return v.name
	return ""


func _board() -> void:
	var me := session.active
	var pen: Array = JSON.parse_string(session.core.vessel_parts(me))
	var my_uid := str(int(pen[0].uid))
	var who: Array = Sim.seats.get(my_uid, [])
	var full := ""
	var best := -1
	var best_gap := INF
	var best_uid := ""
	for v in JSON.parse_string(session.core.vessel_list()):
		var id := int(v.id)
		if id == me or v.get("eva", false):
			continue
		var h: PackedFloat64Array = session.core.eva_hatch(me, id)
		if h[0] < 0:
			continue
		var parts: Array = JSON.parse_string(session.core.vessel_parts(id))
		var cap := int(Sim.part(parts[int(h[0])].part).get("crew", 0))
		var uid := str(int(h[2]))
		if Sim.seats.get(uid, []).size() >= cap:
			full = v.name
			continue
		if h[1] < best_gap:
			best_gap = h[1]
			best = id
			best_uid = uid
	if best < 0:
		hud.toast("%s is full" % full if full != "" else "Nothing to climb into within reach (walk up to a pod)", 2.5)
		return
	var seated: Array = Sim.seats.get(best_uid, [])
	seated.append_array(who)
	Sim.seats[best_uid] = seated
	Sim.seats.erase(my_uid)
	Career.move_results(my_uid, best_uid)  # the samples come aboard too
	session.switch_to(best)
	session.core.vessel_remove(me)
	session.switch_to(best)  # the telemetry cache forgets the penguin
	Sim.save_game()
	Sfx.play("click")
	hud.toast("%s is back aboard" % ", ".join(who), 2.5)


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


## Turns screen-relative slide keys into a vessel-local translation: right
## and up on screen (perpendicular to the nose) plus along the nose.
func _view_translate(right: float, up: float, fwd: float) -> Vector3:
	if right == 0.0 and up == 0.0 and fwd == 0.0:
		return Vector3.ZERO
	var t := session.telemetry()
	if not t.get("alive", false) or not t.has("side"):
		return Vector3.ZERO
	var n := Frame.vec_arr(t.nose).normalized()
	var x := Frame.vec_arr(t.side).normalized()
	var z := x.cross(n)
	var want := _cam.global_basis.x.normalized() * right + _cam.global_basis.y.normalized() * up
	want -= n * want.dot(n)
	if want.length() > 1e-6:
		want = want.normalized() * maxf(absf(right), absf(up))
	want += n * fwd
	return Vector3(clampf(want.dot(x), -1.0, 1.0), clampf(want.dot(n), -1.0, 1.0), clampf(want.dot(z), -1.0, 1.0))


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
		if v.get("eva", false):
			var te: Dictionary = t if id == session.active else JSON.parse_string(core.telemetry(id))
			_views[id].eva = {"ground": int(te.get("contacts", 0)) > 0, "vel": Frame.vec_arr(te.surface_velocity),
				"space": not te.in_atmosphere and te.altitude_terrain > 20.0}
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
	# never below the ground: lift the eye over hills and wreckage pits
	var clear: float = core.ground_clearance(int(t.body_index), origin, PackedFloat64Array(Frame.to_sim(_cam.position)))
	if clear < CAM_CLEARANCE:
		_cam.position += up * (CAM_CLEARANCE - clear)
	_cam.look_at(focus, up)
	# centre the craft in the band between the top panels and the navball
	var vh := get_viewport().get_visible_rect().size.y
	_cam.v_offset = -_cam_dist * tan(deg_to_rad(_cam.fov) * 0.5) * clampf((FlightHUD.BOTTOM_H - FlightHUD.TOP_H) / maxf(vh, 1.0), 0.0, 0.5)
	# v_offset moves the eye itself down the view's up: keep that point
	# above the ground too, or a low camera looks out from under it
	var eye_at := _cam.position + _cam.global_basis.y * _cam.v_offset
	var eye_clear: float = core.ground_clearance(int(t.body_index), origin, PackedFloat64Array(Frame.to_sim(eye_at)))
	if eye_clear < CAM_CLEARANCE:
		_cam.position += up * (CAM_CLEARANCE - eye_clear)
		_cam.look_at(focus, up)
	_inside_view(t, delta)
	_update_booms(delta, drift)
	_update_failure(delta)

	# bodies: sun direction, terrain for the current body
	var bodies: PackedFloat64Array = core.body_render(origin)
	# the light comes from the nearest star (distances in doubles: between
	# the stars they are light-years)
	var sun_dir := Vector3.UP
	var best := INF
	var sun_star := -1
	for si in _stars_idx():
		var sd: Array = Frame.dist_dir(bodies, si * 5)
		if sd[0] < best:
			best = sd[0]
			sun_dir = sd[1]
			sun_star = si
	if sun_star != _sun_star:
		# its light is the colour of its surface (a red dwarf's is ruddy, a
		# hot star's blue-white), eased toward white: eyes adapt
		_sun_star = sun_star
		var T := float(JSON.parse_string(core.body_info(sun_star)).get("temperature", 5800.0))
		_sun.light_color = ScaledSpace._bb_color(T).lerp(Color.WHITE, 0.45) if T != 5800.0 else Color.WHITE
		_scaled.set_sun_color(ScaledSpace._bb_color(T).lerp(Color.WHITE, 0.3) if T != 5800.0 else Color.WHITE)
	_sun.look_at(-sun_dir * 1000.0, up if absf(sun_dir.dot(up)) < 0.99 else north)
	_sun.position = Vector3.ZERO
	var bi := int(t.body_index)
	_terrain.set_body(bi)
	if bi != _sky_body:
		_sky_body = bi
		_scaled.set_sky(Sim.look(str(t.body)))
	var body_rel := Frame.vec(bodies, bi * 5)
	var eye: PackedFloat64Array = core.to_body_fixed(bi, origin, PackedFloat64Array(Frame.to_sim(_cam.position)))
	# the body centre stays in doubles: the ground near us must not jitter
	_terrain.refresh(eye, [bodies[bi * 5], bodies[bi * 5 + 2], -bodies[bi * 5 + 1]], bodies[bi * 5 + 3])
	# cloud deck 7 km up on bodies that have weather; from below it would be
	# an overcast we don't model yet, so it shows once you're above it
	var deck: float = 7000.0
	var has_clouds: bool = Sim.look(t.body).get("clouds", false)
	var above: float = t.altitude - deck
	_clouds.visible = has_clouds and above > 2000.0
	var cover := float(Sim.look(t.body).get("cloud_cover", 0.5))
	_terrain.set_cloud_shadows(has_clouds, Frame.spin(bodies[bi * 5 + 3]).inverse() * sun_dir, deck / bodies[bi * 5 + 4], cover)
	(_clouds.material_override as ShaderMaterial).set_shader_parameter("coverage", cover)
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
		var sky := Sim.col(Sim.look(t.body).get("sky"), Color(0.62, 0.76, 0.93))
		_env.fog_light_color = sky.lerp(Color(0.02, 0.02, 0.05), 1.0 - day)
		_env.fog_density = 0.00003 * density
		_env.ambient_light_color = sky * Color(0.89, 0.86, 0.91)
		_env.ambient_light_energy = 0.1 + 0.4 * density * day
		# a locked world's night never ends: its ice gives back the starlight
		# (and the twilight's glow on the sky), enough to walk by
		var lk: Dictionary = Sim.look(t.body)
		if lk.get("locked", false) and day < 0.5:
			_env.ambient_light_color = _env.ambient_light_color.lerp(Sim.col(lk.get("night"), Color(0.7, 0.75, 0.9)) * Color(0.55, 0.6, 0.8), 1.0 - day * 2.0)
			_env.ambient_light_energy = maxf(_env.ambient_light_energy, 0.22 * (1.0 - day * 2.0))
	else:
		_env.fog_density = 0.0
		_env.ambient_light_energy = 0.08  # airless: hard shadows
	_sun.light_energy = (0.15 + 1.2 * clampf(up.dot(sun_dir) * 4.0 + 0.6, 0.0, 1.0)) if t.altitude < 20000.0 else 1.3
	_scaled.sync(_cam, origin, _cam.position, sun_dir, density, up)
	if t.get("interstellar", false):
		var vg: Array = t.velocity_galactic
		var vx: float = vg[0]
		var vy: float = vg[2]
		var vz: float = -vg[1]
		var vl := sqrt(vx * vx + vy * vy + vz * vz)
		_scaled.set_motion(Vector3(vx, vy, vz) / maxf(vl, 1e-30), float(t.beta), float(t.gamma), bodies)
	else:
		_scaled.set_motion(Vector3.ZERO, 0.0, 1.0, bodies)

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
	hud.follow_mode = _sas_follow
	hud.set_target_text(_target_text())
	hud.set_signal_text(FlightHUD.relic_text(t, Career.heard))
	_note_liftoff(t)
	if computer != null:
		var cp := computer.panel()
		if Career.is_career() and (not (cp.get("scripts", []) as Array).is_empty() or cp.has("program")):
			for m in Career.active_missions():
				if m.goal.get("hands_off", false):
					cp = cp.duplicate()
					cp["hands_off"] = [str(m.title), not hands_used]
					break
		hud.set_computer(cp)
	# crew and mission bookkeeping
	_mission.max_altitude = maxf(_mission.max_altitude, t.altitude)
	_mission.max_g = maxf(_mission.max_g, t.g_force)
	if t.situation == "orbiting" and not _was_orbiting and _launched:
		_mission.orbits += 1
		Sfx.play("chime")
		_react("orbit", 4.0)
		hud.toast("Orbit achieved! The crew presses their beaks to the window.", 4.0)
	_was_orbiting = t.situation == "orbiting"
	# each penguin feels what its own seat does (a spin ring's deck weighs
	# them, the capsule on the axis doesn't)
	var seat_g: Dictionary = t.get("seat_g", {})
	var felt_by := {}
	for uid in Sim.seats:
		if seat_g.has(uid):
			for who in Sim.seats[uid]:
				felt_by[who] = seat_g[uid]
	for p in _portraits:
		var name: String = p.member.get("name", "")
		if felt_by.has(name) and absf(float(felt_by[name]) - float(t.get("g_force", 0.0))) > 1e-3:
			var tp := t.duplicate()
			tp.g_force = felt_by[name]
			p.update_state(tp, t.get("spin", 0.0), delta)
		else:
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
	var air := Frame.vec_arr(t.surface_velocity)
	var wf := {"fronts": [], "exposure": {}}
	if t.density > 0.0 or _reentry.level > 0.0:
		wf = _windward_fronts(-air.normalized() if air.length() > 1.0 else Vector3.DOWN)
	_reentry.update_fx(air, t.density, wf.fronts, delta)
	for id in _views:
		_views[id].set_sheath(wf.exposure if id == session.active and _reentry.visible else {})
	Sfx.wind_level = maxf(Sfx.wind_level, _reentry.level)
	var hint := ""
	var no_tank := _no_tank(t)
	var dry := (t.get("starved", []) as Array).size() - no_tank.size()
	var plan: Array = t.get("stage_plan", [])
	if dry > _dry_seen and int(t.get("engines_lit", 0)) == 0 and _launched:
		hud.toast("Flameout: out of fuel", 2.0)
	_dry_seen = dry
	if not no_tank.is_empty():
		hint = "%s has no fuel: no tank reaches it. In the hangar, put a tank above it, or run a Siphon Feed Line to it from a tank" % Sim.part(no_tank[0]).get("name", "An engine")
	elif dry > 0 and int(t.get("engines_lit", 0)) == 0 and not plan.is_empty() and not t.landed:
		hint = "Out of fuel: SPACE %s" % ("drops the empty stage" if int(plan[0].decouplers) > 0 else "lights the next stage")
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
	var powered: bool = t.get("powered", true)
	if _was_powered and not powered and float(t.get("ec_capacity", 0.0)) > 0.0:
		hud.toast("Batteries flat: the reaction wheels have stopped%s" % ("" if int(t.get("crew_capacity", 0)) > 0 else " and the probe can't be steered"), 4.0)
		Sfx.play("alarm", 0.5)
	_was_powered = powered
	if not powered and float(t.get("ec_capacity", 0.0)) > 0.0 and hint == "":
		hint = "No power: point a solar panel at the sun (or wait for daylight) to charge the batteries"
	if _inside:
		hint = _inside_hint(t)
	elif hint == "" and int(t.get("rooms", 0)) > 0 and not t.get("eva", false):
		hint = "Enter: go inside and walk about" + (" · Home: spin hold (%s)" % ("on" if float(t.get("spin_hold", 0.0)) != 0.0 else "off") if t.has("spin_design") else "")
	if t.get("eva", false):
		var near := _hatch_near()
		var standing: bool = int(t.get("contacts", 0)) > 0
		var fly := ("jetpack on: WASD and Shift / Ctrl fly (%.1f kg mono)" if t.get("rcs", false) else "R turns the jetpack on (%.1f kg mono)") % t.get("mono", 0.0)
		hint = ("On EVA · WASD walk · Space jump · G plant a flag · " if standing else "On EVA, floating · ") + fly + \
			(" · F climbs into %s" % near if near != "" else "")
	hud.set_hint(hint)
	_recover_btn.visible = _launched and t.landed and t.body == "floe" and t.surface_speed < 0.5 and (_mission.max_altitude > 500.0 or _resumed)
	if _map.visible:
		_map.refresh()
	_auto_unwarp(t, delta)
	var warp := ""
	if session.warp_index > 0:
		warp = "WARP %s%dx" % ["" if session.warp_kind == "rails" else "physics ",
			int(FlightSession.RAILS_WARP[session.warp_index]) if session.warp_kind == "rails" else FlightSession.PHYSICS_WARP[session.warp_index]]
	if _slowing_t > 0.0:
		warp = "SLOWING ▼ " + (warp if warp != "" else "1x")
	hud.set_warp_text(warp)
	hud.update_from(t, session.time() - mission_start if _launched else 0.0)


## Time warp backs off to normal speed, a step at a time with a banner, when
## the flight gets interesting: entering the air (drag and heating matter)
## or dropping low over the ground. The pilot may warp again afterwards.
func _auto_unwarp(t: Dictionary, delta: float) -> void:
	var in_air: bool = t.get("in_atmosphere", false)
	var low: bool = not t.landed and t.altitude_terrain < LOW_WARP_ALT and t.vertical_speed < -1.0
	var why := ""
	if in_air and not _was_in_air:
		why = "entering the atmosphere"
	elif low and not _was_low:
		why = "close to the ground"
	_was_in_air = in_air
	_was_low = low
	if why != "" and session.warp_index > 0:
		_slowing_t = 0.01
		hud.toast("Time back to normal: %s" % why, 3.0)
		Sfx.play("click")
	if _slowing_t <= 0.0:
		return
	_slowing_t += delta
	if session.warp_index <= 0:
		if _slowing_t > 1.0:
			_slowing_t = 0.0
		return
	if _slowing_t > 0.25:
		_slowing_t = 0.01
		session.set_warp(session.warp_kind, session.warp_index - 1)


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
	if persistent:
		Sim.sync_crew(_mission)
		Sim.save_game()
		_recorded = true
	elif _launched and not crew_names.is_empty():
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
	if persistent and not _career_snapshot.is_empty():
		Career.restore(_career_snapshot)
	if not _roster_snapshot.is_empty():
		Crew.crew = _roster_snapshot.duplicate(true)
		Crew.save_roster()
	if persistent:
		Sim.seats = _seats_snapshot.duplicate(true)
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
var _dry_seen := 0
var _smoke_t := 0.0
var _wreck := Vector3.ZERO
var _wreck_set := false
var _fail_text: Array = []
var _booms: Array[Explosion] = []
var _shake := 0.0
var _roster_snapshot: Array = []
var _seats_snapshot: Dictionary = {}
var _career_snapshot: Dictionary = {}  # the program before this launch (reverts put it back)
var _launch_cost := 0.0
var _was_powered := true
## Steering relative to the craft (planes) instead of to the view (`).
var steer_craft := false
var _mission_tick := 0.0
var _docked_seen := false
var _science: PanelContainer
var _course: PanelContainer
## The flight lives in the persistent game world (not the tutorial's own).
var persistent := false
## Took over a vessel already flying (no launch to revert to).
var _resumed := false


var _smoke: SmokeTrail
var _smoke_origin := -1
var _chutes_open := 0
var _reentry: ReentryFX
var _front_ids: Array = []  # per rendered part: [radius, half height]
var _front_key := ""  # which vessel (and how many parts) _front_ids describes
var _inside: InteriorView  # a penguin walking about inside the active craft
var computer: ShipComputer  # the ship's computer (F2 opens its desktop)
var _desk: CanvasLayer        # the ship's desktop, full screen (hidden when not)
var _desk_box: SubViewportContainer
var _desk_vp: SubViewport      # where the desktop is drawn: full screen and the cabin's screen
var _de: Control               # NOMINAL's desktop (scripts/os/de.gd)
var _desk_open := false
var _in_who := ""
var _in_yaw := 0.0
var _in_pitch := 0.25
var _in_dist := 2.4
var _in_cam_up := Vector3.UP  # the camera's up, craft-local
var _in_framed_up := Vector3.UP  # the up the view was last framed for
var _in_reframe := false
var _in_looked := false  # the player has turned the view indoors


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
	Sfx.jet_level = clampf(float(t.get("throttle", 0.0)), 0.0, 1.0) if float(t.get("jet_flow", 0.0)) > 0.0 else 0.0
	# a canopy opening is the moment the drag-feed shows a new chute state
	var open := 0
	var feed: PackedFloat64Array = session.core.vessel_render(session.active, session.active)
	for k in feed.size() / VesselView.RPP:
		if int(feed[k * VesselView.RPP + 9]) >= 2:
			open += 1
	if open > _chutes_open:
		Sfx.play("chute")
	_chutes_open = open
