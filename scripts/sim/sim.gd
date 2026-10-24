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
## Crew names picked in the hangar for the next launch.
var pending_crew: Array = []
## A line for the hangar to show after a recovered flight.
var last_mission_note := ""


func _init() -> void:
	core = TrsSim.new()
	var err: String = core.load_system(ProjectSettings.globalize_path(SYSTEM_PATH))
	if err != "":
		push_error("trs_core: " + err)
	err = core.load_catalog(ProjectSettings.globalize_path(CATALOG_PATH))
	if err != "":
		push_error("trs_core: " + err)
	var data: Dictionary = JSON.parse_string(FileAccess.get_file_as_string(CATALOG_PATH))
	for p in data.parts:
		catalog[p.id] = p
		catalog_order.append(p.id)


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
