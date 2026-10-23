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
## Double-precision vector/quaternion helpers. Godot's Vector3/Quaternion are
## single precision in stock builds, so authoritative data (craft geometry)
## stays in plain float arrays: vectors [x, y, z], quaternions [x, y, z, w].
class_name DMath
extends RefCounted


static func add(a: Array, b: Array) -> Array:
	return [a[0] + b[0], a[1] + b[1], a[2] + b[2]]


static func sub(a: Array, b: Array) -> Array:
	return [a[0] - b[0], a[1] - b[1], a[2] - b[2]]


static func scale(a: Array, s: float) -> Array:
	return [a[0] * s, a[1] * s, a[2] * s]


static func dot(a: Array, b: Array) -> float:
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


static func cross(a: Array, b: Array) -> Array:
	return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]


static func length(a: Array) -> float:
	return sqrt(dot(a, a))


static func normalized(a: Array) -> Array:
	var l := length(a)
	return scale(a, 1.0 / l) if l > 0.0 else [0.0, 0.0, 0.0]


static func q_identity() -> Array:
	return [0.0, 0.0, 0.0, 1.0]


static func q_mul(a: Array, b: Array) -> Array:
	return [
		a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
		a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
		a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
		a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2],
	]


static func q_conj(q: Array) -> Array:
	return [-q[0], -q[1], -q[2], q[3]]


static func q_normalized(q: Array) -> Array:
	var l := sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3])
	return [q[0] / l, q[1] / l, q[2] / l, q[3] / l]


static func q_rotate(q: Array, v: Array) -> Array:
	var u := [q[0], q[1], q[2]]
	var t := scale(cross(u, v), 2.0)
	return add(add(v, scale(t, q[3])), cross(u, t))


static func q_axis_angle(axis: Array, angle: float) -> Array:
	var a := normalized(axis)
	var s := sin(0.5 * angle)
	return [a[0] * s, a[1] * s, a[2] * s, cos(0.5 * angle)]


## Quaternion whose rotated basis is (x, y, z) (orthonormal, right-handed).
static func q_from_basis(x: Array, y: Array, z: Array) -> Array:
	var m00: float = x[0]
	var m11: float = y[1]
	var m22: float = z[2]
	var tr := m00 + m11 + m22
	var q: Array
	if tr > 0.0:
		var s := sqrt(tr + 1.0) * 2.0
		q = [(y[2] - z[1]) / s, (z[0] - x[2]) / s, (x[1] - y[0]) / s, 0.25 * s]
	elif m00 > m11 and m00 > m22:
		var s := sqrt(1.0 + m00 - m11 - m22) * 2.0
		q = [0.25 * s, (y[0] + x[1]) / s, (z[0] + x[2]) / s, (y[2] - z[1]) / s]
	elif m11 > m22:
		var s := sqrt(1.0 + m11 - m00 - m22) * 2.0
		q = [(y[0] + x[1]) / s, 0.25 * s, (z[1] + y[2]) / s, (z[0] - x[2]) / s]
	else:
		var s := sqrt(1.0 + m22 - m00 - m11) * 2.0
		q = [(z[0] + x[2]) / s, (z[1] + y[2]) / s, 0.25 * s, (x[1] - y[0]) / s]
	return q_normalized(q)


## Rotation taking unit vector a onto unit vector b.
static func q_from_to(a: Array, b: Array) -> Array:
	a = normalized(a)
	b = normalized(b)
	var d := dot(a, b)
	if d < -0.999999999:
		var axis := cross([1.0, 0.0, 0.0], a)
		if dot(axis, axis) < 1e-12:
			axis = cross([0.0, 1.0, 0.0], a)
		return q_axis_angle(axis, PI)
	var c := cross(a, b)
	return q_normalized([c[0], c[1], c[2], 1.0 + d])


## Snaps tiny float noise (|x| < 1e-12) to exact 0 so saved files stay clean.
static func clean(v: Array) -> Array:
	var out := []
	for x in v:
		out.append(0.0 if absf(x) < 1e-12 else x)
	return out


static func to_v3(a: Array) -> Vector3:
	return Vector3(a[0], a[1], a[2])


static func to_quat(q: Array) -> Quaternion:
	return Quaternion(q[0], q[1], q[2], q[3])
