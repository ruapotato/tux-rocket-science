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
## Dev: view-relative steering. From several camera angles, "right" must
## swing the nose toward the screen's right and "away" must tip it away
## from the camera (or down the screen when it already points away).
extends RefCounted


func run(runner: Node) -> Dictionary:
	var tree := runner.get_tree()
	tree.paused = false
	if tree.current_scene:
		tree.current_scene.queue_free()
	Sim.pending_craft_text = FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + "orbiter.craft.json")
	var scene: Node3D = load("res://scenes/flight.tscn").instantiate()
	tree.root.add_child(scene)
	await tree.process_frame
	var s: FlightSession = scene.session
	var log := PackedStringArray()
	var bad := 0
	for yaw in [0.0, 1.2, 2.5, -2.0]:
		for pitch in [0.1, 0.8, -0.6]:
			for mode in ["right", "away"]:
				s.spawn_in_orbit(Sim.pending_craft_text, "floe", 120000.0, 0.0)
				s.set_sas("off")
				scene._clear_views()
				scene._cam_yaw = yaw
				scene._cam_pitch = pitch
				scene._render(0.016)
				var cam: Camera3D = scene._cam
				var t0 := s.telemetry()
				var n0 := Frame.vec_arr(t0.nose)
				var pr: Vector2 = scene._view_steer(1.0 if mode == "away" else 0.0, 1.0 if mode == "right" else 0.0)
				s.set_input(pr.x, pr.y, 0.0)
				s.step_ticks(25)
				s.set_input(0, 0, 0)
				var n1 := Frame.vec_arr(s.telemetry().nose)
				var dn := n1 - n0
				var want: Vector3
				var fd := n0.dot(-cam.global_basis.z)
				if mode == "right":
					want = cam.global_basis.x
					if absf(n0.dot(want)) > 0.9:
						continue  # already pointing right: nothing to swing toward
				else:
					# away from the camera; down the screen if it already points
					# away, up the screen if it points at the camera
					want = -cam.global_basis.z if absf(fd) < 0.8 else (-cam.global_basis.y if fd > 0.0 else cam.global_basis.y)
				var ok := dn.normalized().dot(want - n0 * n0.dot(want)) > 0.2
				if mode == "away" and absf(fd) > 0.5 and absf(fd) < 0.95:
					# blending between "into the screen" and down/up-screen: either is right
					var alt := (-cam.global_basis.y if fd > 0.0 else cam.global_basis.y)
					ok = ok or dn.normalized().dot(alt - n0 * n0.dot(alt)) > 0.2
				bad += 0 if ok else 1
				log.append("%s yaw %+.1f pitch %+.1f %-5s (nose·fwd %+.2f): nose moved %.3f, along wanted %+.2f" % ["ok  " if ok else "FAIL", yaw, pitch, mode, fd,
					dn.length(), dn.normalized().dot((want - n0 * n0.dot(want)).normalized())])
	return {"status": "PASS" if bad == 0 else "FAIL", "log": "\n".join(log)}
