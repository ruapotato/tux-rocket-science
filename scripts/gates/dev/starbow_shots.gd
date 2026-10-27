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
## Dev (GPU): the sky between the stars at several speeds, looking along
## the motion (and one looking back): aberration, Doppler colour and
## brightness from the relativistic star shader. FOV= camera field of view.
extends RefCounted


func run(runner: Node) -> Dictionary:
	var tree := runner.get_tree()
	tree.paused = false
	for c in tree.root.get_children():
		if c is Node3D or (c is Control and c.name != "GateRunner"):
			if c != tree.root.get_child(0):
				c.queue_free()
	var sky := ScaledSpace.new()
	sky.setup(Sim.core)
	tree.root.add_child(sky)
	sky.size = Vector2i(800, 450)
	var fov := float(OS.get_environment("FOV")) if OS.get_environment("FOV") != "" else 90.0
	sky.cam.fov = fov
	if OS.get_environment("FAR") != "":
		sky.cam.far = float(OS.get_environment("FAR"))
	var log := PackedStringArray()
	log.append("cam near %s far %s" % [sky.cam.near, sky.cam.far])
	var feed: PackedFloat64Array = PackedFloat64Array()
	feed.resize(Sim.core.body_count() * 5)
	for i in feed.size():
		feed[i] = 1.0e20  # every real star far off
	for b in sky._bodies:  # only the stars: no bodies placed here
		b.node.visible = false
		if b.atmo:
			b.atmo.visible = false
		if b.clouds:
			b.clouds.visible = false
	var fwd := Vector3(0, 0, -1)
	var shots := [[0.0, false], [0.5, false], [0.9, false], [0.99, false], [0.999, false], [0.9, true]]
	var images: Array[Image] = []
	for sh in shots:
		var b: float = sh[0]
		var g := 1.0 / sqrt(1.0 - b * b)
		sky.cam.global_transform = Transform3D(Basis.looking_at(fwd if not sh[1] else -fwd, Vector3.UP), Vector3.ZERO)
		sky.set_motion(fwd, b, g, feed)
		(sky._sky_mat as ShaderMaterial).set_shader_parameter("density", 0.0)
		(sky._sky_mat as ShaderMaterial).set_shader_parameter("sun_dir", Vector3(0, -1, 0))  # no sun in the frame
		for k in 4:
			await tree.process_frame
		await RenderingServer.frame_post_draw
		var img := sky.get_texture().get_image()
		images.append(img)
		var lit := 0
		for y in range(0, img.get_height(), 2):
			for x in range(0, img.get_width(), 2):
				var c := img.get_pixel(x, y)
				if c.r + c.g + c.b > 0.15:
					lit += 1
		log.append("beta %.3f%s: %d lit samples; cam forward %s" % [b, " (looking back)" if sh[1] else "", lit, -sky.cam.global_transform.basis.z])
	for i in images.size():
		images[i].save_png("user://dev_shots/starbow_%d.png" % i)
	sky.queue_free()
	return {"status": "PASS", "log": "\n".join(log)}
