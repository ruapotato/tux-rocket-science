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
## Reentry plasma: a glowing sheath on the windward side of the active
## vessel, scaled by the convective heating proxy rho * v^3. Purely visual
## (heating is not simulated in the slice).
class_name ReentryFX
extends Node3D

const SHADER := """
shader_type spatial;
render_mode unshaded, blend_add, cull_disabled, depth_draw_never, shadows_disabled;
uniform float intensity = 0.0;
uniform vec3 flow_local = vec3(0.0, -1.0, 0.0);   // air arriving from this direction (object space)
varying vec3 lp;
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
void vertex() {
	lp = VERTEX;
	// stretch the shell downstream into a tail
	float ahead = dot(normalize(VERTEX), -flow_local);
	VERTEX += flow_local * max(-ahead, 0.0) * 0.3;
}
void fragment() {
	vec3 n = normalize(lp);
	float ahead = dot(n, -flow_local);
	// only the windward cap glows: a thin bow of hot gas, no balloon
	float cap = pow(clamp(ahead, 0.0, 1.0), 3.0);
	float edge = pow(1.0 - abs(dot(NORMAL, VIEW)), 2.0);
	float flick = 0.85 + 0.15 * hash(vec2(floor(TIME * 40.0), 3.0));
	vec3 col = mix(vec3(1.0, 0.45, 0.12), vec3(1.0, 0.92, 0.75), cap) * cap * edge * 1.6;
	ALBEDO = col * pow(intensity, 0.6) * flick * 2.2;
}
"""

var _shell: MeshInstance3D
var _mat: ShaderMaterial
var _light: OmniLight3D
var _trail: GPUParticles3D
var _trail_mat: ParticleProcessMaterial
var level := 0.0


func _ready() -> void:
	_shell = MeshInstance3D.new()
	var sm := SphereMesh.new()
	sm.radius = 1.0
	sm.height = 2.0
	sm.radial_segments = 48
	sm.rings = 24
	_shell.mesh = sm
	_mat = ShaderMaterial.new()
	var sh := Shader.new()
	sh.code = SHADER
	_mat.shader = sh
	_shell.material_override = _mat
	_shell.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	add_child(_shell)
	# streaming plasma trail: render space is centred on the vessel (floating
	# origin), so particles just stream downstream from the windward face
	_trail = GPUParticles3D.new()
	_trail.amount = 1600
	_trail.lifetime = 0.35
	_trail.local_coords = false
	_trail_mat = ParticleProcessMaterial.new()
	_trail_mat.emission_shape = ParticleProcessMaterial.EMISSION_SHAPE_SPHERE_SURFACE
	_trail_mat.emission_sphere_radius = 1.0
	_trail_mat.spread = 8.0
	_trail_mat.initial_velocity_min = 25.0
	_trail_mat.initial_velocity_max = 45.0
	_trail_mat.gravity = Vector3.ZERO
	_trail_mat.scale_min = 0.5
	_trail_mat.scale_max = 1.0
	_trail_mat.particle_flag_align_y = true
	var grad := Gradient.new()
	grad.set_color(0, Color(1.0, 0.9, 0.7, 1.0))
	grad.add_point(0.35, Color(1.0, 0.5, 0.15, 0.8))
	grad.set_color(grad.get_point_count() - 1, Color(0.8, 0.2, 0.05, 0.0))
	var gt := GradientTexture1D.new()
	gt.gradient = grad
	_trail_mat.color_ramp = gt
	var sc := Curve.new()
	sc.add_point(Vector2(0.0, 0.5))
	sc.add_point(Vector2(0.3, 1.0))
	sc.add_point(Vector2(1.0, 1.8))
	var sct := CurveTexture.new()
	sct.curve = sc
	_trail_mat.scale_curve = sct
	_trail.process_material = _trail_mat
	var quad := QuadMesh.new()
	quad.size = Vector2(0.22, 1.6)
	var qm := StandardMaterial3D.new()
	qm.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	qm.blend_mode = BaseMaterial3D.BLEND_MODE_ADD
	qm.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	qm.vertex_color_use_as_albedo = true
	qm.billboard_mode = BaseMaterial3D.BILLBOARD_FIXED_Y
	qm.billboard_keep_scale = true
	qm.albedo_texture = _soft_dot()
	quad.material = qm
	_trail.draw_pass_1 = quad
	_trail.emitting = false
	add_child(_trail)
	_light = OmniLight3D.new()
	_light.light_color = Color(1.0, 0.55, 0.25)
	_light.omni_range = 30.0
	add_child(_light)
	visible = false


## air_vel: velocity of the vessel relative to the air (Godot axes, m/s);
## rho: air density; size: vessel bounding radius (m).
func update_fx(air_vel: Vector3, rho: float, size: float, delta: float) -> void:
	var v := air_vel.length()
	# rho*v^3 in W/m^2-ish units; visible from ~1.5 km/s in the upper air
	var q := rho * v * v * v
	var target := clampf((q - 2.0e6) / 1.2e8, 0.0, 1.0)
	level = move_toward(level, target, delta * 1.5)
	visible = level > 0.01
	_trail.emitting = level > 0.05
	if not visible:
		return
	var flow := air_vel.normalized() if v > 1.0 else Vector3.DOWN
	# sheath sits on the windward side
	var r := size * 0.62
	_shell.scale = Vector3.ONE * r
	_shell.position = -flow * r * 0.15
	_mat.set_shader_parameter("intensity", level)
	_mat.set_shader_parameter("flow_local", flow)
	_light.position = -flow * r
	_light.light_energy = 6.0 * level
	# trail: from the windward face, streaming downstream (flow direction)
	_trail.position = -flow * r * 0.6
	_trail_mat.direction = flow
	_trail_mat.emission_sphere_radius = r * 0.85
	_trail.amount_ratio = clampf(level * 1.5, 0.05, 1.0)
	_trail_mat.initial_velocity_min = 25.0 + 30.0 * level
	_trail_mat.initial_velocity_max = 45.0 + 50.0 * level


static func _soft_dot() -> Texture2D:
	var img := Image.create(32, 32, false, Image.FORMAT_RGBA8)
	for y in 32:
		for x in 32:
			var d := Vector2(x - 15.5, y - 15.5).length() / 16.0
			var a := clampf(1.0 - d, 0.0, 1.0)
			img.set_pixel(x, y, Color(1, 1, 1, a * a))
	return ImageTexture.create_from_image(img)
