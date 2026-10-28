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
## Dev (GPU): frame times near the ground -- for each body a pod landed in
## daylight (the camera close, then swung round), then one in a low orbit
## (ALT m, default 3000) racing over the ground so patches keep coming.
## Reports mean / 95th percentile / worst frame (ms) for each. Env BODIES,
## FRAMES (per run, default 400).
extends RefCounted


func run(runner: Node) -> Dictionary:
	var tree := runner.get_tree()
	tree.paused = false
	var log := PackedStringArray()
	var pod := CraftModel.new()
	pod.place_root(CraftModel.held_single("pod_1"))
	if tree.current_scene:
		tree.current_scene.queue_free()
	await tree.process_frame
	Sim.tutorial = false
	Sim.pending_craft_text = pod.save_text()
	var scene: Node3D = load("res://scenes/flight.tscn").instantiate()
	tree.root.add_child(scene)
	for i in 5:
		await tree.process_frame
	scene.hud.hide_help()
	var s: FlightSession = scene.session
	var core: TrsSim = s.core
	var frames := int(OS.get_environment("FRAMES")) if OS.get_environment("FRAMES") != "" else 400
	var alt := float(OS.get_environment("ALT")) if OS.get_environment("ALT") != "" else 3000.0
	var bodies := (OS.get_environment("BODIES") if OS.get_environment("BODIES") != "" else "kelp,haven,saffron,krill").split(",")
	for bid in bodies:
		var bi: int = core.body_find(bid)
		for mode in ["landed", "settled", "low"]:
			var id: int
			if mode != "low":
				id = core.flight_place_landed(pod.save_text(), bid, 20.0, 30.0)
			else:
				var info: Dictionary = JSON.parse_string(core.body_info(bi))
				id = core.flight_spawn_orbit(pod.save_text(), bid, maxf(alt, float(info.get("atmosphere_height", 0.0)) * 0.0 + alt), 0.0)
			s.switch_to(id)
			scene._clear_views()
			scene._cam_dist = 25.0
			scene._cam_pitch = 0.1
			for i in 120:
				await tree.process_frame
			if mode == "settled":
				# till the ground and its life are all built
				for i in 3000:
					var tv: TerrainView = scene._terrain
					var done := true
					for k in tv._want:
						if not tv._patches.has(k):
							done = false
							break
					if done and tv.life._todo.is_empty():
						break
					await tree.process_frame
			var vp := scene.get_viewport().get_viewport_rid()
			RenderingServer.viewport_set_measure_render_time(vp, true)
			var proc := 0.0
			var gpu := 0.0
			var cpu_r := 0.0
			var draws := 0.0
			var ts := PackedFloat64Array()
			var last := Time.get_ticks_usec()
			for i in frames:
				scene._cam_yaw = float(i) * 0.02
				await tree.process_frame
				var t := Time.get_ticks_usec()
				ts.append((t - last) / 1000.0)
				proc += Performance.get_monitor(Performance.TIME_PROCESS) * 1000.0
				gpu += RenderingServer.viewport_get_measured_render_time_gpu(vp)
				cpu_r += RenderingServer.viewport_get_measured_render_time_cpu(vp) + RenderingServer.get_frame_setup_time_cpu()
				draws += Performance.get_monitor(Performance.RENDER_TOTAL_DRAW_CALLS_IN_FRAME)
				last = t
			var mean := 0.0
			for v in ts:
				mean += v
			mean /= ts.size()
			ts.sort()
			log.append("%-8s %-6s mean %5.1f ms  p95 %5.1f  worst %5.1f  (%d patches) process %.1f, render cpu %.1f, gpu %.1f, %d draws" % [bid, mode, mean, ts[int(ts.size() * 0.95)], ts[ts.size() - 1], scene._terrain._patches.size(), proc / frames, cpu_r / frames, gpu / frames, int(draws / frames)])
			core.vessel_remove(id)
	return {"status": "PASS", "log": "\n".join(log)}
