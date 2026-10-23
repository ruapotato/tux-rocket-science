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
## Sim <-> Godot frame mapping. trs_core is right-handed with +Z as the
## reference-plane normal; Godot is Y-up. G(x, y, z) = (x, z, -y) is a proper
## rotation, so quaternions map as (qx, qz, -qy, qw).
## Only relative (floating-origin) values ever reach these helpers.
class_name Frame
extends RefCounted


static func vec(a: PackedFloat64Array, o: int) -> Vector3:
	return Vector3(a[o], a[o + 2], -a[o + 1])


static func vec_arr(a: Array) -> Vector3:
	return Vector3(a[0], a[2], -a[1])


## For directions/rotations *within* the sim frame (conjugated).
static func quat(a: PackedFloat64Array, o: int) -> Quaternion:
	return Quaternion(a[o], a[o + 2], -a[o + 1], a[o + 3])


## Basis that takes a part/vessel-local vector (Y-up craft frame, as meshes
## are built) to Godot world axes: G * R_sim. Use this for visuals.
static func local_basis(a: PackedFloat64Array, o: int) -> Basis:
	return G * Basis(Quaternion(a[o], a[o + 1], a[o + 2], a[o + 3]))


const G := Basis(Vector3(1, 0, 0), Vector3(0, 0, -1), Vector3(0, 1, 0))


## Godot basis for a rotation of `angle` about the sim +Z axis.
static func spin(angle: float) -> Basis:
	return Basis(Vector3.UP, angle)


## Converts a Godot-space vector back to sim axes (for commands).
static func to_sim(v: Vector3) -> Array:
	return [v.x, -v.z, v.y]
