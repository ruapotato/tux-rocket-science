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
extends RefCounted


func run(runner: Node) -> Dictionary:
	var tree := runner.get_tree()
	tree.paused = false
	if tree.current_scene:
		tree.current_scene.queue_free()
	Sim.pending_craft_text = FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "hopper.craft.json")
	var flight: Node3D = load("res://scenes/flight.tscn").instantiate()
	tree.root.add_child(flight)
	await tree.process_frame
	var s: FlightSession = flight.session
	var core := s.core
	var sub: PackedFloat64Array = core.subsolar_point(core.body_find("krill"))
	s.active = core.flight_place_landed(Sim.pending_craft_text, "krill", sub[0] + 8.0, sub[1] + 30.0)
	flight._clear_views()
	var out := PackedStringArray()
	for cfg in [[0.9, 400.0, "above"], [0.35, 60.0, "low"]]:
		flight._cam_pitch = cfg[0]
		flight._cam_dist = cfg[1]
		for i in 150:
			await tree.process_frame
		await RenderingServer.frame_post_draw
		tree.root.get_texture().get_image().save_png("user://dev_shots/krill_%s.png" % cfg[2])
		var vis := 0
		for p in flight._terrain._patches.values():
			vis += int(p.node.visible)
		out.append("%s: %d patches (%d visible)" % [cfg[2], flight._terrain._patches.size(), vis])
	var any: Dictionary = flight._terrain._patches.values()[0]
	var mi: MeshInstance3D = any.node
	var arr: Array = mi.mesh.surface_get_arrays(0)
	var cols: PackedColorArray = arr[Mesh.ARRAY_COLOR]
	out.append("colors: %d, first %s, mid %s; material vc=%s srgb=%s albedo=%s" % [cols.size(), cols[0], cols[cols.size() / 2],
		mi.material_override.vertex_color_use_as_albedo, mi.material_override.vertex_color_is_srgb, mi.material_override.albedo_color])
	out.append("sun energy %.2f, ambient %.2f, sun basis z %s" % [flight._sun.light_energy, flight._env.ambient_light_energy, flight._sun.global_transform.basis.z])
	var nrm: PackedVector3Array = arr[Mesh.ARRAY_NORMAL]
	out.append("normal sample %s, len %.3f" % [nrm[100], nrm[100].length()])
	return {"status": "PASS", "log": "\n".join(out)}
