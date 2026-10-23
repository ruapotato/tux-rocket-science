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
## Procedural audio: every sound is synthesized here (no sample assets).
## Continuous beds (engine rumble, wind) follow flight state; one-shots
## (stage, explosion, chute, touchdown, UI click, alarm) are short voices.
## In vacuum the rumble is what the crew feels through the hull: low and
## muffled; in air it opens up into a roar.
extends Node

const RATE := 22050.0

var engine_level := 0.0   # 0..1 thrust fraction
var engine_solid := 0.0   # 0..1 share of thrust from solid motors
var air := 1.0            # 0 vacuum .. 1 sea level
var wind_level := 0.0     # 0..1 from dynamic pressure
var master_volume := 0.8
var muted := false

var _player: AudioStreamPlayer
var _playback: AudioStreamGeneratorPlayback
var _voices: Array = []   # {kind, t, dur, ...}
var _rng := RandomNumberGenerator.new()
# filter states
var _brown := 0.0
var _lp1 := 0.0
var _lp2 := 0.0
var _hp_prev_in := 0.0
var _hp_prev_out := 0.0
var _wind_lp := 0.0
var _wind_lp2 := 0.0
var _crackle := 0.0
var _env_eng := 0.0
var _env_wind := 0.0
var _phase := 0.0


const SETTINGS := "user://settings.cfg"


func save_settings() -> void:
	var cf := ConfigFile.new()
	cf.load(SETTINGS)
	cf.set_value("audio", "volume", master_volume)
	cf.set_value("audio", "muted", muted)
	cf.save(SETTINGS)


func _ready() -> void:
	process_mode = Node.PROCESS_MODE_ALWAYS
	var cf := ConfigFile.new()
	if cf.load(SETTINGS) == OK:
		master_volume = cf.get_value("audio", "volume", master_volume)
		muted = cf.get_value("audio", "muted", muted)
	if DisplayServer.get_name() == "headless":
		set_process(false)
		return
	var gen := AudioStreamGenerator.new()
	gen.mix_rate = RATE
	gen.buffer_length = 0.12
	_player = AudioStreamPlayer.new()
	_player.stream = gen
	_player.bus = "Master"
	add_child(_player)
	_player.play()
	_playback = _player.get_stream_playback()
	_rng.seed = 77
	# every button in the game clicks
	get_tree().node_added.connect(func(n: Node):
		if n is BaseButton:
			(n as BaseButton).pressed.connect(func(): play("click")))


## Flight scene feeds the beds every frame.
func set_flight(thrust_frac: float, solid_frac: float, pressure_frac: float, q_kpa: float) -> void:
	engine_level = clampf(thrust_frac, 0.0, 1.0)
	engine_solid = clampf(solid_frac, 0.0, 1.0)
	air = clampf(pressure_frac, 0.0, 1.0)
	wind_level = clampf(q_kpa / 30.0, 0.0, 1.0)


func silence_beds() -> void:
	engine_level = 0.0
	wind_level = 0.0


func play(kind: String, strength := 1.0) -> void:
	if _playback == null:
		return
	var dur: float = {"stage": 0.6, "decouple": 0.35, "boom": 2.2, "chute": 1.4, "touchdown": 0.5, "click": 0.05,
		"alarm": 0.9, "chime": 0.8, "ignite": 0.9}.get(kind, 0.5)
	_voices.append({"kind": kind, "t": 0.0, "dur": dur, "k": clampf(strength, 0.0, 2.0), "lp": 0.0, "lp2": 0.0})


func _process(_dt: float) -> void:
	if _playback == null:
		return
	var n := _playback.get_frames_available()
	if n <= 0:
		return
	var vol := 0.0 if muted else master_volume
	var dt := 1.0 / RATE
	for i in n:
		# smooth the bed envelopes so nothing clicks
		_env_eng += (engine_level - _env_eng) * 0.0008
		_env_wind += (wind_level - _env_wind) * 0.0004
		var s := 0.0
		# --- engine bed: brown noise through two low-passes; air opens it up
		var white := _rng.randf() * 2.0 - 1.0
		_brown = clampf(_brown + white * 0.02, -1.0, 1.0) * 0.998
		var cut := 0.02 + 0.18 * air          # vacuum: hull-borne low rumble
		_lp1 += (white - _lp1) * cut
		_lp2 += (_lp1 - _lp2) * (cut * 0.6)
		var roar := _lp2 * (0.6 + 0.8 * air) + _brown * 0.9
		# solid motors crackle
		if engine_solid > 0.0 and _rng.randf() < 0.004 * engine_solid:
			_crackle = 1.0
		_crackle *= 0.995
		roar += _crackle * white * 0.6 * engine_solid * air
		s += roar * _env_eng * 0.55
		# --- wind: band-passed noise, gusting
		_wind_lp += (white - _wind_lp) * 0.08
		_wind_lp2 += (_wind_lp - _wind_lp2) * 0.04
		var gust := 0.7 + 0.3 * sin(_phase * 0.7) * sin(_phase * 1.9)
		s += (_wind_lp - _wind_lp2) * 2.2 * _env_wind * gust
		_phase += dt
		# --- one-shots
		for v in _voices:
			s += _voice(v, white, dt) * v.k
		_playback.push_frame(Vector2.ONE * clampf(s * vol, -1.0, 1.0))
	for k in range(_voices.size() - 1, -1, -1):
		if _voices[k].t >= _voices[k].dur:
			_voices.remove_at(k)


func _voice(v: Dictionary, white: float, dt: float) -> float:
	var t: float = v.t
	v.t = t + dt
	if t >= v.dur:
		return 0.0
	match v.kind:
		"stage":
			# pyro bolt crack + low thump
			var crack := white * exp(-t * 90.0) * 0.9
			var thump := sin(TAU * 55.0 * t * (1.0 - t * 0.5)) * exp(-t * 7.0) * 0.8
			return crack + thump
		"decouple":
			return white * exp(-t * 40.0) * 0.6 + sin(TAU * 90.0 * t) * exp(-t * 14.0) * 0.4
		"ignite":
			v.lp += (white - v.lp) * 0.05
			return v.lp * minf(t * 8.0, 1.0) * exp(-t * 2.5) * 2.0
		"boom":
			v.lp += (white - v.lp) * (0.25 * exp(-t * 2.0) + 0.01)
			v.lp2 += (v.lp - v.lp2) * 0.05
			var body: float = v.lp2 * 3.0 * exp(-t * 1.8)
			var sub := sin(TAU * 38.0 * t) * exp(-t * 3.0) * 0.8
			return (body + sub + white * exp(-t * 30.0)) * 0.9
		"chute":
			v.lp += (white - v.lp) * 0.12
			var env := sin(PI * clampf(t / v.dur, 0.0, 1.0))
			var flap := 0.6 + 0.4 * sin(TAU * 11.0 * t)
			return v.lp * env * flap * 1.1
		"touchdown":
			return sin(TAU * 70.0 * t) * exp(-t * 12.0) * 0.9 + white * exp(-t * 50.0) * 0.4
		"click":
			return sin(TAU * 1800.0 * t) * exp(-t * 120.0) * 0.25
		"chime":
			var e := exp(-t * 4.0) * 0.25
			return (sin(TAU * 660.0 * t) + sin(TAU * 990.0 * t) * 0.6 * float(t > 0.12)) * e
		"alarm":
			var on := float(fmod(t, 0.3) < 0.18)
			return sin(TAU * 880.0 * t) * on * 0.18
	return 0.0
