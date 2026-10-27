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
## Dev (GPU): a pod set down near life on each world, in daylight, the
## camera looking at it; saves life_<body>_<kind>.png. Env BODIES (comma
## list), KINDS (kind ids to look for, comma list), DIST (m from the find,
## default 30), CAM (camera distance, default 18), PITCH.
extends RefCounted

const DIR := "user://dev_shots/"


func run(runner: Node) -> Dictionary:
	DirAccess.make_dir_recursive_absolute(DIR)
	var tree := runner.get_tree()
	tree.paused = false
	var log := PackedStringArray()
	var dist := float(OS.get_environment("DIST")) if OS.get_environment("DIST") != "" else 30.0
	var pod := CraftModel.new()
	pod.place_root(CraftModel.held_single("pod_1"))
	pod.attach_stack(CraftModel.held_single("tank_s"), 0, "bottom")
	for c in tree.root.get_children():
		if c is Node3D or (c is Control and c.name != "GateRunner"):
			if c != tree.root.get_child(0):
				c.queue_free()
	if tree.current_scene:
		tree.current_scene.queue_free()
	await _frames(tree, 2)
	Sim.tutorial = false
	Sim.pending_craft_text = pod.save_text()
	var scene: Node3D = load("res://scenes/flight.tscn").instantiate()
	tree.root.add_child(scene)
	await _frames(tree, 5)
	scene.hud.hide_help()
	var s: FlightSession = scene.session
	var core: TrsSim = s.core
	var bodies := (OS.get_environment("BODIES") if OS.get_environment("BODIES") != "" else "floe,kelp,haven,narwhal,krill").split(",")
	var only := OS.get_environment("KINDS").split(",", false)
	var rng := RandomNumberGenerator.new()
	rng.seed = 4
	for bid in bodies:
		var bi: int = core.body_find(bid)
		var info: Dictionary = JSON.parse_string(core.body_info(bi))
		var kinds: Array = Sim.life(bid)
		for k in kinds.size():
			var kid: String = kinds[k].id
			if not only.is_empty() and not kid in only:
				continue
			if only.is_empty() and not kinds[k].get("animal", false) and kinds[k].get("shape", "") in ["grass", "flower", "shrub"]:
				continue
			# find one: random spots until a cell holds some
			var found := []
			for tries in 3000:
				var z := rng.randf() * 2.0 - 1.0
				var a := rng.randf() * TAU
				var r := sqrt(1.0 - z * z)
				var d := PackedFloat64Array([r * cos(a), r * sin(a), z])
				if info.has("site") and tries % 2 == 0:
					# half the looks within 15 km of the site (small biomes there)
					var la := deg_to_rad(float(info.site.lat_deg)) + (rng.randf() - 0.5) * 30000.0 / float(info.radius)
					var lo := deg_to_rad(float(info.site.lon_deg)) + (rng.randf() - 0.5) * 30000.0 / float(info.radius)
					d = PackedFloat64Array([cos(la) * cos(lo), cos(la) * sin(lo), sin(la)])
				var cells: PackedFloat64Array = core.life_cells(bi, d, 1.0)
				if cells.size() < 4:
					continue
				var feed: PackedFloat64Array = core.life_cell(bi, cells.slice(0, 4), 1 << k)
				if int(feed[3]) > 0:
					found = [feed[0] + feed[5], feed[1] + feed[6], feed[2] + feed[7]]
					break
			if found.is_empty():
				log.append("%s %s: none found" % [bid, kid])
				continue
			var l := sqrt(found[0] * found[0] + found[1] * found[1] + found[2] * found[2])
			var lat := rad_to_deg(asin(found[2] / l))
			var lon := rad_to_deg(atan2(found[1], found[0]))
			var dlat := rad_to_deg(dist / float(info.radius))
			var id: int = core.flight_place_landed(pod.save_text(), bid, lat - dlat, lon)
			s.switch_to(id)
			for q in 400:
				var sp: PackedFloat64Array = core.subsolar_point(bi)
				var ang := _angle(sp[0], sp[1], lat, lon)
				if ang < 55.0 and ang > 25.0:
					break
				s.warp_to(s.time() + 1800.0)
			scene._clear_views()
			scene._cam_yaw = 0.0  # looking north, at it
			scene._cam_pitch = float(OS.get_environment("PITCH")) if OS.get_environment("PITCH") != "" else 0.1
			scene._cam_dist = float(OS.get_environment("CAM")) if OS.get_environment("CAM") != "" else 18.0
			for q in (int(OS.get_environment("WAIT")) if OS.get_environment("WAIT") != "" else 200):
				await tree.process_frame
			await RenderingServer.frame_post_draw
			scene.get_viewport().get_texture().get_image().save_png(DIR + "life_%s_%s.png" % [bid, kid])
			var t := s.telemetry()
			log.append("%s %s at %.3f, %.3f (%s): saved" % [bid, kid, lat, lon, t.get("biome_name", "?")])
			core.vessel_remove(id)
	return {"status": "PASS", "log": "\n".join(log)}


static func _angle(la1: float, lo1: float, la2: float, lo2: float) -> float:
	var a := Vector3(cos(deg_to_rad(la1)) * cos(deg_to_rad(lo1)), cos(deg_to_rad(la1)) * sin(deg_to_rad(lo1)), sin(deg_to_rad(la1)))
	var b := Vector3(cos(deg_to_rad(la2)) * cos(deg_to_rad(lo2)), cos(deg_to_rad(la2)) * sin(deg_to_rad(lo2)), sin(deg_to_rad(la2)))
	return rad_to_deg(a.angle_to(b))


func _frames(tree: SceneTree, n: int) -> void:
	for i in n:
		await tree.process_frame
