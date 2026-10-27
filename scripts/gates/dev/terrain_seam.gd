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
## Dev: a pod on Floe's tundra (env LAT, LON), the camera low beside it;
## logs the terrain patches drawn near the camera (levels, overlaps) to
## track down a seam.
extends RefCounted


func run(runner: Node) -> Dictionary:
	var tree := runner.get_tree()
	tree.paused = false
	var log := PackedStringArray()
	var pod := CraftModel.new()
	pod.place_root(CraftModel.held_single("pod_1"))
	for c in tree.root.get_children():
		if c is Node3D or (c is Control and c.name != "GateRunner"):
			if c != tree.root.get_child(0):
				c.queue_free()
	await tree.process_frame
	Sim.pending_craft_text = pod.save_text()
	var scene: Node3D = load("res://scenes/flight.tscn").instantiate()
	tree.root.add_child(scene)
	for i in 5:
		await tree.process_frame
	scene.hud.hide_help()
	var s: FlightSession = scene.session
	var lat := float(OS.get_environment("LAT")) if OS.get_environment("LAT") != "" else 53.160
	var lon := float(OS.get_environment("LON")) if OS.get_environment("LON") != "" else 101.998
	var id: int = s.core.flight_place_landed(pod.save_text(), "floe", lat - rad_to_deg(30.0 / 600000.0), lon)
	s.switch_to(id)
	scene._clear_views()
	scene._cam_yaw = 0.0
	scene._cam_pitch = 0.1
	scene._cam_dist = 18.0
	for i in 300:
		await tree.process_frame
	var tv: TerrainView = scene._terrain
	var cam: Camera3D = scene._cam
	var rows := []
	for key in tv._patches:
		var p: Dictionary = tv._patches[key]
		var node: MeshInstance3D = p.node
		var aabb := node.global_transform * node.get_aabb()
		var d := aabb.get_center().distance_to(cam.global_position)
		var lvl := int(key.split("/")[1])
		if d < 2500.0 and node.visible:
			rows.append([d, key, lvl, aabb.size.length()])
	rows.sort()
	for r in rows.slice(0, 40):
		log.append("%7.0f m  %s  level %d  aabb %.0f" % [r[0], r[1], r[2], r[3]])
	# drawn patches overlapping each other (an ancestor and its descendant both visible)
	var vis := []
	for key in tv._patches:
		if tv._patches[key].node.visible:
			vis.append(key)
	var overlaps := 0
	for a in vis:
		var pa := TerrainView._parse(a)
		for b in vis:
			var pb := TerrainView._parse(b)
			if a == b or pa[0] != pb[0] or pb[1] <= pa[1]:
				continue
			var sh: int = pb[1] - pa[1]
			if (pb[2] >> sh) == pa[2] and (pb[3] >> sh) == pa[3]:
				overlaps += 1
				if overlaps < 6:
					log.append("overlap: %s over %s" % [a, b])
	log.append("%d patches drawn, %d overlapping pairs; camera %.1f m above the ground" % [vis.size(), overlaps,
		float(s.core.ground_clearance(int(s.telemetry().body_index), s.active, PackedFloat64Array(Frame.to_sim(cam.position))))])
	await RenderingServer.frame_post_draw
	scene.get_viewport().get_texture().get_image().save_png("user://dev_shots/terrain_seam.png")
	return {"status": "PASS", "log": "\n".join(log)}
