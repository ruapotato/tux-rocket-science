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
## Dev: shots for the playtest feedback round: plumes on the pad, the smoke
## trail on ascent, a vacuum plume, a toppled rocket exploding, the editor
## with attach markers, and the map centred on the vessel.
## ONLY=name renders just that shot.
extends RefCounted

const DIR := "user://dev_shots/"
var _log := PackedStringArray()


func run(runner: Node) -> Dictionary:
	var tree := runner.get_tree()
	tree.paused = false
	if tree.current_scene:
		tree.current_scene.queue_free()
	DirAccess.make_dir_recursive_absolute(DIR)
	var only := OS.get_environment("ONLY")
	if only == "" or only == "pad" or only == "ascent":
		await _ascent(tree, only)
	if only == "" or only == "vacuum":
		await _vacuum(tree)
	if only == "" or only == "boom":
		await _boom(tree)
	if only == "" or only == "hangar":
		await _hangar(tree)
	if only == "" or only == "map":
		await _map(tree)
	if only == "" or only == "heat":
		await _heat(tree)
	return {"status": "PASS", "log": "\n".join(_log)}


func _flight(tree: SceneTree, craft: String) -> Node3D:
	for c in tree.root.get_children():
		if c is Node3D:
			c.queue_free()
	Sim.pending_craft_text = FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + craft + ".craft.json")
	var scene: Node3D = load("res://scenes/flight.tscn").instantiate()
	tree.root.add_child(scene)
	await _frames(tree, 20)
	scene.hud.hide_help()
	return scene


func _ascent(tree: SceneTree, only: String) -> void:
	var scene := await _flight(tree, "krill_lander")
	scene._throttle = 1.0
	scene._stage()
	scene._cam_yaw = 0.9
	scene._cam_pitch = 0.12
	scene._cam_dist = 110.0
	for i in 150:
		await tree.process_frame
		if scene.session.time() - scene.mission_start > 2.6:
			break
	await _frames(tree, 10)
	await _shot(scene, "pad_plume")
	if only == "pad":
		return
	var api := ControlAPI.new(scene.session)
	var ap := AscentAutopilot.new(api)
	ap.step(0.1)
	for i in 400:
		ap.step(0.1)
		scene.session.step_ticks(5)
		await tree.process_frame
		if scene.session.telemetry().altitude > 9000.0:
			break
	scene._cam_yaw = 2.4
	scene._cam_pitch = 0.25
	scene._cam_dist = 120.0
	for i in 40:
		ap.step(0.02)
		scene.session.step_ticks(1)
		await tree.process_frame
	await _shot(scene, "ascent_smoke")
	for sm in scene.find_children("Smoke", "GPUParticles3D", true, false):
		var pm: ParticleProcessMaterial = sm.process_material
		_log.append("smoke %s emitting %s ratio %.2f vis %s dir %s v %.0f pos %s" % [sm.get_path(), sm.emitting, sm.amount_ratio, sm.is_visible_in_tree(), pm.direction, pm.initial_velocity_min, sm.global_position])
	_log.append("ascent at %.0f m, %.0f m/s" % [scene.session.telemetry().altitude, scene.session.telemetry().surface_speed])


func _vacuum(tree: SceneTree) -> void:
	var scene := await _flight(tree, "orbiter")
	var s: FlightSession = scene.session
	s.spawn_in_orbit(Sim.pending_craft_text, "floe", 90000.0, 0.0)
	scene._clear_views()
	s.stage()
	s.stage()
	scene._throttle = 1.0
	scene._cam_dist = 22.0
	scene._cam_pitch = 0.1
	scene._cam_yaw = 1.2
	await _frames(tree, 50)
	await _shot(scene, "vacuum_plume")


func _boom(tree: SceneTree) -> void:
	var scene := await _flight(tree, "orbiter")
	var s: FlightSession = scene.session
	var t := s.telemetry()
	var up := Vector3(t.up[0], t.up[1], t.up[2])
	var east := Vector3(t.east[0], t.east[1], t.east[2])
	var p := Vector3(t.position[0], t.position[1], t.position[2]) + up * 4.0 + east * 30.0
	var v := Vector3(t.velocity[0], t.velocity[1], t.velocity[2]) + east * 9.0 - up * 1.0
	s.core.vessel_remove(s.active)
	s.active = s.core.flight_spawn_state(Sim.pending_craft_text, "floe", PackedFloat64Array([p.x, p.y, p.z, v.x, v.y, v.z]))
	s.set_sas("off")
	scene._clear_views()
	scene._launched = true
	scene._cam_dist = 60.0
	scene._cam_pitch = 0.18
	scene._cam_yaw = 2.6
	var lost_t := -1.0
	for i in 600:
		s.step_ticks(1)
		scene._render(1.0 / 50.0)
		await tree.process_frame
		if scene._mission.pod_lost and lost_t < 0.0:
			lost_t = s.time()
		if lost_t >= 0.0 and s.time() - lost_t >= 0.45:
			break
	await _shot(scene, "boom_early")
	for i in 90:
		await tree.process_frame
	await _shot(scene, "boom_late")
	_log.append("boom: pod lost %s, crew %s" % [scene._mission.pod_lost, scene.crew_names])
	scene._restore_crew()


func _hangar(tree: SceneTree) -> void:
	for c in tree.root.get_children():
		if c is Node3D:
			c.queue_free()
	Sim.pending_craft_text = ""
	var hangar: Node3D = load("res://scenes/hangar.tscn").instantiate()
	tree.root.add_child(hangar)
	await _frames(tree, 5)
	hangar._on_new()
	hangar.model.place_root(CraftModel.held_single("pod_1"))
	hangar.model.attach_stack(CraftModel.held_single("tank_m"), 0, "bottom")
	hangar._frame_craft()
	hangar._held = CraftModel.held_single("engine_lifter")
	await _frames(tree, 3)
	# hover the cursor near the free bottom node
	var nodes: Array = hangar.model.free_nodes()
	for n in nodes:
		if n.node == "bottom":
			var sp: Vector2 = hangar._cam.unproject_position(hangar._craft_root.global_transform * DMath.to_v3(n.pos))
			hangar.get_viewport().warp_mouse(sp + Vector2(12, 8))
	await _frames(tree, 8)
	await _shot(hangar, "hangar_markers")


func _map(tree: SceneTree) -> void:
	var scene := await _flight(tree, "orbiter")
	var s: FlightSession = scene.session
	s.spawn_in_orbit(Sim.pending_craft_text, "floe", 90000.0, 0.0)
	scene._clear_views()
	scene._map.visible = true
	scene._map.focus_on_vessel()
	await _frames(tree, 30)
	await _shot(scene, "map_vessel")


## The Orbiter's capsule coming home fast, shield first, glowing.
func _heat(tree: SceneTree) -> void:
	var scene := await _flight(tree, "orbiter")
	var s: FlightSession = scene.session
	# just the capsule: pod, chute, reaction wheel, heat shield
	var craft: Dictionary = JSON.parse_string(Sim.pending_craft_text)
	craft.parts = craft.parts.slice(0, 4)
	var t := s.telemetry()
	var up := Vector3(t.up[0], t.up[1], t.up[2])
	var east := Vector3(t.east[0], t.east[1], t.east[2])
	var p := Vector3(t.position[0], t.position[1], t.position[2]) + up * 62000.0
	var v := east * 3100.0 - up * 700.0
	s.core.vessel_remove(s.active)
	s.active = s.core.flight_spawn_state(JSON.stringify(craft), "floe", PackedFloat64Array([p.x, p.y, p.z, v.x, v.y, v.z]))
	s.set_sas("retrograde")
	scene._clear_views()
	scene._launched = true
	scene._cam_dist = 14.0
	scene._cam_pitch = 0.15
	scene._cam_yaw = 2.0
	for i in 6000:
		s.step_ticks(1)
		await tree.process_frame
		var tt := s.telemetry()
		if tt.get("heat_temp", 0.0) > 900.0 or tt.altitude < 12000.0 or not tt.alive:
			break
	await _frames(tree, 4)
	var tt := s.telemetry()
	_log.append("heat: %s at %.0f K (%.0f%%), alt %.0f m, %.0f m/s" % [tt.get("heat_part", "?"), tt.get("heat_temp", 0.0), 100.0 * tt.get("heat_frac", 0.0), tt.altitude, tt.surface_speed])
	var feed: PackedFloat64Array = s.core.vessel_render(s.active, s.active)
	for k in feed.size() / VesselView.RPP:
		_log.append("  part %d: %.0f K (%.0f%% of headroom)" % [k, feed[k * VesselView.RPP + 11], 100.0 * feed[k * VesselView.RPP + 12]])
	await _shot(scene, "reentry_heat")


func _frames(tree: SceneTree, n: int) -> void:
	for i in n:
		await tree.process_frame


func _shot(n: Node, name: String) -> void:
	await RenderingServer.frame_post_draw
	n.get_viewport().get_texture().get_image().save_png(DIR + "fb_" + name + ".png")
	_log.append("saved fb_" + name)
