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
## Dev: a pod on Floe's tundra (env LAT, LON, CAM, PITCH), the camera low
## beside it; logs the terrain patches drawn near the camera (levels,
## overlaps, wanted vs built, vertices against the true ground) and saves
## shots plain, coloured by patch level, and without the site / terrain.
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
	scene._cam_pitch = float(OS.get_environment("PITCH")) if OS.get_environment("PITCH") != "" else 0.1
	scene._cam_dist = float(OS.get_environment("CAM")) if OS.get_environment("CAM") != "" else 18.0
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
		var lvl := int(TerrainView._parse(key)[1])
		if d < 2500.0 and node.visible:
			rows.append([d, str(TerrainView._parse(key)), lvl, aabb.size.length()])
	for key in tv._patches:
		var node: MeshInstance3D = tv._patches[key].node
		var aabb := (node.global_transform * node.get_aabb()).grow(400.0)
		if node.visible and aabb.has_point(cam.global_position) and int(TerrainView._parse(key)[1]) < 13:
			log.append("REACHES the camera: %s visible, aabb %s" % [str(TerrainView._parse(key)), aabb.size])
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
					log.append("overlap: %s over %s" % [str(TerrainView._parse(a)), str(TerrainView._parse(b))])
	log.append("%d patches drawn, %d overlapping pairs; camera %.1f m above the ground" % [vis.size(), overlaps,
		float(s.core.ground_clearance(int(s.telemetry().body_index), s.active, PackedFloat64Array(Frame.to_sim(cam.position))))])
	await RenderingServer.frame_post_draw
	scene.get_viewport().get_texture().get_image().save_png("user://dev_shots/terrain_seam.png")
	var eye: PackedFloat64Array = s.core.to_body_fixed(int(s.telemetry().body_index), s.active, PackedFloat64Array(Frame.to_sim(cam.position)))
	var leaves: PackedFloat64Array = s.core.terrain_lod(int(s.telemetry().body_index), eye, TerrainView.MAX_LEVEL, TerrainView.SPLIT)
	var missing := 0
	var hidden := 0
	for k in leaves.size() / 4:
		var key := tv._key(int(leaves[k * 4]), int(leaves[k * 4 + 1]), int(leaves[k * 4 + 2]), int(leaves[k * 4 + 3]))
		if not tv._patches.has(key):
			missing += 1
			if missing < 5:
				log.append("wanted, not built: " + str(TerrainView._parse(key)))
		elif not tv._patches[key].node.visible:
			hidden += 1
			if hidden < 5:
				log.append("wanted, hidden: " + str(TerrainView._parse(key)))
	log.append("%d wanted leaves, %d not built, %d hidden, %d built in all" % [leaves.size() / 4, missing, hidden, tv._patches.size()])
	# where each near patch's middle vertex is drawn vs the ground there
	var worst := 0.0
	var bad := 0
	var bi := int(s.telemetry().body_index)
	for key in tv._patches:
		var node: MeshInstance3D = tv._patches[key].node
		if not node.visible:
			continue
		var arr: Array = node.mesh.surface_get_arrays(0)
		var vs: PackedVector3Array = arr[Mesh.ARRAY_VERTEX]
		var g := node.global_transform * vs[(TerrainView.RES * TerrainView.RES) / 2]
		if g.distance_to(cam.global_position) > 150.0:
			continue
		var cl := 0.0
		for k in TerrainView.RES * TerrainView.RES:
			var gk := node.global_transform * vs[k]
			var ck: float = s.core.ground_clearance(bi, s.active, PackedFloat64Array(Frame.to_sim(gk)))
			if absf(ck) > absf(cl):
				cl = ck
				g = gk
		if absf(cl) > 0.5:
			bad += 1
			if bad < 8:
				log.append("misplaced %s: drawn %.2f m off the ground, %.0f m from the camera" % [key, cl, g.distance_to(cam.global_position)])
		worst = maxf(worst, absf(cl))
	log.append("near patches: worst %.3f m off the ground, %d misplaced" % [worst, bad])
	# each patch coloured by its level
	var cols := {14: Color(0.2, 0.9, 0.2), 13: Color(0.95, 0.9, 0.2), 12: Color(0.95, 0.3, 0.2)}
	for key in tv._patches:
		var lvl := int(TerrainView._parse(key)[1])
		var m := StandardMaterial3D.new()
		m.albedo_color = cols.get(lvl, Color(0.2, 0.4, 1.0)) * (0.8 + 0.2 * (hash(key) % 5) / 4.0)
		m.cull_mode = BaseMaterial3D.CULL_DISABLED
		tv._patches[key].node.material_override = m
	for i in 5:
		await tree.process_frame
	await RenderingServer.frame_post_draw
	scene.get_viewport().get_texture().get_image().save_png("user://dev_shots/terrain_seam_levels.png")
	# hide things one group at a time
	var groups := {"site": [tv._site_node], "patches": [tv]}
	for g in groups:
		for n in groups[g]:
			if n:
				n.visible = false
		for i in 3:
			await tree.process_frame
		await RenderingServer.frame_post_draw
		scene.get_viewport().get_texture().get_image().save_png("user://dev_shots/terrain_seam_no_%s.png" % g)
		for n in groups[g]:
			if n:
				n.visible = true
	log.append("site node at %s, %.0f m from the camera" % [tv._site_node.global_position if tv._site_node else "none", tv._site_node.global_position.distance_to(cam.global_position) if tv._site_node else 0.0])
	return {"status": "PASS", "log": "\n".join(log)}
