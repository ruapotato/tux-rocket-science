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


## Distance (double) and unit direction of a vector at o, less `minus`:
## safe however far away it is (a float32 Vector3 squares to infinity
## past about 1e19 m).
static func dist_dir(a: PackedFloat64Array, o: int, minus := Vector3.ZERO) -> Array:
	var x: float = a[o] - minus.x
	var y: float = a[o + 2] - minus.y
	var z: float = -a[o + 1] - minus.z
	var d := sqrt(x * x + y * y + z * z)
	return [d, Vector3(x / d, y / d, z / d) if d > 0.0 else Vector3.ZERO]


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


## Basis b with its own axes stretched by s (b.scaled stretches the world's:
## a tilted strut or beam comes out the wrong length).
static func scaled_local(b: Basis, s: Vector3) -> Basis:
	return Basis(b.x * s.x, b.y * s.y, b.z * s.z)


## Converts a Godot-space vector back to sim axes (for commands).
static func to_sim(v: Vector3) -> Array:
	return [v.x, -v.z, v.y]
