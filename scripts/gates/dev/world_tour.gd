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
## Dev (GPU): a tour of each world's ground -- a pod set down in each of its
## biomes, in daylight, and the view from beside it (ground level) and from
## above (aerial). Saves tour_<body>_<biome>_<ground|air>.png. Env BODIES
## (comma list, default all with ground), PER (biomes a body, default 5),
## WAIT (frames before each shot), AIR (camera distance up high, default 900).
extends RefCounted

const DIR := "user://dev_shots/"


func run(runner: Node) -> Dictionary:
	DirAccess.make_dir_recursive_absolute(DIR)
	var tree := runner.get_tree()
	tree.paused = false
	var log := PackedStringArray()
	var pod := CraftModel.new()
	pod.place_root(CraftModel.held_single("pod_1"))
	pod.attach_stack(CraftModel.held_single("tank_s"), 0, "bottom")
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
	var all := "floe,krill,kelp,pebble,narwhal,tusk,cinder,haven,wisp"
	var bodies := (OS.get_environment("BODIES") if OS.get_environment("BODIES") != "" else all).split(",")
	var per := int(OS.get_environment("PER")) if OS.get_environment("PER") != "" else 5
	var wait := int(OS.get_environment("WAIT")) if OS.get_environment("WAIT") != "" else 160
	var air := float(OS.get_environment("AIR")) if OS.get_environment("AIR") != "" else 900.0
	var rng := RandomNumberGenerator.new()
	rng.seed = 11
	for bid in bodies:
		var bi: int = core.body_find(bid)
		if bi < 0:
			log.append("%s: no such body" % bid)
			continue
		# a spot in each biome: random places until each has one (or, with
		# LANDMARKS set, beside each landmark, looking at it)
		var spots := {}
		if OS.get_environment("LANDMARKS") != "":
			for lm in Sim.landmarks(bid):
				var R: float = JSON.parse_string(core.body_info(bi)).radius
				var at := {"volcano": float(OS.get_environment("VOLC_AT")) if OS.get_environment("VOLC_AT") != "" else 0.5, "crater": 1.0, "mesa": 1.05}
				var reach := float(lm.get("radius_km", 6.0)) * 1000.0 * float(at.get(lm.kind, 0.0)) + (float(lm.get("width_km", 3.0)) * 900.0 if lm.kind == "canyon" else 0.0)
				spots[lm.id] = [float(lm.lat_deg) - rad_to_deg(reach / R), float(lm.lon_deg), lm.name]
			per = 0
		for tries in (1500 if per > 0 else 0):
			var lat := rad_to_deg(asin(rng.randf() * 2.0 - 1.0))
			var lon := rng.randf() * 360.0 - 180.0
			var key := str(core.biome_at(bi, lat, lon))
			if not spots.has(key):
				spots[key] = [lat, lon, key]
			if spots.size() >= per:
				break
		for key in spots:
			var lat: float = spots[key][0]
			var lon: float = spots[key][1]
			var id: int = core.flight_place_landed(pod.save_text(), bid, lat, lon)
			s.switch_to(id)
			for q in 400:
				var sp: PackedFloat64Array = core.subsolar_point(bi)
				var ang := _angle(sp[0], sp[1], lat, lon)
				if ang < 60.0 and ang > 30.0:
					break
				s.warp_to(s.time() + 1800.0)
			for view in ["ground", "air"]:
				scene._clear_views()
				var lmk := OS.get_environment("LANDMARKS") != ""
				scene._cam_yaw = 0.6 if not lmk else 0.0     # (a landmark: looking north, at it)
				scene._cam_pitch = 0.08 if view == "ground" else (0.55 if not lmk else 0.62)
				scene._cam_dist = 22.0 if view == "ground" else air
				for q in wait:
					await tree.process_frame
				await RenderingServer.frame_post_draw
				if OS.get_environment("FXDBG") != "":
					for r in scene._terrain._fx:
						var fw: Vector3 = -scene._cam.global_transform.basis.z
						var to: Vector3 = (r.node.global_position - scene._cam.global_position).normalized()
						var upv: Vector3 = scene._cam.global_transform.basis.y
						log.append("fx dist %.0f, angle off view %.1f deg, up-component %.2f, visible %s, in tree %s" % [r.node.global_position.distance_to(scene._cam.global_position), rad_to_deg(fw.angle_to(to)), to.dot(upv), r.node.is_visible_in_tree(), r.node.is_inside_tree()])
				scene.get_viewport().get_texture().get_image().save_png(DIR + "tour_%s_%s_%s.png" % [bid, key, view])
			log.append("%s %s (%s) at %.2f, %.2f: saved" % [bid, key, spots[key][2], lat, lon])
			core.vessel_remove(id)
	return {"status": "PASS", "log": "\n".join(log)}


static func _angle(la1: float, lo1: float, la2: float, lo2: float) -> float:
	var a := Vector3(cos(deg_to_rad(la1)) * cos(deg_to_rad(lo1)), cos(deg_to_rad(la1)) * sin(deg_to_rad(lo1)), sin(deg_to_rad(la1)))
	var b := Vector3(cos(deg_to_rad(la2)) * cos(deg_to_rad(lo2)), cos(deg_to_rad(la2)) * sin(deg_to_rad(lo2)), sin(deg_to_rad(la2)))
	return rad_to_deg(a.angle_to(b))


func _frames(tree: SceneTree, n: int) -> void:
	for i in n:
		await tree.process_frame
