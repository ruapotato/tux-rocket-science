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
## Planet textures for scaled space, baked from trs_core's real terrain
## heights so the world seen from orbit is the world you land on.
## Results are cached in user://cache keyed by the body's terrain data.
class_name PlanetBake
extends RefCounted

const W := 2048
const H := 1024
const VERSION := 7

const PLANET_SHADER := """
shader_type spatial;
uniform sampler2D albedo_tex : source_color, filter_linear_mipmap, repeat_enable;
uniform sampler2D normal_tex : filter_linear_mipmap, repeat_enable;
uniform float roughness_v = 0.8;
varying vec3 objp;
varying vec3 up_v;
void vertex() {
	objp = VERTEX;
	up_v = normalize((MODELVIEW_MATRIX * vec4(VERTEX, 0.0)).xyz);
}
void fragment() {
	vec3 s = normalize(vec3(objp.x, -objp.z, objp.y));   // Godot object -> sim body-fixed
	float lon = atan(s.y, s.x);
	float lat = asin(clamp(s.z, -1.0, 1.0));
	vec2 uv = vec2(lon / 6.2831853 + 0.5, 0.5 - lat / 3.14159265);
	// seam-free mip selection across the date line (Tarini's trick)
	vec2 uv2 = vec2(fract(uv.x + 0.5) - 0.5, uv.y);
	vec2 dx = dFdx(uv), dy = dFdy(uv), dx2 = dFdx(uv2), dy2 = dFdy(uv2);
	if (length(dx2) + length(dy2) < length(dx) + length(dy)) { dx = dx2; dy = dy2; }
	vec3 a = textureGrad(albedo_tex, uv, dx, dy).rgb;
	vec3 ns = textureGrad(normal_tex, uv, dx, dy).rgb * 2.0 - 1.0;
	vec3 no = vec3(ns.x, ns.z, -ns.y);                       // sim -> Godot object
	NORMAL = normalize((VIEW_MATRIX * MODEL_MATRIX * vec4(no, 0.0)).xyz);
	ALBEDO = a;
	ROUGHNESS = roughness_v;
	SPECULAR = 0.35;
}
// relief normals must not light slopes on the night side of the planet
void light() {
	float shadow = smoothstep(-0.04, 0.05, dot(up_v, LIGHT));
	float ndl = clamp(dot(NORMAL, LIGHT), 0.0, 1.0);
	DIFFUSE_LIGHT += LIGHT_COLOR * ATTENUATION * ndl * shadow / PI;
}
"""

const GIANT_SHADER := """
shader_type spatial;
uniform vec3 bands[8];
uniform vec3 storm_dir = vec3(0.0, 0.0, 1.0);
uniform float storm_size = 0.0;      // rad
uniform vec3 storm_col = vec3(0.7, 0.35, 0.25);
uniform vec3 sun_obj = vec3(1.0, 0.0, 0.0);   // sun in the planet's frame (Godot object axes)
uniform float ring_in = 0.0;   // planet radii; 0 = no rings
uniform float ring_out = 0.0;
uniform float ring_opacity = 0.7;
varying vec3 objp;
varying vec3 up_v;
varying float ring_sh;
float hash(vec3 p) {
	uint h = uint(int(floor(p.x))) * 73856093u ^ uint(int(floor(p.y))) * 19349663u ^ uint(int(floor(p.z))) * 83492791u;
	h = (h ^ (h >> 13u)) * 1274126177u;
	h ^= h >> 16u;
	return float(h & 16777215u) / 16777216.0;
}
float vn(vec3 p) {
	vec3 i = floor(p), f = fract(p);
	vec3 u = f * f * (3.0 - 2.0 * f);
	return mix(mix(mix(hash(i), hash(i + vec3(1,0,0)), u.x), mix(hash(i + vec3(0,1,0)), hash(i + vec3(1,1,0)), u.x), u.y),
		mix(mix(hash(i + vec3(0,0,1)), hash(i + vec3(1,0,1)), u.x), mix(hash(i + vec3(0,1,1)), hash(i + vec3(1,1,1)), u.x), u.y), u.z);
}
float fbm(vec3 p) {
	float s = 0.0, a = 0.5;
	for (int o = 0; o < 6; o++) { s += a * vn(p); p = p * 2.07 + 3.1; a *= 0.5; }
	return s;
}
float fbm3(vec3 p) {
	return 0.55 * vn(p) + 0.3 * vn(p * 2.07 + 3.1) + 0.15 * vn(p * 4.3 + 7.7);
}
void vertex() {
	objp = VERTEX;
	up_v = normalize((MODELVIEW_MATRIX * vec4(VERTEX, 0.0)).xyz);
}
void fragment() {
	vec3 s = normalize(vec3(objp.x, -objp.z, objp.y));   // Godot object -> sim body-fixed
	float lat = asin(clamp(s.z, -1.0, 1.0));
	// the bands flow east and west by latitude; their edges curl
	float t = TIME * 0.002;
	float shear = sin(lat * 9.0) * t * 6.0;
	vec3 q = vec3(s.x * cos(shear) - s.y * sin(shear), s.x * sin(shear) + s.y * cos(shear), s.z);
	float curl = fbm(q * vec3(5.0, 5.0, 18.0)) - 0.5;
	float v = (lat / 3.14159265 + 0.5) * 7.0 + curl * 0.9 + 0.25 * sin(lat * 31.0);
	int i = int(floor(v));
	float fr = smoothstep(0.25, 0.75, fract(v));
	vec3 col = mix(bands[clamp(i, 0, 7)], bands[clamp(i + 1, 0, 7)], fr);
	// streaks along the flow
	col *= 0.9 + 0.18 * fbm(q * vec3(14.0, 14.0, 90.0));
	// the storm: an oval that turns on itself
	if (storm_size > 0.0) {
		float a = acos(clamp(dot(s, normalize(storm_dir)), -1.0, 1.0)) / storm_size;
		if (a < 1.6) {
			vec3 e = normalize(cross(vec3(0.0, 0.0, 1.0), storm_dir));
			vec3 n = cross(storm_dir, e);
			vec2 l = vec2(dot(s, e), dot(s, n) * 1.7) / sin(storm_size);
			float r = length(l);
			float ang = atan(l.y, l.x) + r * 3.0 - TIME * 0.02;
			float swirl = fbm(vec3(cos(ang) * r * 3.0, sin(ang) * r * 3.0, r * 2.0));
			float k = 1.0 - smoothstep(0.7, 1.05, r);
			col = mix(col, mix(storm_col, storm_col * 1.35, swirl), k * 0.9);
			col = mix(col, col * 0.8, smoothstep(0.9, 1.0, r) * (1.0 - smoothstep(1.0, 1.15, r)));
		}
	}
	// up close, the cloud tops themselves: billows inside billows, each
	// size fading in as the eye comes near enough to see it (view distance
	// in scaled units, 1e-4 of a metre's)
	float vd = length(VERTEX);
	float f1 = 1.0 - smoothstep(20.0, 150.0, vd);
	float f2 = 1.0 - smoothstep(3.0, 25.0, vd);
	float f3 = 1.0 - smoothstep(0.5, 4.0, vd);
	float bil = 0.0;
	if (f1 > 0.0) {
		vec3 w = q + vec3(curl * 0.01);
		bil += (fbm3(w * 220.0) - 0.5) * f1;
		if (f2 > 0.0) bil += (fbm3(w * 1400.0) - 0.5) * 0.8 * f2;
		if (f3 > 0.0) bil += (fbm3(w * 9000.0) - 0.5) * 0.6 * f3;
		col *= 1.0 + bil * 0.4;
		col = mix(col, vec3(0.96, 0.93, 0.87), clamp(bil * 1.2, 0.0, 1.0) * 0.45);   // the billows' tops pale
		// their round tops catch the light: a bump from the same billows
		vec3 dpdx = dFdx(VERTEX), dpdy = dFdy(VERTEX);
		float dhx = dFdx(bil), dhy = dFdy(bil);
		vec3 r1 = cross(dpdy, NORMAL), r2 = cross(NORMAL, dpdx);
		float det = dot(dpdx, r1);
		NORMAL = normalize(abs(det) * NORMAL - sign(det) * (dhx * r1 + dhy * r2) * 0.2);
	}
	// (richer than the data's colours: lit from far off they wash out)
	col = clamp(mix(vec3(dot(col, vec3(0.333))), col, 1.3) * 0.95, 0.0, 1.0);
	// darker toward the limb, through more of the haze above the clouds
	float facing = clamp(dot(NORMAL, VIEW), 0.0, 1.0);
	col *= mix(0.55, 1.0, smoothstep(0.0, 0.45, facing));
	ALBEDO = col;
	ROUGHNESS = 1.0;
	SPECULAR = 0.05;
	// the rings' shadow: from here toward the sun, through the ring plane
	ring_sh = 1.0;
	vec3 sd = normalize(sun_obj);
	vec3 pp = normalize(objp);
	if (ring_out > 0.0 && abs(sd.y) > 1e-4) {
		float tt = -pp.y / sd.y;
		if (tt > 0.0) {
			vec3 hit = pp + sd * tt;
			float rr = length(hit.xz);
			float inside = step(ring_in, rr) * step(rr, ring_out);
			ring_sh = 1.0 - inside * ring_opacity * 0.8;
		}
	}
}
void light() {
	float shadow = smoothstep(-0.06, 0.06, dot(up_v, LIGHT));
	float ndl = clamp((dot(NORMAL, LIGHT) + 0.05) / 1.05, 0.0, 1.0);
	DIFFUSE_LIGHT += LIGHT_COLOR * ATTENUATION * ndl * shadow * ring_sh / PI;
}
"""

const RING_SHADER := """
shader_type spatial;
render_mode blend_mix, depth_draw_never, cull_disabled, unshaded;
uniform vec3 tint : source_color = vec3(0.85, 0.8, 0.72);
uniform float opacity = 0.7;
uniform float r0 = 1.5;
uniform float r1 = 2.9;
uniform vec3 sun_obj = vec3(1.0, 0.0, 0.0);
varying vec3 objp;
float h1(float x) { return fract(sin(x * 127.1) * 43758.5453); }
float n1(float x) { float i = floor(x), f = fract(x); f = f * f * (3.0 - 2.0 * f); return mix(h1(i), h1(i + 1.0), f); }
void vertex() { objp = VERTEX; }
void fragment() {
	float r = length(objp.xz);
	float u = (r - r0) / (r1 - r0);
	// ringlets and gaps: noise along the radius at several scales, a
	// dark division two thirds of the way out, the edges fading
	float dens = 0.55 + 0.25 * n1(u * 40.0) + 0.15 * n1(u * 170.0) + 0.1 * n1(u * 600.0);
	dens *= 1.0 - 0.9 * (1.0 - smoothstep(0.0, 0.012, abs(u - 0.66)));
	dens *= smoothstep(0.0, 0.04, u) * (1.0 - smoothstep(0.92, 1.0, u));
	dens *= mix(0.55, 1.0, smoothstep(0.0, 0.35, u));
	vec3 sd = normalize(sun_obj);
	// the planet's shadow: from here toward the sun, does the ray hit it?
	vec3 p = objp;
	float b = dot(p, sd);
	float c = dot(p, p) - 1.0;
	float lit = (b < 0.0 && b * b - c > 0.0) ? 0.06 : 1.0;
	// sunlight on a flat sheet: lit from either side, dimmer edge-on
	float face = 0.35 + 0.65 * sqrt(abs(sd.y));
	vec3 col = tint * (0.85 + 0.3 * n1(u * 90.0 + 7.0));
	ALBEDO = col * face * lit;
	ALPHA = clamp(dens * opacity, 0.0, 1.0);
}
"""

const CLOUD_SHADER := """
shader_type spatial;
render_mode blend_mix, depth_draw_never, cull_back;
uniform float coverage = 0.5;
uniform float opacity = 1.0;
uniform float shadow_mode = 0.0;   // 1 = draw this deck's shadow on the ground
uniform vec3 sun_obj = vec3(1.0, 0.0, 0.0);  // sun direction in object space
uniform float shadow_offset = 0.0016;        // deck height above ground / radius
varying vec3 objp;
varying vec3 up_v;
float hash(vec3 p) {
	uint h = uint(int(floor(p.x))) * 73856093u ^ uint(int(floor(p.y))) * 19349663u ^ uint(int(floor(p.z))) * 83492791u;
	h = (h ^ (h >> 13u)) * 1274126177u;
	h ^= h >> 16u;
	return float(h & 16777215u) / 16777216.0;
}
float vn(vec3 p) {
	vec3 i = floor(p), f = fract(p);
	vec3 u = f * f * (3.0 - 2.0 * f);
	return mix(mix(mix(hash(i), hash(i + vec3(1,0,0)), u.x), mix(hash(i + vec3(0,1,0)), hash(i + vec3(1,1,0)), u.x), u.y),
		mix(mix(hash(i + vec3(0,0,1)), hash(i + vec3(1,0,1)), u.x), mix(hash(i + vec3(0,1,1)), hash(i + vec3(1,1,1)), u.x), u.y), u.z);
}
mat3 drift() {
	float a = TIME * 0.004;
	return mat3(vec3(cos(a), 0.0, -sin(a)), vec3(0.0, 1.0, 0.0), vec3(sin(a), 0.0, cos(a)));
}
float clouds(vec3 d) {
	vec3 q = d * vec3(7.0, 15.0, 7.0);
	// domain warp for swirls
	q += vec3(vn(q * 0.5), vn(q * 0.5 + 7.3), vn(q * 0.5 + 3.1)) * 1.6;
	float n = 0.0, amp = 0.5;
	for (int o = 0; o < 7; o++) { n += vn(q) * amp; q *= 2.11; amp *= 0.5; }
	float band = 0.5 + 0.5 * sin(d.y * 13.0 + n * 3.0);
	return n * 0.85 + band * 0.2;
}
void vertex() {
	objp = VERTEX;
	up_v = normalize((MODELVIEW_MATRIX * vec4(VERTEX, 0.0)).xyz);
}
void fragment() {
	vec3 d = normalize(objp);
	float facing = clamp(dot(NORMAL, VIEW), 0.0, 1.0);
	float edge = smoothstep(0.03, 0.3, facing);
	if (shadow_mode > 0.5) {
		// project toward the sun up to the deck and test the cloud there
		vec3 s = normalize(sun_obj);
		float sun_h = dot(s, d);
		vec3 tang = s - d * sun_h;
		vec3 dd = normalize(d + tang * shadow_offset / max(sun_h, 0.15));
		float c = smoothstep(coverage, coverage + 0.08, clouds(drift() * dd));
		ALBEDO = vec3(0.0);
		ALPHA = c * 0.45 * opacity * edge * smoothstep(0.0, 0.15, sun_h);
	} else {
		float v = clouds(drift() * d);
		float c = smoothstep(coverage, coverage + 0.08, v);
		// thick cores bright, thin edges and bases a little grey-blue
		float thick = smoothstep(coverage + 0.05, coverage + 0.35, v);
		ALBEDO = mix(vec3(0.72, 0.76, 0.84), vec3(1.0), thick);
		ALPHA = c * 0.92 * opacity * edge;
	}
	ROUGHNESS = 1.0;
	SPECULAR = 0.0;
}
void light() {
	float shadow = smoothstep(-0.05, 0.06, dot(up_v, LIGHT));
	float ndl = clamp((dot(NORMAL, LIGHT) + 0.15) / 1.15, 0.0, 1.0);
	DIFFUSE_LIGHT += LIGHT_COLOR * ATTENUATION * ndl * shadow / PI;
}
"""

static var _planet_shader: Shader
static var _cloud_shader: Shader
static var _giant_shader: Shader
static var _tex_cache := {}   # key -> the textures, shared by every view this run
static var _ring_shader: Shader


## How a world that keeps one face to its star looks (data look "locked"):
## the point under the star (body-fixed, sim axes), the desert's and the
## night's edges (deg from it), and the twilight's and the night's colours.
static func locked_style(lk: Dictionary, day_lon_deg: float) -> Dictionary:
	var lo := deg_to_rad(day_lon_deg)
	return {"locked": true, "locked_dir": Vector3(cos(lo), sin(lo), 0.0), "day_deg": float(lk.get("day_deg", 60.0)),
		"dusk_deg": float(lk.get("dusk_deg", 105.0)), "dusk": Sim.col(lk.get("dusk"), Color(0.2, 0.24, 0.14)),
		"night": Sim.col(lk.get("night"), Color(0.78, 0.83, 0.92))}


## A world light-years away: a plain sphere in its colours until we get there.
static func placeholder_material(lk: Dictionary) -> StandardMaterial3D:
	var m := StandardMaterial3D.new()
	m.albedo_color = Sim.col(lk.get("low"), Color(0.5, 0.5, 0.5))
	m.roughness = 0.9
	return m


## A gas giant: no ground to bake, only weather. Bands of cloud (the data's
## colours, pole to pole) that flow and curl, a great storm, and the shadow
## of its rings across them.
static func giant_material(lk: Dictionary, radius: float) -> ShaderMaterial:
	if _giant_shader == null:
		_giant_shader = Shader.new()
		_giant_shader.code = GIANT_SHADER
	var m := ShaderMaterial.new()
	m.shader = _giant_shader
	# (the data's colours spread over the shader's eight bands, pole to pole,
	# each colour coming back on the far side of the equator)
	var given: Array = lk.get("bands", [[0.8, 0.7, 0.55]])
	var bands := PackedVector3Array()
	for k in 8:
		var c: Array = given[k % given.size()] if k < 4 else given[(7 - k) % given.size()]
		bands.append(Vector3(c[0], c[1], c[2]))
	m.set_shader_parameter("bands", bands)
	var st: Dictionary = lk.get("storm", {})
	if not st.is_empty():
		var la := deg_to_rad(float(st.lat_deg))
		var lo := deg_to_rad(float(st.lon_deg))
		m.set_shader_parameter("storm_dir", Vector3(cos(la) * cos(lo), cos(la) * sin(lo), sin(la)))
		m.set_shader_parameter("storm_size", deg_to_rad(float(st.get("size_deg", 8.0))))
		m.set_shader_parameter("storm_col", Sim.col(st.get("color"), Color(0.7, 0.35, 0.25)))
	var rg: Dictionary = lk.get("rings", {})
	if not rg.is_empty():
		m.set_shader_parameter("ring_in", float(rg.inner) / radius)
		m.set_shader_parameter("ring_out", float(rg.outer) / radius)
		m.set_shader_parameter("ring_opacity", float(rg.get("opacity", 0.7)))
	return m


## A ring system: a flat annulus in the planet's equator (its object frame,
## so it goes wherever the planet's node does; sizes in planet radii).
static func ring_node(rg: Dictionary, radius: float) -> MeshInstance3D:
	if _ring_shader == null:
		_ring_shader = Shader.new()
		_ring_shader.code = RING_SHADER
	var r0 := float(rg.inner) / radius
	var r1 := float(rg.outer) / radius
	var verts := PackedVector3Array()
	var uv := PackedVector2Array()
	var idx := PackedInt32Array()
	var n := 256
	for k in n + 1:
		var a := TAU * k / n
		var d := Vector3(cos(a), 0.0, sin(a))
		verts.append(d * r0)
		verts.append(d * r1)
		uv.append(Vector2(0.0, float(k) / n))
		uv.append(Vector2(1.0, float(k) / n))
		if k < n:
			var b := k * 2
			idx.append_array([b, b + 1, b + 2, b + 1, b + 3, b + 2])
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = verts
	arrays[Mesh.ARRAY_TEX_UV] = uv
	arrays[Mesh.ARRAY_INDEX] = idx
	var am := ArrayMesh.new()
	am.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	var m := ShaderMaterial.new()
	m.shader = _ring_shader
	m.set_shader_parameter("tint", Sim.col(rg.get("color"), Color(0.85, 0.8, 0.72)))
	m.set_shader_parameter("opacity", float(rg.get("opacity", 0.7)))
	m.set_shader_parameter("r0", r0)
	m.set_shader_parameter("r1", r1)
	var mi := MeshInstance3D.new()
	mi.mesh = am
	mi.material_override = m
	mi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	mi.name = "Rings"
	return mi


static func planet_material(core: TrsSim, body: int) -> ShaderMaterial:
	var tex := textures(core, body)
	if _planet_shader == null:
		_planet_shader = Shader.new()
		_planet_shader.code = PLANET_SHADER
	var m := ShaderMaterial.new()
	m.shader = _planet_shader
	m.set_shader_parameter("albedo_tex", tex.albedo)
	m.set_shader_parameter("normal_tex", tex.normal)
	return m


## A world's cloud deck; cover: the look's "cloud_cover" (0.5 is Floe's;
## higher is clearer sky).
static func cloud_material(cover := 0.5) -> ShaderMaterial:
	if _cloud_shader == null:
		_cloud_shader = Shader.new()
		_cloud_shader.code = CLOUD_SHADER
	var m := ShaderMaterial.new()
	m.shader = _cloud_shader
	m.set_shader_parameter("coverage", cover)
	return m


static func textures(core: TrsSim, body: int) -> Dictionary:
	var info: Dictionary = JSON.parse_string(core.body_info(body))
	var bd: Dictionary = Sim.body_data(str(info.id))
	# (everything the map depends on: the ground, its landmarks, where it faces)
	var tdata := JSON.stringify(bd.get("terrain", {})) + str(bd.get("launch_site", {})) + JSON.stringify(bd.get("landmarks", [])) + str(bd.get("day_lon_deg", ""))
	var key := "%s_%d_%d" % [info.id, VERSION, hash(tdata + str(Sim.look(info.id))) & 0x7fffffff]
	if _tex_cache.has(key):
		return _tex_cache[key]
	var dir := "user://cache/"
	DirAccess.make_dir_recursive_absolute(dir)
	var ap := dir + key + "_albedo.png"
	var np := dir + key + "_normal.png"
	var a_img: Image
	var n_img: Image
	if FileAccess.file_exists(ap) and FileAccess.file_exists(np):
		a_img = Image.load_from_file(ProjectSettings.globalize_path(ap))
		n_img = Image.load_from_file(ProjectSettings.globalize_path(np))
	else:
		var baked := _bake(core, body, info)
		a_img = baked[0]
		n_img = baked[1]
		a_img.save_png(ap)
		n_img.save_png(np)
	a_img.generate_mipmaps()
	n_img.generate_mipmaps()
	_tex_cache[key] = {"albedo": ImageTexture.create_from_image(a_img), "normal": ImageTexture.create_from_image(n_img)}
	return _tex_cache[key]


static func _bake(core: TrsSim, body: int, info: Dictionary) -> Array:
	var h: PackedFloat64Array = core.terrain_map(body, W, H)
	var R: float = info.radius
	var a_img := Image.create(W, H, false, Image.FORMAT_RGB8)
	var n_img := Image.create(W, H, false, Image.FORMAT_RGB8)
	var lk: Dictionary = Sim.look(info.id)
	var style: String = lk.get("style", "rock")
	var ice_world: bool = style == "ice"
	var moss: bool = style == "moss"
	var ash: bool = style == "ash"
	var sand: bool = style == "sand"
	var locked: Dictionary = locked_style(lk, Sim.day_lon(str(info.id))) if lk.get("locked", false) else {}
	var sea := float(lk.get("sea_level", 350.0))
	var snow_line := float(lk.get("snow_line", 3200.0))
	var polar_snow := 1.0 if lk.get("polar_snow", true) else 0.0
	var lava := float(lk.get("lava_level", -350.0))
	var c_low := Sim.col(lk.get("low"))
	var c_high := Sim.col(lk.get("high"))
	var c_basin := Sim.col(lk.get("basin"), c_low)
	var c_rock := Sim.col(lk.get("rock"), Color(0.2, 0.2, 0.2))
	var tint := Sim.col(lk.get("tint"), Color(1, 1, 1))  # over the ice and rock styles' own colours
	var vols := []
	for lm in Sim.landmarks(info.id):
		if lm.get("kind", "") == "volcano":
			var la := deg_to_rad(float(lm.lat_deg))
			var lo := deg_to_rad(float(lm.lon_deg))
			vols.append([cos(la) * cos(lo), cos(la) * sin(lo), sin(la), float(lm.radius_km) * 1000.0])
	var noise := FastNoiseLite.new()
	noise.seed = hash(info.id)
	noise.frequency = 0.02
	for y in H:
		var lat := PI / 2 - (y + 0.5) / H * PI
		var cl := cos(lat)
		var sl := sin(lat)
		var dn := R * PI / H                       # metres per pixel north-south
		var de := maxf(R * cl * TAU / W, dn * 0.05) # metres per pixel east-west
		for x in W:
			var lon := (x + 0.5) / W * TAU - PI
			var i := y * W + x
			var hc: float = h[i]
			var he: float = h[y * W + (x + 1) % W]
			var hw: float = h[y * W + (x - 1 + W) % W]
			var hn: float = h[maxi(y - 1, 0) * W + x]
			var hs: float = h[mini(y + 1, H - 1) * W + x]
			var gx := (he - hw) / (2.0 * de)
			var gy := (hn - hs) / (2.0 * dn)
			var slope := sqrt(gx * gx + gy * gy)
			var nz := noise.get_noise_2d(x * 1.0, y * 2.0) * 0.5 + 0.5
			var col: Color
			if ice_world:
				var hk := clampf((hc - 200.0) / 3500.0, 0.0, 1.0)
				col = Color(0.70, 0.80, 0.92).lerp(Color(0.97, 0.98, 1.0), clampf(hk * 2.2 + nz * 0.1, 0.0, 1.0))
				if hc < 450.0:  # basins: blue sea ice with pale pressure ridges
					var b := clampf((450.0 - hc) / 450.0, 0.0, 1.0)
					col = col.lerp(Color(0.36, 0.58, 0.84).lerp(Color(0.62, 0.78, 0.94), nz), b * 0.85)
				col = col.lerp(Color(0.3, 0.31, 0.35), clampf((slope - 0.12) * 5.0, 0.0, 0.9))
				if absf(lat) > 1.2:  # polar caps
					col = col.lerp(Color(1.0, 1.0, 1.0), clampf((absf(lat) - 1.2) * 4.0, 0.0, 1.0))
			elif moss:
				# lowland forests of kelp-moss, olive uplands, dark still seas
				var hk := clampf((hc - 200.0) / 3800.0, 0.0, 1.0)
				col = c_low.lerp(c_high, clampf(pow(hk, 0.8) + nz * 0.12, 0.0, 1.0))
				col = col.lerp(c_rock, clampf((slope - 0.1) * 4.0, 0.0, 0.85))
				# snow up high and at the poles (the same line the near shader uses)
				var sn := clampf((hc - snow_line + 400.0 + (nz - 0.5) * 600.0) / 800.0 + clampf((absf(sin(lat)) - 0.88) / 0.09, 0.0, 1.0) * 5.0 * polar_snow, 0.0, 1.0)
				col = col.lerp(Color(0.92, 0.94, 0.97), sn)
				if hc < sea:
					col = col.lerp(c_basin.lerp(c_basin * 0.7, clampf((sea - hc) / 300.0, 0.0, 1.0)), clampf((sea - hc) / 60.0, 0.0, 1.0))
				col.a = 1.0
			elif sand:
				# sand seas in the low ground, red stone up high and on slopes;
				# a locked world's twilight dark with lichen, its far side ice
				var hk := clampf((hc + 800.0) / 2600.0, 0.0, 1.0)
				col = c_low.lerp(c_high, clampf(hk + (nz - 0.5) * 0.25, 0.0, 1.0))
				col = col.lerp(c_rock, clampf((slope - 0.1) * 4.0, 0.0, 0.85))
				if not locked.is_empty():
					var ld: Vector3 = locked.locked_dir
					var f := cos(lat) * cos(lon) * ld.x + cos(lat) * sin(lon) * ld.y + sin(lat) * ld.z + (nz - 0.5) * 0.06
					var dc := cos(deg_to_rad(float(locked.day_deg)))
					var nc := cos(deg_to_rad(float(locked.dusk_deg)))
					var night := 1.0 - smoothstep(nc - 0.04, nc + 0.04, f)
					var dusk := (1.0 - smoothstep(dc - 0.05, dc + 0.05, f)) * (1.0 - night)
					col = col.lerp(Color(locked.dusk).lerp(c_rock, clampf((slope - 0.1) * 3.0, 0.0, 0.6)), dusk)
					col = col.lerp(Color(locked.night).lerp(c_rock * 0.8, clampf((slope - 0.1) * 3.0, 0.0, 0.5)), night)
				col.a = 1.0
			elif ash:
				# black basalt, paler ash on the flats, slag pits glowing dull red
				var hk := clampf((hc + 1200.0) / 2600.0, 0.0, 1.0)
				col = c_low.lerp(c_high, clampf(hk + (nz - 0.5) * 0.3, 0.0, 1.0))
				col = col.lerp(Color(0.3, 0.28, 0.27), clampf(nz * 0.6 - 0.25, 0.0, 0.15) * (1.0 - clampf(slope * 5.0, 0.0, 1.0)))
				col = col.lerp(c_rock, clampf((slope - 0.08) * 4.0, 0.0, 0.8))
				if hc < lava:
					col = col.lerp(Color(0.45, 0.12, 0.04), clampf((lava - hc) / 400.0, 0.0, 0.8))
				col.a = 1.0
			else:
				var hk := clampf((hc + 1200.0) / 2600.0, 0.0, 1.0)
				col = Color(0.2, 0.195, 0.2).lerp(Color(0.66, 0.64, 0.6), pow(hk, 1.3))
				col = col * (0.95 + 0.1 * nz)
				col = col.lerp(Color(0.13, 0.125, 0.13), clampf((slope - 0.08) * 4.0, 0.0, 0.75))
				col.a = 1.0
			# a volcano's flanks: dark old lava, whatever the ground around it
			for v in vols:
				var c3: float = cos(lat) * cos(lon) * v[0] + cos(lat) * sin(lon) * v[1] + sin(lat) * v[2]
				var vx: float = acos(clampf(c3, -1.0, 1.0)) * R / float(v[3])
				if vx < 1.0:
					var dark := c_rock.lerp(Color(0.12, 0.11, 0.11), 0.5) if not ice_world else Color(0.28, 0.29, 0.33)
					col = col.lerp(dark, (1.0 - vx) * 0.7 * clampf(vx / 0.18, 0.3, 1.0))
			if style != "moss" and style != "ash":
				col = Color(minf(col.r * tint.r, 1.0), minf(col.g * tint.g, 1.0), minf(col.b * tint.b, 1.0))
			a_img.set_pixel(x, y, col)
			# normal in sim body-fixed axes, relief exaggerated for distance
			var k := 3.0 if ice_world or ash or sand else 7.0
			var ln := Vector3(-gx * k, -gy * k, 1.0).normalized()  # (east, north, up)
			var e := Vector3(-sin(lon), cos(lon), 0.0)
			var nv := Vector3(-sl * cos(lon), -sl * sin(lon), cl)
			var u := Vector3(cl * cos(lon), cl * sin(lon), sl)
			var n := (e * ln.x + nv * ln.y + u * ln.z).normalized()
			n_img.set_pixel(x, y, Color(n.x * 0.5 + 0.5, n.y * 0.5 + 0.5, n.z * 0.5 + 0.5))
	return [a_img, n_img]
