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
## Procedural animation for the Tux rig (scenes/tux_rig.tscn): seven
## Node3D joints (pelvis, torso, head, arm_l, arm_r, leg_l, leg_r) turned
## each frame from the current pose and a clock. Adapted from the author's
## own The Legend of Tux animator, cut down to what a penguin on EVA does:
## stand, walk, run, jump, fall, land, float in space and wave.
class_name TuxAnimator
extends RefCounted

const ONE_SHOT := {"land": 0.18, "wave": 1.2}
const BONES := ["pelvis", "torso", "head", "arm_l", "arm_r", "leg_l", "leg_r"]

var tag := "idle"
var speed := 1.0
var _bones := {}
var _rest := {}
var _t := 0.0


## Finds the joints under `rig` and remembers their rest pose.
func setup(rig: Node3D) -> void:
	for b in BONES:
		var n := rig.find_child(b, true, false) as Node3D
		if n:
			_bones[b] = n
			_rest[b] = n.rotation


func play(new_tag: String, speed_mult := 1.0) -> void:
	if new_tag != tag:
		tag = new_tag
		_t = 0.0
	speed = speed_mult


## True while a one-shot pose (land, wave) is still playing.
func busy() -> bool:
	return ONE_SHOT.has(tag) and _t < ONE_SHOT[tag]


func tick(delta: float) -> void:
	_t += delta * speed
	if ONE_SHOT.has(tag):
		_t = minf(_t, ONE_SHOT[tag])
	for b in _bones:
		_bones[b].rotation = _rest[b]
	match tag:
		"walk": _walk(_t, 1.0)
		"run": _walk(_t, 1.7)
		"jump": _jump()
		"fall": _fall(_t)
		"land": _land(_t)
		"float": _float(_t)
		"wave": _wave(_t)
		_: _idle(_t)


func _rot(b: String, e: Vector3) -> void:
	if _bones.has(b):
		_bones[b].rotation = e


func _idle(t: float) -> void:
	_rot("torso", Vector3(sin(t * 1.5) * 0.015, 0, 0))
	_rot("head", Vector3(0, sin(t * 0.7) * 0.08, 0))


func _walk(t: float, intensity: float) -> void:
	var freq := 5.5 * intensity
	var amp_leg := 0.5 * clampf(intensity, 0.5, 1.8)
	var amp_arm := 0.4 * clampf(intensity, 0.5, 1.8)
	var a := sin(t * freq)
	var b := sin(t * freq + PI)
	_rot("leg_l", Vector3(a * amp_leg, 0, 0))
	_rot("leg_r", Vector3(b * amp_leg, 0, 0))
	_rot("arm_l", Vector3(b * amp_arm, 0, 0))
	_rot("arm_r", Vector3(a * amp_arm, 0, 0))
	_rot("torso", Vector3(0, a * 0.07, 0))
	_rot("pelvis", Vector3(0, 0, a * 0.04))


func _jump() -> void:
	_rot("arm_l", Vector3(-1.8, 0, -0.5))
	_rot("arm_r", Vector3(-1.8, 0, 0.5))
	_rot("leg_l", Vector3(-0.5, 0, 0))
	_rot("leg_r", Vector3(-0.7, 0, 0))
	_rot("torso", Vector3(-0.15, 0, 0))


func _fall(t: float) -> void:
	var s := sin(t * 4.0) * 0.1
	_rot("arm_l", Vector3(-1.1 + s, 0, -0.6))
	_rot("arm_r", Vector3(-1.1 - s, 0, 0.6))
	_rot("leg_l", Vector3(-0.2 - s, 0, 0))
	_rot("leg_r", Vector3(-0.4 + s, 0, 0))


func _land(t: float) -> void:
	var d := sin(clampf(t / ONE_SHOT.land, 0.0, 1.0) * PI) * 0.7
	_rot("torso", Vector3(0.6 * d, 0, 0))
	_rot("leg_l", Vector3(-0.6 * d, 0, 0))
	_rot("leg_r", Vector3(-0.6 * d, 0, 0))
	_rot("arm_l", Vector3(-0.4, 0, -0.5))
	_rot("arm_r", Vector3(-0.4, 0, 0.5))


## Weightless: arms out, legs drifting, a slow scull to stay steady.
func _float(t: float) -> void:
	var s := sin(t * 1.6)
	_rot("pelvis", Vector3(0.1, 0, 0))
	_rot("head", Vector3(-0.15, sin(t * 0.5) * 0.12, 0))
	_rot("arm_l", Vector3(0.3, 0, -1.05 + s * 0.15))
	_rot("arm_r", Vector3(0.3, 0, 1.05 - s * 0.15))
	_rot("leg_l", Vector3(s * 0.2, 0, 0.08))
	_rot("leg_r", Vector3(-s * 0.2, 0, -0.08))


func _wave(t: float) -> void:
	_rot("arm_r", Vector3(0, 0, 2.6 + sin(t * 12.0) * 0.35))
	_rot("head", Vector3(0, -0.2, 0.1))
