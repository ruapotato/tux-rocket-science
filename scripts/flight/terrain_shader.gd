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
## Surface shading for terrain patches. Geometry and large-scale shape come
## from trs_core; this adds what you see up close: snow, ice and rock on
## Floe, regolith on Krill, with per-pixel bump detail.
##
## Precision: patch vertices carry coordinates relative to a snapped local
## origin (UV, UV2.x) and an instance uniform holds that origin modulo the
## noise period, so the noise is continuous across patches while float32
## only ever sees small numbers.
class_name TerrainShader
extends RefCounted

const PERIOD := 2048.0

const CODE := """
shader_type spatial;
render_mode cull_back;

uniform int kind = 0;            // 0 = ice world, 1 = airless regolith
uniform vec4 c_low : source_color;
uniform vec4 c_high : source_color;
uniform vec4 c_slope : source_color;
uniform vec4 c_rock : source_color;
uniform float height_range = 1000.0;
instance uniform vec3 detail_origin = vec3(0.0);
uniform sampler2D baked_albedo : source_color, filter_linear_mipmap, repeat_enable;
uniform sampler2D baked_normal : filter_linear_mipmap, repeat_enable;
uniform float has_bake = 0.0;
uniform float has_clouds = 0.0;
uniform float cloud_coverage = 0.5;
uniform float cloud_offset = 0.0117;  // deck height / radius
uniform vec3 sun_cloud = vec3(1.0, 0.0, 0.0);  // sun in the cloud sphere's object frame

varying vec3 lp;        // metres, relative to the patch's snapped origin
varying vec3 up_view;
varying float hgt;
varying float big;      // large-scale variation baked per vertex
varying float surf;     // 0 on LOD skirts
varying vec3 dir_obj;   // body-fixed unit direction (object space = sim axes)
varying float cloud_shade;  // 1 = clear sky to the sun, lower under a cloud

const float PERIOD = 2048.0;

vec3 hash3(vec3 c) {
	c = mod(c, vec3(PERIOD));
	c = vec3(dot(c, vec3(127.1, 311.7, 74.7)), dot(c, vec3(269.5, 183.3, 246.1)), dot(c, vec3(113.5, 271.9, 124.6)));
	return fract(sin(c) * 43758.5453);
}

// periodic value noise; `f` must divide PERIOD into whole cells
float vnoise(vec3 p, float f) {
	vec3 x = p * f;
	vec3 i = floor(x), t = fract(x);
	vec3 u = t * t * (3.0 - 2.0 * t);
	float per = PERIOD * f;
	float n000 = hash3(mod(i, per)).x, n100 = hash3(mod(i + vec3(1,0,0), per)).x;
	float n010 = hash3(mod(i + vec3(0,1,0), per)).x, n110 = hash3(mod(i + vec3(1,1,0), per)).x;
	float n001 = hash3(mod(i + vec3(0,0,1), per)).x, n101 = hash3(mod(i + vec3(1,0,1), per)).x;
	float n011 = hash3(mod(i + vec3(0,1,1), per)).x, n111 = hash3(mod(i + vec3(1,1,1), per)).x;
	return mix(mix(mix(n000, n100, u.x), mix(n010, n110, u.x), u.y), mix(mix(n001, n101, u.x), mix(n011, n111, u.x), u.y), u.z);
}

float c_hash(vec3 p) {
	uint h = uint(int(floor(p.x))) * 73856093u ^ uint(int(floor(p.y))) * 19349663u ^ uint(int(floor(p.z))) * 83492791u;
	h = (h ^ (h >> 13u)) * 1274126177u;
	h ^= h >> 16u;
	return float(h & 16777215u) / 16777216.0;
}
float c_vn(vec3 p) {
	vec3 i = floor(p), f = fract(p);
	vec3 u = f * f * (3.0 - 2.0 * f);
	return mix(mix(mix(c_hash(i), c_hash(i + vec3(1,0,0)), u.x), mix(c_hash(i + vec3(0,1,0)), c_hash(i + vec3(1,1,0)), u.x), u.y),
		mix(mix(c_hash(i + vec3(0,0,1)), c_hash(i + vec3(1,0,1)), u.x), mix(c_hash(i + vec3(0,1,1)), c_hash(i + vec3(1,1,1)), u.x), u.y), u.z);
}
// must match PlanetBake.CLOUD_SHADER's clouds()
float c_field(vec3 d) {
	vec3 q = d * vec3(7.0, 15.0, 7.0);
	q += vec3(c_vn(q * 0.5), c_vn(q * 0.5 + 7.3), c_vn(q * 0.5 + 3.1)) * 1.6;
	float n = 0.0, amp = 0.5;
	for (int o = 0; o < 7; o++) { n += c_vn(q) * amp; q *= 2.11; amp *= 0.5; }
	float band = 0.5 + 0.5 * sin(d.y * 13.0 + n * 3.0);
	return n * 0.85 + band * 0.2;
}

void vertex() {
	lp = vec3(UV.x, UV.y, UV2.x);
	hgt = UV2.y;
	big = COLOR.a;
	surf = COLOR.r;
	dir_obj = CUSTOM0.xyz;
	up_view = normalize((MODELVIEW_MATRIX * vec4(CUSTOM0.xyz, 0.0)).xyz);
}

void fragment() {
	vec3 p = lp + detail_origin;
	float dist = length(VERTEX);
	// detail octaves fade with distance so they never alias
	float fine = 1.0 - smoothstep(40.0, 160.0, dist);
	float mid = 1.0 - smoothstep(150.0, 700.0, dist);
	float far = 1.0 - smoothstep(1500.0, 6000.0, dist);
	float n_big = vnoise(p, 1.0 / 256.0) * 0.6 + mix(0.5, vnoise(p, 1.0 / 64.0), far) * 0.4;
	float n_mid = mix(0.5, vnoise(p, 1.0 / 16.0), far) * 0.6 + mix(0.5, vnoise(p, 1.0 / 4.0), mid) * 0.4;
	float n_fine = mix(0.5, vnoise(p, 1.0), fine) * 0.6 + mix(0.5, vnoise(p, 4.0), fine) * 0.4;
	float slope = 1.0 - clamp(dot(normalize(NORMAL), up_view), 0.0, 1.0);
	float h01 = clamp(hgt / height_range * 0.5 + 0.5, 0.0, 1.0);
	vec3 col;
	float bump;
	float rough;
	float spec = 0.4;
	if (kind == 0) {
		// wind-carved snow ripples running along one direction
		float ripple = sin(p.x * 0.9 + p.y * 0.35 + n_mid * 5.0) * 0.5 + 0.5;
		vec3 snow = mix(c_low.rgb, c_high.rgb, clamp(h01 + (n_big - 0.5) * 0.4, 0.0, 1.0));
		snow *= 0.94 + 0.06 * ripple * mid;
		// blue ice in hollows and on moderate slopes
		float ice = smoothstep(0.08, 0.22, slope + (0.5 - n_big) * 0.12) * (1.0 - smoothstep(0.38, 0.5, slope));
		ice *= 1.0 - 0.8 * smoothstep(600.0, 3000.0, dist);  // edge-on slopes far away read as stripes
		ice = max(ice, smoothstep(0.62, 0.8, big) * 0.6);
		col = mix(snow, c_slope.rgb, ice);
		// low basins are blue sea ice, the same rule the orbital bake uses,
		// so near detail and the distant baked map agree
		float basin = clamp((450.0 - hgt) / 450.0, 0.0, 1.0);
		vec3 sea = mix(vec3(0.36, 0.58, 0.84), vec3(0.62, 0.78, 0.94), n_big);
		col = mix(col, sea, basin * 0.85);
		// rock on steep faces
		float rock = smoothstep(0.42, 0.56, slope + (n_mid - 0.5) * 0.1);
		col = mix(col, c_rock.rgb * (0.8 + 0.4 * n_mid), rock);
		bump = (ripple * 0.06 * mid + n_fine * 0.03 * fine) * (1.0 - rock) + n_mid * 0.25 * rock;
		rough = mix(0.55, 0.25, ice) + rock * 0.35;
		spec = mix(0.5, 0.8, ice);
		// sparkle on fresh snow up close
		float sp = step(0.985, hash3(floor(p * 6.0)).y) * fine * (1.0 - ice) * (1.0 - rock);
		EMISSION = vec3(sp) * 0.6;
	} else {
		vec3 base = mix(c_low.rgb, c_high.rgb, clamp(h01 * 0.6 + n_big * 0.5, 0.0, 1.0));
		base *= mix(0.78, 1.0, smoothstep(0.3, 0.7, big));     // darker plains
		base *= 0.9 + 0.2 * n_mid * mid;
		float rock = smoothstep(0.3, 0.45, slope + (n_mid - 0.5) * 0.15);
		col = mix(base, c_rock.rgb, rock * 0.8);
		// scattered pebbles and boulders up close
		float pebble = smoothstep(0.78, 0.86, n_fine) * fine;
		col *= 1.0 - pebble * 0.35;
		bump = n_mid * 0.35 * mid + n_fine * 0.08 * fine + pebble * 0.05 + rock * n_mid * 0.4;
		rough = 0.92;
		spec = 0.25;
	}
	bump *= surf;  // skirts are hidden walls: no bump, so their seams stay invisible
	// derivative bump mapping is unstable at grazing angles far away: fade it
	bump *= 1.0 - smoothstep(400.0, 1500.0, dist);
	// bump mapping from screen-space derivatives (no tangents needed)
	vec3 dpdx = dFdx(VERTEX), dpdy = dFdy(VERTEX);
	float dhdx = dFdx(bump), dhdy = dFdy(bump);
	vec3 r1 = cross(dpdy, NORMAL), r2 = cross(NORMAL, dpdx);
	float det = dot(dpdx, r1);
	vec3 grad = sign(det) * (dhdx * r1 + dhdy * r2);
	NORMAL = normalize(abs(det) * NORMAL - grad * 1.5);
	// far away: the baked planet maps take over (no aliasing, one look)
	if (has_bake > 0.5) {
		float fb = smoothstep(6000.0, 40000.0, dist);
		if (fb > 0.0) {
			vec3 s = normalize(dir_obj);
			vec2 uv = vec2(atan(s.y, s.x) / 6.2831853 + 0.5, 0.5 - asin(clamp(s.z, -1.0, 1.0)) / 3.14159265);
			vec2 uv2 = vec2(fract(uv.x + 0.5) - 0.5, uv.y);
			vec2 dx = dFdx(uv), dy = dFdy(uv), dx2 = dFdx(uv2), dy2 = dFdy(uv2);
			if (length(dx2) + length(dy2) < length(dx) + length(dy)) { dx = dx2; dy = dy2; }
			vec3 bc = textureGrad(baked_albedo, uv, dx, dy).rgb;
			vec3 bn = textureGrad(baked_normal, uv, dx, dy).rgb * 2.0 - 1.0;
			col = mix(col, bc, fb);
			vec3 nv = normalize((VIEW_MATRIX * MODEL_MATRIX * vec4(bn, 0.0)).xyz);
			// the bake's normals exaggerate relief for orbital views; at grazing
			// angles from the ground they flip faces dark, so use them lightly
			NORMAL = normalize(mix(NORMAL, nv, fb * 0.15));
			rough = mix(rough, 0.85, fb);
			EMISSION *= 1.0 - fb;
		}
	}
	cloud_shade = 1.0;
	if (has_clouds > 0.5) {
		vec3 s = normalize(dir_obj);
		vec3 g = vec3(s.x, s.z, -s.y);               // sim body-fixed -> cloud sphere object
		vec3 sn = normalize(sun_cloud);
		float sh = dot(sn, g);
		vec3 dd = normalize(g + (sn - g * sh) * cloud_offset / max(sh, 0.15));
		float a = TIME * 0.004;
		mat3 drift = mat3(vec3(cos(a), 0.0, -sin(a)), vec3(0.0, 1.0, 0.0), vec3(sin(a), 0.0, cos(a)));
		float c = smoothstep(cloud_coverage, cloud_coverage + 0.08, c_field(drift * dd));
		cloud_shade = 1.0 - 0.55 * c;
	}
	// sub-pixel ridges far away alias into dark dashes: soften their normals
	// toward local up with distance (they'd be washed by haze anyway)
	NORMAL = normalize(mix(NORMAL, up_view, smoothstep(1500.0, 12000.0, dist) * 0.65));
	// a shading normal facing away from the eye on a visible surface makes
	// black slivers at grazing angles: pull it back to the visible side
	float nv_dot = dot(NORMAL, VIEW);
	if (nv_dot < 0.05)
		NORMAL = normalize(NORMAL + VIEW * (0.05 - nv_dot));
	ALBEDO = col;
	ROUGHNESS = rough;
	SPECULAR = spec;
}

// Directional sunlight with the planet's own shadow: a directional light has
// no idea the planet is in the way, so terrain past the terminator would be
// lit straight through the world. Fade by the sun's height at this spot.
void light() {
	float sun_h = dot(up_view, LIGHT);
	float planet_shadow = smoothstep(-0.06, 0.04, sun_h);
	float ndl = clamp(dot(NORMAL, LIGHT), 0.0, 1.0);
	float wrap = clamp((dot(NORMAL, LIGHT) + 0.2) / 1.2, 0.0, 1.0);
	DIFFUSE_LIGHT += LIGHT_COLOR * ATTENUATION * mix(ndl, wrap, 0.35) * planet_shadow * cloud_shade / PI;
	vec3 h = normalize(LIGHT + VIEW);
	float shin = mix(8.0, 96.0, 1.0 - ROUGHNESS);
	SPECULAR_LIGHT += LIGHT_COLOR * ATTENUATION * pow(clamp(dot(NORMAL, h), 0.0, 1.0), shin) * ndl * planet_shadow * cloud_shade * 0.08 * (1.0 - ROUGHNESS);
}
"""

static var _shader: Shader


static func make(style: Dictionary, bake: Dictionary = {}) -> ShaderMaterial:
	if _shader == null:
		_shader = Shader.new()
		_shader.code = CODE
	var m := ShaderMaterial.new()
	m.shader = _shader
	m.set_shader_parameter("kind", int(style.get("kind", 0)))
	m.set_shader_parameter("c_low", style.low)
	m.set_shader_parameter("c_high", style.high)
	m.set_shader_parameter("c_slope", style.slope)
	m.set_shader_parameter("c_rock", style.rock)
	m.set_shader_parameter("height_range", style.range)
	if not bake.is_empty():
		m.set_shader_parameter("baked_albedo", bake.albedo)
		m.set_shader_parameter("baked_normal", bake.normal)
		m.set_shader_parameter("has_bake", 1.0)
	return m
