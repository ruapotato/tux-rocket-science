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
## Exhaust smoke left hanging in the air. Render space is centred on the
## vessel (floating origin), so each puff drifts by minus the vessel's air
## velocity every frame; puffs are laid down evenly along the path the
## nozzle swept, so the trail stays continuous at any speed. Purely visual.
class_name SmokeTrail
extends MultiMeshInstance3D

const MAX_PUFFS := 3000

const SHADER := """
shader_type spatial;
render_mode unshaded, blend_mix, depth_draw_never, cull_disabled, shadows_disabled;
uniform sampler2D dot : source_color, filter_linear;
uniform float light = 1.0;
void vertex() {
	float sc = length(MODEL_MATRIX[0].xyz);
	MODELVIEW_MATRIX = VIEW_MATRIX * mat4(INV_VIEW_MATRIX[0] * sc, INV_VIEW_MATRIX[1] * sc, INV_VIEW_MATRIX[2] * sc, MODEL_MATRIX[3]);
	MODELVIEW_NORMAL_MATRIX = mat3(MODELVIEW_MATRIX);
}
void fragment() {
	vec4 d = texture(dot, UV);
	ALBEDO = COLOR.rgb * light;
	ALPHA = COLOR.a * d.a;
}
"""

var _pos := PackedVector3Array()
var _vel := PackedVector3Array()   # own motion relative to the air (decays)
var _age := PackedFloat32Array()
var _life := PackedFloat32Array()
var _s0 := PackedFloat32Array()
var _s1 := PackedFloat32Array()
var _a := PackedFloat32Array()
var _soot := PackedFloat32Array()  # 0 exhaust (white) .. 1 burning propellant (black)
var _rise := PackedFloat32Array()  # buoyant climb, m/s
var _mat: ShaderMaterial
var _carry := {}  # nozzle key -> distance not yet covered by a puff


func _ready() -> void:
	var mm := MultiMesh.new()
	mm.transform_format = MultiMesh.TRANSFORM_3D
	mm.use_colors = true
	var q := QuadMesh.new()
	q.size = Vector2.ONE
	mm.mesh = q
	mm.instance_count = MAX_PUFFS
	mm.visible_instance_count = 0
	multimesh = mm
	_mat = ShaderMaterial.new()
	var sh := Shader.new()
	sh.code = SHADER
	_mat.shader = sh
	_mat.set_shader_parameter("dot", ReentryFX._soft_dot())
	material_override = _mat
	cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	# puffs reach kilometres away; never cull the whole trail
	custom_aabb = AABB(Vector3.ONE * -1.0e5, Vector3.ONE * 2.0e5)


func clear() -> void:
	_pos.clear()
	_vel.clear()
	_age.clear()
	_life.clear()
	_s0.clear()
	_s1.clear()
	_a.clear()
	_soot.clear()
	_rise.clear()
	_carry.clear()
	multimesh.visible_instance_count = 0


## nozzles: [{key, pos, dir (exhaust direction), thr, r, solid}] in render
## space; air_vel: the vessel's velocity through the air; air: 0..1
## sea-level fraction (thin air leaves little smoke); light: day brightness;
## up / agl: local vertical and the vessel's height above the ground, so
## exhaust hitting the ground rolls out across it instead of sinking in.
func step(delta: float, nozzles: Array, air_vel: Vector3, air: float, light: float, up := Vector3.UP, agl := 1.0e9) -> void:
	_mat.set_shader_parameter("light", light)
	if delta <= 0.0:
		return
	# age and drift what is already in the air
	var k := 0
	var n := _pos.size()
	for i in n:
		var age: float = _age[i] + delta
		if age >= _life[i]:
			continue
		var v: Vector3 = _vel[i] * exp(-delta * 1.2)
		var p: Vector3 = _pos[i] + (v + up * _rise[i] - air_vel) * delta
		var above := p.dot(up) + agl - _s0[i] * 0.8
		if above < 0.0:
			# ground: push it back up and turn the downward rush sideways
			p -= up * above
			var vn := v.dot(up)
			if vn < 0.0:
				var side := v - up * vn
				var spread := side.normalized() if side.length() > 0.5 else up.cross(Vector3(randf_range(-1, 1), randf_range(-1, 1), randf_range(-1, 1))).normalized()
				v = side + spread * -vn * 0.7
		_pos[k] = p
		_vel[k] = v
		_age[k] = age
		_life[k] = _life[i]
		_s0[k] = _s0[i]
		_s1[k] = _s1[i]
		_a[k] = _a[i]
		_soot[k] = _soot[i]
		_rise[k] = _rise[i]
		k += 1
	_resize(k)
	# lay new puffs along the stretch of air each nozzle swept this frame
	var dens := clampf(air * 1.8, 0.0, 1.0)
	if dens > 0.02:
		for nz in nozzles:
			var r: float = nz.r
			var solid: bool = nz.solid
			var out: Vector3 = (nz.dir as Vector3) * (30.0 + 40.0 * float(nz.thr))
			var rel := out - air_vel
			var s0 := r * 2.6
			var spacing := s0 * 0.4
			var dist: float = _carry.get(nz.key, 0.0) + rel.length() * delta
			var count := mini(int(dist / spacing), 60)
			_carry[nz.key] = dist - count * spacing if count < 60 else 0.0
			for c in count:
				if _pos.size() >= MAX_PUFFS:
					_drop_oldest()
				var f := (c + randf()) / maxf(count, 1)
				var life := (9.0 if solid else 5.0) * randf_range(0.8, 1.2)
				_pos.append((nz.pos as Vector3) + rel * delta * f + Vector3(randf_range(-1, 1), randf_range(-1, 1), randf_range(-1, 1)) * r * 0.3)
				_vel.append(out * 0.6)
				_age.append(delta * f)
				_life.append(life)
				_s0.append(s0)
				_s1.append(r * (28.0 if solid else 20.0) * randf_range(0.8, 1.2))
				_a.append((0.6 if solid else 0.4) * dens * clampf(float(nz.thr), 0.35, 1.0))
				_soot.append(0.0)
				_rise.append(0.0)
	# upload
	var mm := multimesh
	var m := _pos.size()
	for i in m:
		var t: float = _age[i] / _life[i]
		var grow := 1.0 - exp(-_age[i] / 0.9)
		var s: float = lerpf(_s0[i], _s1[i], grow)
		mm.set_instance_transform(i, Transform3D(Basis.IDENTITY.scaled(Vector3.ONE * s), _pos[i]))
		var a: float = _a[i] * smoothstep(0.0, 0.04, t) * pow(1.0 - t, 1.3)
		# fresh exhaust is warm-tinted, aged smoke a cool grey
		var col := Color(0.95, 0.88, 0.8).lerp(Color(0.82, 0.83, 0.86), clampf(t * 3.0, 0.0, 1.0))
		if _soot[i] > 0.0:
			# burning propellant: orange-lit black that greys as it thins
			var sc := Color(0.55, 0.25, 0.08).lerp(Color(0.09, 0.08, 0.075), clampf(_age[i] / 0.6, 0.0, 1.0)).lerp(Color(0.38, 0.37, 0.37), t)
			col = col.lerp(sc, _soot[i])
		col.a = a
		mm.set_instance_color(i, col)
	mm.visible_instance_count = m


func _resize(k: int) -> void:
	_pos.resize(k)
	_vel.resize(k)
	_age.resize(k)
	_life.resize(k)
	_s0.resize(k)
	_s1.resize(k)
	_a.resize(k)
	_soot.resize(k)
	_rise.resize(k)


func _drop_oldest() -> void:
	_pos.remove_at(0)
	_vel.remove_at(0)
	_age.remove_at(0)
	_life.remove_at(0)
	_s0.remove_at(0)
	_s1.remove_at(0)
	_a.remove_at(0)
	_soot.remove_at(0)
	_rise.remove_at(0)


## A blast's smoke: a dark, rising cloud of `size` (m, the fireball radius)
## at render-space `at`, with propellant soot. Lives with the exhaust smoke
## so it drifts with the air the same way.
func burst(at: Vector3, size: float, up := Vector3.UP) -> void:
	var n := int(clampf(size * 14.0, 30.0, 160.0))
	for k in n:
		if _pos.size() >= MAX_PUFFS:
			_drop_oldest()
		var d := Vector3(randf_range(-1, 1), randf_range(-0.3, 1), randf_range(-1, 1))
		_pos.append(at + d * size * 0.7)
		_vel.append(d.normalized() * randf_range(2.0, 7.0) * sqrt(size))
		_age.append(0.0)
		_life.append(randf_range(14.0, 24.0))
		_s0.append(size * randf_range(0.6, 1.0))
		_s1.append(size * randf_range(3.0, 5.5))
		_a.append(randf_range(0.55, 0.85))
		_soot.append(1.0)
		_rise.append(randf_range(1.5, 4.0) * sqrt(size) * 0.5)


## Shifts every puff (the floating origin jumped to another vessel).
func shift(d: Vector3) -> void:
	for i in _pos.size():
		_pos[i] += d
