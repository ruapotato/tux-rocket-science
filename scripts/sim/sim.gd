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
## Global access to the authoritative simulation (trs_core). Views read from
## here; nothing in the scene tree is ever the source of truth.
extends Node

const SYSTEM_PATH := "res://data/bodies/austra_system.json"
const CATALOG_PATH := "res://data/parts/stock_parts.json"
const STOCK_CRAFT_DIR := "res://data/craft/"
const USER_CRAFT_DIR := "user://craft/"

var core: TrsSim
var catalog: Dictionary = {}  # id -> part dictionary (display data + nodes)
var catalog_order: Array[String] = []
## Craft handed between scenes (hangar -> flight -> back), canonical text.
var pending_craft_text := ""
var tutorial := false  # the next flight runs the First Flight walkthrough
## Crew names picked in the hangar for the next launch.
var pending_crew: Array = []
## A line for the hangar to show after a recovered flight.
var last_mission_note := ""
## Where the next launch starts: "pad" or "runway".
var pending_site := "pad"
## The next flight takes over this vessel already in the world (-1: launch).
var pending_vessel := -1

## What the last recovery brought home: {research, funds, results}.
var last_recovery := {}

## The persistent game: one world every launch joins, saved to disk.
var game: FlightSession
## Penguins aboard, by the uid of the crewed part they sit in: {"uid": [names]}.
var seats: Dictionary = {}
var save_dir := "user://save/"
var program_chosen := false


func _init() -> void:
	core = TrsSim.new()
	var err: String = core.load_system(ProjectSettings.globalize_path(SYSTEM_PATH))
	if err != "":
		push_error("trs_core: " + err)
	err = core.load_catalog(ProjectSettings.globalize_path(CATALOG_PATH))
	if err != "":
		push_error("trs_core: " + err)
	for b in system_bodies():
		_looks[b.id] = b.get("look", {})
	var data: Dictionary = JSON.parse_string(FileAccess.get_file_as_string(CATALOG_PATH))
	for p in data.parts:
		catalog[p.id] = p
		catalog_order.append(p.id)


var _looks := {}


## How a body looks (data/bodies "look"): style, sky, tau, clouds, colours.
## Every body's data: the home system file and the neighbours it lists,
## in the order the core loads them.
static func system_bodies() -> Array:
	var main: Dictionary = JSON.parse_string(FileAccess.get_file_as_string(SYSTEM_PATH))
	var out: Array = main.bodies.duplicate()
	for f in main.get("neighbours", []):
		out.append_array(JSON.parse_string(FileAccess.get_file_as_string(SYSTEM_PATH.get_base_dir() + "/" + str(f))).bodies)
	return out


func look(body_id: String) -> Dictionary:
	return _looks.get(body_id, {})


static func col(a, fallback := Color(0.5, 0.5, 0.5)) -> Color:
	return Color(a[0], a[1], a[2]) if a is Array and a.size() >= 3 else fallback


func part(id: String) -> Dictionary:
	return catalog.get(id, {})


func node_of(part_id: String, node_name: String) -> Dictionary:
	for n in part(part_id).get("nodes", []):
		if n.name == node_name:
			return n
	return {}


## Craft geometry crosses into trs_core only as doubles (PackedFloat64Array):
## Godot's float<->text conversions are not correctly rounded, so GDScript
## never formats or parses an authoritative number. Meta is strings/ints.

## Readouts for a craft, computed by trs_core against a body's surface
## gravity and sea-level pressure.
func stats(meta: Dictionary, floats: PackedFloat64Array, body_id := "floe") -> Dictionary:
	return JSON.parse_string(core.craft_stats_bin(JSON.stringify(meta), floats, body_id))


## Where a feed line placed on `parent` at line = pos[3] + rot[4] would pipe
## to: [target part or -1, end x, y, z] (vessel-local), as the sim decides.
func feed_aim(meta: Dictionary, floats: PackedFloat64Array, part_id: String, parent: int, line: PackedFloat64Array) -> PackedFloat64Array:
	return core.feed_aim(JSON.stringify(meta), floats, part_id, parent, line)


## Readouts straight from saved craft text (no GDScript number handling).
func stats_text(text: String, body_id := "floe") -> String:
	return core.craft_stats(text, body_id)


## The exact bytes the game writes for a craft (canonical form).
func encode(meta: Dictionary, floats: PackedFloat64Array) -> String:
	return core.craft_encode(JSON.stringify(meta), floats)


## {meta, floats} for saved craft text, or {error}.
func decode(text: String) -> Dictionary:
	var meta: Dictionary = JSON.parse_string(core.craft_decode(text))
	if meta.has("error"):
		return meta
	return {"meta": meta, "floats": core.craft_decode_floats(text)}


# ---- the persistent game -------------------------------------------------------

## Gates and dev runs play in a scratch save that starts empty every run.
func use_sandbox() -> void:
	program_chosen = true  # gates never switch to the player's saves
	save_dir = "user://save_sandbox/"
	game = null
	seats = {}
	DirAccess.make_dir_recursive_absolute(save_dir)
	for f in ["world.json", "game.json", "crew.json", "career.json"]:
		DirAccess.remove_absolute(ProjectSettings.globalize_path(save_dir + f))
	Crew.path = save_dir + "crew.json"
	Crew.load_roster()
	Career.load_from(save_dir + "career.json", "sandbox")


## Switches between the two games: "sandbox" (everything unlocked, free;
## the original save) and "career" (its own world, crew and program).
func use_program(kind: String) -> void:
	program_chosen = true
	save_dir = "user://career/" if kind == "career" else "user://save/"
	game = null
	seats = {}
	DirAccess.make_dir_recursive_absolute(save_dir)
	Crew.path = save_dir + "crew.json" if kind == "career" else "user://crew.json"
	Crew.load_roster()
	Career.load_from(save_dir + "career.json", kind)
	var f := FileAccess.open("user://program.txt", FileAccess.WRITE)
	if f:
		f.store_string(kind)


## The program played last time (sandbox if never chosen).
func last_program() -> String:
	var k := FileAccess.get_file_as_string("user://program.txt").strip_edges()
	return k if k == "career" else "sandbox"


## The game's world, loaded from the save the first time it is needed.
func game_session() -> FlightSession:
	if game == null:
		game = FlightSession.new()
		var wtext := FileAccess.get_file_as_string(save_dir + "world.json")
		if wtext != "" and not game.core.world_load(wtext):
			push_warning("saved world unreadable, starting a new one: " + game.core.last_error())
		var gtext := FileAccess.get_file_as_string(save_dir + "game.json")
		var g = JSON.parse_string(gtext) if gtext != "" else null
		seats = g.get("seats", {}) if g is Dictionary else {}
		if Career.path != save_dir + "career.json":
			Career.load_from(save_dir + "career.json", "career" if save_dir == "user://career/" else "sandbox")
		sync_crew()
	return game


## Writes the world (as trs_core's own text) and the seating to disk.
func save_game() -> void:
	if game == null:
		return
	DirAccess.make_dir_recursive_absolute(save_dir)
	var f := FileAccess.open(save_dir + "world.json", FileAccess.WRITE)
	if f:
		f.store_string(game.core.world_save())
	f = FileAccess.open(save_dir + "game.json", FileAccess.WRITE)
	if f:
		f.store_string(JSON.stringify({"version": 1, "seats": seats}, "  "))


## Vessels in the world: vessel_list entries, plus "crew_names".
func vessels() -> Array:
	var out := []
	if game == null:
		return out
	for v in JSON.parse_string(game.core.vessel_list()):
		var names := []
		for uid in v.get("pods", []):
			names.append_array(seats.get(str(int(uid)), []))
		v["crew_names"] = names
		out.append(v)
	return out


## Seats `names` in the crewed parts of vessel `id`, filling each in turn.
func seat_crew(id: int, names: Array) -> void:
	var left := names.duplicate()
	for p in JSON.parse_string(game.core.vessel_parts(id)):
		var cap := int(part(p.part).get("crew", 0))
		if cap <= 0 or left.is_empty():
			continue
		seats[str(int(p.uid))] = left.slice(0, cap)
		left = left.slice(cap)
	for n in names:
		var c := Crew.find(n)
		if not c.is_empty():
			c.status = "flying"
	Crew.save_roster()


## The crew aboard vessel `id`.
func crew_of(id: int) -> Array:
	for v in vessels():
		if int(v.id) == id:
			return v.crew_names
	return []


## Vessel `id` comes home: its crew are recovered (with these mission
## stats) and it leaves the world.
func recover_vessel(id: int, stats: Dictionary) -> Array:
	var names := crew_of(id)
	last_recovery = Career.recover(id, game.core.vessel_serial(id))
	_unseat(id)
	if not names.is_empty():
		Crew.record_flight(names, "recovered", stats)
	game.core.vessel_remove(id)
	return names


func _unseat(id: int) -> void:
	for v in vessels():
		if int(v.id) == id:
			for uid in v.get("pods", []):
				seats.erase(str(int(uid)))


## Seats whose part no longer exists were lost with it (a crash, burning
## up, or a crewed wreck dropped from the world): their penguins are lost.
## Anyone still marked flying without a seat comes home (old saves).
func sync_crew(stats := {}) -> Array:
	var alive := {}
	var parts := {}
	if game != null:
		for v in JSON.parse_string(game.core.vessel_list()):
			for uid in v.get("pods", []):
				alive[str(int(uid))] = true
			for p in JSON.parse_string(game.core.vessel_parts(int(v.id))):
				parts[str(int(p.uid))] = true
		Career.sync(parts)  # results on lost parts are lost with them
	var lost := []
	for uid in seats.keys():
		if not alive.has(uid):
			lost.append_array(seats[uid])
			seats.erase(uid)
	if not lost.is_empty():
		Crew.record_loss(lost, stats)
	var seated := {}
	for uid in seats:
		for n in seats[uid]:
			seated[n] = true
	var changed := false
	for c in Crew.crew:
		if c.status == "flying" and not seated.has(c.name):
			c.status = "available"
			changed = true
	if changed:
		Crew.save_roster()
	return lost


static func craft_files() -> Array[String]:
	var out: Array[String] = []
	for dir in [STOCK_CRAFT_DIR, USER_CRAFT_DIR]:
		var d := DirAccess.open(dir)
		if d == null:
			continue
		for f in d.get_files():
			if f.ends_with(".craft.json"):
				out.append(dir + f)
	out.sort()
	return out
