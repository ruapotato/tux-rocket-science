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
## What a landmark does that the ground alone can't show: a volcano on a
## world with air breathes a plume of steam from its caldera (Mount Shiver,
## Old Smoke, the Horn); one on an airless world glows red in its caldera
## (the Forge). Built at the landmark's centre in its own local frame (y up);
## TerrainView places it as it does a relic. The plume is a MultiMesh of
## soft billboards, not GPU particles (those misbehave with the floating
## origin), each puff rising and spreading on its own clock.
class_name LandmarkFX
extends Node3D

const PUFFS := 48

var _mm: MultiMesh
var _rise := 1.0       # m: how high a puff goes before it's gone
var _width := 1.0      # m: how wide the vent is
var _seed := []


## The node for a landmark (the body's data entry) on a world with or
## without air, or null when it has nothing to show.
static func build(lm: Dictionary, has_air: bool) -> LandmarkFX:
	if lm.get("kind", "") != "volcano":
		return null
	var fx := LandmarkFX.new()
	var r := float(lm.get("radius_km", 20.0)) * 1000.0
	fx._width = r * 0.015
	if has_air:
		fx._rise = clampf(r * 0.07, 1500.0, 7000.0)
		fx._plume()
	else:
		fx._glow(r * 0.13 * 0.8)   # (the caldera's flat floor)
	return fx


func _plume() -> void:
	var quad := QuadMesh.new()
	quad.size = Vector2(1, 1)
	var mat := ShaderMaterial.new()
	mat.shader = Shader.new()
	mat.shader.code = """
shader_type spatial;
render_mode unshaded, blend_mix, depth_draw_never, cull_disabled;
void vertex() {
	// face the camera, keeping the instance's scale
	vec3 s = vec3(length(MODEL_MATRIX[0].xyz), length(MODEL_MATRIX[1].xyz), length(MODEL_MATRIX[2].xyz));
	MODELVIEW_MATRIX = VIEW_MATRIX * mat4(INV_VIEW_MATRIX[0] * s.x, INV_VIEW_MATRIX[1] * s.y, INV_VIEW_MATRIX[2] * s.z, MODEL_MATRIX[3]);
}
void fragment() {
	vec2 c = UV * 2.0 - 1.0;
	float a = smoothstep(1.0, 0.2, length(c));
	ALBEDO = mix(vec3(0.82, 0.84, 0.88), vec3(0.97), UV.y);
	ALPHA = a * COLOR.a * 0.8;
}
"""
	quad.material = mat
	_mm = MultiMesh.new()
	_mm.transform_format = MultiMesh.TRANSFORM_3D
	_mm.use_colors = true
	_mm.mesh = quad
	_mm.instance_count = PUFFS
	var inst := MultiMeshInstance3D.new()
	inst.multimesh = _mm
	inst.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	inst.custom_aabb = AABB(Vector3(-_rise, -_width, -_rise), Vector3(_rise * 2, _rise * 1.5, _rise * 2))
	add_child(inst)
	var rng := RandomNumberGenerator.new()
	rng.seed = 7
	for i in PUFFS:
		_seed.append([rng.randf(), rng.randf() * TAU, rng.randf_range(0.6, 1.4)])
	_update_puffs(0.0)


func _glow(r: float) -> void:
	# a lake of lava on the caldera's floor (the node sits on the floor at
	# its centre): dark crust plates drifting on a bright melt, a little
	# above the floor so it fills the low ground there and the rises poke
	# through as islands
	var disc := MeshInstance3D.new()
	var m := CylinderMesh.new()
	m.top_radius = r
	m.bottom_radius = r
	m.height = 4.0
	m.radial_segments = 64
	m.rings = 1
	var mat := ShaderMaterial.new()
	mat.shader = Shader.new()
	mat.shader.code = """
shader_type spatial;
render_mode unshaded, cull_disabled;
uniform float scale = 1.0;
float h2(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float vn(vec2 p) {
	vec2 i = floor(p), f = fract(p);
	f = f * f * (3.0 - 2.0 * f);
	return mix(mix(h2(i), h2(i + vec2(1, 0)), f.x), mix(h2(i + vec2(0, 1)), h2(i + vec2(1, 1)), f.x), f.y);
}
float fbm(vec2 p) {
	float s = 0.0, a = 0.5;
	for (int i = 0; i < 5; i++) { s += a * vn(p); p = p * 2.03 + 7.1; a *= 0.5; }
	return s;
}
varying vec3 lp;
void vertex() { lp = VERTEX; }
void fragment() {
	vec2 p = lp.xz / scale;
	float t = TIME * 0.01;
	float n = fbm(p * 6.0 + vec2(t, -t * 0.7) + fbm(p * 3.0 - t) * 1.5);
	// bright melt in the seams between crust plates
	float seam = 1.0 - smoothstep(0.02, 0.09, abs(n - 0.5));
	float hot = fbm(p * 2.0 + t * 2.0);
	vec3 crust = vec3(0.045, 0.02, 0.015);
	// (saturated, not bright: the tonemapper bleaches anything over 1 to beige)
	vec3 melt = mix(vec3(0.9, 0.16, 0.02), vec3(1.0, 0.5, 0.08), hot * hot);
	ALBEDO = mix(crust * (1.0 + hot), melt, clamp(max(seam, 0.35 * hot * hot), 0.0, 1.0));
}
"""
	mat.set_shader_parameter("scale", r)
	m.material = mat
	disc.mesh = m
	disc.position = Vector3(0, 25.0, 0)
	disc.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	add_child(disc)
	var light := OmniLight3D.new()
	light.light_color = Color(1.0, 0.45, 0.15)
	light.omni_range = r * 4.0
	light.omni_attenuation = 0.6
	light.light_energy = 8.0
	light.position = Vector3(0, r * 0.4, 0)
	add_child(light)


func _process(_dt: float) -> void:
	if _mm:
		_update_puffs(Time.get_ticks_msec() / 1000.0)


func _update_puffs(t: float) -> void:
	for i in PUFFS:
		var sd: Array = _seed[i]
		var age := fmod(float(sd[0]) + t / (60.0 * float(sd[2])), 1.0)
		var h := age * _rise
		var spread := _width * (0.4 + age * 1.6)
		var drift := Vector3(cos(float(sd[1])), 0, sin(float(sd[1]))) * spread * 0.5 + Vector3(age * _rise * 0.35, 0, 0)
		var size := _width * (1.0 + age * 2.2)
		var xf := Transform3D(Basis().scaled(Vector3(size, size, size)), drift + Vector3(0, h, 0))
		_mm.set_instance_transform(i, xf)
		_mm.set_instance_color(i, Color(1, 1, 1, sin(age * PI) * (1.0 - age * 0.5)))
