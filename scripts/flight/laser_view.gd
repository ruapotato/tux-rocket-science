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
## The lasers, as the core has them this tick (core.lasers): each turret's
## head turned to where it points, and each beam that fires drawn from its
## mount as far as it reaches, with a flash where it meets something. Ours
## burn red-orange, the Jaegers' green. The beams hum (Sfx beds) while
## they fire near the camera.
class_name LaserView
extends Node3D

const LPP := 11
const SEGS := 6
const OURS := Color(1.0, 0.28, 0.12)
const THEIRS := Color(0.3, 1.0, 0.4)

var _segs: Array[MeshInstance3D] = []
var _flashes: Array[MeshInstance3D] = []
var _lights: Array[OmniLight3D] = []
var _mat_ours: StandardMaterial3D
var _mat_theirs: StandardMaterial3D
var _flash_mat_ours: StandardMaterial3D
var _flash_mat_theirs: StandardMaterial3D
var _was_firing := {}  # "vessel/uid" -> firing last frame (a zap as one starts)
var _t := 0.0


func _ready() -> void:
	name = "Lasers"
	_mat_ours = _glow(OURS)
	_mat_theirs = _glow(THEIRS)
	_flash_mat_ours = _glow(OURS.lerp(Color.WHITE, 0.5))
	_flash_mat_theirs = _glow(THEIRS.lerp(Color.WHITE, 0.5))


static func _glow(c: Color) -> StandardMaterial3D:
	var m := StandardMaterial3D.new()
	m.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	m.albedo_color = c
	m.emission_enabled = true
	m.emission = c
	m.emission_energy_multiplier = 4.0
	m.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	m.blend_mode = BaseMaterial3D.BLEND_MODE_ADD
	m.cull_mode = BaseMaterial3D.CULL_DISABLED
	m.no_depth_test = false
	return m


func _seg(i: int) -> MeshInstance3D:
	while _segs.size() <= i:
		var mi := MeshInstance3D.new()
		var cyl := CylinderMesh.new()
		cyl.top_radius = 1.0
		cyl.bottom_radius = 1.0
		cyl.height = 1.0
		cyl.radial_segments = 8
		cyl.rings = 1
		mi.mesh = cyl
		mi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
		add_child(mi)
		_segs.append(mi)
	return _segs[i]


## The flash and light at beam i's far end.
func _beam(i: int) -> void:
	while _flashes.size() <= i:
		var fl := MeshInstance3D.new()
		var sp := SphereMesh.new()
		sp.radius = 1.0
		sp.height = 2.0
		sp.radial_segments = 10
		sp.rings = 5
		fl.mesh = sp
		fl.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
		add_child(fl)
		_flashes.append(fl)
		var li := OmniLight3D.new()
		li.omni_range = 12.0
		li.light_energy = 3.0
		li.shadow_enabled = false
		add_child(li)
		_lights.append(li)


## A basis whose +Y is unit vector dir.
static func along_y(dir: Vector3) -> Basis:
	var c := dir.dot(Vector3.UP)
	if c > 0.9999:
		return Basis.IDENTITY
	if c < -0.9999:
		return Basis(Vector3.RIGHT, PI)
	return Basis(Quaternion(Vector3.UP, dir))


## views: vessel id -> VesselView; cam: where sounds are heard from.
func refresh(core: TrsSim, origin: int, views: Dictionary, cam: Vector3, dt: float) -> void:
	_t += dt
	var feed: PackedFloat64Array = core.lasers(origin)
	var n := feed.size() / LPP
	var shown := 0
	var segs := 0
	var hum := 0.0
	var buzz := 0.0
	var now := {}
	for k in n:
		var o := k * LPP
		var vid := int(feed[o])
		var uid := int(feed[o + 1])
		var at := Frame.vec(feed, o + 2)
		var dir := Frame.vec(feed, o + 5).normalized()
		var length: float = feed[o + 8]
		var alien := feed[o + 10] > 0.5
		if views.has(vid):
			views[vid].aim_head(uid, dir)
		if length <= 0.0:
			continue
		var key := "%d/%d" % [vid, uid]
		now[key] = true
		if not _was_firing.has(key) and at.distance_to(cam) < 600.0:
			Sfx.play("zap", clampf(1.0 - at.distance_to(cam) / 600.0, 0.2, 1.0))
		# in pieces, each as wide as it must be to show from here (a beam
		# that reaches a kilometre up is a thread to the eye at the far end)
		var mat := _mat_theirs if alien else _mat_ours
		var b := along_y(dir)
		var flick := (1.0 + 0.25 * sin(_t * 60.0 + k)) * (1.4 if alien else 1.0)
		for q in SEGS:
			var a0 := length * q / SEGS
			var mid := at + dir * (a0 + length * 0.5 / SEGS)
			var w := maxf(0.035, mid.distance_to(cam) * 0.0012) * flick
			var mi := _seg(segs)
			segs += 1
			mi.transform = Transform3D(Frame.scaled_local(b, Vector3(w, length / SEGS, w)), mid)
			mi.material_override = mat
			mi.visible = true
		_beam(shown)
		var end := at + dir * length
		var fl := _flashes[shown]
		var fs := 0.25 + 0.15 * sin(_t * 47.0 + k * 3.0)
		fl.transform = Transform3D(Basis.IDENTITY.scaled(Vector3.ONE * fs), end)
		fl.material_override = _flash_mat_theirs if alien else _flash_mat_ours
		fl.visible = true
		var li := _lights[shown]
		li.position = end - dir * 0.3
		li.light_color = THEIRS if alien else OURS
		li.visible = true
		shown += 1
		var near := clampf(1.0 - minf(at.distance_to(cam), end.distance_to(cam)) / 400.0, 0.0, 1.0)
		if alien:
			buzz = maxf(buzz, near)
		else:
			hum = maxf(hum, near)
	for i in range(segs, _segs.size()):
		_segs[i].visible = false
	for i in range(shown, _flashes.size()):
		_flashes[i].visible = false
		_lights[i].visible = false
	_was_firing = now
	Sfx.laser_level = hum
	Sfx.alien_level = buzz
