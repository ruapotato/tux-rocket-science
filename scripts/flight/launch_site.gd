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
## Glacier Shelf Launch Complex: pad, service tower, and PASA signage.
## Built in a local frame whose +Y is the site's up direction.
class_name LaunchSite
extends RefCounted


static func build() -> Node3D:
	var root := Node3D.new()
	root.name = "LaunchSite"
	var concrete := PartVisual.mat("concrete", Color(0.55, 0.57, 0.6), 0.0, 0.9)
	var steel := PartVisual.mat("tower", Color(0.75, 0.3, 0.18), 0.3, 0.6)
	var pad := MeshInstance3D.new()
	var pm := BoxMesh.new()
	pm.size = Vector3(40, 2.0, 40)
	pad.mesh = pm
	pad.position = Vector3(0, -1.0, 0)
	pad.material_override = concrete
	root.add_child(pad)
	var apron := MeshInstance3D.new()
	var am := CylinderMesh.new()
	am.top_radius = 70
	am.bottom_radius = 74
	am.height = 1.6
	apron.mesh = am
	apron.position = Vector3(0, -1.3, 0)
	apron.material_override = PartVisual.mat("apron", Color(0.42, 0.45, 0.5), 0.0, 0.95)
	root.add_child(apron)
	# flame trench mark
	var ring := MeshInstance3D.new()
	var rm := TorusMesh.new()
	rm.inner_radius = 6.0
	rm.outer_radius = 6.6
	ring.mesh = rm
	ring.scale = Vector3(1, 0.05, 1)
	ring.position = Vector3(0, 0.02, 0)
	ring.material_override = PartVisual.mat("pad_ring", PartVisual.ORANGE)
	root.add_child(ring)
	# service tower (lattice of boxes)
	for lvl in 12:
		for corner in 4:
			var post := MeshInstance3D.new()
			var bm := BoxMesh.new()
			bm.size = Vector3(0.35, 4.0, 0.35)
			post.mesh = bm
			post.material_override = steel
			post.position = Vector3(24 + (corner % 2) * 4.0, 2.0 + lvl * 4.0, -2 + (corner / 2) * 4.0)
			root.add_child(post)
		var deck := MeshInstance3D.new()
		var dm := BoxMesh.new()
		dm.size = Vector3(4.4, 0.25, 4.4)
		deck.mesh = dm
		deck.material_override = steel
		deck.position = Vector3(26, 4.0 + lvl * 4.0, 0)
		root.add_child(deck)
	var sign := Label3D.new()
	sign.text = "PASA"
	sign.font_size = 512
	sign.pixel_size = 0.02
	sign.modulate = Color(1, 1, 1)
	sign.outline_size = 24
	sign.position = Vector3(26, 52, 2.4)
	root.add_child(sign)
	var sub := Label3D.new()
	sub.text = "GLACIER SHELF LAUNCH COMPLEX"
	sub.font_size = 96
	sub.pixel_size = 0.02
	sub.position = Vector3(0, 0.9, 21)
	sub.rotation = Vector3(-PI / 2, 0, 0)
	sub.modulate = Color(1.0, 0.55, 0.15)
	root.add_child(sub)
	return root


## Site centre in the body-fixed frame, Godot axes.
static func site_fixed_godot(core: TrsSim, body: int) -> Vector3:
	var info: Dictionary = JSON.parse_string(core.body_info(body))
	var s: Dictionary = info.get("site", {})
	if s.is_empty():
		return Vector3.ZERO
	var lat := deg_to_rad(s.lat_deg)
	var lon := deg_to_rad(s.lon_deg)
	var r: float = info.radius + s.altitude
	return Frame.vec_arr([r * cos(lat) * cos(lon), r * cos(lat) * sin(lon), r * sin(lat)])


## Local frame at the site: +Y up, +X east (Godot axes, body-fixed).
static func site_basis(site_pos: Vector3) -> Basis:
	var up := site_pos.normalized()
	var east := Vector3.UP.cross(up).normalized()  # sim +Z is Godot +Y
	var north := up.cross(east)
	return Basis(east, up, -north)
