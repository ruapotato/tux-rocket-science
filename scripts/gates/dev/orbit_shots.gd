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
## Dev (GPU): each world from orbit -- a pod in orbit round it, the camera
## pulled back so the world fills the view, lit from the side. Saves
## orbit_<body>.png. Env BODIES (comma list), ALT (orbit height in body
## radii, default 1.2), DIST (camera distance in radii, default 0.9),
## YAW / PITCH (camera), WAIT (frames before the shot); ABS_ALT (m) and
## CAM_M (camera distance, m) for a close look from inside the air; MAP
## (a map distance) also saves map_<body>.png, the map on its parent.
extends RefCounted

const DIR := "user://dev_shots/"


func run(runner: Node) -> Dictionary:
	DirAccess.make_dir_recursive_absolute(DIR)
	var tree := runner.get_tree()
	tree.paused = false
	var log := PackedStringArray()
	var pod := CraftModel.new()
	pod.place_root(CraftModel.held_single("pod_1"))
	if tree.current_scene:
		tree.current_scene.queue_free()
	await _frames(tree, 2)
	Sim.tutorial = false
	Sim.pending_craft_text = pod.save_text()
	var scene: Node3D = load("res://scenes/flight.tscn").instantiate()
	tree.root.add_child(scene)
	await _frames(tree, 5)
	scene.hud.hide_help()
	scene.hud.visible = OS.get_environment("HUD") != ""
	var s: FlightSession = scene.session
	var core: TrsSim = s.core
	var env := func(k: String, d: float) -> float: return float(OS.get_environment(k)) if OS.get_environment(k) != "" else d
	var bodies := OS.get_environment("BODIES").split(",") if OS.get_environment("BODIES") != "" else PackedStringArray(["gloam", "soot", "maelstrom", "floss", "selkie", "barnacle", "saffron", "grit", "mere", "reed", "cairn"])
	for bid in bodies:
		var bi: int = core.body_find(bid)
		if bi < 0:
			log.append("%s: no such body" % bid)
			continue
		var info: Dictionary = JSON.parse_string(core.body_info(bi))
		var R: float = info.radius
		var alt: float = maxf(R * float(env.call("ALT", 1.2)), float(info.get("atmosphere_height", 0.0)) + 20000.0)
		if OS.get_environment("ABS_ALT") != "":
			alt = float(env.call("ABS_ALT", 0.0))
		var id: int = core.flight_spawn_orbit(pod.save_text(), bid, alt, float(env.call("INC", 0.0)))
		if id < 0:
			log.append("%s: no orbit" % bid)
			continue
		s.switch_to(id)
		# round to the day side: the pod over ground the sun is high above
		for q in 200:
			var t := s.telemetry()
			var sp: PackedFloat64Array = core.subsolar_point(bi)
			if _angle(sp[0], sp[1], float(t.get("lat_deg", 0.0)), float(t.get("lon_deg", 0.0))) < float(env.call("SUN", 35.0)):
				break
			s.warp_to(s.time() + 120.0)
			await tree.process_frame
		scene._clear_views()
		scene._cam_yaw = float(env.call("YAW", 0.0))
		scene._cam_pitch = float(env.call("PITCH", 1.1))
		scene._cam_dist = R * float(env.call("DIST", 0.9)) if OS.get_environment("CAM_M") == "" else float(env.call("CAM_M", 30.0))
		for q in int(env.call("WAIT", 200)):
			await tree.process_frame
		await RenderingServer.frame_post_draw
		scene.get_viewport().get_texture().get_image().save_png(DIR + "orbit_%s.png" % bid)
		if OS.get_environment("MAP") != "":
			# and the map there, zoomed out to the system (MAP = distance)
			scene._map.visible = true
			scene._map.focus_on_vessel()
			scene._map.focus_vessel = false
			scene._map.focus_body = int(info.parent)
			scene._map._dist = float(env.call("MAP", 30.0)) * maxf(1.0, float(JSON.parse_string(core.body_info(int(info.parent))).radius) * MapView.SCALE * 4.0)
			for q in 60:
				await tree.process_frame
			await RenderingServer.frame_post_draw
			scene.get_viewport().get_texture().get_image().save_png(DIR + "map_%s.png" % bid)
			scene._map.visible = false
		log.append("%s: saved (orbit %.0f km)" % [bid, alt / 1000.0])
		core.vessel_remove(id)
	return {"status": "PASS", "log": "\n".join(log)}


static func _angle(la1: float, lo1: float, la2: float, lo2: float) -> float:
	var a := Vector3(cos(deg_to_rad(la1)) * cos(deg_to_rad(lo1)), cos(deg_to_rad(la1)) * sin(deg_to_rad(lo1)), sin(deg_to_rad(la1)))
	var b := Vector3(cos(deg_to_rad(la2)) * cos(deg_to_rad(lo2)), cos(deg_to_rad(la2)) * sin(deg_to_rad(lo2)), sin(deg_to_rad(la2)))
	return rad_to_deg(a.angle_to(b))


func _frames(tree: SceneTree, n: int) -> void:
	for i in n:
		await tree.process_frame
