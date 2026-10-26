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
## Single-scattering atmosphere, ray-marched per pixel on a shell around the
## planet. Density falls off exponentially with the body's real scale height,
## so the limb is a thin glow: white where the line of sight grazes the dense
## lower air (optical depth saturates every colour), blue above it, fading
## smoothly to black. Sunlight reaching the air is itself attenuated, which
## turns it orange along the terminator. No painted gradients.
##
## Units inside the shader are planet radii, so precision is independent of
## the scaled-space size.
class_name Atmosphere
extends RefCounted

const SHADER := """
shader_type spatial;
render_mode unshaded, blend_add, cull_front, depth_draw_never, depth_test_disabled, shadows_disabled;

uniform vec3 sun_world = vec3(1.0, 0.0, 0.0);
uniform float shell_ratio = 1.1167;      // atmosphere top / planet radius
uniform float scale_height = 0.00967;    // H / planet radius
uniform vec3 tau = vec3(0.035, 0.10, 0.34); // vertical optical depth at sea level (Rayleigh-like)
uniform float sun_power = 22.0;
uniform float exposure = 1.0;
uniform float intensity = 1.0;
uniform float dbg = 0.0;

const int STEPS = 24;
const int LIGHT_STEPS = 6;

vec2 sphere(vec3 ro, vec3 rd, float r) {
	float b = dot(ro, rd);
	float c = dot(ro, ro) - r * r;
	float d = b * b - c;
	if (d < 0.0) return vec2(1e9, -1e9);
	d = sqrt(d);
	return vec2(-b - d, -b + d);
}

void fragment() {
	// shell centre and radius in view space; rescale so the planet radius is 1
	vec3 centre = (VIEW_MATRIX * MODEL_MATRIX * vec4(0.0, 0.0, 0.0, 1.0)).xyz;
	float shell_r = length((MODEL_MATRIX * vec4(1.0, 0.0, 0.0, 0.0)).xyz);
	float unit = shell_r / shell_ratio;
	vec3 ro = -centre / unit;                // camera relative to planet centre
	vec3 rd = normalize(VERTEX);
	vec3 sun = normalize((VIEW_MATRIX * vec4(sun_world, 0.0)).xyz);

	vec2 ta = sphere(ro, rd, shell_ratio);
	if (ta.x > ta.y) discard;
	float t0 = max(ta.x, 0.0);
	float t1 = ta.y;
	vec2 tp = sphere(ro, rd, 1.0);
	if (tp.x < tp.y && tp.x > 0.0) t1 = min(t1, tp.x);
	if (t1 <= t0) discard;

	vec3 beta = tau / scale_height;          // extinction per planet radius at sea level
	float ds = (t1 - t0) / float(STEPS);
	vec3 od = vec3(0.0);
	vec3 sum = vec3(0.0);
	for (int i = 0; i < STEPS; i++) {
		vec3 p = ro + rd * (t0 + ds * (float(i) + 0.5));
		float r = length(p);
		float dens = exp(-(r - 1.0) / scale_height);
		vec3 d_od = beta * dens * ds;
		// sunlight: the ground's shadow, softened over a few km so the
		// terminator glows instead of banding
		float sb = dot(p, sun);
		float shadow = 1.0;
		if (sb < 0.0) {
			float dmin = length(p - sun * sb);
			shadow = smoothstep(0.997, 1.0 + scale_height * 0.6, dmin);
		}
		if (shadow <= 0.0) {
			od += d_od;
			continue;
		}
		vec2 ls = sphere(p, sun, shell_ratio);
		float lds = max(ls.y, 0.0) / float(LIGHT_STEPS);
		vec3 lod = vec3(0.0);
		for (int j = 0; j < LIGHT_STEPS; j++) {
			vec3 q = p + sun * lds * (float(j) + 0.5);
			lod += beta * exp(-(length(q) - 1.0) / scale_height) * lds;
		}
		vec3 trans = exp(-(od + d_od * 0.5 + lod));
		sum += trans * d_od * shadow;
		od += d_od;
	}
	float mu = dot(rd, sun);
	float phase = 0.0597 * (1.0 + mu * mu);  // Rayleigh, 3/(16 pi)
	vec3 col = sum * phase * sun_power;
	// gentle tonemap so the dense limb clips to white instead of a hard edge
	col = 1.0 - exp(-col * exposure);
	ALBEDO = col * intensity;
	if (dbg > 0.5) ALBEDO = vec3(sum.b * 50.0, phase * 5.0, 0.0);
}
"""

static var _shader: Shader


## A shell mesh whose material renders the atmosphere for a body.
## Scale the node to the atmosphere top radius (scaled units).
static func make(planet_radius: float, atmo_height: float, scale_h: float, tau: Array = []) -> MeshInstance3D:
	if _shader == null:
		_shader = Shader.new()
		_shader.code = SHADER
	var mi := MeshInstance3D.new()
	var sm := SphereMesh.new()
	sm.radius = 1.0
	sm.height = 2.0
	sm.radial_segments = 96
	sm.rings = 48
	mi.mesh = sm
	var m := ShaderMaterial.new()
	m.shader = _shader
	m.set_shader_parameter("shell_ratio", (planet_radius + atmo_height) / planet_radius)
	m.set_shader_parameter("scale_height", scale_h / planet_radius)
	if tau.size() >= 3:
		m.set_shader_parameter("tau", Vector3(tau[0], tau[1], tau[2]))
	mi.material_override = m
	mi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	mi.set_meta("radius", planet_radius + atmo_height)
	return mi


static func set_sun(mi: MeshInstance3D, sun_world: Vector3) -> void:
	(mi.material_override as ShaderMaterial).set_shader_parameter("sun_world", sun_world.normalized())


static func set_intensity(mi: MeshInstance3D, k: float) -> void:
	(mi.material_override as ShaderMaterial).set_shader_parameter("intensity", k)
