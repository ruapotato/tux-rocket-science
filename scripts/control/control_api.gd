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
## The control interface: JSON commands in, JSON replies out. The TCP control
## socket, the scripted gates, and agents all drive the game through this one
## function, so the 3D view is never needed to verify behaviour.
##
## Commands ({"cmd": name, ...}):
##   load_craft {path | text}         launch {body?}        revert {to: launch}
##   spawn_orbit {body, altitude, inc_deg?}
##   throttle {value}   attitude {pitch, yaw, roll}   sas {mode, target?[3]}
##   stage   legs {deployed}   switch {id}
##   warp {kind: physics|rails, index}   step {ticks | seconds}
##   query   vessels   events   predict {patches?, horizon?}   time
class_name ControlAPI
extends RefCounted

var session: FlightSession
var craft_text := ""


func _init(s: FlightSession = null) -> void:
	session = s if s != null else FlightSession.new()


func handle(cmd: Dictionary) -> Dictionary:
	var name: String = cmd.get("cmd", "")
	match name:
		"load_craft":
			var text: String = cmd.get("text", "")
			if text == "" and cmd.has("path"):
				var p: String = cmd.path
				if not p.begins_with("res://") and not p.begins_with("user://") and not p.begins_with("/"):
					p = Sim.STOCK_CRAFT_DIR + p + ("" if p.ends_with(".craft.json") else ".craft.json")
				if not FileAccess.file_exists(p):
					return _err("no such craft: " + p)
				text = FileAccess.get_file_as_string(p)
			var d := Sim.decode(text)
			if d.has("error"):
				return _err(str(d.error))
			craft_text = text
			return _ok({"name": d.meta.name, "parts": d.meta.parts.size()})
		"launch":
			if craft_text == "":
				return _err("load_craft first")
			if not session.launch(craft_text, cmd.get("body", "floe")):
				return _err(session.core.last_error())
			return _ok({"vessel": session.active})
		"spawn_orbit":
			if craft_text == "":
				return _err("load_craft first")
			if not session.spawn_in_orbit(craft_text, cmd.get("body", "floe"), float(cmd.get("altitude", 100000.0)),
					float(cmd.get("inc_deg", 0.0))):
				return _err(session.core.last_error())
			return _ok({"vessel": session.active})
		"revert":
			return _ok({"vessel": session.active}) if session.revert_to_launch() else _err("nothing to revert to")
		"throttle":
			session.set_throttle(float(cmd.get("value", 0.0)))
			return _ok()
		"attitude":
			session.set_input(float(cmd.get("pitch", 0.0)), float(cmd.get("yaw", 0.0)), float(cmd.get("roll", 0.0)))
			return _ok()
		"sas":
			var tgt := PackedFloat64Array()
			for x in cmd.get("target", []):
				tgt.append(float(x))
			return _ok() if session.set_sas(cmd.get("mode", "stability"), tgt) else _err("unknown sas mode")
		"stage":
			return _ok({"new_vessels": session.stage()})
		"legs":
			session.set_legs(bool(cmd.get("deployed", true)))
			return _ok()
		"switch":
			session.switch_to(int(cmd.get("id", session.active)))
			return _ok({"vessel": session.active})
		"warp":
			var reason := session.set_warp(cmd.get("kind", "rails"), int(cmd.get("index", 0)))
			return _ok() if reason == "" else _err(reason)
		"step":
			var n := int(cmd.get("ticks", 0))
			if cmd.has("seconds"):
				n = int(round(float(cmd.seconds) * FlightSession.PHYSICS_HZ))
			session.step_ticks(n)
			return _ok({"t": session.time()})
		"advance":
			session.advance(float(cmd.get("seconds", 0.0)))
			return _ok({"t": session.time()})
		"query":
			return _ok({"telemetry": session.telemetry()})
		"vessels":
			return _ok({"vessels": JSON.parse_string(session.core.vessel_list())})
		"events":
			var e := session.events.duplicate()
			session.events.clear()
			return _ok({"events": e})
		"predict":
			return _ok({"patches": session.predict(int(cmd.get("patches", 4)), float(cmd.get("horizon", 4.0e6)))})
		"time":
			return _ok({"t": session.time()})
		_:
			return _err("unknown command: " + name)


static func _ok(extra := {}) -> Dictionary:
	var d := {"ok": true}
	d.merge(extra)
	return d


static func _err(msg: String) -> Dictionary:
	return {"ok": false, "error": msg}
