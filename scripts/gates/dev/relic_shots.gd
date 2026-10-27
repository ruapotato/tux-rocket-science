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
## Dev (GPU): a pod set down DIST m (env, default 90) from each relic, the
## camera behind it looking at the relic; saves relic_<id>.png.
extends RefCounted

const DIR := "user://dev_shots/"


func run(runner: Node) -> Dictionary:
	DirAccess.make_dir_recursive_absolute(DIR)
	var tree := runner.get_tree()
	tree.paused = false
	var log := PackedStringArray()
	var dist := float(OS.get_environment("DIST")) if OS.get_environment("DIST") != "" else 90.0
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
	for bi in core.body_count():
		var info: Dictionary = JSON.parse_string(core.body_info(bi))
		for r in info.get("relics", []):
			if OS.get_environment("ONLY") != "" and OS.get_environment("ONLY") != r.id:
				continue
			var dlat := rad_to_deg(dist / float(info.radius))
			var id: int = core.flight_place_landed(pod.save_text(), info.id, float(r.lat_deg) - dlat, float(r.lon_deg))
			s.switch_to(id)
			# wait for the morning there: the sun 30-60 degrees up
			for k in 400:
				var sp: PackedFloat64Array = core.subsolar_point(bi)
				var a := _angle(sp[0], sp[1], float(r.lat_deg), float(r.lon_deg))
				if a < 60.0 and a > 30.0:
					break
				var why := s.warp_to(s.time() + 1800.0)
				if k == 0 and why != "":
					log.append("  warp: " + why)
			scene._clear_views()
			for k in 120:
				await tree.process_frame
			var t := s.telemetry()
			var rv := Career.relic_in(t, r.id)
			scene._cam_yaw = deg_to_rad(float(rv.bearing_deg))
			scene._cam_pitch = 0.12
			scene._cam_dist = 25.0
			for k in int(OS.get_environment("WAIT")) if OS.get_environment("WAIT") != "" else 60:
				await tree.process_frame
			await RenderingServer.frame_post_draw
			scene.get_viewport().get_texture().get_image().save_png(DIR + "relic_%s.png" % r.id)
			var sp2: PackedFloat64Array = core.subsolar_point(bi)
			log.append("%s on %s: %.1f m away, heading %.1f; the sun %.0f deg from overhead" % [r.id, info.id, float(rv.surface_distance), float(rv.bearing_deg),
				_angle(sp2[0], sp2[1], float(r.lat_deg), float(r.lon_deg))])
	return {"status": "PASS", "log": "\n".join(log)}


static func _angle(la1: float, lo1: float, la2: float, lo2: float) -> float:
	var a := Vector3(cos(deg_to_rad(la1)) * cos(deg_to_rad(lo1)), cos(deg_to_rad(la1)) * sin(deg_to_rad(lo1)), sin(deg_to_rad(la1)))
	var b := Vector3(cos(deg_to_rad(la2)) * cos(deg_to_rad(lo2)), cos(deg_to_rad(la2)) * sin(deg_to_rad(lo2)), sin(deg_to_rad(la2)))
	return rad_to_deg(a.angle_to(b))


func _frames(tree: SceneTree, n: int) -> void:
	for i in n:
		await tree.process_frame
